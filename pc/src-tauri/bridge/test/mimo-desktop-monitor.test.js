"use strict";

const assert = require("node:assert/strict");
const { mkdtempSync, rmSync } = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { DatabaseSync } = require("node:sqlite");
const { setTimeout: delay } = require("node:timers/promises");
const { test } = require("node:test");
const { startMimoDesktopMonitor } = require("../agents/mimo-desktop-monitor");

test("Desktop follows turn metadata and leaves CLI and completed history to their existing paths", async (t) => {
  const directory = mkdtempSync(path.join(os.tmpdir(), "mimo-desktop-monitor-"));
  const filename = path.join(directory, "mimocode.db");
  const writer = new DatabaseSync(filename);
  t.after(() => {
    writer.close();
    rmSync(directory, { recursive: true, force: true });
  });
  writer.exec(`
    PRAGMA journal_mode=WAL;
    CREATE TABLE session (id TEXT PRIMARY KEY, version TEXT);
    CREATE TABLE inbox (id TEXT PRIMARY KEY, receiver_session_id TEXT, receiver_actor_id TEXT,
      type TEXT, state TEXT, outcome TEXT, input_message_id TEXT, created_at INTEGER, time_updated INTEGER);
    CREATE TABLE message (id TEXT PRIMARY KEY, session_id TEXT, time_created INTEGER, data TEXT);
    CREATE TABLE part (message_id TEXT, data TEXT);
    INSERT INTO session VALUES ('desktop', 'desktop-test'), ('cli', '2.1.309');
  `);
  const insert = writer.prepare("INSERT INTO inbox VALUES (?, ?, 'main', 'user', ?, ?, ?, ?, ?)");
  const started = Date.now();
  insert.run("past", "desktop", "settled", "success", "past-input", started - 10000, started - 9000);
  insert.run("cli", "cli", "active", null, "cli-input", started, started);

  const states = [];
  const monitor = startMimoDesktopMonitor({ filename, onState: value => states.push(value) });
  t.after(() => monitor.stop());
  await delay(300);
  assert.deepEqual(states, []);

  const now = Date.now();
  insert.run("current", "desktop", "active", null, "current-input", now, now);
  const message = writer.prepare("INSERT INTO message VALUES (?, 'desktop', ?, ?)");
  message.run("old-answer", now, JSON.stringify({ role: "assistant", parentID: "past-input" }));
  message.run("answer", now, JSON.stringify({ role: "assistant", parentID: "current-input" }));
  const part = writer.prepare("INSERT INTO part VALUES (?, ?)");
  part.run("old-answer", JSON.stringify({ type: "text", text: "previous private reply" }));
  part.run("answer", JSON.stringify({ type: "reasoning", text: "private reasoning" }));
  const waitForState = async expected => {
    for (let attempt = 0; attempt < 35 && states.at(-1)?.state !== expected; attempt++) await delay(100);
    assert.equal(states.at(-1)?.state, expected);
  };
  await waitForState("working");
  await delay(1100);
  assert.deepEqual(states.map(event => event.state), ["working"]);

  part.run("answer", JSON.stringify({ type: "text", text: "private reply" }));
  writer.prepare("UPDATE inbox SET state = 'settled', outcome = 'success', time_updated = ? WHERE id = 'current'").run(Date.now());
  await waitForState("speaking");
  await waitForState("done");
  assert.deepEqual(states.map(event => [event.agent_id, event.state, event.session_id]), [
    ["mimocode", "working", "desktop"], ["mimocode", "speaking", "desktop"], ["mimocode", "done", "desktop"],
  ]);
  assert.doesNotMatch(JSON.stringify(states), /private|past-input|current-input|cli-input/);
  monitor.stop();
});
