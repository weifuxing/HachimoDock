"use strict";
// Real Hook CLI -> authenticated HTTP ingress -> metrics -> Agent Bus and
// USB-poller snapshot. Synthetic homes, capabilities and ephemeral ports only.
// The harness simulates Windows ACL startup latency.
const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const http = require("node:http");
const { spawn } = require("node:child_process");
const { syncWorkBuddyHooks } = require("../hooks/workbuddy-install");
const { createCapability, createCapabilityStore } = require("../packages/agent-session-bus/src/local-capability");
const { createAgentSessionBus, WorkBuddyAdapter } = require("../packages/agent-session-bus/src");
const { HookHttpServer, LocalStatePublisher, publishClawdState } = require("../packages/clawd-backend-service/src/headless-mqtt");
const { SessionMetricsTracker } = require("../packages/clawd-backend-service/src/status-metrics");

test("Windows WorkBuddy installed budget survives valid slow ACL checks and reaches the bus", async t => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "workbuddy-ingress-"));
  t.after(() => fs.rmSync(home, { recursive: true, force: true }));
  const root = path.join(home, ".workbuddy"), capDir = path.join(home, "capability");
  fs.mkdirSync(root);
  // Include an old installed hook: an upgrade must migrate its deadline too.
  const settings = path.join(root, "settings.json");
  fs.writeFileSync(settings, JSON.stringify({ hooks: { UserPromptSubmit: [{ hooks: [
    { type: "command", command: "old workbuddy-pet-manager-hook.js", timeout: 2 },
    { type: "command", command: "unrelated-user-hook", timeout: 2 },
  ] }] } }));
  const options = { home, env: {}, platform: "win32", nodeBin: "C:\\fixture\\node.exe" };
  syncWorkBuddyHooks(options);
  const config = JSON.parse(fs.readFileSync(settings));
  const own = config.hooks.UserPromptSubmit[0].hooks[0];
  assert.equal(config.hooks.UserPromptSubmit[0].hooks[1].timeout, 2);
  assert.equal(config.hooks.PreToolUse[0].hooks[0].timeout, own.timeout);
  assert.equal(syncWorkBuddyHooks(options).updated, false);

  const bootstrap = { epoch: "a".repeat(64), tokens: { hook: "b".repeat(64), read: "c".repeat(64), control: "d".repeat(64) } };
  const capability = createCapability(bootstrap);
  const descriptor = { version: 1, epoch: bootstrap.epoch, hookPort: 23333, busPort: null, tokens: { hook: bootstrap.tokens.hook } };
  const descriptorFile = path.join(capDir, "hook.json");
  const store = createCapabilityStore();
  // Elevated Windows runners can give Node-created directories a default
  // owner other than the current user SID. Let the production writer create
  // this private leaf with its explicit owner and protected ACL; precreating
  // it would correctly trigger the foreign-existing-owner rejection.
  assert.equal(fs.existsSync(capDir), false);
  store.writeDescriptor(descriptor, descriptorFile);
  assert.doesNotThrow(() => store.readDescriptor(descriptorFile), "production reader must verify the created capability ACL");
  const metricsTracker = new SessionMetricsTracker();
  const localStateDir = path.join(home, "snapshots");
  const publisher = new LocalStatePublisher({ enabledSources: ["workbuddy"], selectedSource: "workbuddy", localStateDir });
  let accepted = 0;
  const ingress = new HookHttpServer({ capability, onState(payload) {
    accepted++;
    publishClawdState({ publisher, metricsTracker }, payload);
  } });
  // Avoid HookHttpServer.start(): it intentionally publishes the real runtime
  // discovery file. This fixture only exercises its actual request handler.
  const server = http.createServer((req, res) => ingress.handleRequest(req, res));
  await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
  t.after(() => new Promise(resolve => server.close(resolve)));
  const port = server.address().port;
  const bus = createAgentSessionBus({ port: 0, capability, log() {},
    adapters: [new WorkBuddyAdapter({ home, env: {}, platform: "win32" })],
    sessionStatusProvider: id => metricsTracker.sessionStatuses(id),
    sessionEventProvider: (id, opts) => metricsTracker.sessionEvents(id, opts),
  });
  const busPort = await bus.start(); t.after(() => bus.stop());

  const preload = path.join(home, "preload.cjs");
  fs.writeFileSync(preload, `
    const http = require('node:http');
    const cap = require(${JSON.stringify(require.resolve("../packages/agent-session-bus/src/local-capability"))});
    const headers = cap.headersFor;
    cap.headersFor = (...args) => {
      // Model two valid 1.2s PowerShell checks, both below the production 5s limit.
      if (process.env.FIXTURE_SLOW_ACL === '1') {
        for (let i=0;i<2;i++) Atomics.wait(new Int32Array(new SharedArrayBuffer(4)),0,0,1200);
      }
      return headers(...args); // real private descriptor + port/role checks
    };
    const transport = require(${JSON.stringify(require.resolve("../hooks/server-config"))});
    const post = transport.postStateToRunningServer;
    transport.postStateToRunningServer = (body, options, done) => post(body, {
      ...options, preferredPort: 23333, runtimePort: 23333,
      probePort: (_port, _timeout, callback) => callback(false),
      httpRequest: (options, callback) => {
        if (options.hostname !== '127.0.0.1' || options.port !== 23333) throw new Error('fixture endpoint');
        return http.request({...options, port: ${port}}, callback);
      },
    }, done);
  `);
  const hook = path.resolve(__dirname, "../hooks/workbuddy-pet-manager-hook.js");
  async function run(event, timeoutMs, slow = false) {
    const child = spawn(process.execPath, ["--require", preload, hook], {
      env: { ...process.env, HOME: home, USERPROFILE: home, WORKBUDDY_CONFIG_DIR: root,
        HACHIMODOCK_BRIDGE_CAPABILITY_DIR: capDir, FIXTURE_SLOW_ACL: slow ? "1" : "0" },
      stdio: ["pipe", "pipe", "pipe"],
    });
    let stdout = "", stderr = "";
    child.stdout.on("data", data => { stdout += data; });
    child.stderr.on("data", data => { stderr += data; });
    const timer = setTimeout(() => child.kill(), timeoutMs);
    child.stdin.end(JSON.stringify({ hook_event_name: event, session_id: "synthetic-session", generation_id: "synthetic-turn",
      prompt: "Synthetic task", last_assistant_message: "Synthetic result", tool_input: { secret: "NEVER_FORWARD_SENTINEL" } }));
    const result = await new Promise((resolve, reject) => {
      child.on("error", reject); child.on("close", (code, signal) => resolve({ code, signal }));
    }).finally(() => clearTimeout(timer));
    return { ...result, stdout, stderr };
  }
  const old = await run("UserPromptSubmit", 2000, true);
  assert.equal(old.signal, "SIGTERM"); assert.equal(accepted, 0, "old budget kills the event before ingress");
  const repaired = await run("UserPromptSubmit", own.timeout * 1000, true);
  assert.equal(repaired.code, 0, "installed timeout must cover successful Windows authentication");
  assert.equal(repaired.stdout.trim(), "{}"); assert.equal(repaired.stderr, "");
  const auth = { headers: bus.authorizationHeaders("read") };
  for (const event of ["PreToolUse", "PostToolUse"]) {
    assert.equal((await run(event, own.timeout * 1000)).code, 0);
    const active = await (await fetch(`http://127.0.0.1:${busPort}/agent/sessions?agentId=workbuddy`, auth)).json();
    assert.equal(active.sessions[0].state, "working");
    assert.ok(active.sessions[0].displayContent, "an executing task must have bubble content before Stop");
    const snapshot = JSON.parse(fs.readFileSync(path.join(localStateDir, "workbuddy.json")));
    assert.equal(snapshot.state, "working"); assert.ok(snapshot.display.content);
  }
  assert.equal((await run("Stop", own.timeout * 1000)).code, 0);
  assert.equal(accepted, 4);
  const sessions = await (await fetch(`http://127.0.0.1:${busPort}/agent/sessions?agentId=workbuddy`, auth)).json();
  assert.equal(sessions.sessions[0].id, "synthetic-session");
  const events = await (await fetch(`http://127.0.0.1:${busPort}/agent/session-events?agentId=workbuddy&cursor=0`, auth)).json();
  assert.deepEqual(events.events.map(e => e.event), ["UserPromptSubmit", "PreToolUse", "PostToolUse", "Stop"]);
  const snapshot = JSON.parse(fs.readFileSync(path.join(localStateDir, "workbuddy.json")));
  assert.equal(snapshot.source, "workbuddy"); assert.equal(snapshot.state, "done");
  assert.equal(snapshot.display.content, "Synthetic result");
  assert.doesNotMatch(JSON.stringify({ sessions, events, snapshot }), /NEVER_FORWARD_SENTINEL/);
  // Increasing the process budget must not make stale/revoked credentials work.
  store.writeDescriptor({ ...descriptor, tokens: { hook: "e".repeat(64) } }, descriptorFile);
  assert.equal((await run("PreToolUse", own.timeout * 1000)).code, 0);
  assert.equal(accepted, 4);
});

