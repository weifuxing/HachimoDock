//! Agent presentation and authenticated status for a paired WiFi6 board.
#[path = "lan_agent/discovery.rs"]
mod discovery;
#[path = "lan_agent/component.rs"]
pub(crate) mod component;
#[path = "lan_agent/appearance.rs"]
pub(crate) mod appearance;
#[path = "lan_agent/ota.rs"]
pub(crate) mod ota;
#[path = "lan_agent/reboot.rs"]
pub(crate) mod reboot;
#[path = "lan_agent/trust.rs"]
pub(crate) mod trust;
#[path = "lan_agent/realtime.rs"]
pub(crate) mod realtime;
pub(crate) mod voice;
pub(crate) mod private_feed;
#[path = "lan_agent/widget_data.rs"]
pub(crate) mod widget_data;
pub(crate) mod press;
mod press_state;
pub(crate) mod bubble_pixels;
#[cfg(test)]
#[path = "lan_agent/confirmation_tests.rs"]
mod confirmation_tests;
use num_bigint_dig::BigUint;
use ring::{
    aead, digest, hmac,
    rand::{SecureRandom, SystemRandom},
};
use serde_json::{json, Map, Value};
use std::{
    io::{self, BufRead, Read, Write},
    net::{Ipv4Addr, SocketAddr, TcpStream, UdpSocket},
    path::PathBuf,
    sync::Mutex,
    time::{Duration, Instant},
};
use zeroize::Zeroizing;

const PORT: u16 = 29461;
const MAX_BODY: usize = 16384;
const REDISCOVER_INTERVAL: Duration = Duration::from_secs(15);
const PROBE_INTERVAL: Duration = Duration::from_secs(5);
const IO_TIMEOUT: Duration = Duration::from_millis(2200);
const STATUS_MAX_AGE_MS: u64 = 30_000;
const SNAPSHOT_MAX_AGE: Duration = Duration::from_secs(9);
const PURPOSE: &str = "wifi6-agent-lan-v1";
const PROBE: u8 = 0;
const DEVICE_STATUS: u8 = 6;
const DEVICE_STATUS_MAX: usize = 576;
const VOLUME_REQUEST: u8 = 11;
const VOLUME_RESULT: u8 = 8;
const KEYBOARD_CONFIG: u8 = 15;
const KEYBOARD_CONFIG_REPLY_MAX: usize = 512;
const PANEL_CONTROL: u8 = 17;
const PANEL_REQUEST_MAX: usize = 8192;
const PAIR_PIN: u8 = 3;
const PAIR_TIMEOUT: Duration = Duration::from_secs(10);
const SRP_BYTES: usize = 384;
const SRP_IDENTITY: &[u8] = b"Hachimodock";
// RFC 5054 3072-bit group, matching ESP_NG_3072 on the device.
const SRP_PRIME: &str = concat!(
    "ffffffffffffffffc90fdaa22168c234c4c6628b80dc1cd129024e088a67cc74",
    "020bbea63b139b22514a08798e3404ddef9519b3cd3a431b302b0a6df25f1437",
    "4fe1356d6d51c245e485b576625e7ec6f44c42e9a637ed6b0bff5cb6f406b7ed",
    "ee386bfb5a899fa5ae9f24117c4b1fe649286651ece45b3dc2007cb8a163bf05",
    "98da48361c55d39a69163fa8fd24cf5f83655d23dca3ad961c62f356208552bb",
    "9ed529077096966d670c354e4abc9804f1746c08ca18217c32905e462e36ce3b",
    "e39e772c180e86039b2783a2ec07a28fb5c55df06f4c52c9de2bcbf695581718",
    "3995497cea956ae515d2261898fa051015728e5a8aaac42dad33170d04507a33",
    "a85521abdf1cba64ecfb850458dbef0a8aea71575d060c7db3970f85a6e1e4c7",
    "abf5ae8cdb0933d71e8c94e04a25619dcee3d2261ad2ee6bf12ffa06d98a0864",
    "d87602733ec86a64521f2b18177b200cbbe117577a615d6c770988c0bad946e2",
    "08e24fa074e5ab3143db5bfce0fd108e4b82d120a93ad2caffffffffffffffff",
);
const SNAPSHOT: u8 = 2;

#[derive(Clone)]
struct Peer {
    address: SocketAddr,
    key: Zeroizing<[u8; 16]>,
}

fn config_path() -> Result<PathBuf, String> {
    Ok(crate::get_home_dir()?.join(".claw-pet/lan-agent/peer.secure.json"))
}

fn parse_address(ip: &str) -> Result<SocketAddr, String> {
    let ip: Ipv4Addr = ip
        .parse()
        .map_err(|_| "Enter the IPv4 address shown on the device.")?;
    if ip.is_unspecified() || ip.is_loopback() || ip.is_multicast() || ip.is_broadcast() {
        return Err("Enter the Wi-Fi address shown on the device.".into());
    }
    Ok((ip, PORT).into())
}

fn parse_peer(ip: &str, code: &str) -> Result<Peer, String> {
    let address = parse_address(ip)?;
    let code = Zeroizing::new(
        code.chars()
            .filter(|c| *c != '-' && !c.is_ascii_whitespace())
            .collect::<String>(),
    );
    if code.len() != 32 || !code.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err("Invalid stored LAN key.".into());
    }
    let mut key = Zeroizing::new([0; 16]);
    for (i, byte) in key.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&code[i * 2..i * 2 + 2], 16)
            .map_err(|_| "Invalid stored LAN key.")?;
    }
    Ok(Peer { address, key })
}

fn session_digest(master: &[u8], client: &[u8; 16], server: &[u8; 16]) -> hmac::Tag {
    let master = hmac::Key::new(hmac::HMAC_SHA256, master);
    let mut ctx = hmac::Context::with_key(&master);
    ctx.update(b"P4LAN1");
    ctx.update(client);
    ctx.update(server);
    ctx.sign()
}

fn key_from_digest(digest: &hmac::Tag) -> aead::LessSafeKey {
    aead::LessSafeKey::new(
        aead::UnboundKey::new(&aead::AES_128_GCM, &digest.as_ref()[..16])
            .expect("AES-128 key length"),
    )
}

#[cfg(test)]
fn session_key(master: &[u8], client: &[u8; 16], server: &[u8; 16]) -> aead::LessSafeKey {
    key_from_digest(&session_digest(master, client, server))
}

fn nonce(direction: u8) -> aead::Nonce {
    let mut bytes = [0; 12];
    bytes[0] = direction;
    aead::Nonce::assume_unique_for_key(bytes)
}

fn exchange(peer: &Peer, mode: u8, body: &[u8]) -> io::Result<()> {
    exchange_response(peer, mode, body, None).map(|_| ())
}

fn exchange_response(peer: &Peer, mode: u8, body: &[u8], response_max: Option<usize>) -> io::Result<Vec<u8>> {
    exchange_response_with_timeout(peer, mode, body, response_max, IO_TIMEOUT)
}

fn exchange_response_with_timeout(peer: &Peer, mode: u8, body: &[u8], response_max: Option<usize>,
                                  response_timeout: Duration) -> io::Result<Vec<u8>> {
    exchange_connection(peer, mode, body, response_max, response_timeout).map(|(_, _, reply)| reply)
}

