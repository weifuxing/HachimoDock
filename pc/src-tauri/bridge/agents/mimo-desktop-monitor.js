"use strict";

const { statSync } = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const POLL_MS = 250;
// A short turn still needs to survive the device forwarder's 800 ms poll.
const PHASE_MS = 1000;
const HEARTBEAT_MS = 20000;
const RECENT_MS = 5 * 60 * 1000;

function databasePath(env = process.env) {
  const home = env.HOME || env.USERPROFILE || os.homedir();
  const data = env.MIMOCODE_HOME
    ? path.join(env.MIMOCODE_HOME, "data")
    : env.XDG_DATA_HOME || path.join(home, ".local", "share");
  return path.join(data, "mimocode", "mimocode.db");
}

// The Desktop inbox records the current answer and its outcome. Select only
// lifecycle metadata; assistant text is checked for presence inside SQLite.
const READ_TURNS = `
  SELECT i.id, i.receiver_session_id AS sessionId, i.state, i.outcome,
    EXISTS (
      SELECT 1 FROM message AS m
      JOIN part AS p ON p.message_id = m.id
      WHERE m.session_id = i.receiver_session_id AND m.time_created >= i.created_at - 1000
        AND json_extract(m.data, '$.role') = 'assistant'
        AND json_extract(m.data, '$.parentID') = i.input_message_id
        AND json_extract(p.data, '$.type') = 'text'
        AND NOT coalesce(json_extract(p.data, '$.ignored'), 0)
        AND NOT coalesce(json_extract(p.data, '$.synthetic'), 0)
        AND length(trim(json_extract(p.data, '$.text'))) > 0
    ) AS hasReply
  FROM inbox AS i JOIN session AS s ON s.id = i.receiver_session_id
  WHERE i.type = 'user' AND i.receiver_actor_id = 'main' AND s.version GLOB 'desktop-*'
    AND i.time_updated >= ? AND (i.state <> 'settled' OR i.time_updated >= ?)
  ORDER BY i.created_at DESC, i.id DESC LIMIT 32
`;

function startMimoDesktopMonitor({ onState, filename = databasePath(), onUnavailable = () => {} }) {
  const { DatabaseSync } = require("node:sqlite");
  let db;
  let identity;
  let readTurns;
  let warned = false;
  let startedAt = Date.now();
  const reported = new Map();

  function close() {
    db?.close();
    db = null;
    readTurns = null;
  }

  function poll() {
    try {
      const file = statSync(filename, { throwIfNoEntry: false });
      if (!file?.isFile()) {
        if (db) {
          close();
          reported.clear();
          startedAt = Date.now();
        }
        return;
      }
      const currentIdentity = `${file.dev}:${file.ino}`;
      if (identity !== undefined && identity !== currentIdentity) {
        close();
        reported.clear();
        startedAt = Date.now();
      }
      if (!db) {
        db = new DatabaseSync(filename, { readOnly: true });
        db.exec("PRAGMA busy_timeout=25");
        identity = currentIdentity;
      }
      readTurns ??= db.prepare(READ_TURNS);

      const now = Date.now();
      const seenSessions = new Set();
      for (const turn of readTurns.all(now - RECENT_MS, startedAt)) {
        if (seenSessions.has(turn.sessionId)) continue;
        seenSessions.add(turn.sessionId);
        const previous = reported.get(turn.sessionId);
        if (previous?.id === turn.id && previous.terminal) continue;

        const sameTurn = previous?.id === turn.id;
        let state = "working";
        if (sameTurn) {
          if (now - previous.since < PHASE_MS) continue;
          if (turn.hasReply) state = "speaking";
          if (turn.state === "settled" && (!turn.hasReply || previous.state === "speaking")) {
            state = turn.outcome === "success" ? "done"
              : turn.outcome === "assistant_error" ? "error" : "idle";
          }
          if (state === previous.state && now - previous.sentAt < HEARTBEAT_MS) continue;
        }
        onState({
          agent_id: "mimocode",
          state,
          event: `mimo-desktop.${state}`,
          session_id: turn.sessionId,
          display_title: "MiMo Desktop",
        });
        reported.set(turn.sessionId, {
          id: turn.id,
          state,
          since: sameTurn && state === previous.state ? previous.since : now,
          sentAt: now,
          terminal: state === "done" || state === "error" || state === "idle",
        });
      }
      for (const sessionId of reported.keys()) {
        if (!seenSessions.has(sessionId)) reported.delete(sessionId);
      }
      warned = false;
    } catch {
      if (!warned) onUnavailable();
      warned = true;
    }
  }

  poll();
  const timer = setInterval(poll, POLL_MS);
  timer.unref();
  return { stop() { clearInterval(timer); close(); } };
}

module.exports = { databasePath, startMimoDesktopMonitor };
