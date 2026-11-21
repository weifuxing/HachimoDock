"use strict";
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { displayText, snapshotPayload, createSnapshotTracker, isDshFollowEnabled, startPolling, startDshMonitor } = require('../agents/dsh-status-monitor');
const { LocalStatePublisher } = require('../packages/clawd-backend-service/src/headless-mqtt');
const { SessionMetricsTracker } = require('../packages/clawd-backend-service/src/status-metrics');
const row = (id = 'synthetic', state = 'working', revision = 1, extra = {}) => ({ id, state,
  stateDetailUnknown: false, epoch: 'a'.repeat(64), transitionRevision: revision, sourceSeq: revision,
  name: `fixture ${id}`, lastModified: revision, ...extra });
const flush = () => new Promise(resolve => setImmediate(resolve));

// Only synthetic rows, workers and private temporary directories. No DSH install/user profile.
test('explicit enabled list controls both platforms, including stale selection on disable', () => {
  for (const platform of ['win32', 'darwin']) {
    assert.equal(isDshFollowEnabled({ platform }), false);
    assert.equal(isDshFollowEnabled({ platform, CLAWD_ENABLED_AGENTS: 'codex', CLAWD_SELECTED_AGENT_ID: 'deepseek-harness' }), false);
    assert.equal(isDshFollowEnabled({ platform, CLAWD_ENABLED_AGENTS: '', CLAWD_SELECTED_AGENT_ID: 'deepseek-harness' }), false);
    assert.equal(isDshFollowEnabled({ platform, CLAWD_ENABLED_AGENTS: 'codex, deepseek-harness' }), true);
    assert.equal(isDshFollowEnabled({ platform, CLAWD_SELECTED_AGENT_ID: 'deepseek-harness' }), true);
  }
});

test('payload is bounded, Unicode safe, allowlisted and sanitized even for terminal/title', () => {
  const privateText = '<think>hidden-thought</think> ```private-command``` Bearer private-token password=private-password C:\\Users\\fixture\\private /home/fixture/private sk-abcdefghijklmno';
  const p = snapshotPayload(row('safe', 'done', 2, { name: privateText, displayContent: privateText,
    cwd: 'private-cwd', transcriptPath: 'private-path', raw: 'private-raw', token: 'private-key' }), 100);
  for (const secret of ['hidden-thought', 'private-command', 'private-token', 'private-password', 'private-cwd', 'private-path', 'private-raw', 'private-key', 'abcdefghijklmno']) assert.ok(!JSON.stringify(p).includes(secret), secret);
  assert.ok(!p.display.content.includes('fixture'));
  const unicode = displayText('汉'.repeat(318) + '🙂后续文字');
  assert.equal(Buffer.from(unicode).toString(), unicode);
  assert.ok(Array.from(unicode).length <= 320);
  assert.equal(snapshotPayload(row('bad\nidentity')), null);
  assert.equal(snapshotPayload(row('x', 'done', 2, { epoch: '' })), null);
  assert.equal(snapshotPayload(row('x', 'unknown', 0, { stateDetailUnknown: true, active: false, activitySource: 'live-agent-status' })), null);
  assert.equal(snapshotPayload(row('x', 'unknown', 0, { stateDetailUnknown: true, active: true, activitySource: 'live-agent-status' })).state, 'working');
  assert.equal(snapshotPayload(row('x', 'stopped', 2)).state, 'stopped');
  assert.equal(snapshotPayload(row('x', 'working', 3, { displayContent: 'old answer' })).display.content, '正在思考');
});

test('done/error/stopped never heartbeat, replay at 60 seconds, disappearance or cache eviction', () => {
  for (const state of ['done', 'error', 'stopped']) {
    const t = createSnapshotTracker({ maxSessions: 1 });
    assert.equal(t.consume([row('x', state, 2)], 1).length, 0, 'bootstrap terminal is silent');
    assert.equal(t.consume([row('x', 'working', 3)], 2).length, 1);
    assert.equal(t.consume([row('x', state, 4)], 3).length, 1);
    for (const time of [25003, 60004, 120004]) assert.deepEqual(t.consume([row('x', state, 4)], time), []);
    t.consume([], 120005);
    assert.deepEqual(t.consume([row('x', state, 4)], 120006), []);
    t.consume([row('y')], 120007); // Evicted tombstone must also fail closed.
    assert.deepEqual(t.consume([row('x', state, 4)], 120008), []);
  }
});

