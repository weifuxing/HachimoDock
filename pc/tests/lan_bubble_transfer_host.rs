//! Host mocks surround the unchanged production LAN controller; no sockets,
//! pairing files, Tauri, audio, or serial are linked.
use std::{sync::{Mutex, OnceLock}, time::{Duration, Instant}};
use serde_json::{json, Value};
use base64::Engine;
mod usb_serial {
    #[path = "production_shared.rs"] pub(crate) mod bubble_pixels;
    pub(crate) use bubble_pixels::BubbleFrame;
    pub(crate) struct UsbSerialManager;
    pub(crate) struct Status { pub connected: bool }
    impl UsbSerialManager {
        pub fn status(&self) -> Status { Status {connected: super::fake().lock().unwrap().usb} }
    }
}
#[derive(Clone)]
struct Peer { address: u32, key: [u8;16] }
#[derive(Default)]
struct Fake {
    usb: bool, revoked: bool, legacy: bool, untrusted: bool,
    glyph_cells: bool, body_masks: bool, owner_ready: bool,
    failure: String, calls: Vec<Value>, times: Vec<Instant>, need: Option<Value>,
}
static FAKE: OnceLock<Mutex<Fake>> = OnceLock::new();
fn fake() -> &'static Mutex<Fake> { FAKE.get_or_init(|| Mutex::new(Fake::default())) }
fn load_peer() -> Option<Peer> {
    Some(Peer {address: 1, key: if fake().lock().unwrap().revoked {[2;16]} else {[1;16]}})
}
struct DeviceStatus {control_trusted: bool, panel_control: bool, wifi_connected: bool, board_device_id: String, boot_id: String}
fn query_status(_: &Peer) -> Result<DeviceStatus, ()> {
    Ok(DeviceStatus {control_trusted: !fake().lock().unwrap().untrusted, panel_control:true,
        wifi_connected:true, board_device_id:"board".into(),boot_id:"1234567890abcdef".into()})
}
const PANEL_CONTROL: u8 = 17;
const MAX_BODY: usize = 16384;
const SNAPSHOT_MAX_AGE: Duration = Duration::from_secs(9);
static PAIRING_MUTATION: Mutex<()> = Mutex::new(());
static FOLLOW: Mutex<()> = Mutex::new(());
struct AuthenticatedLink { reply: Vec<u8> }
fn exchange_link_bounded(peer: &Peer, mode: u8, body: &[u8], max: Option<usize>, timeout: Duration, until: Option<Instant>) -> Result<AuthenticatedLink, ()> {
    assert!(until.is_some_and(|at| at > Instant::now() && at <= Instant::now() + Duration::from_millis(900)));
    assert_eq!(peer.key, [1;16]); assert_eq!(mode, 17); assert_eq!(max, Some(4096));
    assert!(timeout <= Duration::from_millis(900)); assert_eq!(&body[..16], b"1234567890abcdef");
    assert_eq!(body[16], 11); assert!(body.len() <= 1553);
    let payload: Value = serde_json::from_slice(&body[17..]).unwrap();
    let phase = payload["phase"].as_str().unwrap();
    let mut f = fake().lock().unwrap();
    f.calls.push(payload.clone());
    f.times.push(Instant::now());
    if f.legacy {return Ok(AuthenticatedLink {reply: vec![0]});}
    let mut reply=payload.clone(); reply["boardDeviceId"]=json!("board");reply["ok"]=json!(true);
    reply["boot"]=json!("1122334455667788");
    match phase {
        "capabilities" => {reply["schema"]=json!("p4-text-alpha-v2");reply["glyphCells"]=json!(f.glyph_cells);reply["bodyMasks"]=json!(f.body_masks);},
        "status" => {
            reply["ok"]=json!(f.owner_ready);
            if f.failure=="stale_status" {reply["version"]=json!("ffffffffffffffff");}
        },
        "need" => {
            if let Some(need)=f.need.take() {reply["need"]=need;}
            match f.failure.as_str() {
                "need_busy" | "busy_boot" | "busy_board" | "busy_request" => {
                    reply["ok"]=json!(false); reply["busy"]=json!(true);
                    if f.failure=="busy_boot" {reply["boot"]=json!("ffffffffffffffff");}
                    if f.failure=="busy_board" {reply["boardDeviceId"]=json!("other-board");}
                    if f.failure=="busy_request" {reply["requestId"]=json!("stale");}
                },
                "need_declined" => reply["ok"]=json!(false),
                "need_unknown" => return Err(()),
                _ => {},
            }
        },
        "begin" => {reply["offset"]=json!(0);if f.failure=="cached" {reply["cached"]=json!(true);}},
        "chunk" => {
            let data=base64::engine::general_purpose::STANDARD.decode(payload["data"].as_str().unwrap()).unwrap();
            assert!(data.len()<=512);
            reply["offset"]=json!(payload["offset"].as_u64().unwrap()+data.len() as u64);
            match f.failure.as_str() {
                "unknown" => return Err(()),
                "stale" => reply["owner"]=json!("f".repeat(16)),
                "offset" => reply["offset"]=json!(0),
                "usb" => f.usb=true,
                "pairing" => f.revoked=true,
                "audio" => reply["ok"]=json!(false),
                _ => {},
            }
        },
        "end" => {},
        _ => panic!("unexpected fixed operation"),
    }
    Ok(AuthenticatedLink {reply: serde_json::to_vec(&reply).unwrap()})
}
#[path="production_lan.rs"] mod bubble_pixels;
