//! Passive UART recovery for a possibly incomplete appearance raw frame.
//! A USB re-enumeration is not evidence that an independently powered P4 reset.
use super::appearance_safety::{valid_boot_id, RawQuarantine};
use super::*;

const UNPROVEN: &str = "appearance_reset_unproven";
const BOOT_HEARTBEAT_MIN_GAP: Duration = Duration::from_millis(1500);
const BOOT_HEARTBEAT_MAX_GAP: Duration = Duration::from_secs(8);

#[derive(Clone, Debug, PartialEq, Eq)]
struct BootAnnouncement {
    id: String,
    build: String,
}

fn announcement(
    frame: &SerialFrame,
    board: &str,
    before: Option<&str>,
) -> Option<BootAnnouncement> {
    if frame.integrity.framed {
        return None;
    }
    let message = parse_serial_frame(frame)?;
    let p = &message.payload;
    let caps = &p["capabilities"];
    let id = caps["debugBootId"].as_str().filter(|s| valid_boot_id(s))?;
    let build = p["buildId"]
        .as_str()
        .filter(|s| !s.is_empty() && s.len() <= 128)?;
    if message.topic != "hello"
        || p["runtime"] != "esp-p4"
        || p["boardDeviceId"] != board
        || caps["debugEvidenceIdentity"] != "p4-evidence-identity-v1"
        || caps["serialControlOffer"] != control_integrity::ENGINE
        || caps.get("serialControlIntegrity").is_some()
        || caps.get("serialControlNonce").is_some()
        || caps["transport"]["usbUartBaud"].as_u64() != Some(u64::from(P4_UNIFIED_UART_BAUD))
        || caps["transport"]["rawAssetReadyAck"] != true
        || before == Some(id)
    {
        return None;
    }
    // Earlier journals did not record the boot ID. On the listed firmware
    // the UART emits this unnegotiated heartbeat periodically only before
    // desktop bind. The client's raw upload followed a verified nonempty bind
    // on that same route; while in raw receive, the UART cannot parse binds.
    // Do not infer this legacy reset proof from an arbitrary older firmware.
    if before.is_none()
        && !matches!(
            p["fw"].as_str(),
            Some(
                "0.7.103-p4-r179"
                    | "0.7.103-p4-r180"
                    | "0.7.103-p4-r181"
                    | "0.7.103-p4-r188"
                    | "0.7.103-p4-r188-wifi6"
                    | "0.7.103-p4-r188-pico"
                | "0.7.103-p4-r189-wifi6"
                | "0.7.103-p4-r189-pico"
            )
        )
    {
        return None;
    }
    Some(BootAnnouncement {
        id: id.into(),
        build: build.into(),
    })
}

#[derive(Default)]
struct Heartbeats(Option<(Instant, BootAnnouncement)>);

impl Heartbeats {
    fn observe(&mut self, now: Instant, hello: BootAnnouncement) -> bool {
        if let Some((first, previous)) = &self.0 {
            if *previous == hello {
                let gap = now.saturating_duration_since(*first);
                if gap < BOOT_HEARTBEAT_MIN_GAP {
                    return false;
                }
                if gap <= BOOT_HEARTBEAT_MAX_GAP {
                    return true;
                }
            }
        }
        self.0 = Some((now, hello));
        false
    }
}

fn wait_for_frame<R: BufRead, T>(
    reader: &mut R,
    deadline: Instant,
    mut accept: impl FnMut(&SerialFrame) -> Option<T>,
) -> Result<T, String> {
    let mut buffer = Vec::with_capacity(32 * 1024);
    while Instant::now() < deadline {
        match read_serial_frame(reader, &mut buffer) {
            Ok(Some(frame)) => {
                if let Some(result) = accept(&frame) {
                    return Ok(result);
                }
            }
            Ok(None) => return Err(UNPROVEN.into()),
            Err(ref error) if serial_read_recovery_delay(error).is_some() => {
                let delay = serial_read_recovery_delay(error).unwrap();
                if !delay.is_zero() {
                    thread::sleep(delay);
                }
            }
            Err(ref error) if error.kind() == std::io::ErrorKind::InvalidData => continue,
            Err(_) => return Err(UNPROVEN.into()),
        }
    }
    Err(UNPROVEN.into())
}

fn matches_handshake(
    frame: &SerialFrame,
    board: &str,
    boot: &BootAnnouncement,
    nonce: &str,
) -> Option<SerialMessage> {
    let mut message = parse_serial_frame(frame)?;
    validate_hello_message(&mut message).ok()?;
    let p = &message.payload;
    if !frame.integrity.framed
        || control_integrity::confirmed_nonce(p) != Some(nonce)
        || p["boardDeviceId"] != board
        || p["runtime"] != "esp-p4"
        || p["buildId"] != boot.build
        || p["capabilities"]["debugBootId"] != boot.id
    {
        return None;
    }
    Some(message)
}

// Keep this boundary independent of OS handles: a failed passive read can
// never call the closure that writes the handshake.
fn confirm_then_bind<R: BufRead>(
    reader: &mut R,
    board: &str,
    previous_boot: Option<&str>,
    nonce: &str,
    mut now: impl FnMut() -> Instant,
    send_bind: impl FnOnce() -> Result<(), String>,
) -> Result<SerialMessage, String> {
    let mut heartbeats = Heartbeats::default();
    let boot = wait_for_frame(
        reader,
        Instant::now() + SERIAL_P4_COLD_BOOT_TIMEOUT,
        |frame| {
            let hello = announcement(frame, board, previous_boot)?;
            heartbeats.observe(now(), hello.clone()).then_some(hello)
        },
    )?;
    send_bind().map_err(|_| "appearance_reset_handshake_unconfirmed")?;
    wait_for_frame(reader, Instant::now() + SERIAL_PROBE_TIMEOUT, |frame| {
        matches_handshake(frame, board, &boot, nonce)
    })
    .map_err(|_| "appearance_reset_handshake_unconfirmed".into())
}

