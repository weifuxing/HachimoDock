"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const os = require("node:os");
const http = require("node:http");
const { createCapability, descriptorPath, readDescriptor, headersFor, createCapabilityStore } = require("../src/local-capability");
const { createAgentSessionBus } = require("../src/index");
const { HookHttpServer } = require("../../clawd-backend-service/src/headless-mqtt");
const { syncPermissionHook } = require("../../../hooks/permission-install");

async function withDirectory(fn) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "bridge-cap-test-"));
  const dir = path.join(root, "capability");
  const previous = process.env.HACHIMODOCK_BRIDGE_CAPABILITY_DIR;
  process.env.HACHIMODOCK_BRIDGE_CAPABILITY_DIR = dir;
  try {
    // Elevated Windows runners may give mkdtemp an Administrators owner.
    // Let the production writer create its own protected child directory.
    createCapabilityStore().prepareDirectory(descriptorPath());
    await fn(dir);
  } finally {
    if (previous === undefined) delete process.env.HACHIMODOCK_BRIDGE_CAPABILITY_DIR;
    else process.env.HACHIMODOCK_BRIDGE_CAPABILITY_DIR = previous;
    fs.rmSync(root, { recursive: true, force: true });
  }
}
const req = (cap, role) => ({ headers: role ? { authorization: cap.headers(role).Authorization } : {} });

test("fresh epochs and distinct roles; restart invalidates old capability", async () => withDirectory(async dir => {
  const first = createCapability(); first.publish({ busPort: 8181, hookPort: 23333 });
  const client = readDescriptor(descriptorPath());
  const hook = readDescriptor(descriptorPath("hook"));
  assert.deepEqual(Object.keys(client.tokens).sort(), ["control", "read"]);
  assert.deepEqual(Object.keys(hook.tokens), ["hook"]);
  assert.equal(new Set([...Object.values(client.tokens), ...Object.values(hook.tokens)]).size, 3);
  if (process.platform === "win32") {
    const { windowsAcl } = require("../src/adapters/windows-acl");
    const receipt = windowsAcl("verify-file", descriptorPath());
    assert.equal(receipt.item.owner, receipt.currentSid);
    assert.equal(receipt.item.protected, true);
    assert.equal(receipt.item.rawAceCount, 2);
  } else {
    assert.equal(fs.statSync(dir).mode & 0o077, 0);
    assert.equal(fs.statSync(descriptorPath()).mode & 0o077, 0);
  }
  assert.deepEqual(headersFor("hook", 23333), first.headers("hook"));
  assert.throws(() => headersFor("hook", 23334));
  assert.throws(() => headersFor("hook", 8181));
  const second = createCapability(); second.publish({ busPort: 8181, hookPort: 23333 });
  assert.equal(second.authorize(req(first, "read"), ["read"]), false);
  first.clear();
  assert.deepEqual(headersFor("read", 8181), second.headers("read"));
  second.clear(); assert.equal(fs.existsSync(descriptorPath()), false);
}));

test("descriptor rejects oversized and malformed files", async () => withDirectory(async dir => {
  const cap = createCapability(); cap.publish({ busPort: 8181 });
  const file = descriptorPath();
  fs.writeFileSync(file, "x".repeat(4097)); assert.throws(() => readDescriptor());
  fs.writeFileSync(file, "{}"); assert.throws(() => readDescriptor());

}));

test("Unix descriptor rejects symlinks and shared modes", { skip: process.platform === "win32" }, async () => withDirectory(async dir => {
  const cap = createCapability(); cap.publish({ busPort: 8181 });
  const file = descriptorPath(), backup = path.join(dir, "saved.json");
  fs.renameSync(file, backup); fs.symlinkSync(backup, file);
  assert.throws(() => readDescriptor()); fs.unlinkSync(file); fs.renameSync(backup, file);
  fs.chmodSync(file, 0o644); assert.throws(() => readDescriptor()); fs.chmodSync(file, 0o600);
  fs.chmodSync(dir, 0o755); assert.throws(() => cap.publish({ busPort: 8181 })); fs.chmodSync(dir, 0o700);
}));

test("Windows descriptor rejects a junction capability directory", { skip: process.platform !== "win32" }, async () => withDirectory(async dir => {
  const cap = createCapability(); cap.publish({ busPort: 8181 });
  const saved = `${dir}-saved`;
  fs.renameSync(dir, saved);
  try {
    fs.symlinkSync(saved, dir, "junction");
    assert.throws(() => readDescriptor());
  } finally {
    if (fs.existsSync(dir)) fs.unlinkSync(dir);
    fs.renameSync(saved, dir);
  }
}));

