//! Shared publication route for already projected widget/data snapshots.
use super::{exchange_response, load_peer, query_status, require_no_usb, Peer};
use crate::usb_serial::UsbSerialManager;
use serde_json::Value;
use std::{
    sync::{Mutex, RwLock, RwLockReadGuard, RwLockWriteGuard},
    time::{Duration, Instant},
};

const MODE: u8 = 19;
// Includes the fixed identity prefix and worst-case escaping for 20 widget rows.
const BODY_MAX: usize = 10 * 1024;
const STATUS_INTERVAL: Duration = Duration::from_secs(4);

#[derive(Clone, PartialEq, Eq)]
pub(crate) enum Route {
    Usb { board: String, connection: u64 },
    Wifi { board: String, boot: String },
}

pub(crate) struct Target {
    pub(crate) route: Route,
    wifi: Option<Peer>,
    expiry: bool,
    device_watchlist: bool,
}

struct CheckedPeer {
    peer: Peer,
    at: Instant,
    route: Option<Route>,
}

static STATUS: Mutex<Option<CheckedPeer>> = Mutex::new(None);
// Foreground operations can retain their existing concurrency. Each background
// exchange needs the lane to itself and yields immediately if it is occupied.
static LAN_LANE: RwLock<()> = RwLock::new(());

pub(super) fn foreground() -> Result<RwLockReadGuard<'static, ()>, String> {
    LAN_LANE.read().map_err(|_| "无线传输暂不可用".into())
}

fn background(usb: &UsbSerialManager) -> Option<RwLockWriteGuard<'static, ()>> {
    let lane = LAN_LANE.try_write().ok()?;
    require_no_usb(usb).ok()?;
    if usb.transfer_in_progress()
        || crate::component_audio::voice_busy()
        || crate::component_audio::active_for_exit()
    {
        return None;
    }
    Some(lane)
}

fn same_peer(a: &Peer, b: &Peer) -> bool {
    a.address == b.address && a.key.as_ref() == b.key.as_ref()
}

fn valid_identity(board: &str, boot: &str) -> bool {
    boot.len() == 16
        && boot.bytes().all(|b| b.is_ascii_hexdigit())
        && board
            .strip_prefix("p4-")
            .is_some_and(|mac| mac.len() == 12 && mac.bytes().all(|b| b.is_ascii_hexdigit()))
}

impl Target {
    pub(crate) fn current(usb: &UsbSerialManager) -> Option<Self> {
        let status = usb.status();
        if status.connected {
            let connection = status.connection_id.parse::<u64>().ok()?;
            return (connection != 0
                && !status.board_device_id.is_empty()
                && status.capabilities["widgetData"] == crate::widget_data::PROTOCOL)
                .then(|| Self {
                    route: Route::Usb {
                        board: status.board_device_id,
                        connection,
                    },
                    wifi: None,
                    expiry: status.capabilities["widgetDataExpiry"] == "p4-data-expiry-v1",
                    device_watchlist: status.capabilities["widgetWifiData"] == "p4-wifi-data-v1",
                });
        }

        let _lane = background(usb)?;
        let peer = load_peer()?;
        let mut cached = STATUS.lock().ok()?;
        if cached
            .as_ref()
            .is_none_or(|old| !same_peer(&old.peer, &peer) || old.at.elapsed() >= STATUS_INTERVAL)
        {
            let route = query_status(&peer).ok().and_then(|status| {
                (status.wifi_connected
                    && status.control_trusted
                    && status.widget_data_delivery
                    && status.wireless_realtime_busy != Some(true)
                    && valid_identity(&status.board_device_id, &status.boot_id))
                .then_some(Route::Wifi {
                    board: status.board_device_id,
                    boot: status.boot_id,
                })
            });
            *cached = Some(CheckedPeer {
                peer: peer.clone(),
                at: Instant::now(),
                route,
            });
        }
        let route = cached.as_ref()?.route.clone()?;
        Some(Self {
            route,
            wifi: Some(peer),
            expiry: true,
            device_watchlist: true,
        })
    }

    pub(crate) fn supports_expiry(&self) -> bool {
        self.expiry
    }

    pub(crate) fn supports_device_watchlist(&self) -> bool {
        self.device_watchlist
    }

    pub(crate) fn publish(&self, usb: &UsbSerialManager, payload: &Value) -> Result<(), String> {
        match &self.route {
            Route::Usb { board, connection } => {
                usb.send_widget_data_scoped(board, Some(*connection), payload)
            }
            Route::Wifi { board, boot } => {
                let body = request_body(board, boot, payload).ok_or("组件数据超过无线传输限制")?;
                let reply = self.exchange(usb, &body, 1)?;
                (reply == [1])
                    .then_some(())
                    .ok_or_else(|| "设备未接收组件数据".into())
            }
        }
    }

    fn exchange(&self, usb: &UsbSerialManager, body: &[u8], max: usize) -> Result<Vec<u8>, String> {
        let _lane = background(usb).ok_or("无线组件数据传输忙")?;
        let peer = self.wifi.as_ref().ok_or("无线组件数据未连接")?;
        let current = load_peer().ok_or("无线配对已变化")?;
        if !same_peer(&current, peer) {
            return Err("无线配对已变化".into());
        }
        require_no_usb(usb)?;
        exchange_response(peer, MODE, body, Some(max)).map_err(|_| "无线组件数据未获确认".into())
    }
}

fn request_body(board: &str, boot: &str, payload: &Value) -> Option<Vec<u8>> {
    let mut body = boot.as_bytes().to_vec();
    body.extend_from_slice(board.as_bytes());
    serde_json::to_writer(&mut body, payload).ok()?;
    (body.len() <= BODY_MAX).then_some(body)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn request_preserves_projected_snapshot_schema_and_ttl() {
        let board = "p4-001122334455";
        let boot = "0123456789abcdef";
        assert!(valid_identity(board, boot));
        assert!(!valid_identity("p4-00112233445x", boot));
        assert!(!valid_identity(board, "bad-boot"));
        for source in [
            "computer.status",
            "todos.upcoming",
            "location.current",
            "github.me",
            "stocks.watchlist",
            "fetch.example",
        ] {
            let payload = json!({
                "schema": 1, "source": source, "rows": [], "ttlMs": 30000,
                "status": "empty", "message": "", "clearOnExpire": true,
            });
            let body = request_body(board, boot, &payload).unwrap();
            assert_eq!(&body[..16], boot.as_bytes());
            assert_eq!(&body[16..31], board.as_bytes());
            assert_eq!(
                serde_json::from_slice::<Value>(&body[31..]).unwrap(),
                payload
            );
        }
        assert!(request_body(board, boot, &json!({"message": "x".repeat(BODY_MAX)})).is_none());
    }

    #[test]
    fn body_budget_covers_a_full_list_with_escaped_values() {
        let rows: Vec<_> = (0..20)
            .map(|i| {
                json!({
                    "id": format!("{i:02}{}", "\\".repeat(21)),
                    "label": "\\".repeat(36), "value": "\\".repeat(24),
                    "detail": "\\".repeat(24), "meta": "\\".repeat(63), "tone": -1,
                })
            })
            .collect();
        let payload = json!({
            "schema": 1, "source": format!("fetch.{}", "a".repeat(41)),
            "rows": rows, "ttlMs": 300000, "status": "error", "date": "2026-11-19",
            "message": "\\".repeat(90), "clearOnExpire": true,
        });
        assert!(request_body("p4-001122334455", "0123456789abcdef", &payload).is_some());
    }
}