pub(super) fn probe_after_reset(
    port: &str,
    target: &RawQuarantine,
    desktop_id: &str,
) -> Result<ProbedSerialPort, String> {
    if desktop_id.trim().is_empty() {
        return Err(UNPROVEN.into());
    }
    let (mut writer, reader) = open_serial_pair_with_retry(port, LEGACY_USB_SERIAL_BAUD)
        .map_err(|_| "appearance_reset_usb_open_failed")?;
    writer
        .set_baud_rate(P4_UNIFIED_UART_BAUD)
        .map_err(|_| "appearance_reset_usb_open_failed")?;
    // Discard buffered data from the retired connection. Until the boot proof
    // is complete this path must send no bytes, including probe/bind.
    writer
        .clear(serialport::ClearBuffer::Input)
        .map_err(|_| "appearance_reset_usb_open_failed")?;
    let mut reader = BufReader::new(reader);
    // Do not close/reopen or try other rates after proof. The nonce binds the
    // normal handshake to the same handles, board and newly observed boot.
    let nonce = uuid::Uuid::new_v4().simple().to_string();
    let hello = confirm_then_bind(
        &mut reader,
        &target.board,
        target.boot_id.as_deref(),
        &nonce,
        Instant::now,
        || send_serial_probe_handshakes(writer.as_mut(), desktop_id, &nonce, false),
    )?;
    Ok(ProbedSerialPort::from_verified_reader(
        writer,
        reader,
        P4_UNIFIED_UART_BAUD,
        hello,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::{json, Value};
    use std::cell::Cell;

    const BOARD: &str = "p4-reset-test";
    const OLD: &str = "0123456789abcdef";
    const NEW: &str = "1123456789abcdef";
    const NONCE: &str = "0123456789abcdef0123456789abcdef";

    fn hello(boot: &str) -> Value {
        json!({
            "topic": "hello",
            "payload": {
                "boardDeviceId": BOARD, "runtime": "esp-p4", "fw": "0.7.103-p4-r181",
                "buildId": "0.7.103-p4-r181+test",
                "capabilities": {
                    "debugEvidenceIdentity": "p4-evidence-identity-v1", "debugBootId": boot,
                    "serialControlOffer": control_integrity::ENGINE,
                    "transport": {"usbUartBaud": P4_UNIFIED_UART_BAUD, "rawAssetReadyAck": true}
                }
            }
        })
    }

    fn guarded(boot: &str, nonce: &str) -> Vec<u8> {
        let mut msg = hello(boot);
        let caps = &mut msg["payload"]["capabilities"];
        caps["serialControlNonce"] = json!(nonce);
        caps["serialControlIntegrity"] = json!(control_integrity::ENGINE);
        let body = msg.to_string();
        format!(
            "!P4C1:{:05}:{:08x}:{body}\n",
            body.len(),
            control_integrity::fingerprint(body.as_bytes())
        )
        .into_bytes()
    }

    fn plain(msg: &Value) -> Vec<u8> {
        format!("{msg}\n").into_bytes()
    }

    #[test]
    fn writes_only_after_two_fresh_heartbeats_and_ignores_stale_or_wrong_handshake() {
        for before in [None, Some(OLD)] {
            let pulse = plain(&hello(NEW));
            let wire = [
                pulse.clone(),
                pulse,
                guarded(OLD, NONCE),
                guarded(NEW, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
                guarded(NEW, NONCE),
            ]
            .concat();
            let start = Instant::now();
            let ticks = Cell::new(0);
            let writes = Cell::new(0);
            let response = confirm_then_bind(
                &mut std::io::Cursor::new(wire),
                BOARD,
                before,
                NONCE,
                || {
                    let n = ticks.get();
                    ticks.set(n + 1);
                    start + Duration::from_secs(2 * n)
                },
                || {
                    assert_eq!(ticks.get(), 2);
                    writes.set(writes.get() + 1);
                    Ok(())
                },
            )
            .unwrap();
            assert_eq!(response.payload["capabilities"]["debugBootId"], NEW);
            assert_eq!(writes.get(), 1);
        }
    }

    #[test]
    fn never_writes_on_old_boot_other_board_unrelated_firmware_or_buffered_duplicates() {
        let old = plain(&hello(OLD));
        let fresh = plain(&hello(NEW));
        let mut wrong_board = hello(NEW);
        wrong_board["payload"]["boardDeviceId"] = json!("p4-other");
        let wrong_board = plain(&wrong_board);
        let mut wrong_fw = hello(NEW);
        wrong_fw["payload"]["fw"] = json!("unverified");
        let wrong_fw = plain(&wrong_fw);
        for (wire, before, gap) in [
            ([old.clone(), old].concat(), Some(OLD), 2),
            ([wrong_board.clone(), wrong_board].concat(), Some(OLD), 2),
            ([wrong_fw.clone(), wrong_fw].concat(), None, 2),
            ([fresh.clone(), fresh.clone()].concat(), Some(OLD), 0),
            ([fresh, guarded(NEW, NONCE)].concat(), Some(OLD), 2),
        ] {
            let start = Instant::now();
            let ticks = Cell::new(0);
            assert_eq!(
                confirm_then_bind(
                    &mut std::io::Cursor::new(wire),
                    BOARD,
                    before,
                    NONCE,
                    || {
                        let n = ticks.get();
                        ticks.set(n + 1);
                        start + Duration::from_secs(gap * n)
                    },
                    || panic!("unproven reset must send no bytes")
                )
                .err()
                .as_deref(),
                Some(UNPROVEN)
            );
        }
    }
}