test("Windows budget migration also repairs unchanged hook commands and preserves user hooks", t => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "workbuddy-budget-"));
  t.after(() => fs.rmSync(home, { recursive: true, force: true }));
  const root = path.join(home, ".workbuddy"); fs.mkdirSync(root);
  const settings = path.join(root, "settings.json");
  const options = { home, env: {}, platform: "win32", nodeBin: "C:\\fixture\\node.exe" };
  syncWorkBuddyHooks(options);
  const config = JSON.parse(fs.readFileSync(settings));
  const originalCommand = config.hooks.PreToolUse[0].hooks[0].command;
  config.hooks.PreToolUse[0].hooks[0].timeout = 2;
  config.hooks.Stop[0].hooks[0].timeout = 30;
  config.hooks.PreToolUse.push({ hooks: [{ type: "command", command: "user-custom", timeout: 1 }] });
  fs.writeFileSync(settings, JSON.stringify(config));
  assert.equal(syncWorkBuddyHooks(options).updated, true);
  const repaired = JSON.parse(fs.readFileSync(settings));
  assert.equal(repaired.hooks.PreToolUse[0].hooks[0].command, originalCommand);
  assert.equal(repaired.hooks.PreToolUse[0].hooks[0].timeout, 15);
  assert.equal(repaired.hooks.PreToolUse[1].hooks[0].timeout, 1);
  assert.equal(repaired.hooks.Stop[0].hooks[0].timeout, 30);
  assert.equal(syncWorkBuddyHooks(options).updated, false);
  const macRoot = path.join(home, "mac-config"); fs.mkdirSync(macRoot);
  syncWorkBuddyHooks({ home, platform: "darwin", env: { WORKBUDDY_CONFIG_DIR: macRoot } });
  const mac = JSON.parse(fs.readFileSync(path.join(macRoot, "settings.json")));
  assert.equal(mac.hooks.PreToolUse[0].hooks[0].timeout, 2);
});