fn retry_uncommitted_offer<T>(offer: impl Fn() -> std::io::Result<T>) -> std::io::Result<T> {
    use std::io::ErrorKind::*;
    offer().or_else(|error| {
        if matches!(error.kind(), TimedOut | WouldBlock | UnexpectedEof | ConnectionReset) {
            // Offers only establish a fresh authenticated socket. They do not
            // arm a microphone, write flash or start a component transaction.
            std::thread::sleep(Duration::from_millis(180));
            offer()
        } else {
            Err(error)
        }
    })
}

fn exchange_connection(peer: &Peer, mode: u8, body: &[u8], response_max: Option<usize>,
                       response_timeout: Duration) -> io::Result<(TcpStream, aead::LessSafeKey, Vec<u8>)> {
    let session = exchange_link(peer, mode, body, response_max, response_timeout)?;
    Ok((session.stream, session.key, session.reply))
}

struct AuthenticatedLink {
    stream: TcpStream,
    key: aead::LessSafeKey,
    reply: Vec<u8>,
    subkey: Option<Zeroizing<[u8; 16]>>,
}

fn exchange_link(peer: &Peer, mode: u8, body: &[u8], response_max: Option<usize>,
                 response_timeout: Duration) -> io::Result<AuthenticatedLink> {
    exchange_link_bounded(peer, mode, body, response_max, response_timeout, None)
}

// Optional assets have a total deadline including TCP/authentication, rather
// than just an ACK timeout. Existing control/audio callers keep their budgets.
fn exchange_link_bounded(peer: &Peer, mode: u8, body: &[u8], response_max: Option<usize>,
                 response_timeout: Duration, until: Option<Instant>) -> io::Result<AuthenticatedLink> {
    let budget = |limit: Duration| -> io::Result<Duration> {
        let remaining = until.map(|at| at.saturating_duration_since(Instant::now())).unwrap_or(limit);
        if remaining.is_zero() { return Err(io::ErrorKind::TimedOut.into()); }
        Ok(limit.min(remaining))
    };
    let invalid = || io::Error::new(io::ErrorKind::InvalidData, "LAN authentication failed");
    if body.len() > MAX_BODY {
        return Err(invalid());
    }
    let mut client = [0; 16];
    SystemRandom::new()
        .fill(&mut client)
        .map_err(|_| invalid())?;
    let mut stream = TcpStream::connect_timeout(&peer.address, budget(IO_TIMEOUT)?)?;
    let deadline = Instant::now() + budget(IO_TIMEOUT)?;
    stream.set_write_timeout(Some(budget(IO_TIMEOUT)?))?;
    stream.set_nodelay(true)?;
    stream.write_all(&client)?;
    let mut server = [0; 16];
    read_before(&mut stream, &mut server, deadline)?;
    let material = session_digest(peer.key.as_ref(), &client, &server);
    let key = key_from_digest(&material);
    let subkey = (mode == realtime::MODE).then(|| {
        let master = hmac::Key::new(hmac::HMAC_SHA256, peer.key.as_ref());
        let mut context = hmac::Context::with_key(&master);
        context.update(b"P4LAN-RTC-v1");
        context.update(&client);
        context.update(&server);
        context.update(body);
        let tag = context.sign();
        let mut key = Zeroizing::new([0; 16]);
        key.copy_from_slice(&tag.as_ref()[..16]);
        key
    });
    let length = (body.len() as u16).to_be_bytes();
    let header = [mode, length[0], length[1]];
    let mut ciphertext = Zeroizing::new(Vec::with_capacity(body.len() + 16));
    ciphertext.extend_from_slice(body);
    key.seal_in_place_append_tag(nonce(0), aead::Aad::from(header), &mut *ciphertext)
        .map_err(|_| invalid())?;
    let mut frame = Vec::with_capacity(header.len() + ciphertext.len());
    frame.extend_from_slice(&header);
    frame.extend_from_slice(&ciphertext);
    stream.set_write_timeout(Some(budget(IO_TIMEOUT)?))?;
    stream.write_all(&frame)?;
    let deadline = Instant::now() + budget(response_timeout)?;
    let mut aad = header.to_vec();
    let length = if let Some(max) = response_max {
        let mut bytes = [0; 2];
        read_before(&mut stream, &mut bytes, deadline)?;
        let length = u16::from_be_bytes(bytes) as usize;
        if !(1..=max).contains(&length) {
            return Err(invalid());
        }
        aad.extend_from_slice(&bytes);
        length
    } else {
        0
    };
    let mut reply = vec![0; length + aead::AES_128_GCM.tag_len()];
    read_before(&mut stream, &mut reply, deadline)?;
    let length = key.open_in_place(nonce(1), aead::Aad::from(aad), &mut reply)
        .map_err(|_| invalid())?.len();
    reply.truncate(length);
    Ok(AuthenticatedLink { stream, key, reply, subkey })
}

#[derive(serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct LanDeviceStatus {
    firmware: String,
    boot_id: String,
    wifi_connected: bool,
    clock_synchronized: bool,
    https_successes: u32,
    https_failures: u32,
    #[serde(default)]
    trusted_volume: bool,
    #[serde(default)]
    component_delivery: bool,
    #[serde(default)]
    component_bundle: bool,
    #[serde(default, rename(deserialize = "ac"))]
    appearance_capacity: u32,
    #[serde(default, rename(deserialize = "wd"))]
    widget_data_delivery: bool,
    #[serde(default, rename(deserialize = "ptt"))]
    wireless_voice_input: bool,
    #[serde(default, rename(deserialize = "pf"))]
    widget_credential_storage_ready: bool,
    #[serde(default)]
    input_double_press: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    system_settings: Option<bool>,
    // Keep this on the old desktop's 512-byte status wire budget.
    #[serde(default, rename(deserialize = "ops"))]
    panel_control: bool,
    #[serde(default)]
    reboot_supported: bool,
    #[serde(default)]
    control_trusted: bool,
    #[serde(default)]
    wireless_ota: bool,
    #[serde(default)]
    wireless_realtime: bool,
    #[serde(default)]
    rtc_key: u8,
    #[serde(default)]
    wireless_realtime_busy: Option<bool>,
    #[serde(default)]
    board_device_id: String,
}

// The retry here only applies to a read. A volume request is sent once.
fn read_with_recovery<T>(read: impl Fn(&Peer) -> io::Result<T>, error: &str) -> Result<(Peer, T), String> {
    let peer = read_peer()?.ok_or("请先配对 Wi-Fi 设备。")?;
    if let Ok(result) = read(&peer).or_else(|_| read(&peer)) {
        return Ok((peer, result));
    }
    recover_pairing(&peer).map_err(|_| error)?;
    let moved = read_peer()?.filter(|current| {
        current.address != peer.address && current.key.as_ref() == peer.key.as_ref()
    }).ok_or(error)?;
    let result = read(&moved).map_err(|_| error)?;
    Ok((moved, result))
}

fn query_status(peer: &Peer) -> io::Result<LanDeviceStatus> {
    // The single-byte request opts into extended fields. Released firmware
    // returns the original status only for an empty request.
    let reply = exchange_response(peer, DEVICE_STATUS, &[1], Some(DEVICE_STATUS_MAX))
        .or_else(|error| match error.kind() {
            io::ErrorKind::UnexpectedEof | io::ErrorKind::ConnectionReset | io::ErrorKind::BrokenPipe =>
                exchange_response(peer, DEVICE_STATUS, &[], Some(DEVICE_STATUS_MAX)),
            _ => Err(error),
        })?;
    serde_json::from_slice(&reply).map_err(|error| io::Error::new(io::ErrorKind::InvalidData, error))
}

