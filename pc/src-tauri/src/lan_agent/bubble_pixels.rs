//! On-demand optional Agent-bubble images over the existing paired panel lane.
//! No enrollment, cloud access, arbitrary topic forwarding, or uncertain retry.
use super::*;
use crate::usb_serial::{bubble_pixels as shared, BubbleFrame, UsbSerialManager};
use base64::Engine;
use std::sync::OnceLock;

const ACTION: u8 = 11;
const RATE: usize = 12 * 1024;
const RETRY_PROBE: Duration = Duration::from_secs(15);
const FRAME_TIMEOUT: Duration = Duration::from_millis(900);

struct Session {
    peer: Peer,
    board: String,
    system_boot: String,
    pixel_boot: String,
    body_masks: bool,
    warm: Vec<(Value, Instant)>,
    generation: u64,
    source: String,
    sent: Option<Instant>,
    polled: Instant,
}
#[derive(Default)]
struct Controller {
    bridge: shared::RenderBridge,
    session: Option<Session>,
    generation: u64,
    probe: Option<Instant>,
}
static CONTROLLER: OnceLock<Mutex<Controller>> = OnceLock::new();
fn controller() -> &'static Mutex<Controller> {
    CONTROLLER.get_or_init(|| Mutex::new(Controller::default()))
}
fn same_peer(a: &Peer, b: &Peer) -> bool {
    a.address == b.address && a.key.as_ref() == b.key.as_ref()
}
fn retire(c: &mut Controller) {
    c.session = None;
    c.bridge.clear_peer();
}
pub(crate) fn reset() {
    if let Ok(mut c) = controller().lock() {
        retire(&mut c);
        c.probe = None;
    }
}
pub(crate) fn set_renderer(instance: &str, sink: Option<shared::RenderSink>) {
    if let Ok(mut c) = controller().lock() {
        if c.bridge.set_sink(instance, sink) {
            // Old descriptors/renderer callbacks cannot outlive their WebView.
            retire(&mut c);
            c.probe = None;
        }
    }
}
pub(crate) fn current(frame: &BubbleFrame) -> bool {
    controller().lock().ok().is_some_and(|c| {
        frame.wifi
            && c.bridge.current(frame)
            && c.session
                .as_ref()
                .is_some_and(|s| s.sent.is_some_and(|at| at.elapsed() < SNAPSHOT_MAX_AGE))
    })
}

// Called before spawning the native worker: never block an IPC executor on a
// network phase holding the controller. Only this pre-admission contention is
// retryable by the bounded renderer queue.
pub(crate) fn admit(frame: &BubbleFrame) -> Result<(), String> {
    let c = controller().try_lock().map_err(|error| match error {
        std::sync::TryLockError::WouldBlock => "BUBBLE_BUSY: LAN controller occupied",
        std::sync::TryLockError::Poisoned(_) => "BUBBLE_FALLBACK: lifecycle",
    })?;
    if frame.wifi
        && c.bridge.current(frame)
        && c.session
            .as_ref()
            .is_some_and(|s| s.sent.is_some_and(|at| at.elapsed() < SNAPSHOT_MAX_AGE))
    {
        Ok(())
    } else {
        Err("BUBBLE_FALLBACK: stale content".into())
    }
}