test("Windows descriptor rejects an extra public read ACE", { skip: process.platform !== "win32" }, async () => withDirectory(async () => {
  const cap = createCapability(); cap.publish({ busPort: 8181 });
  const { spawnSync } = require("node:child_process");
  const { windowsAcl } = require("../src/adapters/windows-acl");
  const file = descriptorPath();
  const icacls = path.join(process.env.SystemRoot, "System32", "icacls.exe");
  try {
    // Only the disposable fixture is changed; SID syntax avoids localized names.
    const changed = spawnSync(icacls, [file, "/grant", "*S-1-1-0:(R)"], { windowsHide: true, encoding: "utf8", timeout: 5000 });
    assert.equal(changed.status, 0, "fixture ACL edit must succeed");
    assert.throws(() => readDescriptor());
  } finally { windowsAcl("protect-file", file); }
  assert.deepEqual(headersFor("read", 8181), cap.headers("read"));
}));

function request(port, pathname, method, headers = {}, payload = {}) {
  return new Promise((resolve, reject) => {
    const r = http.request({ hostname: "127.0.0.1", port, path: pathname, method,
      headers: { "Content-Type": "application/json", ...headers } }, response => {
      let body = ""; response.on("data", c => body += c);
      response.on("end", () => resolve({ status: response.statusCode, body: JSON.parse(body) }));
    });
    r.on("error", reject); r.end(method === "POST" ? JSON.stringify(payload) : undefined);
  });
}

test("HTTP gates run before providers/control; public health discloses no sessions", async () => {
  let reads = 0, writes = 0;
  const cap = createCapability();
  const bus = createAgentSessionBus({ port: 0, capability: cap, adapters: [], log() {} });
  bus.registry.statusAll = async () => { reads++; return []; };
  const port = await bus.start();
  const hook = new HookHttpServer({ capability: cap, onState() { writes++; }, onPermission() { writes++; return null; }, onMockButtonInject() { writes++; return {}; } });
  // Exercise the production handler without runtime.json/user-directory writes.
  const socket = http.createServer((q, s) => hook.handleRequest(q, s));
  await new Promise(resolve => socket.listen(0, "127.0.0.1", resolve));
  const hp = socket.address().port;
  try {
    assert.deepEqual(await request(port, "/agent/health", "GET"), { status: 200, body: { ok: true, service: "agent-session-bus" } });
    for (const route of ["/agent/status", "/agent/sessions", "/agent/session-events"]) {
      for (const headers of [{}, cap.headers("hook"), { Authorization: "Bearer wrong" }]) {
        assert.equal((await request(port, route, "GET", headers)).status, 401);
      }
    }
    for (const route of ["/agent/inject", "/agent/cancel"]) {
      assert.equal((await request(port, route, "POST", cap.headers("read"))).status, 401);
      assert.equal((await request(port, route, "POST", cap.headers("hook"))).status, 401);
    }
    assert.equal(reads, 0);
    assert.equal((await request(port, "/agent/status", "GET", cap.headers("read"))).status, 200);
    assert.equal(reads, 1);
    for (const route of ["/state", "/permission", "/mock-button-inject"]) {
      assert.equal((await request(hp, route, "POST")).status, 401);
      assert.equal((await request(hp, route, "POST", cap.headers("read"))).status, 401);
    }
    assert.equal((await request(hp, "/mock-button-inject", "POST", cap.headers("hook"))).status, 401);
    assert.equal(writes, 0);
    assert.equal((await request(hp, "/state", "POST", cap.headers("hook"))).status, 200);
    assert.equal((await request(hp, "/permission", "POST", cap.headers("hook"))).status, 200);
    assert.equal((await request(hp, "/mock-button-inject", "POST", cap.headers("control"))).status, 200);
    assert.equal(writes, 3);
    assert.equal((await request(port, "/agent/status?token=ignored", "GET")).status, 401);
  } finally { await bus.stop(); await new Promise(resolve => socket.close(resolve)); }
});

