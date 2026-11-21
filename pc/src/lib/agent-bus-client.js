/**
 * [Input] Local Agent Bus HTTP endpoints plus an optional caller AbortSignal.
 * [Output] Bounded status, Session snapshot, and ordered Session-event requests.
 * [Pos] Transport-only Agent Bus client shared by dashboard orchestration hooks.
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import { invoke } from "@tauri-apps/api/core";

export const AGENT_BUS_URL = "http://127.0.0.1:8181";
export const AGENT_BUS_REQUEST_TIMEOUT_MS = 3000;

function normalizeText(value) {
  return typeof value === "string" ? value.trim() : "";
}

export async function fetchAgentBusJson(url, signal) {
  const parsed = new URL(url);
  if (parsed.origin !== AGENT_BUS_URL) throw new Error("invalid local bridge route");
  if (signal?.aborted) throw new DOMException("Aborted", "AbortError");
  let timeoutId, abortFromCaller;
  const cancelled = new Promise((_, reject) => {
    abortFromCaller = () => reject(new DOMException("Aborted", "AbortError"));
    signal?.addEventListener("abort", abortFromCaller, { once: true });
    timeoutId = setTimeout(abortFromCaller, AGENT_BUS_REQUEST_TIMEOUT_MS);
  });
  try {
    return await Promise.race([
      invoke("local_bridge_request", { route: parsed.pathname + parsed.search, body: null }),
      cancelled,
    ]);
  } finally {
    clearTimeout(timeoutId);
    signal?.removeEventListener("abort", abortFromCaller);
  }
}

export async function fetchAgentBusStatus(signal) {
  const body = await fetchAgentBusJson(`${AGENT_BUS_URL}/agent/status`, signal);
  const agents = Array.isArray(body?.adapters)
    ? body.adapters
    : Array.isArray(body?.agents)
      ? body.agents
      : [];
  return { ok: body?.ok !== false, agents };
}

export async function fetchAgentSessions(agentId, signal) {
  if (!agentId) return [];
  const body = await fetchAgentBusJson(
    `${AGENT_BUS_URL}/agent/sessions?agentId=${encodeURIComponent(agentId)}&limit=20`,
    signal,
  );
  return Array.isArray(body?.sessions) ? body.sessions : [];
}

export async function fetchAgentSessionEvents(agentId, cursor, streamId, signal) {
  if (!agentId) return { cursor: 0, streamId: "", reset: false, events: [] };
  const params = new URLSearchParams({ agentId, limit: "100" });
  if (Number.isFinite(cursor)) params.set("cursor", String(cursor));
  if (streamId) params.set("streamId", streamId);
  const body = await fetchAgentBusJson(
    `${AGENT_BUS_URL}/agent/session-events?${params.toString()}`,
    signal,
  );
  return {
    cursor: Number.isFinite(Number(body?.cursor)) ? Number(body.cursor) : 0,
    streamId: normalizeText(body?.streamId),
    reset: body?.reset === true,
    events: Array.isArray(body?.events) ? body.events : [],
  };
}
