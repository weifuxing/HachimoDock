//! Physical hold-to-dictate, using the paired control channel and the audio codec.
use super::{exchange_response, load_peer, query_status, realtime::AudioTarget, Peer};
use crate::{
    active_device_voice_context, confirm_pending_device_voice_draft, fail_device_voice_context,
    finish_device_voice_capture, push_device_voice_pcm, start_device_voice_context,
    realtime_chat::lan_audio::{LanAudio, Record, HEADER, PCM},
    usb_serial::UsbSerialManager,
};
use std::{
    io::{Read, Write},
    net::TcpStream,
    sync::{atomic::{AtomicBool, Ordering}, Arc},
    thread,
    time::{Duration, Instant},
};
use zeroize::Zeroizing;

const CONTROL_MODE: u8 = 20;
const POLL_INTERVAL: Duration = Duration::from_millis(250);
const STATUS_INTERVAL: Duration = Duration::from_secs(5);
const MAX_CAPTURE: Duration = Duration::from_secs(31);
const MAX_FRAMES: u64 = 1500;

#[derive(Clone)]
pub(crate) struct Route {
    target: AudioTarget,
    usb: UsbSerialManager,
    valid: Arc<AtomicBool>,
}

impl Route {
    fn active_link(&self) -> bool {
        self.valid.load(Ordering::Acquire) && !self.usb.status().connected
    }

    pub(crate) fn is_current(&self) -> bool {
        self.active_link() && self.target.ensure_current_pair().is_ok()
    }

    fn retire(&self) {
        self.valid.store(false, Ordering::Release);
    }
}

#[derive(Debug, PartialEq, Eq)]
struct Controls {
    hold: u32,
    confirm: u32,
}

fn decode_controls(reply: &[u8]) -> Option<Controls> {
    if reply.len() != 8 {
        return None;
    }
    Some(Controls {
        hold: u32::from_be_bytes(reply[..4].try_into().ok()?),
        confirm: u32::from_be_bytes(reply[4..].try_into().ok()?),
    })
}

fn read_record(
    stream: &mut TcpStream, codec: &mut LanAudio,
) -> Option<(Record, Zeroizing<Vec<u8>>)> {
    let mut header = [0; HEADER];
    stream.read_exact(&mut header).ok()?;
    let size = codec.record_size(&header)?;
    let mut wire = Zeroizing::new(vec![0; size]);
    wire[..HEADER].copy_from_slice(&header);
    stream.read_exact(&mut wire[HEADER..]).ok()?;
    let record = codec.open(&mut wire)?;
    Some((record, wire))
}

struct Capture {
    token: u32,
    utterance_id: String,
}

fn capture(app: &tauri::AppHandle, route: &Route, token: u32) -> Option<Capture> {
    let (mut stream, key) = route.target.open_voice(token).ok()?;
    if !route.is_current() {
        return None;
    }
    let mut codec = LanAudio::new(key);
    stream.write_all(&codec.seal(Record::Keepalive, &[])?).ok()?;
    let (Record::VoiceReady, ready) = read_record(&mut stream, &mut codec)? else {
        return None;
    };
    let queue_empty = match ready.get(HEADER) {
        Some(0) => false,
        Some(1) => true,
        _ => return None,
    };
    if !route.is_current() {
        return None;
    }
    let board = route.target.board_id();
    let boot = route.target.boot_id();
    let utterance_id = uuid::Uuid::new_v4().to_string();
    start_device_voice_context(app, board, &utterance_id, &utterance_id, "", boot,
        queue_empty, Some(route.clone()));
    let context = active_device_voice_context().lock().ok()
        .and_then(|current| current.as_ref().filter(|context| context.utterance_id == utterance_id).cloned())?;

    let deadline = Instant::now() + MAX_CAPTURE;
    let mut next_pair_check = Instant::now();
    let mut sequence = 0;
    while !context.cancelled.load(Ordering::Acquire)
        && !context.final_handled.load(Ordering::Acquire)
        && Instant::now() < deadline
        && route.active_link()
    {
        if Instant::now() >= next_pair_check {
            if !route.is_current() { break; }
            next_pair_check = Instant::now() + POLL_INTERVAL;
        }
        match read_record(&mut stream, &mut codec) {
            Some((Record::Audio { vad: None }, wire))
                if sequence < MAX_FRAMES && wire.len() == HEADER + PCM =>
            {
                push_device_voice_pcm(&context, sequence, wire[HEADER..].to_vec());
                sequence += 1;
            }
            Some((Record::Close, _)) if route.is_current() => {
                finish_device_voice_capture(&context, sequence > 0);
                return Some(Capture { token, utterance_id });
            }
            _ => break,
        }
    }
    fail_device_voice_context(&context, "Wi-Fi 录音未完整结束，请重新按住说话。");
    None
}

fn probe(peer: &Peer, usb: &UsbSerialManager) -> Option<Route> {
    let status = query_status(peer).ok()?;
    if !status.wireless_voice_input || usb.status().connected {
        return None;
    }
    Some(Route {
        target: AudioTarget::from_status(peer.clone(), status, "").ok()?,
        usb: usb.clone(),
        valid: Arc::new(AtomicBool::new(true)),
    })
}

pub(super) fn start(app: tauri::AppHandle, usb: UsbSerialManager) {
    thread::spawn(move || {
        let mut route: Option<Route> = None;
        let mut completed: Option<Capture> = None;
        let mut last_offer = 0;
        let mut next_probe = Instant::now();
        loop {
            thread::sleep(POLL_INTERVAL);
            let Some(peer) = load_peer().filter(|_| !usb.status().connected) else {
                if let Some(old) = route.take() {
                    old.retire();
                }
                next_probe = Instant::now();
                continue;
            };
            if route.as_ref().is_some_and(|route| !route.target.matches_peer(Some(&peer))) {
                if let Some(old) = route.take() {
                    old.retire();
                }
                next_probe = Instant::now();
            }
            if route.is_none() && Instant::now() >= next_probe {
                route = probe(&peer, &usb);
                next_probe = Instant::now() + STATUS_INTERVAL;
                completed = None;
                last_offer = 0;
            }
            let Some(current) = route.as_ref() else { continue };
            let controls = exchange_response(
                &peer, CONTROL_MODE, current.target.boot_id().as_bytes(), Some(8),
            ).ok().and_then(|reply| decode_controls(&reply));
            let Some(controls) = controls.filter(|_| current.active_link()) else {
                current.retire();
                route = None;
                next_probe = Instant::now() + POLL_INTERVAL;
                continue;
            };
            if controls.hold != 0 && controls.hold != last_offer {
                last_offer = controls.hold;
                completed = capture(&app, current, controls.hold);
            }
            if completed.as_ref().is_some_and(|capture| capture.token == controls.confirm) {
                let Some(capture) = completed.take() else { continue };
                confirm_pending_device_voice_draft("input/event", &serde_json::json!({
                    "action": "page_enter",
                    "boardDeviceId": current.target.board_id(),
                    "voiceUtteranceId": capture.utterance_id,
                }));
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn controls_decode_only_fixed_length_and_separate_the_two_tokens() {
        assert_eq!(decode_controls(&[0, 0, 0, 1, 0, 0, 1, 0]), Some(Controls { hold: 1, confirm: 256 }));
        assert_eq!(decode_controls(&[0; 7]), None);
        assert_eq!(decode_controls(&[0; 9]), None);
    }
}
