"use strict";

const assert = require("node:assert/strict");
const { mkdtempSync, rmSync } = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { DatabaseSync } = require("node:sqlite");
const { test } = require("node:test");
const { readState, startHermesMonitor } = require("../agents/hermes-monitor");

test("Hermes follows metadata from existing SQLite schemas without forwarding conversation text", async () => {
  const directory = mkdtempSync(path.join(os.tmpdir(), "hermes-monitor-"));
  const filename = path.join(directory, "state.db");
  const writer = new DatabaseSync(filename);
  let monitor;
  try {
    writer.exec(`
      PRAGMA journal_mode=WAL;
      CREATE TABLE sessions (id TEXT PRIMARY KEY, ended_at REAL);
      CREATE TABLE messages (id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, finish_reason TEXT, timestamp REAL, content TEXT);
      INSERT INTO sessions VALUES ('session', NULL);
    `);
    const now = Date.now() / 1000;
    const insert = writer.prepare("INSERT INTO messages VALUES (?, 'session', ?, ?, ?, 'private conversation')");
    insert.run(1, "user", null, now);
    assert.equal(readState(writer, now + 400), null);
    const states = [];
    monitor = startHermesMonitor({ filename, onState: value => states.push(value) });
    assert.equal(states[0].state, "working");
    assert.equal(states[0].agent_id, "hermes");
    assert.doesNotMatch(JSON.stringify(states[0]), /private conversation/);
    insert.run(2, "assistant", "stop", now);
    for (let i = 0; i < 25 && states.length < 2; i++) {
      await new Promise(resolve => setTimeout(resolve, 100));
    }
    assert.equal(states[1]?.state, "attention");
  } finally {
    monitor?.stop();
    writer.close();
    rmSync(directory, { recursive: true, force: true });
  }
});
