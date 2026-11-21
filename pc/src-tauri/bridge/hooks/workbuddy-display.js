"use strict";
const fs = require("node:fs");
const path = require("node:path");
const { createHash } = require("node:crypto");
const { workbuddyPaths } = require("./workbuddy-paths");
const MAX_TAIL_BYTES = 256 * 1024;
const TITLE_WINDOW_BYTES = 64 * 1024;

function displayText(value, limit = 320) {
  if (typeof value !== "string") return "";
  // Display only bounded prose, not secrets, file paths, hidden reasoning or
  // executable/tool blocks. This is also applied at the Bridge ingress.
  const text = value.slice(0, 24000)
    .replace(/<(think|thinking|analysis)>[\s\S]*?(?:<\/\1>|$)/gi, "")
    .replace(/```[\s\S]*?(?:```|$)/g, "[代码省略]")
    .replace(/-----BEGIN [^-]*PRIVATE KEY-----[\s\S]*?(?:-----END [^-]*PRIVATE KEY-----|$)/g, "[敏感信息已隐藏]")
    .replace(/\b(?:sk-|ark-)[a-zA-Z0-9_-]{12,}/g, "[密钥已隐藏]")
    .replace(/\b(Bearer)\s+\S+/gi, "$1 [已隐藏]")
    .replace(/\b(api[_-]?key|access[_-]?token|refresh[_-]?token|password|secret)\b["']?\s*[:=]\s*(?:"(?:\\[\s\S]|[^"\\])*(?:"|$)|'(?:\\[\s\S]|[^'\\])*(?:'|$)|[^\s,"'}]+)/gi, "$1=[已隐藏]")
    .replace(/(?:\/Users\/|\/home\/|[A-Z]:\\Users\\)[^\s"'<>，。；]+/gi, "[本地路径]")
    .replace(/[\u0000-\u001f\u007f]/g, " ")
    .replace(/\s+/g, " ").trim();
  const chars = Array.from(text);
  return chars.length > limit ? chars.slice(0, limit - 1).join("") + "…" : text;
}

function withTranscript(payload, options, read) {
  // Only the transcript explicitly named by this hook, under WorkBuddy's own
  // projects directory, matching this session. No database/history scan.
  const id = payload.session_id;
  if (typeof id !== "string" || !/^[a-zA-Z0-9_-]{1,128}$/.test(id)
      || typeof payload.transcript_path !== "string") return undefined;
  let fd;
  try {
    const root = fs.realpathSync(path.join(workbuddyPaths(options).root, "projects"));
    const file = fs.realpathSync(payload.transcript_path);
    const rel = path.relative(root, file);
    if (!rel || rel.startsWith(".." + path.sep) || path.isAbsolute(rel)
        || path.basename(file) !== `${id}.jsonl`) return undefined;
    const expected = fs.lstatSync(file);
    if (!expected.isFile()) return undefined;
    fd = fs.openSync(file, fs.constants.O_RDONLY | (fs.constants.O_NOFOLLOW || 0) | (fs.constants.O_NONBLOCK || 0));
    const stat = fs.fstatSync(fd);
    if (!stat.isFile() || expected.dev !== stat.dev || expected.ino !== stat.ino
        || fs.realpathSync(file) !== file) return undefined;
    return read(fd, stat.size);
  } catch { /* Missing, rotating or unsupported transcript: retain status. */ }
  finally { if (fd !== undefined) fs.closeSync(fd); }
  return undefined;
}

function readRows(fd, start, length, clippedEnd = false) {
  const bytes = Buffer.alloc(length);
  const read = fs.readSync(fd, bytes, 0, bytes.length, start);
  const lines = bytes.subarray(0, read).toString("utf8").split("\n");
  if (start > 0) {
    const boundary = Buffer.alloc(1);
    if (fs.readSync(fd, boundary, 0, 1, start - 1) !== 1 || boundary[0] !== 0x0a) lines.shift();
  }
  if (clippedEnd || read < length) lines.pop();
  return lines.flatMap(line => { try { const row = JSON.parse(line); return row && typeof row === "object" ? [row] : []; } catch { return []; } });
}

function transcriptRows(payload, options) {
  return withTranscript(payload, options, (fd, size) => {
    const length = Math.min(size, MAX_TAIL_BYTES);
    return readRows(fd, size - length, length);
  }) || [];
}

function readCurrentReply(payload, options = {}) {
    const rows = transcriptRows(payload, options);
    for (let i = rows.length - 1; i >= 0; i--) {
      const row = rows[i];
      if (row.sessionId !== payload.session_id || row.type !== "message") continue;
      // Do not recycle an older turn's answer when this turn has none.
      if (row.role === "user") return "";
      if (row.role !== "assistant" || row.status !== "completed") continue;
      if (payload.generation_id && row.providerData?.conversationRequestId !== payload.generation_id) continue;
      if (!Array.isArray(row.content)) continue;
      const text = row.content.filter(c => c?.type === "output_text" && typeof c.text === "string")
        .map(c => c.text).join("\n");
      return displayText(text);
    }
    return "";
}

function readCompletionReceipt(payload, options = {}) {
  if (payload.hook_event_name !== "Stop") return null;
  const rows = transcriptRows(payload, options);
  const hash = text => createHash("sha256").update(text).digest("hex");
  const textOf = (row, type) => Array.isArray(row.content)
    ? row.content.filter(c => c?.type === type && typeof c.text === "string").map(c => c.text).join("\n") : "";
  // Match the actual Stop, not simply the latest reply. A missing/changed hook
  // generation can use its exact full reply; a cropped preview cannot prove it.
  const completed = rows.filter(r => r.sessionId === payload.session_id && r.type === "message"
    && r.role === "assistant" && r.status === "completed" && textOf(r, "output_text"));
  const generationMatches = payload.generation_id
    ? completed.filter(r => r.providerData?.conversationRequestId === payload.generation_id) : [];
  const reply = typeof payload.last_assistant_message === "string" ? payload.last_assistant_message.trim() : "";
  const answers = reply ? (generationMatches.length ? generationMatches : completed)
    .filter(r => textOf(r, "output_text").trim() === reply) : generationMatches;
  if (answers.length !== 1) return null;
  const answer = answers[0];
  const byId = new Map();
  for (const row of rows) {
    if (typeof row.id !== "string" || !row.id || row.id.length > 512) continue;
    if (byId.has(row.id)) return null; // Do not guess across duplicated snapshots.
    byId.set(row.id, row);
  }
  let row = answer;
  const seen = new Set();
  for (let depth = 0; depth < 128; depth++) {
    if (!row || row.sessionId !== payload.session_id || !byId.has(row.id) || seen.has(row.id)) return null;
    seen.add(row.id);
    if (row.type === "message" && row.role === "user") {
      if (!Number.isSafeInteger(row.timestamp) || row.timestamp <= 0 || row.timestamp > Date.now()
        || !Array.isArray(row.content) || !row.content.length
        || row.content.some(c => c?.type !== "input_text" || typeof c.text !== "string")) return null;
      const prompt = textOf(row, "input_text").trim();
      if (!prompt) return null;
      return { userHash: hash(prompt), turnId: hash(`workbuddy-message:${answer.id}`),
        userTimestamp: row.timestamp, receiptSource: "transcript_chain", reply: displayText(textOf(answer, "output_text")) };
    }
    if (!["message", "reasoning", "function_call", "function_call_result"].includes(row.type)
      || (row.type === "message" && row.role !== "assistant")) return null;
    const parent = byId.get(row.parentId);
    if (!parent || !Number.isSafeInteger(row.timestamp) || !Number.isSafeInteger(parent.timestamp)
      || parent.timestamp > row.timestamp) return null;
    row = parent;
  }
  return null;
}

function readSessionTitle(payload, options = {}) {
  const id = payload?.session_id;
  if (typeof id !== "string" || !id.trim() || id.length > 256) return "";
  const sanitize = value => displayText(typeof value === "string"
    ? value.replace(/[\u061c\u200e\u200f\u202a-\u202e\u2066-\u2069]/g, "") : value, 120);
  const explicit = payload.session_title_explicit === false ? "" : sanitize(payload.session_title);
  if (explicit) return explicit;

  const latestTitle = rows => {
    for (let i = rows.length - 1; i >= 0; i--) {
      const row = rows[i];
      if (row.type !== "ai-title" || row.sessionId !== id) continue;
      const title = sanitize(row.aiTitle);
      if (title) return title;
    }
    return "";
  };
  // Check the newest records first. Opening once binds both bounded windows
  // to the same file while WorkBuddy appends or rotates the transcript.
  return withTranscript(payload, options, (fd, size) => {
    const length = Math.min(size, TITLE_WINDOW_BYTES);
    const start = size - length;
    const title = latestTitle(readRows(fd, start, length));
    return title || (start > 0 ? latestTitle(readRows(fd, 0, length, true)) : "");
  }) || "";
}

function hookDisplay(payload, options) {
  const event = payload.hook_event_name;
  if (event === "UserPromptSubmit") return displayText(payload.prompt);
  if (event === "Stop") return displayText(payload.last_assistant_message) || readCurrentReply(payload, options);
  if (event === "Notification" || event === "PermissionRequest") return displayText(payload.message);
  return "";
}
module.exports = { displayText, hookDisplay, readCurrentReply, readCompletionReceipt, readSessionTitle };