#[tauri::command]
pub(crate) async fn lan_device_status(app: tauri::AppHandle) -> Result<PairedLanStatus, String> {
    tauri::async_runtime::spawn_blocking(move || {
        read_with_recovery(query_status, "无法读取 Wi-Fi 设备状态，请确认设备在线且固件支持无线状态查询。")
            .map(|(peer, device)| {
                let (version, rollback_blocked) = ota::bundled_wifi6_status(&app, &device.board_device_id);
                PairedLanStatus {
                    address: peer.address.ip().to_string(),
                    bundled_firmware_version: version,
                    bundled_firmware_rollback_blocked: rollback_blocked,
                    device,
                }
            })
    }).await.map_err(|_| "Wi-Fi 设备状态查询任务未完成。")?
}

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct PairedLanStatus {
    address: String,
    bundled_firmware_version: Option<&'static str>,
    bundled_firmware_rollback_blocked: bool,
    #[serde(flatten)]
    device: LanDeviceStatus,
}

#[tauri::command]
pub(crate) async fn lan_bluetooth_keyboard_config(
    usb_manager: tauri::State<'_, crate::usb_serial::UsbSerialManager>,
    board_device_id: String,
    expected_boot_id: String,
    enabled: Option<bool>,
    slot: Option<u8>,
    chord: Option<u16>,
) -> Result<Value, String> {
    let payload = crate::bluetooth_keyboard_payload(enabled, slot, chord)?;
    let reading = enabled.is_none() && slot.is_none();
    let usb = usb_manager.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        require_no_usb(&usb)?;
        let (peer, status) = read_with_recovery(query_status, "无法读取 Wi-Fi 设备状态。")?;
        if !status.control_trusted || !status.wifi_connected
            || status.board_device_id != board_device_id || status.boot_id != expected_boot_id
            || expected_boot_id.len() != 16 || !expected_boot_id.bytes().all(|b| b.is_ascii_hexdigit())
        {
            return Err("请确认当前 Wi-Fi 设备已配对并在线，再重新读取配置。".into());
        }
        let mut body = expected_boot_id.into_bytes();
        body.extend_from_slice(payload.to_string().as_bytes());
        require_no_usb(&usb)?;
        let bytes = exchange_response_with_timeout(&peer, KEYBOARD_CONFIG, &body,
            Some(KEYBOARD_CONFIG_REPLY_MAX), Duration::from_secs(5)).map_err(|_| {
                if reading { "无法读取 Wi-Fi 蓝牙快捷键配置，请确认设备在线且固件支持此功能。" }
                else { "Wi-Fi 未返回保存确认，请重新读取配置确认结果。" }
            })?;
        if bytes == [0] {
            return Err("设备未接受 Wi-Fi 配置请求，请确认 USB 已断开且设备空闲。".into());
        }
        let reply: Value = serde_json::from_slice(&bytes).map_err(|_| "设备返回的蓝牙配置无效。")?;
        if reply["boardDeviceId"].as_str() != Some(board_device_id.as_str()) {
            return Err("设备身份已变化，请重新读取配置。".into());
        }
        if reply["ok"].as_bool() != Some(true) {
            return Err(reply["message"].as_str().unwrap_or("设备未接受蓝牙配置。").into());
        }
        Ok(reply)
    }).await.map_err(|_| "Wi-Fi 蓝牙配置任务未完成。")?
}

#[derive(Clone, Copy, serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) enum LanPanelOperation {
    ReadInput = 1,
    WriteInput = 2,
    ReadVolume = 3,
    ReadDiagnostics = 4,
    ReturnHome = 5,
    ReadComponents = 6,
    ResetInput = 7,
    RemoveComponent = 8,
    EnableMicrophone = 9,
    DisableMicrophone = 10,
}

#[derive(serde::Deserialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct LanComponentRemoval {
    component_id: String,
    preserve_save: bool,
    clear_device_save_confirmed: bool,
}

#[tauri::command]
pub(crate) async fn lan_panel_control(
    usb_manager: tauri::State<'_, crate::usb_serial::UsbSerialManager>,
    board_device_id: String,
    expected_boot_id: String,
    operation: LanPanelOperation,
    bindings: Option<Vec<crate::ButtonConfigBinding>>,
    removal: Option<LanComponentRemoval>,
) -> Result<Value, String> {
    if expected_boot_id.len() != 16 || !expected_boot_id.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err("无法确认设备的启动身份，请重新连接。".into());
    }
    let mut body = expected_boot_id.clone().into_bytes();
    body.push(operation as u8);
    match (operation, bindings, removal) {
        (LanPanelOperation::WriteInput, Some(bindings), None) if (1..=19).contains(&bindings.len()) => {
            let rows = crate::normalize_button_config_bindings(bindings, true)?;
            body.extend_from_slice(json!({"bindings": rows}).to_string().as_bytes());
        }
        (LanPanelOperation::RemoveComponent, None, Some(removal))
            if component::valid_id(&removal.component_id, 47)
                && removal.preserve_save != removal.clear_device_save_confirmed =>
        {
            body.extend_from_slice(json!({
                "componentId": removal.component_id,
                "preserveSave": removal.preserve_save,
            }).to_string().as_bytes());
        }
        (LanPanelOperation::WriteInput | LanPanelOperation::RemoveComponent, _, _)
        | (_, Some(_), _) | (_, _, Some(_)) => return Err("设备操作参数无效。".into()),
        (_, None, None) => {}
    }
    if body.len() > PANEL_REQUEST_MAX {
        return Err("设备操作内容超过长度限制。".into());
    }
    let usb = usb_manager.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        require_no_usb(&usb)?;
        let (peer, status) = read_with_recovery(query_status, "无法读取 Wi-Fi 设备状态。")?;
        if !status.wifi_connected || status.board_device_id != board_device_id || status.boot_id != expected_boot_id {
            return Err("Wi-Fi 设备身份已变化，请重新读取状态。".into());
        }
        if !status.panel_control
            || (matches!(operation, LanPanelOperation::EnableMicrophone | LanPanelOperation::DisableMicrophone)
                && !status.wireless_voice_input)
        {
            return Err("当前设备固件暂不支持这项无线操作。".into());
        }
        if !status.control_trusted {
            return Err("当前设备尚未授权 Wi-Fi 控制。".into());
        }
        require_no_usb(&usb)?;
        let bytes = exchange_response_with_timeout(&peer, PANEL_CONTROL, &body,
            Some(MAX_BODY - 2), Duration::from_secs(10)).map_err(|_| "设备未返回确认，请重新读取当前状态。")?;
        if bytes == [0] {
            return Err("设备未接受无线请求，请检查连接和设备状态。".into());
        }
        let response: Value = serde_json::from_slice(&bytes).map_err(|_| "设备返回无效数据。")?;
        if response["boardDeviceId"].as_str() != Some(board_device_id.as_str()) {
            return Err("设备回报身份与当前连接不一致。".into());
        }
        if response["ok"].as_bool() != Some(true) {
            return Err(response["message"].as_str().unwrap_or("设备拒绝这项无线操作。").into());
        }
        Ok(response)
    }).await.map_err(|_| "无线设备操作任务未完成。")?
}

#[derive(serde::Serialize, PartialEq, Debug)]
#[serde(rename_all = "camelCase")]
pub(crate) enum LanVolumeResult {
    Unknown,
    Pending,
    Applying,
    Applied,
    Failed,
    Cancelled,
}

