/**
 * [Input] Mock local Agent Bus HTTP responses.
 * [Output] Contract coverage for status normalization and cursor request encoding.
 * [Pos] Test node for the Agent Bus transport client.
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  fetchAgentBusStatus,
  fetchAgentSessionEvents,
} from "./agent-bus-client.js";

test("Agent Bus client normalizes adapters and encodes ordered Session cursors", async () => {
  const previousWindow = globalThis.window;
  const requests = [];
  globalThis.window = { __TAURI_INTERNALS__: { invoke: async (command, options) => {
    assert.equal(command, "local_bridge_request");
    const url = options.route;
    requests.push({ url: String(url) });
    const body = String(url).includes("session-events")
      ? { cursor: 9, streamId: "stream-a", events: [{ sequence: 9 }] }
      : { ok: true, adapters: [{ agentId: "codex" }] };
    return body;
  } } };

  try {
    assert.deepEqual(await fetchAgentBusStatus(), {
      ok: true,
      agents: [{ agentId: "codex" }],
    });
    assert.deepEqual(
      await fetchAgentSessionEvents("claude code", 8, "stream/old"),
      {
        cursor: 9,
        streamId: "stream-a",
        reset: false,
        events: [{ sequence: 9 }],
      },
    );
    assert.match(requests[1].url, /agentId=claude\+code/);
    assert.match(requests[1].url, /cursor=8/);
    assert.match(requests[1].url, /streamId=stream%2Fold/);
    const ctl = new AbortController();
    ctl.abort();
    await assert.rejects(fetchAgentBusStatus(ctl.signal), { name: "AbortError" });
    assert.equal(requests.length, 2);
  } finally {
    globalThis.window = previousWindow;
  }
});


test("cancelled Agent read never resolves with stale IPC data", async () => {
  const previous = globalThis.window;
  let resolve;
  globalThis.window = { __TAURI_INTERNALS__: { invoke: () => new Promise(r => { resolve = r; }) } };
  try {
    const ctl = new AbortController();
    const pending = fetchAgentBusStatus(ctl.signal);
    ctl.abort();
    resolve({ ok: true, adapters: [{ agentId: "stale" }] });
    await assert.rejects(pending, { name: "AbortError" });
  } finally { globalThis.window = previous; }
});
