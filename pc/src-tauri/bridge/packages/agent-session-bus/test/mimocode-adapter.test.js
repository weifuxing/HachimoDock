"use strict";

/*
 * [Input] Fake MiMoCode CLI output.
 * [Output] Regression coverage for MiMoCode discovery, multi-session ordering, and phase-one input rejection.
 * [Pos] Node tests for the agent-session-bus MiMoCode adapter.
 */

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const { MiMoCodeAdapter } = require("../src/adapters/mimocode");

const FAKE_MIMO = path.join(__dirname, "fixtures", "fake-mimo.js");

function createFakeMiMoEntry(home) {
  if (process.platform === "win32") {
    const shim = path.join(home, "fake-mimo.cmd");
    fs.writeFileSync(
      shim,
      `@echo off\r\n"${process.execPath.replace(/"/g, '""')}" "${FAKE_MIMO.replace(/"/g, '""')}" %*\r\n`,
      "utf8",
    );
    return shim;
  }

  const shim = path.join(home, "fake-mimo");
  fs.writeFileSync(
    shim,
    `#!/bin/sh\nexec "${process.execPath.replace(/"/g, '\\"')}" "${FAKE_MIMO.replace(/"/g, '\\"')}" "$@"\n`,
    "utf8",
  );
  fs.chmodSync(shim, 0o755);
  return shim;
}

async function withFakeHome(fn) {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "bus-mimocode-test-"));
  try {
    return await fn(home);
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
}

function makeAdapter(home, extraEnv = {}) {
  return new MiMoCodeAdapter({
    env: {
      HOME: home,
      USERPROFILE: home,
      PATH: "",
      MIMOCODE_CLI_PATH: createFakeMiMoEntry(home),
      FAKE_MIMO_INVOCATIONS: path.join(home, "invocations.jsonl"),
      ...extraEnv,
    },
    cwd: home,
    fallbackPaths: [],
    extraPathDirs: [],
  });
}

function readInvocations(home) {
  const file = path.join(home, "invocations.jsonl");
  if (!fs.existsSync(file)) return [];
  return fs.readFileSync(file, "utf8").trim().split("\n").map(JSON.parse);
}

test("availability checks locate the executable without launching it", async () => {
  await withFakeHome(async (home) => {
    const adapter = makeAdapter(home);
    assert.equal((await adapter.isAvailable()).ready, true);
    assert.equal((await adapter.isAvailable()).ready, true);
    assert.deepEqual(readInvocations(home), []);
  });
});

test("listSessions shares one isolated query and preserves ordering and limits", async () => {
  await withFakeHome(async (home) => {
    const adapter = makeAdapter(home, {
      FAKE_MIMO_SESSIONS: JSON.stringify([
        {
          id: "older",
          title: "旧会话",
          updated: 100,
          created: 10,
          directory: "D:/old",
        },
        {
          id: "newer",
          title: "新会话",
          updated: 200,
          created: 20,
          directory: "D:/new",
        },
      ]),
    });
    const [sessions, limited] = await Promise.all([
      adapter.listSessions({ limit: 10 }),
      adapter.listSessions({ limit: 1 }),
    ]);
    assert.deepEqual(sessions.map((session) => session.id), ["newer", "older"]);
    assert.equal(sessions[0].name, "新会话");
    assert.equal(sessions[0].cwd, "D:/new");
    assert.deepEqual(limited, sessions.slice(0, 1));
    assert.deepEqual(await adapter.listSessions(), sessions);
    const invocations = readInvocations(home);
    assert.equal(invocations.length, 1);
    assert.deepEqual(invocations[0].args, ["session", "list", "--format", "json", "--max-count", "200"]);
    assert.match(path.basename(invocations[0].temp), /^hachimi-mimocode-/);
    assert.equal(fs.existsSync(invocations[0].temp), false);
  });
});

test("a failed query removes its temporary directory and backs off repeated calls", async () => {
  await withFakeHome(async (home) => {
    const adapter = makeAdapter(home, { FAKE_MIMO_FAIL: "1" });
    await assert.rejects(adapter.listSessions(), /exited code=2/);
    await assert.rejects(adapter.listSessions(), /exited code=2/);
    const invocations = readInvocations(home);
    assert.equal(invocations.length, 1);
    assert.equal(fs.existsSync(invocations[0].temp), false);
  });
});

test("phase one rejects MiMoCode input without silently routing elsewhere", async () => {
  await withFakeHome(async (home) => {
    const adapter = makeAdapter(home);
    const events = [];
    for await (const event of adapter.inject({
      sessionId: "mimo-session",
      text: "测试语音",
    })) {
      events.push(event);
    }
    assert.equal(events.length, 1);
    assert.equal(events[0].kind, "error");
    assert.equal(events[0].code, "AGENT_INPUT_UNSUPPORTED");
  });
});