fn volume_result(reply: &[u8]) -> io::Result<LanVolumeResult> {
    match reply {
        [0] => Ok(LanVolumeResult::Unknown),
        [2] => Ok(LanVolumeResult::Pending),
        [3 | 4] => Ok(LanVolumeResult::Applying),
        [5] => Ok(LanVolumeResult::Applied),
        [6] => Ok(LanVolumeResult::Failed),
        [7] => Ok(LanVolumeResult::Cancelled),
        _ => Err(io::ErrorKind::InvalidData.into()),
    }
}

fn volume_request_body(id: [u8; 16], boot: &str, volume: u8) -> Result<[u8; 33], String> {
    if boot.len() != 16 || !boot.bytes().all(|b| b.is_ascii_hexdigit()) || volume > 100 {
        return Err("请确认设备启动 ID 及 0–100 的整数音量。".into());
    }
    let mut body = [0; 33];
    body[..16].copy_from_slice(boot.as_bytes());
    body[16..32].copy_from_slice(&id);
    body[32] = volume;
    Ok(body)
}

#[tauri::command]
pub(crate) async fn lan_volume_request(
    expected_boot_id: String,
    volume: u8,
    usb_manager: tauri::State<'_, crate::usb_serial::UsbSerialManager>,
) -> Result<([u8; 16], LanVolumeResult), String> {
    let usb = usb_manager.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        require_no_usb(&usb)?;
        let mut id = [0; 16];
        SystemRandom::new().fill(&mut id).map_err(|_| "无法生成音量请求。")?;
        let body = volume_request_body(id, &expected_boot_id, volume)?;
        let (peer, status) = read_with_recovery(query_status, "设备当前不可达，音量请求未发送。")?;
        if !status.trusted_volume || !status.control_trusted || status.boot_id != expected_boot_id {
            return Err("音量请求需要已授权的当前设备，请重新读取状态。".into());
        }
        require_no_usb(&usb)?;
        let result = match exchange_response(&peer, VOLUME_REQUEST, &body, Some(1)).as_deref() {
            Ok([0]) => return Err("设备未接受音量请求，请确认设备在线且空闲。".into()),
            Ok([3]) => LanVolumeResult::Applying,
            _ => LanVolumeResult::Unknown,
        };
        Ok((id, result))
    }).await.map_err(|_| "音量请求任务未完成。")?
}

#[tauri::command]
pub(crate) async fn lan_volume_result(id: [u8; 16]) -> Result<LanVolumeResult, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let (_, result) = read_with_recovery(|peer| {
            let reply = exchange_response(peer, VOLUME_RESULT, &id, Some(1))?;
            volume_result(&reply)
        }, "无法查询音量结果，请检查设备连接。")?;
        Ok(result)
    }).await.map_err(|_| "音量查询任务未完成。")?
}

fn read_before(stream: &mut TcpStream, mut buffer: &mut [u8], deadline: Instant) -> io::Result<()> {
    while !buffer.is_empty() {
        let timeout = deadline.saturating_duration_since(Instant::now());
        if timeout.is_zero() {
            return Err(io::ErrorKind::TimedOut.into());
        }
        stream.set_read_timeout(Some(timeout))?;
        let read = stream.read(buffer)?;
        if read == 0 {
            return Err(io::ErrorKind::UnexpectedEof.into());
        }
        buffer = &mut buffer[read..];
    }
    Ok(())
}

pub(crate) fn pairing_cli() -> Option<Result<(), String>> {
    let mut args = std::env::args_os().skip(1);
    match args.next()?.to_str()? {
        "--lan-agent-pair" => Some((|| {
            let ip = args
                .next()
                .and_then(|value| value.into_string().ok())
                .ok_or("Usage: --lan-agent-pair <device IPv4>; pass the code on stdin.")?;
            if args.next().is_some() {
                return Err("Pass the pairing code on stdin only.".into());
            }
            let mut code = Zeroizing::new(String::new());
            io::stdin()
                .lock()
                .take(96)
                .read_line(&mut code)
                .map_err(|_| "Cannot read the pairing code.")?;
            pair(&ip, &code).map(|_| ())
        })()),
        "--lan-agent-forget" => Some((|| {
            if args.next().is_some() {
                return Err("Usage: --lan-agent-forget".into());
            }
            forget()
        })()),
        _ => None,
    }
}

fn sha512(parts: &[&[u8]]) -> Zeroizing<[u8; 64]> {
    let mut hash = digest::Context::new(&digest::SHA512);
    for part in parts {
        hash.update(part);
    }
    let mut output = Zeroizing::new([0; 64]);
    output.copy_from_slice(hash.finish().as_ref());
    output
}

fn srp_client_secret(
    prime: &BigUint,
    private_a: &BigUint,
    public_a: &[u8],
    public_b: &[u8],
    salt: &[u8; 16],
    pin: &[u8],
) -> io::Result<(Zeroizing<[u8; 64]>, Zeroizing<[u8; 64]>)> {
    let generator = BigUint::from(5u8);
    let b = BigUint::from_bytes_be(public_b);
    if public_b.is_empty() || public_b.len() > SRP_BYTES || public_b[0] == 0 || &b >= prime {
        return Err(io::ErrorKind::InvalidData.into());
    }

    let prime_bytes = prime.to_bytes_be();
    let mut padded_g = [0; SRP_BYTES];
    padded_g[SRP_BYTES - 1] = 5;
    let padding = [0; SRP_BYTES];
    let k = BigUint::from_bytes_be(sha512(&[&prime_bytes, &padded_g]).as_ref());
    let u = BigUint::from_bytes_be(
        sha512(&[public_a, &padding[..SRP_BYTES - public_b.len()], public_b]).as_ref(),
    );
    if u == BigUint::from(0u8) {
        return Err(io::ErrorKind::InvalidData.into());
    }
    let password_hash = sha512(&[SRP_IDENTITY, b":", pin]);
    let x = Zeroizing::new(BigUint::from_bytes_be(
        sha512(&[salt, password_hash.as_ref()]).as_ref(),
    ));
    let verifier = Zeroizing::new(generator.modpow(&x, prime));
    let kv = Zeroizing::new((k * &*verifier) % prime);
    let base = Zeroizing::new((b + prime - &*kv) % prime);
    if *base == BigUint::from(0u8) {
        return Err(io::ErrorKind::InvalidData.into());
    }
    let exponent = Zeroizing::new(private_a + u * &*x);
    let shared = Zeroizing::new(base.modpow(&exponent, prime));
    let shared_bytes = Zeroizing::new(shared.to_bytes_be());
    let secret = sha512(&[&shared_bytes]);
    let mut group_hash = sha512(&[&prime_bytes]);
    for (byte, other) in group_hash.iter_mut().zip(sha512(&[&padded_g]).iter()) {
        *byte ^= *other;
    }
    let identity_hash = sha512(&[SRP_IDENTITY]);
    let proof = sha512(&[
        group_hash.as_ref(),
        identity_hash.as_ref(),
        salt,
        public_a,
        public_b,
        secret.as_ref(),
    ]);
    Ok((secret, proof))
}

