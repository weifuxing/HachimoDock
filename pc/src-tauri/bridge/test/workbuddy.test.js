"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const { syncWorkBuddyHooks, EVENTS } = require("../hooks/workbuddy-install");
const { workbuddyPaths } = require("../hooks/workbuddy-paths");
const { stateForHook } = require("../hooks/workbuddy-pet-manager-hook");
const { displayText, readCurrentReply, readSessionTitle } = require("../hooks/workbuddy-display");
const { createAgentSessionBus, WorkBuddyAdapter } = require("../packages/agent-session-bus/src");
const { LocalStatePublisher, publishClawdState } = require("../packages/clawd-backend-service/src/headless-mqtt");
const { SessionMetricsTracker } = require("../packages/clawd-backend-service/src/status-metrics");

test("WorkBuddy lifecycle reaches the device with bounded current-turn prose only", () => {
  for (const event of EVENTS) {
    const payload = stateForHook({ hook_event_name: event, session_id: "s", prompt: "PRIVATE_SENTINEL" });
    let received;
    publishClawdState({ publisher: { publishSource(value) { received = value; } } }, payload);
    assert.equal(received.source, "workbuddy");
    assert.equal(received.sessionId, "s");
    assert.ok(received.display.title);
    assert.ok(received.display.content);
    assert.equal(received.display.status, received.state);
    if (event === "UserPromptSubmit") assert.equal(received.display.content, "PRIVATE_SENTINEL");
    else assert.doesNotMatch(JSON.stringify(received), /PRIVATE_SENTINEL/);
  }
});

