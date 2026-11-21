# Bubble text pixel transfer and verification

The negotiated `p4-text-alpha-v2` transport uses the following boundaries. See the [firmware protocol](../firmware/protocol.md) for the current handshake and frame contract.

- Native owns desired descriptors by peer/boot/renderer/owner/version, uses exact bounded identities with unique native-issued owner tokens (no hash
  identity), canonicalizes state/speech display roles, checks the expected content asset whitelist, bounds history to12 owners, and passes u64
  connection IDs as decimal strings. Main-window IPC only; stale renderer stop
  is compare-and-clear. No old-window callback clears a new instance.
- JS has one raster/invoke and FIFO whole-asset turns, latest-per-owner coalescing,
  bounded pre-admission busy retry, an absolute owner deadline, lazy pages and
  fallback cooldown. It has no device-resident sent TTL. Explicit, rate-bounded
  device missing notices refresh current assets/pages after eviction.
- One native worker is reserved BEFORE decode/spawn. Each bounded physical frame
  separately holds the existing asset guard, rechecks current peer/boot/content,
  capture/media/update protection at final commit, then releases it before ACK
  wait/token sleep. Unknown delivery is not replayed. The worker uses existing
  paced serial writes; it does not open another port or replace its driver.
- Device stage admission requires matching boot/client/route/owner/version AND an
  expected asset key/style; each text manifest replaces the entire bounded owner
  snapshot across roles, retiring omitted owners immediately; READY precedes chunks. It incrementally checks RLE
  geometry/checksum, ACKs each exact phase/offset, and retires on supersession,
  idle/absolute timeout, protected work or explicit matching abort. Another begin
  cannot overwrite it. Resource limits are8 cache slots/256KiB plus32KiB staging.
- Latest text always wins. Same-frame immutable read pins cover title/current body
  and survive concurrent peer retirement. The negotiated readiness rules apply to
  each independently delivered title or body asset.
  Offscreen items no longer refresh pins. Cache revisions invalidate overlay.

FNVs/nonces are correlation/error checks, not new authentication claims. Pixel
budgets and timeouts are resource limits, not proof of wall-time guarantees.

MiSans16/32 exact data retain public attribution and the unmodified official
Agreement + embedded-font notice, included in Tauri resources and Windows staging.
The font generator defaults to native32-only; rebuilding fallback is explicit and
retains existing codepoints.

Run the focused host tests with synthetic profiles, loopback mocks, and no connected devices or service credentials:

```sh
python -m pytest -p no:cacheprovider firmware/tests/p4_bubble_pixels_test.py \
  firmware/tests/p4_font_test.py firmware/tests/p4_title_masks_test.py \
  firmware/tests/protocol_contract_test.py
node --test pc/src/lib/bubble-pixels.test.js pc/src/lib/p4-session-service.test.js \
  pc/src-tauri/bridge/test/codex-log-monitor.test.js \
  pc/src-tauri/bridge/packages/agent-session-bus/test/codex-adapter.test.js \
  pc/src-tauri/bridge/packages/agent-session-bus/test/server.test.js \
  pc/scripts/misans-license.test.mjs
python pc/scripts/test-bubble-pixels-native.py --deps EXISTING_RELEASE_DEPS \
  --rustc EXISTING_RUSTC --output NEW_TEST_OUTPUT
```

The Rust runner compiles the production controller and transfer logic against
prebuilt Rust dependencies with a simulated wire and ACK adapter. Full desktop
builds and device acceptance cover target resources, OTA headroom, RAM, frame
timing, and audio separately.