fn pair_with_pin(address: SocketAddr, pin: &[u8]) -> io::Result<Peer> { pair_exchange(address,pin,false) }
fn pair_exchange(address: SocketAddr, pin: &[u8], confirm:bool) -> io::Result<Peer> {
    let prime = BigUint::parse_bytes(SRP_PRIME.as_bytes(), 16).expect("SRP group prime");
    let generator = BigUint::from(5u8);
    let rng = SystemRandom::new();
    let mut private = Zeroizing::new([0u8; 32]);
    let (private_a, public_a) = loop {
        rng.fill(private.as_mut())
            .map_err(|_| io::ErrorKind::Other)?;
        private[0] |= 0x80;
        let a = Zeroizing::new(BigUint::from_bytes_be(private.as_ref()));
        let public = generator.modpow(&a, &prime).to_bytes_be();
        if public.len() == SRP_BYTES && public[0] < 0x80 {
            break (a, public);
        }
    };
    let mut client_nonce = [0; 16];
    rng.fill(&mut client_nonce)
        .map_err(|_| io::ErrorKind::Other)?;
    let mut stream = TcpStream::connect_timeout(&address, IO_TIMEOUT)?;
    let deadline = Instant::now() + if confirm {Duration::from_secs(20)} else {PAIR_TIMEOUT};
    stream.set_write_timeout(Some(IO_TIMEOUT))?;
    stream.set_nodelay(true)?;
    stream.write_all(&client_nonce)?;
    let mut server_nonce = [0; 16];
    read_before(&mut stream, &mut server_nonce, deadline)?;
    let size = (SRP_BYTES as u16).to_be_bytes();
    stream.write_all(&[if confirm {5} else {PAIR_PIN}, size[0], size[1]])?;
    stream.write_all(&public_a)?;
    let mut salt = [0; 16];
    read_before(&mut stream, &mut salt, deadline)?;
    let mut length = [0; 2];
    read_before(&mut stream, &mut length, deadline)?;
    let length = u16::from_be_bytes(length) as usize;
    if !(1..=SRP_BYTES).contains(&length) {
        return Err(io::ErrorKind::InvalidData.into());
    }
    let mut public_b = [0; SRP_BYTES];
    read_before(&mut stream, &mut public_b[..length], deadline)?;
    let (secret, proof) = srp_client_secret(
        &prime,
        &private_a,
        &public_a,
        &public_b[..length],
        &salt,
        pin,
    )?;
    let master = hmac::Key::new(hmac::HMAC_SHA256, secret.as_ref());
    let mut transcript = hmac::Context::with_key(&master);
    transcript.update(b"P4LAN1");
    transcript.update(&client_nonce);
    transcript.update(&server_nonce);
    let mut key = Zeroizing::new([0; 16]);
    key.copy_from_slice(&transcript.sign().as_ref()[..16]);
    let peer = Peer { address, key };

    let mut response = Zeroizing::new([0; 64]);
    let result = stream
        .write_all(proof.as_ref())
        .and_then(|_| read_before(&mut stream, response.as_mut(), deadline));
    drop(stream);
    if result.is_ok() {
        let expected = sha512(&[&public_a, proof.as_ref(), secret.as_ref()]);
        // Use a constant-time comparison of the server's proof.
        #[allow(deprecated)]
        ring::constant_time::verify_slices_are_equal(expected.as_ref(), response.as_ref())
            .map_err(|_| io::ErrorKind::InvalidData)?;
    }
    // A lost acknowledgement may follow a successful device commit. Verify
    // the derived key with a read-only probe, without replaying the pairing.
    exchange(&peer, PROBE, &[])?;
    Ok(peer)
}

fn valid_pin(code: &str) -> bool {
    code.len() == 4 && code.bytes().all(|b| b.is_ascii_digit())
}

fn pair(ip: &str, code: &str) -> Result<String, String> {
    if ip.len() > 32 || code.len() > 96 {
        return Err("Invalid LAN pairing input.".into());
    }
    let code = code.trim();
    if !valid_pin(code) {
        return Err("Enter the four digits shown on the device.".into());
    }
    let address = parse_address(ip.trim())?;
    let path = config_path()?;
    crate::ensure_private_directory(path.parent().ok_or("Invalid pairing directory.")?)?;
    let peer = pair_with_pin(address, code.as_bytes())
        .map_err(|_| "Pairing failed. Generate a new code on the device before retrying.")?;
    save_pairing(&path, &peer)
}
static PAIRING_MUTATION: Mutex<()> = Mutex::new(());

fn save_pairing(path: &std::path::Path, peer: &Peer) -> Result<String, String> {
    let _guard = PAIRING_MUTATION.lock().map_err(|_| "配对记录繁忙")?;
    save_peer(path, peer)
}

fn save_peer(path:&std::path::Path, peer:&Peer)->Result<String,String>{
    let address=peer.address.ip().to_string();
    let mut config=Zeroizing::new(format!("{address}\n"));
    for byte in peer.key.iter(){use std::fmt::Write as _;write!(config,"{byte:02x}").expect("String");}
    crate::secure_config::write(path,PURPOSE,config.as_bytes())?;
    Ok(address)
}
static DISCOVERED: Mutex<Vec<(String,SocketAddr,Instant,bool)>>=Mutex::new(Vec::new());
fn scan_network(timeout: Duration) -> Result<Vec<Value>, String> {
    let socket=UdpSocket::bind((Ipv4Addr::UNSPECIFIED,0)).map_err(|_|"无法开启局域网发现")?;
    socket.set_broadcast(true).map_err(|_|"无法开启局域网发现")?;
    socket.set_read_timeout(Some(Duration::from_millis(200))).map_err(|_|"无法开启局域网发现")?;
    let nonce=uuid::Uuid::new_v4().simple().to_string();
    let request=format!("HACHI-DISCOVER-1 {nonce}");
    let networks=sysinfo::Networks::new_with_refreshed_list();
    let targets=discovery::broadcast_targets(networks.iter().flat_map(|(_,network)|network.ip_networks()).filter_map(|network|match network.addr {
        std::net::IpAddr::V4(ip)=>Some((ip,network.prefix)), _=>None,
    }));
    let mut results=Vec::new();let mut peers=Vec::new();let deadline=Instant::now()+timeout;
    let mut sent=false;let mut send_error=String::new();
    let mut last=Instant::now()-Duration::from_secs(1);
    while Instant::now()<deadline{
      if last.elapsed()>Duration::from_millis(700){
        // macOS may deny the first datagram while showing its permission prompt.
        // Give authorization time to complete instead of immediately failing.
        for target in &targets {
          match socket.send_to(request.as_bytes(),(*target,29462)) {
            Ok(_)=>sent=true,
            Err(error)=>send_error=error.to_string(),
          }
        }
        last=Instant::now();
      }
      let mut data=[0u8;512];if let Ok((n,from))=socket.recv_from(&mut data){
        let Ok(v)=serde_json::from_slice::<Value>(&data[..n]) else{continue};
        if v["protocol"]!=2 || v["nonce"]!=nonce || from.port()!=29462{continue}
        let Some(id)=v["id"].as_str().filter(|s|s.starts_with("p4-")&&s.len()==15&&s[3..].bytes().all(|b|b.is_ascii_hexdigit())) else{continue};
        let address=SocketAddr::new(from.ip(),PORT);
        if parse_address(&from.ip().to_string()).is_err() || peers.iter().any(|(_,a,_,_)|*a==address){continue}
        let automatic = v["pairing"] == "auto-unpaired-v1";
        let ready = automatic && v["ready"] == true;
        peers.push((id.to_owned(),address,Instant::now(),ready));
        results.push(json!({"id":id,"ip":from.ip().to_string(),"ready":ready,
          "supportsAutomatic":automatic,"paired":v["paired"].as_bool()}));
        if peers.len()>=32{break}
      }
    }
    if !sent {return Err(format!("无法发送设备发现请求：{send_error}。请允许客户端访问本地网络后重试。"));}
    Ok(results)
}
fn discover() -> Result<Vec<Value>, String> {
    let results = scan_network(Duration::from_secs(6))?;
    let now = Instant::now();
    *DISCOVERED.lock().map_err(|_| "设备发现繁忙")? = results.iter().filter_map(|result| {
        Some((
            result["id"].as_str()?.to_owned(),
            parse_address(result["ip"].as_str()?).ok()?,
            now,
            result["ready"] == true,
        ))
    }).collect();
    Ok(results)
}