fn request(
    peer: &Peer,
    system_boot: &str,
    board: &str,
    payload: &Value,
    timeout: Duration,
) -> Result<Value, String> {
    let encoded = serde_json::to_vec(payload).map_err(|_| "BUBBLE_FALLBACK: JSON")?;
    if !shared::valid_boot(system_boot) || encoded.len() > 1536 {
        return Err("BUBBLE_FALLBACK: frame budget".into());
    }
    let mut body = system_boot.as_bytes().to_vec();
    body.push(ACTION);
    body.extend_from_slice(&encoded);
    let bytes = exchange_link_bounded(
        peer,
        PANEL_CONTROL,
        &body,
        Some(4096),
        timeout,
        Some(Instant::now() + timeout),
    )
    .map_err(|_| "BUBBLE_FALLBACK: unknown ACK; no replay")?
    .reply;
    let reply: Value =
        serde_json::from_slice(&bytes).map_err(|_| "BUBBLE_FALLBACK: invalid ACK")?;
    if reply["boardDeviceId"] != board
        || reply["phase"] != payload["phase"]
        || reply["requestId"] != payload["requestId"]
        || (reply["ok"] != true
            && !(payload["phase"] == "need" && reply["ok"] == false && reply["busy"] == true))
    {
        return Err("BUBBLE_FALLBACK: device declined or stale ACK".into());
    }
    Ok(reply)
}
fn read_phase(phase: &str) -> Value {
    json!({"phase":phase,"requestId":uuid::Uuid::new_v4().simple().to_string()})
}
fn emit(bridge: &shared::RenderBridge, events: Vec<Value>, demand_only: bool) {
    if let Some(sink) = bridge.sink() {
        for mut event in events {
            event["wifi"] = json!(true);
            event["demandOnly"] = json!(demand_only);
            sink(event);
        }
    }
}

fn queue_warm(bridge: &shared::RenderBridge, s: &mut Session, events: &[Value]) {
    for event in events {
        s.warm.retain(|(old, _)| old["owner"] != event["owner"]);
        if s.body_masks && event["retired"] != true && bridge.prefetch_probe(event).is_some() {
            s.warm.push((event.clone(), Instant::now()));
        }
    }
    s.warm.retain(|(event, at)| {
        at.elapsed() < Duration::from_millis(shared::DEADLINE_MS)
            && bridge.prefetch_probe(event).is_some()
    });
    if s.warm.len() > 12 {
        s.warm.drain(..s.warm.len() - 12);
    }
}

fn confirm_warm(bridge: &shared::RenderBridge, s: &mut Session) {
    queue_warm(bridge, s, &[]);
    if !s.sent.is_some_and(|at| at.elapsed() < SNAPSHOT_MAX_AGE) {
        return;
    }
    let Some((event, _)) = s.warm.first() else {
        return;
    };
    let Some(mut probe) = bridge.prefetch_probe(event) else {
        return;
    };
    probe["requestId"] = json!(uuid::Uuid::new_v4().simple().to_string());
    // A snapshot ACK only confirms mailbox acceptance. The existing read-only
    // status operation proves the owner/version has reached the state lane.
    // Rechecking this read on a later tick never replays an image mutation.
    let Ok(reply) = request(
        &s.peer,
        &s.system_boot,
        &s.board,
        &probe,
        Duration::from_millis(200),
    ) else {
        return;
    };
    if ["boot", "client", "owner", "version", "transfer", "id"]
        .iter()
        .any(|key| reply[*key] != probe[*key])
    {
        return;
    }
    let events = s.warm.drain(..).map(|(event, _)| event).collect();
    emit(bridge, events, false);
}

