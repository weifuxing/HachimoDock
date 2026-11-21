# agent-session-bus

A local Node.js HTTP service providing a common session interface for coding
agents. It runs inside Pet Manager's managed bridge. Desktop voice dictation
and confirmation are controlled by the client; this service provides the
underlying session APIs rather than defining when a voice draft is submitted.

## Why this exists

Without this bus, voice-service-node would have to know how to talk to Claude
Code, to Codex, and to OpenClaw — each of which has a totally different write
path:

| agent          | write path                                                    |
|----------------|---------------------------------------------------------------|
| `claude-code`  | spawn `claude -p ... --resume <sid> --output-format stream-json` |
| `codex`        | resume Desktop threads via `codex app-server --listen stdio://`; new sessions use `codex exec --json` |
| `openclaw`     | spawn helper → `import('openclaw/dist/plugin-sdk/agent-runtime').agentCommand({...})` |
| `mimocode`     | cached official `mimo session list --format json` in an isolated temporary directory; installation detection does not launch the CLI and live status comes from the managed MiMoCode plugin |
| `workbuddy`    | status-only WorkBuddy lifecycle hooks; sessions received during the current Bridge run; text injection and session creation explicitly unsupported |

Centralising the "find the user's latest session, pipe text in, stream tokens
out" logic here keeps voice-service-node a pure audio frontend, and keeps each
agent's transport peculiarities behind a single `AgentAdapter` interface.

## Wire shape

```
voice-service-node                 agent-session-bus              <agent>
     │                                   │                          │
     │ POST /agent/inject                │                          │
     │ { agentId, sessionId, text }      │                          │
     │──────────────────────────────────>│                          │
     │                                   │ resolveSession(...)      │
     │                                   │ inject(sid, text)        │
     │                                   │─────────────────────────>│
     │                                   │                          │
     │ <─── SSE: token, tool, done ──────│ <─── streamed events ────│
```

### `POST /agent/inject` (SSE response)

Body:
```jsonc
{
  "agentId": "claude-code" | "codex" | "openclaw" | "mimocode",
  "sessionId": "auto" | "<adapter-specific id>",
  "text": "<STT utterance>",
  "metadata": { "source": "voice", "stt": {...}, "locale": "zh-CN" }
}
```

Response: an SSE stream where each event is one of
```
event: token
data: {"text": "..."}

event: tool
data: {"name": "Edit", "phase": "start" | "end", "input": {...}, "ok": true}

event: done
data: {"sessionId": "...", "tokens": 42, "stopReason": "end_turn"}

event: error
data: {"code": "AGENT_UNAVAILABLE", "message": "..."}
```

### `GET /agent/status`

Returns availability info for every registered adapter:
```jsonc
{
  "adapters": [
    { "agentId": "claude-code", "ready": true,  "detail": null },
    { "agentId": "codex",       "ready": true,  "detail": null },
    { "agentId": "openclaw",    "ready": false, "detail": "openclaw npm pkg not found" },
    { "agentId": "mimocode",    "ready": true,  "detail": null }
  ]
}
```

### `GET /agent/sessions?agentId=<id>&limit=<n>`

Returns the most-recent sessions for the given agent (used by the UI session
dropdown).

### `GET /agent/session-events?agentId=<id>&cursor=<n>&streamId=<id>`

Returns lightweight, ordered in-memory lifecycle updates. Omit `cursor` for a
cold bootstrap: only currently active sessions are returned, so completed
history is never replayed. Subsequent calls pass the returned `cursor` and
`streamId`; `reset: true` means the bounded log expired or the bridge restarted.
### `POST /agent/cancel`

Body: `{ "runId": "..." }` — cancels an in-flight `inject` SSE. The agent run
itself keeps going; only our pipe is closed.

## Embedding it

The bus is **not** a standalone sidecar. It is mounted inside the existing
`clawd-backend-service` Node sidecar process. See `src/index.js` for the
factory:

```js
const { createAgentSessionBus } = require("agent-session-bus");
const { ClaudeCodeAdapter } = require("agent-session-bus/adapters/claude-code");

const bus = createAgentSessionBus({
  port: 8181,
  log,
  adapters: [
    new ClaudeCodeAdapter({ /* ... */ }),
    // codex, openclaw, ...
  ],
});

await bus.start();
// ...
await bus.stop();
```

## Adapter interface

See `src/adapters/base.js` for the full `AgentAdapter` interface. New agents
plug in by extending `BaseAdapter` and implementing four methods:
`isAvailable`, `listSessions`, `openNew`, `inject`. (`resolveActive` has a
default implementation selecting the newest entry in `listSessions()`;
callers can provide an explicit session ID.)

### Local caller capabilities

All session reads and all injection/cancel operations require a per-start bearer capability in addition to the loopback/Host/origin boundary. `/agent/health` is the only public Bus route; it reports only `ok` and the static service identifier. The hook server's public `GET /state` is also health-only. `POST /state` and `/permission` require the hook capability; `/mock-button-inject` requires control. Origin checks alone are not authentication.

The managed bridge mints independent 256-bit random `read`, `control`, and `hook` values on every start. It atomically publishes owner-private descriptors under `~/.claw-pet/bridge-runtime/` (0700 directory, 0600 files on Unix; protected current-user + SYSTEM ACL on Windows):

- `client.json`: schema `version: 1`, epoch, exact `busPort`/`hookPort`, `tokens.read` and `tokens.control`.
- `hook.json`: same endpoint metadata with `tokens.hook` only.

Tokens are not compiled into the app, written into third-party settings, sent in URLs, or returned to the webview. These protect the **OS user boundary**, not mutually untrusted programs already running as the same user. The Tauri `local_bridge_request` command is local-main-window-only and accepts a fixed read/inject route allowlist, not arbitrary URLs. Redirects/proxies are disabled for authenticated native requests. The renderer can cancel/discard stale reads without resending a control operation.

Managed status hooks read the current hook descriptor for each request and send it only to its recorded loopback port. Claude/CodeBuddy permission hooks use the bundled `permission-hook.js` command wrapper; failure leaves approval with the original agent. Installer migration preserves unrelated HTTP endpoints and scripts, including same-named scripts in other directories. MiMo's managed copied plugin stores the absolute bundled capability-module path, never a token; restart MiMo after a plugin update so it reloads that path.

External callers must update: unauthenticated curl/HTTP integrations, old native clients, remote-forwarded hooks lacking a private bootstrap, manually copied MiMo plugins, and external voice services that know only `VOICE_BUS_URL` are **not** granted a bypass. A same-user external client can use `local-capability.js`'s permission-verifying `readDescriptor`/`headersFor` helpers, select only the required role, and reread on restart. Never forward credentials to a discovered/scanned port, use a stale credential to retry a control operation, put tokens in settings/command arguments, or weaken file permissions. Official managed hook paths above are migrated; unsupported legacy clients fail closed.

Windows currently verifies the descriptor ACL before and after opening it using the existing bundled PowerShell verifier. This is uncached and incurs two process launches per descriptor read (plus publication checks at startup). Frequent status polling may be costly on Windows; it has not been timed on a Windows host. No TTL/mtime credential cache was added because it could miss permission or ownership changes. The optional `HACHIMODOCK_BRIDGE_CAPABILITY_DIR` is a trusted launcher/test path override, not a credential or an authentication-disable switch.