fn find_paired_peer(
    previous: &Peer,
    candidates: impl IntoIterator<Item = SocketAddr>,
    mut authenticate: impl FnMut(&Peer) -> bool,
) -> Option<Peer> {
    candidates.into_iter().filter(|address| *address != previous.address).take(8).find_map(|address| {
        let candidate = Peer { address, key: previous.key.clone() };
        authenticate(&candidate).then_some(candidate)
    })
}

fn recover_pairing(previous: &Peer) -> Result<(), String> {
    let results = scan_network(Duration::from_millis(1700))?;
    let addresses = results.iter().filter(|item| item["paired"] != false)
        .filter_map(|item| parse_address(item["ip"].as_str()?).ok());
    let Some(recovered) = find_paired_peer(previous, addresses, |candidate| {
        exchange(candidate, PROBE, &[]).is_ok()
    }) else {
        return Ok(());
    };
    let _guard = PAIRING_MUTATION.lock().map_err(|_| "配对记录繁忙")?;
    if read_peer()?.is_some_and(|current| current.address == previous.address && current.key.as_ref() == previous.key.as_ref()) {
        save_peer(&config_path()?, &recovered)?;
    }
    Ok(())
}

#[tauri::command]
pub(crate) async fn lan_agent_discover()->Result<Vec<Value>,String>{tauri::async_runtime::spawn_blocking(discover).await.map_err(|_|"设备发现失败")?}
#[tauri::command]
pub(crate) async fn lan_agent_pair_confirm(
    app: tauri::AppHandle, id: String, ip: String,
) -> Result<String, String> {
    let address = DISCOVERED.lock().map_err(|_| "设备发现繁忙")?.iter()
        .find(|(board_id, address, seen, ready)| *ready && board_id == &id
            && address.ip().to_string() == ip && seen.elapsed() < Duration::from_secs(180))
        .map(|(_, address, _, _)| *address)
        .ok_or("设备发现已过期，请重新发现")?;
    let saved = tauri::async_runtime::spawn_blocking(move || {
        let path = config_path()?;
        crate::ensure_private_directory(path.parent().ok_or("配对目录无效")?)?;
        let _pairing = PAIRING_MUTATION.lock().map_err(|_| "配对记录繁忙")?;
        let peer = pair_exchange(address, b"Hachimodock-auto-v1", true)
            .map_err(|_| "配对未完成，请重新发现后重试。设备若已与其他电脑配对，请先在设备上解除配对。")?;
        let status = query_status(&peer).or_else(|_| query_status(&peer))
            .map_err(|_| "无法核对设备身份，请在设备上解除配对后重试。")?;
        if status.board_device_id != id || !status.wifi_connected {
            return Err("配对设备与所选设备不一致，原有本机配对已保留。".into());
        }
        save_peer(&path, &peer)
    }).await.map_err(|_| "配对任务失败")??;
    std::thread::spawn(move || { let _ = crate::ensure_bridge_runtime_blocking(app, None); });
    Ok(saved)
}

fn forget() -> Result<(), String> {
    let _guard = PAIRING_MUTATION.lock().map_err(|_| "配对记录繁忙")?;
    match std::fs::remove_file(config_path()?) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(_) => Err("Cannot remove the local LAN pairing.".into()),
    }
}

#[tauri::command]
pub(crate) async fn lan_agent_pair(
    app: tauri::AppHandle,
    ip: String,
    code: String,
) -> Result<String, String> {
    let code = Zeroizing::new(code);
    let address = tauri::async_runtime::spawn_blocking(move || pair(&ip, &code))
        .await
        .map_err(|_| "LAN pairing worker unavailable.")??;
    std::thread::spawn(move || {
        let _ = crate::ensure_bridge_runtime_blocking(app, None);
    });
    Ok(address)
}

#[tauri::command]
pub(crate) async fn lan_agent_forget() -> Result<(), String> {
    tauri::async_runtime::spawn_blocking(forget)
        .await
        .map_err(|_| "LAN pairing worker unavailable.")?
}

#[tauri::command]
pub(crate) async fn lan_agent_pair_status() -> Result<Option<String>, String> {
    tauri::async_runtime::spawn_blocking(|| {
        read_peer().map(|peer| peer.map(|peer| peer.address.ip().to_string()))
    })
    .await
    .map_err(|_| "LAN pairing worker unavailable.")?
}

fn read_peer() -> Result<Option<Peer>, String> {
    let Some(config) = crate::secure_config::read(&config_path()?, PURPOSE)? else {
        return Ok(None);
    };
    let config = Zeroizing::new(config);
    let (ip, code) = config.split_once('\n').ok_or("Invalid LAN pairing data.")?;
    parse_peer(ip, code).map(Some)
}

fn load_peer() -> Option<Peer> {
    read_peer().ok().flatten()
}

pub(crate) fn paired() -> bool {
    load_peer().is_some()
}

struct SessionSnapshot {
    value: Value,
    observed: Instant,
}
static SESSIONS: Mutex<Option<SessionSnapshot>> = Mutex::new(None);
// Order explicit follow changes with background presentation snapshots.
static FOLLOW: Mutex<()> = Mutex::new(());

fn clipped(value: &Value, bytes: usize) -> Value {
    let mut text = String::new();
    for c in value
        .as_str()
        .unwrap_or("")
        .chars()
        .filter(|c| !c.is_control() || *c == '\n' || *c == '\r' || *c == '\t')
    {
        if text.len() + c.len_utf8() > bytes {
            break;
        }
        text.push(c);
    }
    Value::String(text)
}

fn session_payload(input: &Value, agent: &str) -> Result<Value, String> {
    let rows = input["sessions"]
        .as_array()
        .filter(|rows| rows.len() <= 8)
        .ok_or("Invalid LAN session queue.")?;
    let active = input["activeSessionIds"]
        .as_array()
        .filter(|ids| ids.len() <= 8)
        .ok_or("Invalid LAN active sessions.")?;
    let sessions: Vec<_> = rows.iter().map(|row| json!({
        "id": clipped(&row["id"], 127),
        "title": clipped(&row["title"], 191),
        "content": clipped(&row["content"], 383),
        "state": clipped(&row["state"], 23),
        "transitionRevision": row["transitionRevision"].as_u64().unwrap_or(0).min(9_007_199_254_740_991),
        "terminalRemainingMs": row["terminalRemainingMs"].as_u64().unwrap_or(0).min(60_000),
    })).collect();
    Ok(json!({
        "agentId": agent,
        "sessionId": clipped(&input["sessionId"], 127),
        "title": clipped(&input["title"], 191),
        "index": input["index"].as_u64().unwrap_or(0).min(999),
        "count": input["count"].as_u64().unwrap_or(0).min(999),
        "displayEnabled": input["displayEnabled"].as_bool().unwrap_or(false),
        "activeSessionIds": active.iter().map(|id| clipped(id, 127)).collect::<Vec<_>>(),
        "sessions": sessions,
    }))
}

#[tauri::command]
pub(crate) fn lan_agent_sessions(input: Value) -> Result<(), String> {
    let agent = input["agentId"]
        .as_str()
        .and_then(crate::normalize_agent_id)
        .ok_or("Invalid LAN Agent.")?;
    let value = session_payload(&input, &agent)?;
    *SESSIONS
        .lock()
        .map_err(|_| "LAN session queue unavailable.")? = Some(SessionSnapshot {
        value,
        observed: Instant::now(),
    });
    Ok(())
}

