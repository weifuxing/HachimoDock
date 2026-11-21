use super::{
    exchange_response_with_timeout, find_paired_peer, parse_address, query_status,
    read_with_recovery, scan_network, LanDeviceStatus, Peer,
};
use std::time::{Duration, Instant};

const MODE: u8 = 10;
const CONFIRM_TIMEOUT: Duration = Duration::from_secs(4);
const OBSERVATION_WINDOW: Duration = Duration::from_secs(35);

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct LanRebootResult {
    scheduled: bool,
    observed: Option<LanDeviceStatus>,
}

fn valid_boot_id(id: &str) -> bool {
    id.len() == 16 && id.bytes().all(|byte| byte.is_ascii_hexdigit())
}

fn target_matches(status: &LanDeviceStatus, expected_boot_id: &str) -> bool {
    status.wifi_connected
        && status.reboot_supported
        && status.control_trusted
        && valid_boot_id(expected_boot_id)
        && status.boot_id == expected_boot_id
}

fn is_new_boot(status: &LanDeviceStatus, old_boot_id: &str) -> bool {
    status.wifi_connected && valid_boot_id(&status.boot_id) && status.boot_id != old_boot_id
}

fn observe_new_boot(peer: &Peer, old_boot_id: &str) -> Option<LanDeviceStatus> {
    let deadline = Instant::now() + OBSERVATION_WINDOW;
    let mut next_scan = Instant::now() + Duration::from_secs(3);
    while Instant::now() < deadline {
        match query_status(peer) {
            Ok(status) if is_new_boot(&status, old_boot_id) => return Some(status),
            Ok(_) => {}
            Err(_) if Instant::now() >= next_scan && Instant::now() < deadline => {
                // Probe candidates only with the original key. The verification
                // changes neither the peer we sent to nor the saved pairing.
                if let Ok(results) = scan_network(Duration::from_millis(1700)) {
                    let addresses = results
                        .iter()
                        .filter(|item| item["paired"] != false)
                        .filter_map(|item| parse_address(item["ip"].as_str()?).ok());
                    let mut observed = None;
                    find_paired_peer(peer, addresses, |candidate| {
                        if Instant::now() >= deadline {
                            return false;
                        }
                        observed = query_status(candidate)
                            .ok()
                            .filter(|status| is_new_boot(status, old_boot_id));
                        observed.is_some()
                    });
                    if observed.is_some() {
                        return observed;
                    }
                }
                next_scan = Instant::now() + Duration::from_secs(5);
            }
            Err(_) => {}
        }
        std::thread::sleep(Duration::from_millis(700));
    }
    None
}

#[tauri::command]
pub(crate) async fn lan_reboot(
    expected_boot_id: String,
    usb_manager: tauri::State<'_, crate::usb_serial::UsbSerialManager>,
) -> Result<LanRebootResult, String> {
    let usb = usb_manager.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        super::require_no_usb(&usb)?;
        let (peer, status) = read_with_recovery(query_status, "设备当前不可达，重启请求未发送。")?;
        if !target_matches(&status, &expected_boot_id) {
            return Err("目标启动 ID 或重启能力已变化，请重新读取设备状态。".into());
        }
        super::require_no_usb(&usb)?;
        let response = exchange_response_with_timeout(
            &peer,
            MODE,
            expected_boot_id.as_bytes(),
            Some(1),
            CONFIRM_TIMEOUT,
        );
        let scheduled = match response.as_deref() {
            Ok([0]) => return Err("设备未安排重启。请确认 USB 授权及设备是否空闲。".into()),
            Ok([1]) => true,
            _ => false,
        };
        Ok(LanRebootResult {
            scheduled,
            observed: observe_new_boot(&peer, &expected_boot_id),
        })
    })
    .await
    .map_err(|_| "重启结果未确认，请重新读取设备状态。")?
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn authorization_and_verification_use_the_exact_boot() {
        let original = "0123456789abcdef";
        let mut status = LanDeviceStatus {
            firmware: "test".into(),
            boot_id: original.into(),
            wifi_connected: true,
            clock_synchronized: false,
            https_successes: 0,
            https_failures: 0,
            trusted_volume: false,
            component_delivery: false,
            component_bundle: false,
            appearance_capacity: 0,
            widget_data_delivery: false,
            wireless_voice_input: false,
            widget_credential_storage_ready: false,
            input_double_press: false,
            system_settings: None,
            panel_control: false,
            reboot_supported: true,
            control_trusted: true,
            wireless_ota: false,
            wireless_realtime: false,
            rtc_key: 0,
            wireless_realtime_busy: None,
            board_device_id: "test".into(),
        };
        assert!(target_matches(&status, original));
        assert!(!is_new_boot(&status, original));
        status.boot_id = "1123456789abcdef".into();
        assert!(!target_matches(&status, original));
        assert!(is_new_boot(&status, original));
        status.wifi_connected = false;
        assert!(!is_new_boot(&status, original));
        status.wifi_connected = true;
        status.boot_id.clear();
        assert!(!is_new_boot(&status, original));
    }
}