function fixture(t) {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "pet-workbuddy-test-"));
  t.after(() => fs.rmSync(home, { recursive: true, force: true }));
  const options = { home, env: {}, platform: "linux" };
  return { home, options, ...workbuddyPaths(options) };
}
test("WorkBuddy config remains isolated from CodeBuddy and Windows paths are discovered", () => {
  assert.equal(workbuddyPaths({ home: "/fixture", env: { CODEBUDDY_CONFIG_DIR: "/other" } }).root, path.join("/fixture", ".workbuddy"));
  assert.equal(workbuddyPaths({ home: "/fixture", env: { WORKBUDDY_CONFIG_DIR: "/custom" } }).root, "/custom");
  const win = workbuddyPaths({ home: "/fixture", platform: "win32", env: { LOCALAPPDATA: "/local", ProgramFiles: "/programs" } });
  assert.deepEqual(win.apps, [
    path.join("/local", "Programs", "WorkBuddy", "WorkBuddy.exe"),
    path.join("/programs", "WorkBuddy", "WorkBuddy.exe"),
  ]);
});
test("WorkBuddy hooks preserve user config, back up, update paths and are idempotent", t => {
  const f = fixture(t); fs.mkdirSync(f.root);
  const original = JSON.stringify({ unrelated: { custom: true }, hooks: { Stop: [{ hooks: [{ type: "command", command: "user-hook" }] }] } });
  fs.writeFileSync(f.settings, original);
  assert.equal(syncWorkBuddyHooks(f.options).updated, true);
  assert.equal(fs.readFileSync(`${f.settings}.pet-manager-backup`, "utf8"), original);
  const installed = fs.readFileSync(f.settings, "utf8");
  const config = JSON.parse(installed);
  assert.equal(config.unrelated.custom, true);
  assert.equal(config.hooks.Stop[0].hooks[0].command, "user-hook");
  for (const event of EVENTS) assert.ok(config.hooks[event].some(e => e.hooks.some(h => h.command.includes("workbuddy-pet-manager-hook.js"))));
  assert.equal(syncWorkBuddyHooks(f.options).updated, false);
  assert.equal(fs.readFileSync(f.settings, "utf8"), installed);
  syncWorkBuddyHooks({ ...f.options, nodeBin: "/new path/node" });
  const updated = JSON.parse(fs.readFileSync(f.settings, "utf8"));
  assert.equal(updated.hooks.Stop.length, 2);
  assert.match(updated.hooks.Stop[1].hooks[0].command, /new path\/node/);
  assert.equal(fs.readFileSync(`${f.settings}.pet-manager-backup`, "utf8"), original);
});
test("missing, invalid and explicitly disabled WorkBuddy config is not overwritten", t => {
  const f = fixture(t);
  assert.equal(syncWorkBuddyHooks(f.options).skipped, true);
  assert.equal(fs.existsSync(f.root), false);
  fs.mkdirSync(f.root);
  for (const bad of ["{", "[]", '{"hooks":{"Stop":{}}}']) {
    fs.writeFileSync(f.settings, bad);
    assert.throws(() => syncWorkBuddyHooks(f.options));
    assert.equal(fs.readFileSync(f.settings, "utf8"), bad);
  }
  const disabled = '{"disableAllHooks":true}';
  fs.writeFileSync(f.settings, disabled);
  assert.equal(syncWorkBuddyHooks(f.options).skipped, true);
  assert.equal(fs.readFileSync(f.settings, "utf8"), disabled);
});
test("WorkBuddy UTF-8 BOM settings install safely and keep the exact first backup", t => {
  const f = fixture(t); fs.mkdirSync(f.root);
  const original = '\ufeff' + JSON.stringify({ custom: { keep: true }, hooks: {
    PreToolUse: [{ hooks: [{ type: "command", command: "user-hook", timeout: 7 }] }],
  } });
  fs.writeFileSync(f.settings, original);
  assert.equal(syncWorkBuddyHooks({ ...f.options, platform: "win32" }).updated, true);
  assert.equal(fs.readFileSync(`${f.settings}.pet-manager-backup`, "utf8"), original);
  const updated = fs.readFileSync(f.settings, "utf8");
  assert.notEqual(updated.charCodeAt(0), 0xfeff);
  const config = JSON.parse(updated);
  assert.equal(config.custom.keep, true);
  assert.equal(config.hooks.PreToolUse[0].hooks[0].command, "user-hook");
  assert.equal(config.hooks.PreToolUse[0].hooks[0].timeout, 7);
  assert.equal(syncWorkBuddyHooks({ ...f.options, platform: "win32" }).updated, false);
});
test("BOM does not override disabled hooks or make malformed settings writable", t => {
  const f = fixture(t); fs.mkdirSync(f.root);
  const disabled = '\ufeff{"disableAllHooks":true}';
  fs.writeFileSync(f.settings, disabled);
  assert.equal(syncWorkBuddyHooks(f.options).skipped, true);
  assert.equal(fs.readFileSync(f.settings, "utf8"), disabled);
  assert.equal(fs.existsSync(`${f.settings}.pet-manager-backup`), false);
  for (const bad of ['\ufeff{', '\ufeff[]', '\ufeff{"hooks":{"Stop":{}}}']) {
    fs.writeFileSync(f.settings, bad);
    assert.throws(() => syncWorkBuddyHooks(f.options));
    assert.equal(fs.readFileSync(f.settings, "utf8"), bad);
  }
});
test("Windows WorkBuddy discovery and installer follow USERPROFILE rather than a shell HOME", t => {
  const f = fixture(t);
  const profile = path.join(f.home, "windows-profile"), shellHome = path.join(f.home, "shell-home");
  for (const home of [profile, shellHome]) fs.mkdirSync(path.join(home, ".workbuddy"), { recursive: true });
  const options = { platform: "win32", env: { USERPROFILE: profile, HOME: shellHome } };
  assert.equal(workbuddyPaths(options).root, path.join(profile, ".workbuddy"));
  assert.equal(workbuddyPaths({ ...options, env: { ...options.env, WORKBUDDY_CONFIG_DIR: "/override" } }).root, "/override");
  assert.equal(workbuddyPaths({ ...options, platform: "darwin" }).root, path.join(shellHome, ".workbuddy"));
  assert.equal(syncWorkBuddyHooks(options).updated, true);
  assert.ok(fs.existsSync(path.join(profile, ".workbuddy", "settings.json")));
  assert.equal(fs.existsSync(path.join(shellHome, ".workbuddy", "settings.json")), false);
});
test("WorkBuddy lifecycle rejects raw secrets, paths and permission decisions", () => {
  for (const [event, state] of Object.entries({ UserPromptSubmit: "working", PreToolUse: "working", PermissionRequest: "waiting_user", Stop: "done", StopFailure: "error" })) {
    const result = stateForHook({ hook_event_name: event, session_id: "session-a", prompt: "用户的问题", api_key: "secret", transcript_path: "/private", tool_input: { command: "secret" } });
    assert.equal(result.state, state); assert.equal(result.agent_id, "workbuddy");
    assert.doesNotMatch(JSON.stringify(result), /secret|private|decision|permissionDecision/);
  }
  assert.equal(stateForHook({ hook_event_name: "SubagentStop", session_id: "a" }), null);
  assert.equal(stateForHook({ hook_event_name: "Stop" }), null);
  assert.equal(stateForHook({ hook_event_name: "__proto__", session_id: "a" }), null);
  const result = spawnSync(process.execPath, [path.join(__dirname, "../hooks/workbuddy-pet-manager-hook.js")], { input: "invalid json", encoding: "utf8", timeout: 2000 });
  assert.equal(result.status, 0); assert.equal(result.stdout.trim(), "{}");
});