fn current_sessions(agent: &str) -> Option<Value> {
    let snapshot = SESSIONS.lock().ok()?;
    let snapshot = snapshot.as_ref()?;
    let age = snapshot.observed.elapsed();
    if age > SNAPSHOT_MAX_AGE || snapshot.value["agentId"] != agent {
        return None;
    }
    let mut value = snapshot.value.clone();
    for row in value["sessions"].as_array_mut()? {
        let remaining = row["terminalRemainingMs"].as_u64().unwrap_or(0);
        row["terminalRemainingMs"] = json!(remaining.saturating_sub(age.as_millis() as u64));
    }
    Some(value)
}

#[tauri::command]
pub(crate) async fn lan_follow_agent(
    board_device_id: String,
    boot_id: String,
    agent_id: String,
    usb_manager: tauri::State<'_, crate::usb_serial::UsbSerialManager>,
) -> Result<(), String> {
    let usb = usb_manager.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let agent = crate::normalize_agent_id(&agent_id).ok_or("目标 Agent 无效。")?;
        require_no_usb(&usb)?;
        let _follow = FOLLOW.lock().map_err(|_| "切换跟随暂不可用。")?;
        let (peer, status) = read_with_recovery(query_status, "无法确认已配对的 Wi-Fi 设备。")?;
        if board_device_id.is_empty() || boot_id.is_empty() || !status.wifi_connected
            || status.board_device_id != board_device_id || status.boot_id != boot_id
        {
            return Err("设备或启动状态已变化，本次未切换跟随。".into());
        }
        let _pairing = PAIRING_MUTATION.lock().map_err(|_| "配对记录繁忙")?;
        require_no_usb(&usb)?;
        if !load_peer().is_some_and(|current| current.address == peer.address
            && current.key.as_ref() == peer.key.as_ref())
        {
            return Err("Wi-Fi 配对已变化，本次未切换跟随。".into());
        }
        let sessions = current_sessions(&agent).unwrap_or_else(|| json!({
            "agentId": agent, "sessionId": "auto", "sessions": [],
            "activeSessionIds": [], "displayEnabled": false,
        }));
        let body = serde_json::to_vec(&json!({
            "source": agent, "sessions": sessions, "status": {"state": "idle"},
        }))
            .map_err(|_| "无法准备 Wi-Fi 跟随指令。")?;
        exchange(&peer, SNAPSHOT, &body).map_err(|_| "Wi-Fi 设备未确认本次切换，请检查连接。")?;
        crate::save_agent_selection(crate::AgentSelectionInput {
            enabled_agents: vec![agent.clone()],
            selected_agent_id: Some(agent),
        }).map_err(|_| "设备已收到指令，桌面跟随配置保存失败。")?;
        Ok(())
    }).await.map_err(|_| "Wi-Fi 跟随任务未完成。")?
}

fn status_payload(input: &Value) -> Value {
    let mut result = Map::new();
    for (key, max) in [
        ("state", 31),
        ("status", 31),
        ("event", 95),
        ("statusText", 95),
        ("sessionId", 127),
        ("sessionTitle", 191),
        ("displayTitle", 191),
        ("displayContent", 383),
        ("text", 383),
        ("message", 383),
        ("title", 191),
    ] {
        if input[key].is_string() {
            result.insert(key.to_string(), clipped(&input[key], max));
        }
    }
    if let Some(ts) = input["tsMs"].as_u64() {
        result.insert("tsMs".into(), json!(ts));
    }
    Value::Object(result)
}

fn matches_session(sessions: Option<&Value>, payload: &Value) -> bool {
    let Some(target) = sessions.and_then(|snapshot| snapshot["sessionId"].as_str()) else {
        return true;
    };
    target.is_empty() || target == "auto" || payload["sessionId"].as_str() == Some(target)
}

fn current_speech(source: &str, sessions: Option<&Value>, now_ms: u64) -> Option<Value> {
    let directory = std::env::temp_dir().join("pet-manager-bridge-speech");
    crate::recent_bridge_json_paths(&directory, now_ms)
        .into_iter()
        .filter(|path| {
            path.file_stem()
                .and_then(|stem| stem.to_str())
                .is_some_and(|stem| crate::usb_state_source_from_file_stem(stem) == source)
        })
        .filter_map(|path| {
            let raw = std::fs::File::open(path).ok()?;
            let mut bytes = Vec::new();
            raw.take(MAX_BODY as u64 + 1).read_to_end(&mut bytes).ok()?;
            if bytes.len() > MAX_BODY {
                return None;
            }
            let payload: Value = serde_json::from_slice(&bytes).ok()?;
            let ts = payload["tsMs"].as_u64()?;
            let expires = payload["expiresAtMs"]
                .as_u64()
                .unwrap_or(ts.saturating_add(STATUS_MAX_AGE_MS));
            (ts <= now_ms
                && now_ms - ts <= STATUS_MAX_AGE_MS
                && now_ms < expires
                && matches_session(sessions, &payload))
            .then_some((ts, payload))
        })
        .max_by_key(|(ts, _)| *ts)
        .map(|(_, payload)| status_payload(&payload))
}

pub(crate) fn require_no_usb(usb: &crate::usb_serial::UsbSerialManager) -> Result<(), String> {
    if usb.status().connected {
        return Err("USB 已连接，请使用 USB 控制当前设备。".into());
    }
    Ok(())
}

