"use strict";
/*
 * [Input] Authenticated live DSH snapshots, never transcripts or approval SSE.
 * [Output] Bounded, sanitized per-session transitions and active-only heartbeats.
 * [Pos] DSH follow lifecycle. ACL/network polling lives off the hook event loop.
 */
const { Worker, isMainThread, parentPort, workerData } = require("node:worker_threads");
const SOURCE = "deepseek-harness";
const labels = { working: "正在思考", thinking: "正在思考", tool_running: "正在执行工具",
  waiting_user: "等待确认", done: "已完成", error: "执行失败", stopped: "已停止" };
const terminal = state => ["done", "error", "stopped"].includes(state);
const boundedDelay = (value, fallback, min, max) => Number.isFinite(value)
  ? Math.max(min, Math.min(max, value)) : fallback;

function displayText(value, limit = 320) {
  if (typeof value !== "string") return "";
  const text = value.slice(0, 24000)
    .replace(/<(think|thinking|analysis)>[\s\S]*?(?:<\/\1>|$)/gi, "")
    .replace(/```[\s\S]*?(?:```|$)/g, "[代码省略]")
    .replace(/-----BEGIN [^-]*PRIVATE KEY-----[\s\S]*?(?:-----END [^-]*PRIVATE KEY-----|$)/g, "[敏感信息已隐藏]")
    .replace(/\b(?:sk-|ark-)[a-zA-Z0-9_-]{12,}/g, "[密钥已隐藏]")
    .replace(/\bBearer\s+\S+/gi, "Bearer [已隐藏]")
    .replace(/\b(api[_-]?key|access[_-]?token|refresh[_-]?token|password|secret)\b["']?\s*[:=]\s*["']?[^\s,"'}]+/gi, "$1=[已隐藏]")
    .replace(/(?:\/Users\/|\/home\/|[A-Z]:[\\/]|\\\\)[^\s"'<>，。；]+/gi, "[本地路径]")
    .replace(/[\u0000-\u001f\u007f-\u009f\u202a-\u202e\u2066-\u2069]/g, " ")
    .replace(/\s+/g, " ").trim();
  const chars = Array.from(text);
  return chars.length > limit ? chars.slice(0, limit - 1).join("") + "…" : text;
}

function snapshotPayload(row, now = Date.now()) {
  if (!row || typeof row.id !== "string" || !/^[^\x00-\x20\x7f]{1,200}$/.test(row.id)
      || !/^[a-f0-9]{64}$/i.test(row.epoch || "")) return null;
  const known = row.stateDetailUnknown === false && Object.hasOwn(labels, row.state);
  const rawState = known ? row.state
    : row.active === true && row.activitySource === "live-agent-status" ? "working" : null;
  if (!rawState) return null;
  const state = ["thinking", "tool_running"].includes(rawState) ? "working"
    : rawState;
  const title = displayText(row.name, 120) || "DeepSeek Harness";
  const content = rawState === "done" ? displayText(row.displayContent) || labels.done : labels[rawState];
  return { source: SOURCE, channel: "dsh-session-snapshot", state, rawState,
    event: "session/snapshot", reason: `dsh.session.${rawState}`, sessionId: row.id,
    sessionTitle: title, sessionTitleExplicit: true, session: { id: row.id, title },
    display: { title, content, status: state, event: "session/snapshot", updatedAtMs: now },
    epoch: row.epoch, transitionRevision: Number.isSafeInteger(row.transitionRevision) ? row.transitionRevision : 0,
    sourceSeq: Number.isSafeInteger(row.sourceSeq) ? row.sourceSeq : -1,
    waitKind: ["question", "toolapproval"].includes(row.waitKind) ? row.waitKind : null,
    tsMs: now, ts: new Date(now).toISOString() };
}

function createSnapshotTracker({ heartbeatMs = 25000, maxSessions = 512 } = {}) {
  const heartbeat = boundedDelay(heartbeatMs, 25000, 5000, 60000);
  const limit = boundedDelay(maxSessions, 512, 1, 512);
  const seen = new Map(), retiredEpochs = new Set();
  let epoch = null, primaryId = null;
  return { consume(rows, now = Date.now()) {
    if (!Array.isArray(rows)) return [];
    const candidates = rows.slice(0, 128).flatMap(row => {
      const payload = snapshotPayload(row, now);
      return payload ? [{ payload, modified: Number.isFinite(row.lastModified) ? row.lastModified : 0 }] : [];
    });
    const epochs = new Set(candidates.map(c => c.payload.epoch));
    if (epochs.size > 1) return []; // Never combine different plugin incarnations.
    const nextEpoch = candidates[0]?.payload.epoch;
    if (nextEpoch && nextEpoch !== epoch) {
      if (retiredEpochs.has(nextEpoch)) return [];
      if (epoch) retiredEpochs.add(epoch);
      while (retiredEpochs.size > 8) retiredEpochs.delete(retiredEpochs.values().next().value);
      seen.clear(); primaryId = null; epoch = nextEpoch;
    }
    // Duplicate registry rows cannot make an older generation win by array order.
    candidates.sort((a, b) => b.payload.sourceSeq - a.payload.sourceSeq
      || b.payload.transitionRevision - a.payload.transitionRevision);
    const latest = new Map();
    for (const candidate of candidates) {
      if (!latest.has(candidate.payload.sessionId)) latest.set(candidate.payload.sessionId, candidate);
    }
    const output = [], active = [];
    // Older transitions first; the most recent active session owns the source file.
    const ordered = [...latest.values()].sort((a, b) => a.modified - b.modified || a.payload.sessionId.localeCompare(b.payload.sessionId));
    for (const { payload: p } of ordered) {
      const id = p.sessionId;
      const previous = seen.get(id);
      if (previous && (p.sourceSeq < previous.payload.sourceSeq
          || p.transitionRevision < previous.payload.transitionRevision)) continue;
      const ended = terminal(p.rawState);
      const signature = JSON.stringify([p.transitionRevision, p.rawState, p.waitKind, p.sessionTitle, p.display.content]);
      const changed = !previous || signature !== previous.signature;
      // Bootstrap/reconnect/eviction cannot resurrect historical completions.
      // A terminal is delivered only after this monitor observed activity.
      const publish = ended ? previous?.armed === true && now - previous.observedAt <= 60000
        : changed || now - previous.at >= heartbeat;
      const entry = { payload: p, signature, armed: !ended, observedAt: now,
        at: publish ? now : previous?.at ?? now };
      if (!changed && previous) p.display.updatedAtMs = previous.payload.display.updatedAtMs;
      seen.delete(id); seen.set(id, entry);
      if (!ended) active.push(p);
      if (publish) output.push(p);
    }
    // Retain tombstones across missing/empty polls; bound memory rather than
    // expiring after the device's 60s completion dwell time.
    while (seen.size > limit) seen.delete(seen.keys().next().value);
    const previousPrimary = seen.get(primaryId);
    const presentPrimary = active.at(-1);
    // A temporarily missing row must not let an older session's completion
    // replace a still-current source. Do not heartbeat an absent session.
    const primary = presentPrimary || (previousPrimary?.armed
      && now - previousPrimary.observedAt < 15000 ? previousPrimary.payload : null);
    if (presentPrimary && primaryId !== primary.sessionId && !output.includes(primary)) output.push(primary);
    primaryId = primary?.sessionId ?? null;
    return output.map(payload => ({ payload, sessionOnly: !!primary && payload.sessionId !== primary.sessionId }));
  } };
}

function isDshFollowEnabled(env = process.env) {
  // Desktop restarts the managed bridge when this profile changes, on both OSes.
  // An explicit empty/other enabled list means disabled even if selection is stale.
  if (Object.hasOwn(env, "CLAWD_ENABLED_AGENTS")) {
    return String(env.CLAWD_ENABLED_AGENTS).split(",").some(s => s.trim() === SOURCE);
  }
  return env.CLAWD_SELECTED_AGENT_ID?.trim() === SOURCE;
}

function startPolling({ adapter, onRows, onUnavailable, pollIntervalMs = 1500,
  schedule = setTimeout, cancel = clearTimeout }) {
  const delay = boundedDelay(pollIntervalMs, 1500, 1500, 60000);
  let stopped = false, timer, failures = 0;
  async function poll() {
    try {
      const rows = await adapter.listSessions({ limit: 128, strict: true });
      if (stopped) return;
      onRows(rows); failures = 0;
    } catch {
      if (stopped) return;
      if (!failures) onUnavailable();
      failures = Math.min(failures + 1, 5);
    } finally {
      if (!stopped) timer = schedule(poll, Math.min(30000, delay * 2 ** failures));
    }
  }
  void poll();
  return { stop() { stopped = true; cancel(timer); } };
}

function startDshMonitor({ enabled = true, onState, onUnavailable = () => {}, pollIntervalMs,
  heartbeatMs, WorkerClass = Worker, env = process.env, schedule = setTimeout, cancel = clearTimeout }) {
  if (!enabled) return { stop() {} };
  const tracker = createSnapshotTracker({ heartbeatMs });
  let stopped = false, worker, retry;
  function launch() {
    if (stopped) return;
    let current;
    try {
      current = new WorkerClass(__filename, { env, workerData: { kind: "dsh-follow", pollIntervalMs } });
    } catch { onUnavailable(); retry = schedule(launch, 5000); return; }
    worker = current;
    let failed = false;
    const fail = () => {
      if (stopped || failed) return;
      failed = true; onUnavailable();
      void current.terminate();
      retry = schedule(launch, 5000);
    };
    current.on("message", message => {
      if (stopped || failed) return;
      if (message?.kind === "rows") {
        for (const { payload, sessionOnly } of tracker.consume(message.rows)) onState(payload, { sessionOnly });
      } else if (message?.kind === "unavailable") onUnavailable();
    });
    current.on("error", fail);
    current.on("exit", fail);
  }
  launch();
  return { stop() { stopped = true; cancel(retry); return worker?.terminate(); } };
}

if (!isMainThread && workerData?.kind === "dsh-follow") {
  const { DshAdapter } = require("../packages/agent-session-bus/src/adapters/dsh");
  startPolling({ adapter: new DshAdapter({ log() {} }), pollIntervalMs: workerData.pollIntervalMs,
    onRows: rows => parentPort.postMessage({ kind: "rows", rows: rows.map(row => ({
      // No cwd, raw errors, transcript paths, credentials or extra server fields.
      id: row.id, epoch: row.epoch, sourceSeq: row.sourceSeq, transitionRevision: row.transitionRevision,
      state: row.state, stateDetailUnknown: row.stateDetailUnknown, active: row.active,
      activitySource: row.activitySource, waitKind: row.waitKind, lastModified: row.lastModified,
      name: displayText(row.name, 120), displayContent: row.state === "done" ? displayText(row.displayContent) : "",
    })) }), onUnavailable: () => parentPort.postMessage({ kind: "unavailable" }) });
}
module.exports = { displayText, snapshotPayload, createSnapshotTracker, isDshFollowEnabled, startPolling, startDshMonitor };