test("permission migration preserves user same-name hooks and unrelated HTTP hooks", () => {
  const user = { type: "command", command: '"node" "/user/tools/permission-hook.js"' };
  const remote = { type: "http", url: "https://example.invalid/permission" };
  const localOther = { type: "http", url: "http://127.0.0.1:4444/permission" };
  const legacy = { type: "http", url: "http://127.0.0.1:23333/permission", timeout: 600 };
  const script = path.resolve(__dirname, "../../../hooks/clawd-hook.js");
  const oldCommand = { type: "command", command: `"node" "${script}" PermissionRequest` };
  const settings = { hooks: { PermissionRequest: [{ matcher: "", hooks: [user, remote, localOther, legacy, oldCommand] }] } };
  assert.equal(syncPermissionHook(settings, "node", "clawd-hook.js"), true);
  assert.deepEqual(settings.hooks.PermissionRequest[0].hooks, [user, remote, localOther]);
  const managed = settings.hooks.PermissionRequest[1].hooks[0];
  assert.equal(managed.type, "command");
  assert.match(managed.command, /permission-hook\.js/);
  assert.equal(syncPermissionHook(settings, "node", "clawd-hook.js"), false);
  assert.doesNotMatch(JSON.stringify(settings), /Authorization|Bearer/);
});

test("permission wrapper and state hooks read new epoch each time, without tokens in settings", async () => withDirectory(async () => {
  const { spawn } = require("node:child_process");
  const { postStateToPort } = require("../../../hooks/server-config");
  let cap = createCapability(), writes = 0;
  const hook = new HookHttpServer({ capability: cap, onState() { writes++; }, onPermission() { writes++; return null; } });
  const socket = http.createServer((q, s) => hook.handleRequest(q, s));
  await new Promise(resolve => socket.listen(0, "127.0.0.1", resolve));
  const port = socket.address().port;
  const wrapper = path.resolve(__dirname, "../../../hooks/permission-hook.js");
  const invoke = () => new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [wrapper], { env: process.env, stdio: ["pipe", "pipe", "pipe"] });
    let out = "", err = "";
    child.stdout.on("data", c => out += c); child.stderr.on("data", c => err += c);
    child.on("error", reject); child.on("close", code => resolve({ code, out, err }));
    child.stdin.end(JSON.stringify({ hook_event_name: "PermissionRequest", tool_name: "synthetic-test" }));
  });
  try {
    cap.publish({ hookPort: port });
    for (let i = 0; i < 2; i++) {
      const result = await invoke();
      assert.equal(result.code, 0); assert.equal(result.err, ""); assert.deepEqual(JSON.parse(result.out), {});
      assert.equal(await new Promise(resolve => postStateToPort(port, "{}", 500, resolve)), true);
      cap = createCapability(); hook.capability = cap; cap.publish({ hookPort: port });
    }
    assert.equal(writes, 4);
    // Descriptor absence must not issue an unauthenticated write or auto-approve.
    cap.clear();
    assert.deepEqual(JSON.parse((await invoke()).out), {});
    assert.equal(await new Promise(resolve => postStateToPort(port, "{}", 500, resolve)), false);
    assert.equal(writes, 4);
  } finally { cap.clear(); await new Promise(resolve => socket.close(resolve)); }
}));

test("both managed installers migrate only their own permission script and managed endpoint", async () => withDirectory(async dir => {
  const { registerHooks } = require("../../../hooks/install");
  const { registerCodeBuddyHooks } = require("../../../hooks/codebuddy-install");
  for (const [name, install] of [["claude", registerHooks], ["codebuddy", registerCodeBuddyHooks]]) {
    const settingsPath = path.join(dir, `${name}-settings.json`);
    const user = { type: "command", command: '"node" "/user/permission-hook.js"' };
    fs.writeFileSync(settingsPath, JSON.stringify({ hooks: { PermissionRequest: [{ hooks: [user, { type: "http", url: "http://127.0.0.1:23333/permission" }] }] } }));
    const options = { settingsPath, nodeBin: process.execPath, silent: true, claudeVersionInfo: { version: "2.1.0", source: "fixture", status: "known" } };
    install(options);
    const result = JSON.parse(fs.readFileSync(settingsPath, "utf8"));
    assert.deepEqual(result.hooks.PermissionRequest[0].hooks, [user]);
    assert.equal(result.hooks.PermissionRequest.at(-1).hooks[0].type, "command");
    const previous = fs.readFileSync(settingsPath, "utf8");
    install(options); assert.equal(fs.readFileSync(settingsPath, "utf8"), previous);
    assert.doesNotMatch(previous, /Bearer|Authorization/);
  }
}));