pub(crate) fn start(app: tauri::AppHandle, usb: crate::usb_serial::UsbSerialManager) {
    voice::start(app.clone(), usb.clone());
    std::thread::spawn(move || {
        let mut presses = press::Poller::default();
        let mut last_sent: Option<(SocketAddr, Value, Instant)> = None;
        let mut next_probe = Instant::now();
        let mut next_discovery = Instant::now();
        let mut failures = 0u8;
        let mut last_address = None;
        loop {
            std::thread::sleep(Duration::from_millis(800));
            if usb.status().connected {
                presses.reset();
                bubble_pixels::reset();
                last_sent = None;
                continue;
            }
            let Some(peer) = load_peer() else {
                presses.reset();
                bubble_pixels::reset();
                last_address = None;
                last_sent = None;
                continue;
            };
            let press_polled = presses.tick(&app, &usb, &peer);
            let Ok(_follow) = FOLLOW.try_lock() else {
                continue;
            };
            if usb.status().connected {
                last_sent = None;
                continue;
            }
            if last_address != Some(peer.address) {
                last_address = Some(peer.address);
                failures = 0;
                next_probe = Instant::now();
                next_discovery = next_probe;
            }
            let probe_due = Instant::now() >= next_probe;
            let mut connected = |success| {
                if success {
                    failures = 0;
                } else {
                    failures = failures.saturating_add(1);
                    if failures >= 2 && Instant::now() >= next_discovery {
                        let _ = recover_pairing(&peer);
                        next_discovery = Instant::now() + REDISCOVER_INTERVAL;
                    }
                }
                next_probe = Instant::now() + PROBE_INTERVAL;
            };
            let source = crate::resolve_usb_inject_agent_id();
            let sessions = current_sessions(&source);
            let now_ms = crate::current_timestamp_ms();
            let best = crate::pick_best_bridge_state(false, |candidate, payload| {
                candidate == source
                    && matches_session(sessions.as_ref(), payload)
                    && payload["tsMs"]
                        .as_u64()
                        .is_some_and(|ts| ts <= now_ms && now_ms - ts <= STATUS_MAX_AGE_MS)
            });
            let mut body = Map::new();
            if let Some((_, status)) = best {
                body.insert("status".into(), status_payload(&status));
            }
            if let Some(speech) = current_speech(&source, sessions.as_ref(), now_ms) {
                body.insert("speech".into(), speech);
            }
            if let Some(sessions) = sessions {
                body.insert("sessions".into(), sessions);
            }
            if body.is_empty() {
                bubble_pixels::reset();
                last_sent = None;
                if probe_due {
                    connected(press_polled || exchange(&peer, PROBE, &[]).is_ok());
                }
                continue;
            }
            body.insert("source".into(), json!(source));
            let mut body = Value::Object(body);
            bubble_pixels::prepare(&peer, &usb, &mut body);
            let mut signature = body.clone();
            if let Some(rows) = signature
                .pointer_mut("/sessions/sessions")
                .and_then(Value::as_array_mut)
            {
                for row in rows {
                    row.as_object_mut()
                        .map(|row| row.remove("terminalRemainingMs"));
                }
            }
            if last_sent.as_ref().is_some_and(|(address, previous, at)| {
                *address == peer.address
                    && *previous == signature
                    && at.elapsed() < Duration::from_secs(4)
            }) {
                continue;
            }
            if let Ok(payload) = serde_json::to_vec(&body) {
                let success = exchange(&peer, SNAPSHOT, &payload).is_ok();
                connected(success);
                if success {
                    bubble_pixels::sent(&peer, &body);
                    last_sent = Some((peer.address, signature, Instant::now()));
                }
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(data: &[u8]) -> String {
        data.iter().map(|byte| format!("{byte:02x}")).collect()
    }

    #[test]
    fn volume_wire_is_bounded_and_distinguishes_acceptance_from_readback() {
        let id = [42; 16];
        let body = volume_request_body(id, "0123456789abcdef", 100).unwrap();
        assert_eq!(&body[..16], b"0123456789abcdef");
        assert_eq!(&body[16..32], &id);
        assert_eq!(body[32], 100);
        assert!(volume_request_body(id, " 1020304", 0).is_err());
        assert!(volume_request_body(id, "0123456789abcdef", 101).is_err());
        assert_eq!(volume_result(&[2]).unwrap(), LanVolumeResult::Pending);
        assert_eq!(volume_result(&[3]).unwrap(), LanVolumeResult::Applying);
        assert_eq!(volume_result(&[5]).unwrap(), LanVolumeResult::Applied);
        assert_eq!(volume_result(&[7]).unwrap(), LanVolumeResult::Cancelled);
        assert!(volume_result(&[1]).is_err());
        assert!(volume_result(&[5, 0]).is_err());
    }

    #[test]
    fn confirmation_exchange_keeps_existing_credentials_on_early_disconnect() {
        use std::net::TcpListener;
        let listener=TcpListener::bind("127.0.0.1:0").unwrap();let address=listener.local_addr().unwrap();
        let thread=std::thread::spawn(move||{let (mut stream,_)=listener.accept().unwrap();let mut nonce=[0;16];stream.read_exact(&mut nonce).unwrap();stream.write_all(&[1;16]).unwrap();let mut header=[0;3];stream.read_exact(&mut header).unwrap();assert_eq!(header,[5,1,128]);let mut public=[0;384];stream.read_exact(&mut public).unwrap();});
        assert!(pair_exchange(address,b"Hachimodock-auto-v1",true).is_err());thread.join().unwrap();
        // pair_exchange cannot persist anything: only a successful proof and
        // authenticated read-only probe return a Peer to save_peer.
    }
    #[test]
    fn p4lan1_known_answer_and_replay_rejection() {
        let master: [u8; 16] = std::array::from_fn(|i| i as u8);
        let client = [0x11; 16];
        let server = [0x22; 16];
        let key = session_key(&master, &client, &server);
        let mut request = br#"{"source":"codex","status":{"state":"working"}}"#.to_vec();
        let header = [SNAPSHOT, 0, request.len() as u8];
        key.seal_in_place_append_tag(nonce(0), aead::Aad::from(header), &mut request)
            .unwrap();
        assert_eq!(hex(&request),
            "8f0b825356e46c298ad8cd7b32907b4aa579e20a8766656267384a9456fd406cb018f72e2b236de9cee0136df12f1b1774745801a2fd222f17ce07898c1d07");
        let mut ack = Vec::new();
        key.seal_in_place_append_tag(nonce(1), aead::Aad::from(header), &mut ack)
            .unwrap();
        assert_eq!(hex(&ack), "c1638d062f619d9be75f4aa4f657c48e");
        assert!(session_key(&master, &client, &[0x33; 16])
            .open_in_place(nonce(0), aead::Aad::from(header), &mut request.clone())
            .is_err());
        assert!(session_key(&master, &[0x33; 16], &server)
            .open_in_place(nonce(0), aead::Aad::from(header), &mut request.clone())
            .is_err());
        assert!(key
            .open_in_place(nonce(0), aead::Aad::from(header), &mut ack.clone())
            .is_err());
        let mut wrong_header = header;
        wrong_header[0] = PAIR_PIN;
        assert!(key
            .open_in_place(nonce(1), aead::Aad::from(wrong_header), &mut ack.clone())
            .is_err());
        assert!(key
            .open_in_place(nonce(1), aead::Aad::from(header), &mut ack)
            .unwrap()
            .is_empty());
    }

    #[test]
    fn local_pairing_code_is_exact_and_rejects_invalid_destinations() {
        let code = "0001 0203 0405 0607\n0809 0a0b 0c0d 0e0f";
        let peer = parse_peer("192.0.2.12", code).unwrap();
        assert_eq!(peer.address, ([192, 0, 2, 12], PORT).into());
        assert_eq!(hex(peer.key.as_ref()), "000102030405060708090a0b0c0d0e0f");
        for address in [
            "localhost",
            "127.0.0.1",
            "0.0.0.0",
            "224.0.0.1",
            "255.255.255.255",
        ] {
            assert!(parse_peer(address, code).is_err());
        }
        assert!(parse_peer("192.0.2.12", "123456").is_err());
    }

    #[test]
    fn session_wire_preserves_revision_and_caps_presentation() {
        let row = json!({
            "id": "task-id",
            "title": "名".repeat(90),
            "content": "\\\"".repeat(240),
            "cwd": "/private/worktree",
            "state": "done",
            "transitionRevision": 892381u64,
            "terminalRemainingMs": 90000,
        });
        let input = json!({
            "agentId": "claude-code", "sessionId": "task-id", "sessions": vec![row; 8],
            "displayEnabled": true, "activeSessionIds": [],
        });
        let payload = session_payload(&input, "codex").unwrap();
        assert_eq!(payload["agentId"], "codex");
        assert_eq!(payload["sessions"][0]["transitionRevision"], 892381);
        assert_eq!(payload["sessions"][0]["terminalRemainingMs"], 60000);
        assert_eq!(payload["sessions"][0]["title"].as_str().unwrap().len(), 189);
        assert!(payload["sessions"][0].get("cwd").is_none());
        assert!(matches_session(
            Some(&payload),
            &json!({"sessionId": "task-id"})
        ));
        assert!(!matches_session(
            Some(&payload),
            &json!({"sessionId": "other"})
        ));
        assert!(serde_json::to_vec(&payload).unwrap().len() < MAX_BODY);
    }
}
