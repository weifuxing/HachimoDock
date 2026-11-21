# Wi-Fi 6 OTA adapter

The firmware entry points are in `pet_p4_wireless_ota.h` and the desktop
transaction is in `pc/src-tauri/src/wireless_ota.rs`. Mode 12 in the paired
LAN dispatcher connects these adapters. The desktop command selects the
bundled Wi-Fi 6 image for the chip revision read from the authenticated peer.

- Admit only an authenticated controller with current, persistent USB-backed
  trust for this board. The dispatcher supplies the trust decision and a fresh,
  nonzero, connection-local 16-byte owner. Neither comes from the request.
  Serialize that owner's calls with trust revocation and disconnect. Call
  `pet_p4_wireless_ota_close` with the old owner on either event. First-claim
  pairing alone does not admit OTA. A trusted controller needs no per-update
  touch confirmation.
- Select the Wi-Fi 6 image for the authenticated board and chip revision using
  the bundled release catalog. The desktop needs two trusted digests for that
  exact selected artifact. `sha256` covers every byte of the distributed
  binary. `installed_digest` uses ESP-IDF application partition SHA-256
  semantics, which may use the digest embedded in a validated application
  image. Compute both digests from the bundled artifact selected by the backend.
  A filename, discovery record, or value supplied by an untrusted caller is insufficient.
- Use typed commands for boot, begin, raw chunk and commit. Correlate each
  response within the authenticated connection. Bound one raw chunk to 4096
  bytes and wait for its response before sending the next. Begin must echo the
  authenticated valid boot's ID, partition address and native digest, plus a
  fresh nonzero transfer ID and exact artifact bytes/digests. Chunk, commit and
  close act only on the connection's own transfer. Do not use a raw firmware
  topic relay or transparently reconnect a mutation.
- After complete byte count and streaming SHA-256 verification, the firmware
  checks the inactive image and its project identity with the existing A/B
  updater. It compares the native partition digest before selecting the boot
  partition. It then uses the existing deferred restart, station stop and
  runtime-health rollback. The desktop reports success only after the same
  authenticated board has a different boot ID, a valid running image and the
  committed partition address and native digest.
- The LAN driver must classify a missing/malformed authenticated response as
  unknown and close the original link. Do not automatically retry, resume, or
  send another mutation to resolve an unknown result. A missing commit receipt
  remains unknown in the current desktop adapter.

The initial mode-12 message and response have zero request bytes and one
admission byte. Subsequent records carry a big-endian 16-bit length, AES-GCM
ciphertext and a 16-byte tag under the paired session key. The length is AAD.
The first nonce byte is 2 for commands or 3 for responses. The last four nonce
bytes are the big-endian record ordinal, starting at zero. The command byte
selects identity (0), boot (1), begin (2), raw chunk (3) or commit (4).
The response starts with 1 and its typed body or 0 and an error message. The
connection closes after a rejection, commit, revocation, timeout or link loss.