test("WorkBuddy completed answer reaches the exact-session device display", () => {
  const payload = stateForHook({ hook_event_name: "Stop", session_id: "session-a",
    last_assistant_message: "北京今天晴，出门记得带水。", tool_input: { api_key: "DO_NOT_FORWARD" } });
  let received;
  publishClawdState({ publisher: { publishSource(value) { received = value; } } }, payload);
  assert.equal(received.sessionId, "session-a");
  assert.equal(received.display.content, "北京今天晴，出门记得带水。");
  assert.equal(received.state, "done");
  assert.doesNotMatch(JSON.stringify(received), /DO_NOT_FORWARD/);
});

test("WorkBuddy display strips reasoning, code, known credentials and local paths", () => {
  const text = displayText('<think>HIDDEN_THOUGHT</think>回复🙂 ```KEY_CODE``` sk-abcdefghijklmnop {"api_key":"HIDDEN_KEY"} Bearer HIDDEN_TOKEN /Users/example/private.txt');
  assert.match(text, /回复🙂/);
  assert.doesNotMatch(text, /HIDDEN_|KEY_CODE|abcdefghijklmnop|\/Users/);
  assert.equal(Array.from(displayText("🙂".repeat(1000))).length, 320);
});

test("WorkBuddy transcript fallback is session, turn, path and role scoped", t => {
  const f = fixture(t); const dir = path.join(f.root, "projects", "example");
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, "session-a.jsonl");
  const user = { type: "message", role: "user", sessionId: "session-a", content: [{ type: "input_text", text: "QUESTION" }] };
  const answer = { type: "message", role: "assistant", status: "completed", sessionId: "session-a",
    providerData: { conversationRequestId: "turn-a" }, content: [{ type: "output_text", text: "这就是本轮回答。" }, { type: "thinking", text: "HIDDEN" }] };
  const payload = { hook_event_name: "Stop", session_id: "session-a", generation_id: "turn-a", transcript_path: file };
  fs.writeFileSync(file, [user, answer].map(JSON.stringify).join("\n") + "\n");
  assert.equal(readCurrentReply(payload, f.options), "这就是本轮回答。");
  assert.equal(stateForHook(payload, f.options).display_content, "这就是本轮回答。");
  assert.equal(readCurrentReply({ ...payload, generation_id: "turn-b" }, f.options), "");
  assert.equal(readCurrentReply({ ...payload, session_id: "other" }, f.options), "");
  fs.appendFileSync(file, JSON.stringify(user) + "\n");
  assert.equal(readCurrentReply(payload, f.options), "");
  const outside = path.join(f.home, "session-a.jsonl");
  fs.writeFileSync(outside, JSON.stringify(answer));
  assert.equal(readCurrentReply({ ...payload, transcript_path: outside }, f.options), "");
  fs.unlinkSync(file); fs.symlinkSync(outside, file);
  assert.equal(readCurrentReply(payload, f.options), "");
});

test("WorkBuddy transcript tail is bounded and ignores malformed/tool records", t => {
  const f = fixture(t); const dir = path.join(f.root, "projects"); fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, "session-a.jsonl");
  fs.writeFileSync(file, "x".repeat(300000) + "\n" + JSON.stringify({ type: "function_call_result", sessionId: "session-a", output: "PRIVATE_TOOL_RESULT" }) + "\n");
  const payload = { session_id: "session-a", transcript_path: file };
  assert.equal(readCurrentReply(payload, f.options), "");
  fs.appendFileSync(file, JSON.stringify({ type: "message", role: "assistant", status: "completed", sessionId: "session-a", content: [{ type: "output_text", text: "新回复" }] }) + "\n");
  assert.equal(readCurrentReply(payload, f.options), "新回复");
});

