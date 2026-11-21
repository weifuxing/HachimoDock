import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { runInNewContext } from 'node:vm';
import { createDshConnectionController } from './dsh-connection.js';

test('only selecting DeepSeek installs, once across tab/StrictMode replay; explicit retry is available', async () => {
  const calls = [];
  const c = createDshConnectionController(async command => { calls.push(command); return { ok: true }; });
  for (const id of ['', 'workbuddy', 'codex', 'mimocode']) await c.select(id);
  assert.equal(calls.length, 0);
  await Promise.all([c.select('deepseek-harness'), c.select('deepseek-harness')]);
  await c.select('deepseek-harness');
  assert.deepEqual(calls, ['connect_dsh_desktop']);
  assert.equal(c.getState().phase, 'configured');
  assert.notEqual(c.getState().phase, 'connected', 'installation is not host readiness');
  await c.retry();
  assert.equal(calls.length, 2);
});

test('installation failure is visible and retry recovers without exposing native error details', async () => {
  let calls = 0;
  const c = createDshConnectionController(async () => {
    if (++calls === 1) throw new Error('synthetic-private-path-and-token');
    return { ok: true };
  });
  await c.select('deepseek-harness');
  assert.equal(c.getState().phase, 'error');
  assert.ok(!JSON.stringify(c.getState()).includes('synthetic-private'));
  await c.retry();
  assert.equal(c.getState().phase, 'configured');
});

test('selection changes cannot publish stale status or race two installers', async () => {
  const pending = []; let calls = 0;
  const c = createDshConnectionController(() => { calls++; return new Promise(resolve => pending.push(resolve)); });
  const old = c.select('deepseek-harness'); await Promise.resolve();
  await c.select('workbuddy'); c.select('deepseek-harness');
  assert.equal(calls, 1);
  pending.shift()({ ok: true }); await old; await Promise.resolve();
  assert.equal(calls, 2);
  const current = c.select('deepseek-harness');
  pending.shift()({ ok: true }); await current;
  assert.equal(c.getState().phase, 'configured');
});

test('leaving DeepSeek before the queued invocation starts never installs for another agent', async () => {
  let calls = 0;
  const c = createDshConnectionController(async () => { calls++; return { ok: true }; });
  const pending = c.select('deepseek-harness');
  await c.select('workbuddy');
  await pending;
  assert.equal(calls, 0);
  assert.equal(c.getState().phase, 'idle');
});

test('owner retirement cancels unsubmitted work and ignores late results without notifying an unmounted view', async () => {
  let calls = 0, resolve;
  const states = [];
  const c = createDshConnectionController(() => { calls++; return new Promise(r => { resolve = r; }); }, s => states.push(s.phase));
  const pending = c.select('deepseek-harness');
  await Promise.resolve();
  c.retire();
  const before = states.length;
  resolve({ ok: true });
  await pending;
  assert.equal(calls, 1);
  assert.equal(states.length, before);
  assert.equal(c.getState().phase, 'idle');
  await c.retry();
  assert.equal(calls, 1);
});

test('StrictMode setup-cleanup-setup admits only the current queued installer', async () => {
  let calls = 0;
  const c = createDshConnectionController(async () => { calls++; return { ok: true }; });
  const retired = c.select('deepseek-harness');
  c.retire();
  c.select('deepseek-harness');
  await retired;
  await c.select('deepseek-harness');
  assert.equal(calls, 1);
  assert.equal(c.getState().phase, 'configured');
});

test('repeated retry shares an outstanding request and malformed receipts cannot report success', async () => {
  let resolve, calls = 0;
  const c = createDshConnectionController(() => { calls++; return new Promise(r => { resolve = r; }); });
  const pending = c.select('deepseek-harness');
  assert.strictEqual(c.retry(), pending);
  assert.strictEqual(c.retry(), pending);
  await Promise.resolve();
  resolve({ ok: false });
  await pending;
  assert.equal(calls, 1);
  assert.equal(c.getState().phase, 'error');
});

test('App is the single persistent follow-install owner, including no-dashboard LAN feed', () => {
  const read = p => readFileSync(new URL(p, import.meta.url), 'utf8');
  const app = read('../App.jsx');
  assert.match(app, /useDshFollowConnection\(hasTauriRuntime\(\) \? selectedAgentId : ""\)/);
  assert.ok(app.indexOf('useDshFollowConnection(hasTauriRuntime()') < app.indexOf('<LanAgentSessionFeed'));
  assert.match(app, /!dashboardBinding && hasTauriRuntime\(\)/);
  assert.match(app, /selectedAgentId === "deepseek-harness" && \(isDashboard \|\| isSetup\)\s*&& <DshConnectionPanel/);
  for (const f of ['../DeviceDashboard.jsx', '../dashboard/VoiceAssistantPanel.jsx', '../dashboard/LanAgentSessionFeed.jsx']) {
    assert.doesNotMatch(read(f), /ensureOptionalDshDesktop|useDshFollowConnection\(/);
  }
  const panel = read('../dashboard/DshConnectionPanel.jsx');
  assert.match(panel, /onClick=\{connection.retry\}/);
  assert.match(panel, /重试连接/);
  assert.match(panel, /return \(\) => owner\.retire\(\)/);
});

test('configured/idle follow is silent while pending/error renders only useful status and retry', () => {
  const require = createRequire(import.meta.url);
  const React = require('react');
  const { renderToStaticMarkup } = require('react-dom/server');
  const { transformSync } = require('esbuild');
  const module = { exports: {} };
  const { code } = transformSync(readFileSync(new URL('../dashboard/DshConnectionPanel.jsx', import.meta.url), 'utf8'),
    { loader: 'jsx', format: 'cjs' });
  runInNewContext(code, { module, exports: module.exports, require: id => {
    if (id === 'react') return React;
    if (id === '../lib/dsh-connection.js') return { createDshConnectionController };
    if (id === '@tauri-apps/api/core') return { invoke: () => { throw Error('render must not invoke native'); } };
    throw Error(`unexpected import ${id}`);
  } });
  const render = phase => renderToStaticMarkup(React.createElement(module.exports.default,
    { connection: { phase, message: '合成连接状态', retry() {} } }));
  assert.equal(render('idle'), '');
  assert.equal(render('configured'), '');
  assert.match(render('pending'), /role="status"/);
  assert.doesNotMatch(render('pending'), /<button/);
  assert.match(render('error'), /role="alert"/);
  assert.match(render('error'), /重试连接/);
});
