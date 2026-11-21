"use strict";

const { existsSync, statSync } = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const pollIntervalMs = 900;
const recentSessionSeconds = 300;

function databasePath() {
  return path.join(process.env.HERMES_HOME || path.join(os.homedir(), ".hermes"), "state.db");
}

function readState(db, now) {
  // Read only the Hermes session and message metadata used to animate the pet.
  // Session titles and chat content may contain private material.
  const row = db.prepare(`
    SELECT recent.session_id, recent.role, recent.finish_reason, recent.timestamp, sessions.ended_at
    FROM messages AS recent
    JOIN sessions ON sessions.id = recent.session_id
    WHERE recent.role IN ('user', 'assistant', 'tool')
    ORDER BY recent.id DESC LIMIT 1
  `).get();
  if (!row?.session_id || !Number.isFinite(row.timestamp) || now - row.timestamp > recentSessionSeconds) return null;
  if (row.ended_at >= row.timestamp) return { state: "attention", event: "hermes_session_ended", sessionId: row.session_id };
  const state = row.role === "assistant"
    ? row.finish_reason === "tool_calls" || row.finish_reason === "function_call" ? "working" : "attention"
    : "working";
  return { state, event: `hermes_${row.role}`, sessionId: row.session_id };
}

function startHermesMonitor({ onState, filename = databasePath(), onUnavailable = () => {} }) {
  const { DatabaseSync } = require("node:sqlite");
  let db;
  let identity = "";
  let last = null;
  let lastReport = 0;
  let warned = false;
  function poll() {
    try {
      if (!existsSync(filename)) {
        db?.close();
        db = null;
        return;
      }
      const file = statSync(filename);
      if (!file.isFile()) return;
      const currentIdentity = `${file.dev}:${file.ino}`;
      if (db && identity !== currentIdentity) {
        db.close();
        db = null;
      }
      if (!db) {
        db = new DatabaseSync(filename, { readOnly: true, timeout: 500 });
        db.exec("PRAGMA busy_timeout=500");
        identity = currentIdentity;
      }
      const now = Date.now() / 1000;
      const state = readState(db, now);
      if (!state && !last) return;
      const next = state || { state: "idle", event: "hermes_idle", sessionId: last.sessionId };
      const changed = !last || next.state !== last.state || next.sessionId !== last.sessionId;
      if (changed || (state && now - lastReport >= 25)) {
        onState({
          agent_id: "hermes",
          state: next.state,
          event: next.event,
          session_id: next.sessionId,
          session_title: "Hermes Agent",
          session_title_explicit: false,
          headless: true,
        });
        last = state ? next : null;
        lastReport = now;
      }
      warned = false;
    } catch {
      if (!warned) onUnavailable();
      warned = true;
    }
  }
  poll();
  const timer = setInterval(poll, pollIntervalMs);
  timer.unref();
  return { stop() { clearInterval(timer); db?.close(); db = null; } };
}

module.exports = { databasePath, readState, startHermesMonitor };