/// Called by the ordinary snapshot worker under FOLLOW, never an audio callback.
/// Legacy peers get the original four-key snapshot, without new metadata.
pub(super) fn prepare(peer: &Peer, usb: &UsbSerialManager, body: &mut Value) {
    let Ok(_pairing) = PAIRING_MUTATION.try_lock() else {
        return;
    };
    let Ok(mut c) = controller().try_lock() else {
        return;
    };
    if usb.status().connected
        || c.bridge.sink().is_none()
        || !load_peer().as_ref().is_some_and(|p| same_peer(peer, p))
    {
        retire(&mut c);
        return;
    }
    if c.session
        .as_ref()
        .is_some_and(|s| !same_peer(&s.peer, peer))
    {
        retire(&mut c);
        c.probe = None;
    }
    if c.session.is_none() {
        if c.probe.is_some_and(|at| at.elapsed() < RETRY_PROBE) {
            return;
        }
        c.probe = Some(Instant::now());
        let Ok(status) = query_status(peer) else {
            return;
        };
        if !status.control_trusted
            || !status.panel_control
            || !status.wifi_connected
            || status.board_device_id.is_empty()
            || usb.status().connected
        {
            return;
        }
        let Ok(reply) = request(
            peer,
            &status.boot_id,
            &status.board_device_id,
            &read_phase("capabilities"),
            FRAME_TIMEOUT,
        ) else {
            return;
        };
        let boot = reply["boot"].as_str().unwrap_or("");
        if reply["schema"] != shared::SCHEMA || !shared::valid_boot(boot) {
            return;
        }
        let Some(generation) = c.generation.checked_add(1) else {
            return;
        };
        c.generation = generation;
        c.bridge.set_format(
            reply["glyphCells"] == true,
            reply["bodyMasks"] == true,
            reply["titleMasks"] == true,
        );
        c.session = Some(Session {
            peer: peer.clone(),
            board: status.board_device_id,
            system_boot: status.boot_id,
            pixel_boot: boot.into(),
            body_masks: reply["bodyMasks"] == true,
            warm: Vec::new(),
            generation,
            source: String::new(),
            sent: None,
            polled: Instant::now(),
        });
    }
    let Controller {
        bridge, session, ..
    } = &mut *c;
    let s = session.as_mut().unwrap();
    let source = body["source"].as_str().unwrap_or("");
    if s.source != source {
        bridge.clear_peer();
        s.source = source.into();
        s.sent = None;
        s.warm.clear();
    }
    // Speech overrides the current state body on the device. Preserve state
    // title fields when a speech-only update does not carry a title.
    let mut presentation = body["status"].as_object().cloned().unwrap_or_default();
    if let Some(speech) = body["speech"].as_object() {
        for (key, value) in speech {
            presentation.insert(key.clone(), value.clone());
        }
    }
    let events = bridge.observe(
        "state/lan",
        &Value::Object(presentation),
        s.generation,
        &s.board,
        &s.pixel_boot,
    );
    queue_warm(bridge, s, &events);
    emit(bridge, events, true);
    let sessions = body
        .get("sessions")
        .cloned()
        .unwrap_or_else(|| json!({"sessions":[]}));
    let events = bridge.observe(
        "session/current",
        &sessions,
        s.generation,
        &s.board,
        &s.pixel_boot,
    );
    queue_warm(bridge, s, &events);
    emit(bridge, events, true);
    body["textPixels"] = bridge.metadata("state/lan", &s.pixel_boot);
    if body.to_string().len() > MAX_BODY {
        // Optional images must not make an otherwise valid text snapshot fail.
        retire(&mut c);
        body.as_object_mut().unwrap().remove("textPixels");
        return;
    }
    if s.sent.is_some() && s.polled.elapsed() >= Duration::from_millis(1000) {
        s.polled = Instant::now();
        if usb.status().connected {
            retire(&mut c);
            body.as_object_mut().unwrap().remove("textPixels");
            return;
        }
        if !s.warm.is_empty() {
            confirm_warm(bridge, s);
            return;
        }
        match request(
            peer,
            &s.system_boot,
            &s.board,
            &read_phase("need"),
            FRAME_TIMEOUT,
        ) {
            Ok(reply) if reply["boot"] == s.pixel_boot => {
                // Only an explicitly temporary, authenticated read refusal can
                // retain the manifest. Never replay an image write, accept a
                // stale boot, or treat a bare refusal/unknown ACK as busy.
                if reply["ok"] == true {
                    let events = bridge.need(&reply["need"], s.generation, &s.board);
                    emit(bridge, events, false);
                }
            }
            _ => {
                // No phase retry on failure. Rediscover support later; ordinary
                // text continues and clears the device's optional bitmap owner.
                retire(&mut c);
                c.probe = Some(Instant::now());
                body.as_object_mut().unwrap().remove("textPixels");
            }
        }
    }
}
pub(super) fn sent(peer: &Peer, body: &Value) {
    if body.get("textPixels").is_none() {
        return;
    }
    if let Ok(mut c) = controller().lock() {
        let Controller {
            bridge, session, ..
        } = &mut *c;
        if let Some(s) = session.as_mut().filter(|s| same_peer(&s.peer, peer)) {
            s.sent = Some(Instant::now());
            confirm_warm(bridge, s);
        }
    }
}