test("WorkBuddy AI title uses the newest current-session record from bounded transcript windows", t => {
  const f = fixture(t);
  const dir = path.join(f.root, "projects", "example");
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, "session-a.jsonl");
  const payload = { hook_event_name: "PreToolUse", session_id: "session-a", transcript_path: file };
  const row = (aiTitle, sessionId = "session-a") => JSON.stringify({ type: "ai-title", aiTitle, sessionId }) + "\n";
  const padding = "x".repeat(70 * 1024) + "\n";
  fs.writeFileSync(file, row("合成会话标题") + padding + row("窗口之外") + padding + row("另一个会话", "session-b"));
  assert.equal(readSessionTitle(payload, f.options), "合成会话标题");
  const state = stateForHook(payload, f.options);
  assert.equal(state.session_title, "合成会话标题");
  assert.equal(state.session_title_explicit, true);
  assert.equal(Object.hasOwn(state, "transcript_path"), false);

  const renamed = row("合成重命名");
  fs.appendFileSync(file, renamed + "x".repeat(64 * 1024 - Buffer.byteLength(renamed) - 1) + "\n");
  assert.equal(readSessionTitle(payload, f.options), "合成重命名"); // Exactly at the tail boundary.
  assert.equal(readSessionTitle({ ...payload, session_title: "合成显式名" }, f.options), "合成显式名");
  assert.equal(readSessionTitle({ ...payload, session_title: "默认名", session_title_explicit: false }, f.options), "合成重命名");
});

test("WorkBuddy titles require the current session and redact private title fragments", t => {
  const f = fixture(t);
  const dir = path.join(f.root, "projects");
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, "session-a.jsonl");
  const payload = { hook_event_name: "PreToolUse", session_id: "session-a", transcript_path: file };
  const row = (type, aiTitle, sessionId = "session-a") => JSON.stringify({ type, aiTitle, sessionId });
  fs.writeFileSync(file, [row("ai-title", "外部标题", "session-b"), row("message", "正文不能用作标题")].join("\n"));
  assert.equal(stateForHook(payload, f.options).session_title, "WorkBuddy 会话");
  assert.equal(stateForHook(payload, f.options).session_title_explicit, false);
  fs.writeFileSync(file, row("ai-title", "绑定标题"));
  assert.equal(readSessionTitle({ ...payload, session_id: "session-b" }, f.options), "");
  assert.equal(readSessionTitle({ session_title: "未绑定" }, f.options), "");

  const otherDir = path.join(f.home, "other");
  fs.mkdirSync(otherDir);
  const outside = path.join(otherDir, "session-a.jsonl");
  fs.writeFileSync(outside, row("ai-title", "目录外标题"));
  assert.equal(readSessionTitle({ ...payload, transcript_path: outside }, f.options), "");
  fs.unlinkSync(file);
  fs.symlinkSync(outside, file);
  assert.equal(readSessionTitle(payload, f.options), "");
  fs.unlinkSync(file);
  fs.mkdirSync(file);
  assert.equal(readSessionTitle(payload, f.options), "");

  const session_title = '合成标题\u202e <think>HIDDEN_THOUGHT</think> password="HIDDEN FIRST SECOND" Bearer HIDDEN_TOKEN /Users/example/private.txt';
  const safe = stateForHook({ ...payload, session_title }, f.options);
  assert.equal(safe.session_title_explicit, true);
  assert.equal(safe.session_title, "合成标题 password=[已隐藏] Bearer [已隐藏] [本地路径]");
  assert.equal(Array.from(readSessionTitle({ ...payload, session_title: "测".repeat(128) }, f.options)).length, 120);
});

