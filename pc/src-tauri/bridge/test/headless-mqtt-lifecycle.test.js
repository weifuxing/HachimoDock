"use strict";

/*
 * [Input] Managed bridge parent-process environment.
 * [Output] Regression coverage for orphan Bridge shutdown after desktop exit.
 * [Pos] Lifecycle tests for the headless MQTT sidecar.
 */

const assert = require("node:assert/strict");
const fs = require("node:fs");
const net = require("node:net");
const os = require("node:os");
const path = require("node:path");
const { spawn } = require("node:child_process");
const { once } = require("node:events");
const { Readable } = require("node:stream");
const { test } = require("node:test");

const { startParentProcessWatchdog } = require(
  "../packages/clawd-backend-service/src/headless-mqtt",
);
const { readManagedBootstrap } = require("../packages/agent-session-bus/src/local-capability");

test("managed bridge notices when its desktop parent no longer exists", async () => {
  const previous = process.env.PET_MANAGER_PARENT_PID;
  process.env.PET_MANAGER_PARENT_PID = "2147483646";
  try {
    const parentPid = await new Promise((resolve, reject) => {
      const timeout = setTimeout(
        () => reject(new Error("parent watchdog did not fire")),
        2500,
      );
      const watchdog = startParentProcessWatchdog((parentPid) => {
        clearTimeout(timeout);
        watchdog.stop();
        resolve(parentPid);
      });
    });

    assert.equal(parentPid, 2147483646);
  } finally {
    if (previous === undefined) {
      delete process.env.PET_MANAGER_PARENT_PID;
    } else {
      process.env.PET_MANAGER_PARENT_PID = previous;
    }
  }
});

test("managed bridge fails closed when its embedded Agent Bus cannot start", () => {
  const source = fs.readFileSync(
    path.join(
      __dirname,
      "..",
      "packages",
      "clawd-backend-service",
      "src",
      "headless-mqtt.js",
    ),
    "utf8",
  );

  assert.match(source, /if \(!busModule\) \{[\s\S]*throw new Error\("agent-session-bus is required/);
  assert.match(source, /agent-session-bus failed to start[\s\S]*agentBus = null;[\s\S]*throw error;/);
});

test("managed capability requires distinct, well-formed credentials", async () => {
  const tokens = { read: "a".repeat(64), control: "b".repeat(64), hook: "c".repeat(64) };
  const valid = { epoch: "d".repeat(64), tokens };
  const read = data => readManagedBootstrap(Readable.from([data]));
  assert.deepEqual(await read(JSON.stringify(valid)), valid);
  for (const invalid of ["{", "x".repeat(1025), JSON.stringify({ ...valid, epoch: tokens.read })]) {
    await assert.rejects(read(invalid), /invalid managed bridge bootstrap/);
  }
});

async function unusedLoopbackPort() {
  const listener = net.createServer().listen(0, "127.0.0.1");
  await once(listener, "listening");
  const { port } = listener.address();
  await new Promise(resolve => listener.close(resolve));
  return port;
}

test("desktop bridge remains authenticated when the external descriptor is unwritable", { timeout: 20000 }, async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "hachi-桥接-"));
  const blocked = path.join(dir, "blocked");
  fs.writeFileSync(blocked, "keep");
  const bridgePort = await unusedLoopbackPort();
  const busPort = await unusedLoopbackPort();
  const entry = require.resolve("../packages/clawd-backend-service/src/headless-mqtt");
  const tokens = { read: "a".repeat(64), control: "b".repeat(64), hook: "c".repeat(64) };
  const child = spawn(process.execPath, [entry, "--managed-stdio-capability"], {
    env: {
      ...process.env,
      PET_MANAGER_PARENT_PID: String(process.pid),
      STATUS_BRIDGE_LOCAL_STATE_DIR: path.join(dir, "state"),
      HACHIMODOCK_BRIDGE_CAPABILITY_DIR: blocked,
      CLAWD_BRIDGE_PORT: String(bridgePort),
      CLAWD_BRIDGE_STRICT_PORT: "1",
      AGENT_BUS_PORT: String(busPort),
      AGENT_BUS_DISABLED: "1",
      CLAWD_ENABLED_AGENTS: "codex",
      CLAWD_SELECTED_AGENT_ID: "codex",
      OPENCLAW_ENABLE: "false",
      CLAWD_SYNC_HOOKS: "false",
      CLAWD_ENABLE_CODEX_MONITOR: "false",
      CLAWD_ENABLE_CLAUDE_LOG_MONITOR: "false",
      CLAWD_ENABLE_MIMOCODE: "false",
    },
    stdio: ["pipe", "pipe", "pipe"],
  });
  const exited = once(child, "exit");
  let output = "";
  child.stdout.setEncoding("utf8").on("data", data => { output += data; });
  child.stderr.setEncoding("utf8").on("data", data => { output += data; });
  try {
    child.stdin.end(JSON.stringify({ epoch: "d".repeat(64), tokens }));
    const deadline = Date.now() + 15000;
    while (!output.includes("external bridge capability unavailable") && child.exitCode === null && Date.now() < deadline) {
      await new Promise(resolve => setTimeout(resolve, 25));
    }
    assert.equal(child.exitCode, null);
    assert.match(output, /external bridge capability unavailable/);
    assert.ok(Object.values(tokens).every(token => !output.includes(token)));
    assert.equal(fs.readFileSync(blocked, "utf8"), "keep");
    const status = `http://127.0.0.1:${busPort}/agent/status`;
    assert.equal((await fetch(status)).status, 401);
    assert.equal((await fetch(status, { headers: { Authorization: `Bearer ${tokens.hook}` } })).status, 401);
    assert.equal((await fetch(status, { headers: { Authorization: `Bearer ${tokens.read}` } })).status, 200);
    const hook = `http://127.0.0.1:${bridgePort}/state`;
    const request = { method: "POST", body: "{}", headers: { "Content-Type": "application/json" } };
    assert.equal((await fetch(hook, request)).status, 401);
    request.headers.Authorization = `Bearer ${tokens.hook}`;
    assert.equal((await fetch(hook, request)).status, 200);
  } finally {
    child.kill();
    await exited;
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