test('active heartbeat is throttled without changing display age; duplicates and old revisions are inert', () => {
  const t = createSnapshotTracker();
  const first = t.consume([row()], 100)[0].payload;
  assert.equal(t.consume([row(), row()], 101).length, 0);
  assert.equal(t.consume([row()], 25099).length, 0);
  const heartbeat = t.consume([row()], 25100)[0].payload;
  assert.equal(heartbeat.display.updatedAtMs, first.display.updatedAtMs);
  assert.equal(heartbeat.tsMs, 25100);
  t.consume([row('synthetic', 'working', 5)], 26000);
  assert.equal(t.consume([row('synthetic', 'done', 4)], 26001).length, 0);
  assert.equal(t.consume([row('synthetic', 'done', 6)], 26002).length, 1);
  assert.equal(t.consume([row('synthetic', 'done', 6, { name: 'renamed old title' })], 90000).length, 0);
});

test('plugin epochs are isolated and old boot cannot revive a completed card', () => {
  const t = createSnapshotTracker();
  t.consume([row()], 1);
  assert.equal(t.consume([row('synthetic', 'done', 2, { epoch: 'b'.repeat(64) })], 2).length, 0);
  assert.equal(t.consume([row('synthetic', 'working', 3)], 3).length, 0);
  assert.equal(t.consume([row('synthetic', 'working', 3, { epoch: 'b'.repeat(64) })], 4).length, 1);
  assert.equal(t.consume([row('a'), row('b', 'working', 1, { epoch: 'b'.repeat(64) })], 5).length, 0);
});

test('real publisher/metrics keep working source while independent session completes; no repeated completion event', async t => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'dsh-follow-test-')); t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const tracker = createSnapshotTracker(), metrics = new SessionMetricsTracker();
  const publisher = new LocalStatePublisher({ localStateDir: dir });
  const publish = (rows, now) => tracker.consume(rows, now).map(({ payload, sessionOnly }) => {
    const enriched = metrics.apply(payload, now); publisher.publishSource(enriched, { sessionOnly }); return payload;
  });
  const now = Date.now();
  publish([row('old', 'working', 1), row('current', 'working', 5)], now);
  const terminal = publish([row('old', 'done', 6), row('current', 'working', 5)], now + 100);
  assert.equal(terminal.length, 1);
  const read = () => JSON.parse(fs.readFileSync(path.join(dir, 'deepseek-harness.json'), 'utf8'));
  assert.equal(read().sessionId, 'current'); assert.equal(read().state, 'working');
  const initial = metrics.sessionEvents('deepseek-harness', { cursor: 0 });
  assert.equal(initial.events.filter(e => e.state === 'done').length, 1);
  const done = initial.events.find(e => e.state === 'done');
  assert.equal(done.session.epoch, 'a'.repeat(64)); assert.equal(done.session.transitionRevision, 6);
  for (const elapsed of [25100, 60100, 120100]) publish([row('old', 'done', 6), row('current', 'working', 5)], now + elapsed);
  assert.equal(metrics.sessionEvents('deepseek-harness', { cursor: initial.cursor }).events.length, 0, 'heartbeats do not create completion/sound edges');
  assert.equal(read().sessionId, 'current');
  // Exercise the production 60s visibility policy, including repeated terminal snapshots.
  const { buildP4ConversationQueue } = await import('../../../src/lib/p4-session-service.js');
  let queue = buildP4ConversationQueue(initial.events.filter(e => e.state === 'working').map(e => e.session), [], now);
  queue = buildP4ConversationQueue(initial.events.slice(-2).map(e => e.session), queue, now + 100);
  assert.ok(queue.some(s => s.id === 'old' && s.state === 'done'));
  const snapshots = [done.session, initial.events.find(e => e.session.id === 'current').session];
  queue = buildP4ConversationQueue(snapshots, queue, now + 60101);
  assert.ok(!queue.some(s => s.id === 'old'));
  queue = buildP4ConversationQueue(snapshots, queue, now + 120101);
  assert.deepEqual(queue.map(s => s.id), ['current']);
});

test('poller is single-flight, minimum throttled, backs off failures and cancels late results', async () => {
  let finish, calls = 0, warnings = 0, results = 0;
  const timers = [];
  const adapter = { listSessions(options) { calls++; assert.equal(options.strict, true); return new Promise(resolve => { finish = resolve; }); } };
  const poller = startPolling({ adapter, pollIntervalMs: 0, onRows() { results++; }, onUnavailable() { warnings++; }, schedule(fn, delay) { timers.push({ fn, delay }); return 1; }, cancel() {} });
  await flush(); assert.equal(calls, 1); assert.equal(timers.length, 0);
  finish([]); await flush(); assert.equal(timers[0].delay, 1500);
  timers.shift().fn(); await flush(); assert.equal(calls, 2);
  poller.stop(); finish([]); await flush(); assert.equal(results, 1); assert.equal(timers.length, 0); assert.equal(warnings, 0);
  const failures = [];
  const failing = startPolling({ adapter: { async listSessions() { throw Error('private raw credential/path'); } }, onRows() { assert.fail(); }, onUnavailable(...args) { assert.deepEqual(args, []); warnings++; }, schedule(fn, delay) { failures.push({ fn, delay }); }, cancel() {} });
  await flush(); assert.equal(failures[0].delay, 3000);
  failures.shift().fn(); await flush(); assert.equal(failures[0].delay, 6000); assert.equal(warnings, 1);
  failing.stop();
});