test("WorkBuddy explicit title reaches the shared USB and LAN session card payload for its own session", async t => {
  const f = fixture(t);
  const dir = path.join(f.root, "projects");
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, "session-a.jsonl");
  fs.writeFileSync(file, JSON.stringify({ type: "ai-title", sessionId: "session-a", aiTitle: "合成会话甲" }));
  const metricsTracker = new SessionMetricsTracker();
  const localStateDir = path.join(f.home, "bridge-state");
  const publisher = new LocalStatePublisher({ localStateDir });
  const publish = fields => publishClawdState({ publisher, metricsTracker },
    stateForHook({ hook_event_name: "PreToolUse", ...fields }, f.options));
  publish({ session_id: "session-b" });
  publish({ session_id: "session-a", transcript_path: file });
  const snapshot = JSON.parse(fs.readFileSync(path.join(localStateDir, "workbuddy.json"), "utf8"));
  assert.equal(snapshot.sessionId, "session-a");
  assert.equal(snapshot.sessionTitle, "合成会话甲");
  assert.equal(snapshot.sessionTitleExplicit, true);
  assert.equal(snapshot.display.title, "合成会话甲");
  assert.doesNotMatch(JSON.stringify(snapshot), /session-a\.jsonl|aiTitle/);

  // The next title-less hook must not replace the existing exact-session card title.
  publish({ session_id: "session-a" });
  const titleless = JSON.parse(fs.readFileSync(path.join(localStateDir, "workbuddy.json"), "utf8"));
  assert.equal(titleless.display.title, "WorkBuddy");
  const bus = createAgentSessionBus({
    port: 0, adapters: [new WorkBuddyAdapter(f.options)], log: () => {},
    sessionStatusProvider: agentId => metricsTracker.sessionStatuses(agentId),
  });
  const port = await bus.start();
  try {
    const { sessions } = await (await fetch(`http://127.0.0.1:${port}/agent/sessions?agentId=workbuddy`,
      { headers: bus.authorizationHeaders("read") })).json();
    const { p4SessionDisplayTitle, buildP4DeviceSessionTransportPayload } = await import("../../../src/lib/p4-session-service.js");
    const payload = buildP4DeviceSessionTransportPayload(sessions.map(session => ({
      id: session.id, title: p4SessionDisplayTitle(session), state: session.state,
    })));
    assert.equal(payload.find(session => session.id === "session-a").title, "合成会话甲");
    assert.equal(payload.find(session => session.id === "session-b").title, "WorkBuddy");
  } finally { await bus.stop(); }
});

test("WorkBuddy raw state display uses only a sanitized explicit current-session title", () => {
  const payload = { agent_id: "workbuddy", session_id: "session-a", event: "PreToolUse", state: "working" };
  const rawTitle = fields => {
    let state;
    publishClawdState({ publisher: { publishSource(value) { state = value; } } }, {
      ...payload, session_title: "合成标题 <think>HIDDEN_THOUGHT</think> password=\"HIDDEN FIRST SECOND\"", ...fields,
    });
    return state.display.title;
  };
  assert.equal(rawTitle({ session_title_explicit: true }), "合成标题 password=[已隐藏]");
  assert.equal(rawTitle({ session_title_explicit: false }), "WorkBuddy");
  assert.equal(rawTitle({}), "WorkBuddy");
  assert.equal(rawTitle({ session_title_explicit: true, session_id: "" }), "WorkBuddy");
  assert.equal(rawTitle({ session_title_explicit: true, session_title: "<think>HIDDEN</think>" }), "WorkBuddy");
});

test("WorkBuddy hook sessions enter the bus without reading private databases or injecting text", async t => {
  const f = fixture(t); fs.mkdirSync(f.root);
  const adapter = new WorkBuddyAdapter(f.options);
  assert.equal((await adapter.isAvailable()).ready, true);
  const bus = createAgentSessionBus({ port: 0, adapters: [adapter], log: () => {}, sessionStatusProvider: id => id === "workbuddy" ? [
    { id: "workbuddy:session-a", state: "working", updatedAt: 20 },
    { id: "workbuddy:session-b", state: "done", updatedAt: 10 },
  ] : [] });
  const port = await bus.start();
  try {
    const data = await (await fetch(`http://127.0.0.1:${port}/agent/sessions?agentId=workbuddy`, { headers: bus.authorizationHeaders("read") })).json();
    assert.deepEqual(data.sessions.map(s => s.id), ["session-a", "session-b"]);
    assert.equal(data.sessions[0].state, "working");
    assert.equal(data.sessions[0].name, "WorkBuddy 会话");
    const events = []; for await (const event of adapter.inject({ text: "hello" })) events.push(event);
    assert.equal(events[0].code, "AGENT_INPUT_UNSUPPORTED");
    await assert.rejects(() => adapter.openNew(), /WorkBuddy/);
  } finally { await bus.stop(); }
});