pub(crate) fn send(usb: &UsbSerialManager, frame: BubbleFrame) -> Result<(), String> {
    let bytes = base64::engine::general_purpose::STANDARD
        .decode(&frame.data)
        .map_err(|_| "BUBBLE_FALLBACK: base64")?;
    shared::validate_frame(&frame, &bytes)?;
    if !frame.wifi {
        return Err("BUBBLE_FALLBACK: transport".into());
    }
    let deadline = Instant::now() + Duration::from_millis(shared::DEADLINE_MS);
    let transfer = uuid::Uuid::new_v4().simple().to_string();
    let mut next_write = Instant::now();
    shared::transfer_frame(&frame, &bytes, |phase, extra| {
        while Instant::now() < next_write {
            if Instant::now() >= deadline || usb.status().connected || !current(&frame) {
                return Err("BUBBLE_FALLBACK: retired".into());
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        if Instant::now() >= deadline {
            return Err("BUBBLE_FALLBACK: deadline".into());
        }
        // Release between chunks for OTA, input, media and new text snapshots.
        let _follow = loop {
            match FOLLOW.try_lock() {
                Ok(guard) => break guard,
                Err(std::sync::TryLockError::Poisoned(_)) => {
                    return Err("BUBBLE_FALLBACK: control lane".into())
                }
                Err(std::sync::TryLockError::WouldBlock) => {
                    if Instant::now() >= deadline || usb.status().connected || !current(&frame) {
                        return Err("BUBBLE_FALLBACK: retired".into());
                    }
                    std::thread::sleep(Duration::from_millis(10));
                }
            }
        };
        let _pairing = PAIRING_MUTATION
            .try_lock()
            .map_err(|_| "BUBBLE_FALLBACK: pairing busy")?;
        let c = controller()
            .lock()
            .map_err(|_| "BUBBLE_FALLBACK: lifecycle")?;
        let s = c.session.as_ref().ok_or("BUBBLE_FALLBACK: retired")?;
        if usb.status().connected
            || !c.bridge.current(&frame)
            || !s.sent.is_some_and(|at| at.elapsed() < SNAPSHOT_MAX_AGE)
            || !load_peer().as_ref().is_some_and(|p| same_peer(&s.peer, p))
        {
            return Err("BUBBLE_FALLBACK: retired".into());
        }
        let mut payload = json!({"phase":phase,"requestId":uuid::Uuid::new_v4().simple().to_string(),
            "boot":frame.boot_id,"client":frame.renderer_id,"owner":frame.owner,
            "version":frame.version,"transfer":transfer,"id":frame.id,"style":frame.style});
        for (k, v) in extra.as_object().unwrap() {
            payload[k] = v.clone();
        }
        // Include the client nonce, panel prefix, mode/length and GCM tag.
        let cost = payload.to_string().len() + 17 + 16 + 3 + 16;
        let reply = request(
            &s.peer,
            &s.system_boot,
            &s.board,
            &payload,
            FRAME_TIMEOUT.min(deadline.saturating_duration_since(Instant::now())),
        )?;
        next_write = Instant::now() + Duration::from_secs_f64(cost as f64 / RATE as f64);
        for key in ["boot", "client", "owner", "version", "transfer", "id"] {
            if reply[key] != payload[key] {
                return Err("BUBBLE_FALLBACK: stale ACK".into());
            }
        }
        Ok(reply)
    })
}