test('disabled never creates a Worker; restart retains dedup; stop fences callbacks and retry', async () => {
  const workers = [], retries = [];
  class FakeWorker extends EventEmitter {
    constructor() { super(); workers.push(this); }
    terminate() { this.terminated = true; return Promise.resolve(0); }
  }
  let emissions = 0;
  startDshMonitor({ enabled: false, WorkerClass: FakeWorker }).stop(); assert.equal(workers.length, 0);
  const monitor = startDshMonitor({ WorkerClass: FakeWorker, onState() { emissions++; }, schedule(fn, delay) { retries.push({ fn, delay }); }, cancel() {} });
  workers[0].emit('message', { kind: 'rows', rows: [row()] });
  workers[0].emit('message', { kind: 'rows', rows: [row('synthetic', 'done', 2)] });
  assert.equal(emissions, 2);
  workers[0].emit('error', Error('private-error')); workers[0].emit('exit', 1);
  assert.equal(retries.length, 1); assert.equal(retries[0].delay, 5000);
  retries.shift().fn(); workers[1].emit('message', { kind: 'rows', rows: [row('synthetic', 'done', 2)] });
  assert.equal(emissions, 2);
  await monitor.stop(); workers[1].emit('message', { kind: 'rows', rows: [row('synthetic', 'working', 3)] });
  workers[1].emit('exit', 0); assert.equal(emissions, 2); assert.equal(retries.length, 0);
});

test('packaged resources include runtime only; startup/shutdown and native Windows launch stay wired', () => {
  const root = path.resolve(__dirname, '../../../..');
  const config = JSON.parse(fs.readFileSync(path.join(root, 'pc/src-tauri/tauri.conf.json')));
  const resources = Object.keys(config.bundle.resources);
  const included = relative => resources.some(entry => relative === entry || relative.startsWith(`${entry}/`));
  assert.ok(included('bridge/agents/dsh-status-monitor.js'));
  assert.ok(!included('bridge/test/dsh-status-monitor.test.js'));
  assert.ok(!included('../../integrations/dsh-hachimodock/test/follow-monitor.test.js'));
  assert.ok(!resources.some(entry => /follow-fixes|source-layout|installed-layout|test-dsh-monitor|Downloads/.test(entry)));
  const headless = fs.readFileSync(path.join(root, 'pc/src-tauri/bridge/packages/clawd-backend-service/src/headless-mqtt.js'), 'utf8');
  assert.match(headless, /enabled: config\.dsh\.enabled/); assert.match(headless, /dshMonitor\.stop\(\)/);
  assert.match(headless, /agentId === "deepseek-harness" \? \[\] : metricsTracker.sessionStatuses/);
  const native = fs.readFileSync(path.join(root, 'pc/src-tauri/src/lib.rs'), 'utf8');
  assert.match(native, /\$env:CLAWD_ENABLED_AGENTS = \{enabled_agents\}/);
  assert.match(native, /command.env\("CLAWD_ENABLED_AGENTS", enabled_agents_csv\(profile\)\)/);
});


test('brief missing active session retains source priority; a long outage cannot announce old completion', () => {
  const t = createSnapshotTracker();
  t.consume([row('old', 'working', 1), row('current', 'working', 5)], 1000);
  const result = t.consume([row('old', 'done', 2)], 2000);
  assert.equal(result.length, 1); assert.equal(result[0].sessionOnly, true);
  t.consume([], 65000);
  assert.equal(t.consume([row('current', 'done', 6)], 70000).length, 0);
});


test('duplicate rows resolve by live sequence instead of input order or stale mtime', () => {
  const t = createSnapshotTracker();
  t.consume([row('x', 'working', 1)], 1);
  const result = t.consume([row('x', 'working', 1, { lastModified: 999 }), row('x', 'done', 2)], 2);
  assert.equal(result.length, 1); assert.equal(result[0].payload.state, 'done');
  assert.equal(t.consume([row('x', 'done', 2), row('x', 'working', 1)], 3).length, 0);
});
