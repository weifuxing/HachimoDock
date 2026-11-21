# Production serial comparison adapter

Internal macOS-only hardware harness; not shipped in Pet Manager. The library
includes the production connection and live-frame writer by path. Python owns
board identity checks, consent, bounded capture verification and stop handling.
It is not a full-client or Windows integration test. No PCM is persisted.

Build/test from the repository root:

```sh
cargo test --manifest-path pc/tools/serial-probe/Cargo.toml --offline
cargo build --release --manifest-path pc/tools/serial-probe/Cargo.toml --offline
```

Pass the built dylib to `firmware/tools/duplex_memory_probe.py` with
`--rust-transport-library`. The probe also requires explicit device/version,
an unused report path, a 30–1800 second duration, and `--allow-audio` after human
consent. Exit Pet Manager first. Do not run simultaneous serial owners.

The preclose report is evidence only: `portReleased: false` means release has
not been confirmed, even if capture stopped. Only the final report plus stop
confirmation can pass. Never rebuild/replace a library loaded by a running test.
