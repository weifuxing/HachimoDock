//! Internal test adapter, not bundled. Uses the production serial open/clone
//! and live-frame writer. No logs, PCM storage, UI, model calls or auto-replay.
#![allow(dead_code)]
use std::io::{Read, Write};
use std::sync::{Arc, Mutex, OnceLock};

struct SerialMessage;
#[path = "../../../src-tauri/src/usb_serial/connection_handle.rs"]
mod connection_handle;
#[path = "../../../src-tauri/src/usb_serial/live_audio_pacing.rs"]
mod live_audio_pacing;
#[cfg(test)]
#[path = "../../../src-tauri/src/realtime_audio_macos.rs"]
mod realtime_audio_macos;

struct Port {
    reader: Mutex<Box<dyn Read + Send>>,
    writer: Mutex<Box<dyn Write + Send>>,
    stats: Mutex<[u64; 4]>,
}
static PORT: OnceLock<Mutex<Option<Arc<Port>>>> = OnceLock::new();
fn slot() -> &'static Mutex<Option<Arc<Port>>> { PORT.get_or_init(|| Mutex::new(None)) }
fn port() -> Option<Arc<Port>> { slot().lock().ok()?.clone() }

/// Explicit test-only open. Caller must check exclusivity and verify board identity.
#[no_mangle]
pub unsafe extern "C" fn probe_open(path: *const u8, len: usize) -> i32 {
    if path.is_null() || len == 0 || len > 1024 { return -1; }
    let Ok(name) = std::str::from_utf8(std::slice::from_raw_parts(path, len)) else { return -1; };
    let Ok(mut state) = slot().lock() else { return -1; };
    if state.is_some() { return -1; }
    let Ok((mut writer, reader)) = connection_handle::open_serial_pair_with_retry(name, 4_000_000) else { return -2; };
    // Match usb_serial::probe_serial_port, including subsequent reopens after
    // macOS drops modem lines on close. Do not depend on a previous pyserial open.
    if writer.write_data_terminal_ready(true).is_err() || writer.write_request_to_send(true).is_err() { return -2; }
    *state = Some(Arc::new(Port {reader: Mutex::new(reader), writer: Mutex::new(writer), stats: Mutex::new([0;4])}));
    0
}

#[no_mangle]
pub unsafe extern "C" fn probe_read(buffer: *mut u8, cap: usize) -> isize {
    if buffer.is_null() || cap == 0 || cap > 8192 { return -1; }
    let Some(p) = port() else { return -1; };
    let Ok(mut reader) = p.reader.lock() else { return -1; };
    match reader.read(std::slice::from_raw_parts_mut(buffer, cap)) {
        Ok(0) => -1,
        Ok(n) => n as isize,
        Err(e) if matches!(e.kind(), std::io::ErrorKind::TimedOut | std::io::ErrorKind::WouldBlock | std::io::ErrorKind::Interrupted) => 0,
        Err(_) => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn probe_send(buffer: *const u8, len: usize) -> i32 {
    if buffer.is_null() || len == 0 || len > 16384 { return -1; }
    let Some(p) = port() else { return -1; };
    let Ok(mut writer) = p.writer.lock() else { return -1; };
    let start = std::time::Instant::now();
    let result = live_audio_pacing::write_serial_live_frame(
        &mut **writer, std::slice::from_raw_parts(buffer, len),
        // The macOS production audio/play_chunk profile in usb_serial.rs.
        // This harness is deliberately macOS-only, not a Windows transport test.
        64, std::time::Duration::from_micros(250));
    let (r, failed) = match &result { Ok(r) => (r, 0), Err(e) => (&e.report, 1) };
    if let Ok(mut s) = p.stats.lock() {
        *s = [r.bytes_written as u64, r.retries as u64, start.elapsed().as_micros() as u64, failed];
    }
    if result.is_ok() { 0 } else { -2 }
}

#[no_mangle]
pub unsafe extern "C" fn probe_stats(out: *mut u64, cap: usize) -> i32 {
    if out.is_null() || cap != 4 { return -1; }
    let Some(p) = port() else { return -1; };
    let Ok(s) = p.stats.lock() else { return -1; };
    std::slice::from_raw_parts_mut(out, 4).copy_from_slice(&*s);
    0
}

#[no_mangle]
pub extern "C" fn probe_close() {
    // In-flight operations own Arcs, so close cannot invalidate their handles.
    if let Ok(mut state) = slot().lock() { *state = None; }
}

#[cfg(test)]
mod tests {
    #[test]
    fn invalid_calls_do_not_open_hardware() {
        unsafe {
            assert_eq!(super::probe_open(std::ptr::null(), 0), -1);
            assert_eq!(super::probe_read(std::ptr::null_mut(), 0), -1);
            assert_eq!(super::probe_send(std::ptr::null(), 0), -1);
            assert_eq!(super::probe_stats(std::ptr::null_mut(), 0), -1);
        }
        super::probe_close();
        assert!(super::port().is_none());
    }
}
