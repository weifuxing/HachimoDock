use super::{exchange_link, query_status, read_with_recovery, retry_uncommitted_offer, Peer};
use std::{net::TcpStream, time::Duration};
use zeroize::Zeroizing;

pub(super) const MODE: u8 = 13;

#[derive(Clone)]
pub(crate) struct AudioTarget {
    peer: Peer,
    boot: String,
    board_id: String,
    hud: bool,
}

impl AudioTarget {
    pub(crate) fn find(expected_board_id: &str) -> Result<Self, String> {
        let (peer, status) = read_with_recovery(query_status, "无法读取已配对的 Wi-Fi 设备。")?;
        Self::from_status(peer, status, expected_board_id)
    }

    pub(super) fn from_status(peer: Peer, status: super::LanDeviceStatus, expected_board_id: &str) -> Result<Self, String> {
        if status.rtc_key & 2 != 0 || !status.wifi_connected
            || !status.wireless_realtime
            || !status.control_trusted
            || status.boot_id.len() != 16
            || !status.boot_id.bytes().all(|byte| byte.is_ascii_hexdigit())
            || status.board_device_id.len() != 15
            || (!expected_board_id.is_empty() && status.board_device_id != expected_board_id)
        {
            return Err("当前配对设备尚未开放 Wi-Fi 实时对话，请检查设备连接和固件版本。".into());
        }
        Ok(Self {
            peer,
            boot: status.boot_id,
            board_id: status.board_device_id,
            hud: status.rtc_key & 4 != 0,
        })
    }

    // The key and address come from the accepted press, never a fresh pairing lookup.
    pub(super) fn from_bound(peer: Peer, status: super::LanDeviceStatus, board: &str, boot: &str) -> Result<Self, String> {
        let target = Self::from_status(peer, status, board)?;
        if !target.boot_matches(boot) { return Err("Wi-Fi 设备已重启，请重新长按开始".into()); }
        target.ensure_current_pair()?;
        Ok(target)
    }

    pub(super) fn matches_peer(&self, peer: Option<&Peer>) -> bool {
        peer.is_some_and(|peer| peer.address == self.peer.address && peer.key.as_ref() == self.peer.key.as_ref())
    }

    pub(crate) fn ensure_current_pair(&self) -> Result<(), String> {
        if self.matches_peer(super::load_peer().as_ref()) { Ok(()) }
        else { Err("Wi-Fi 配对已变化，请重新长按开始".into()) }
    }

    pub(crate) fn is_idle(&self) -> bool {
        let validate = |status: super::LanDeviceStatus| {
            status.control_trusted
                && status.board_device_id == self.board_id
                && status.wireless_realtime
                && status.wireless_realtime_busy == Some(false)
        };
        if let Ok(status) = query_status(&self.peer) {
            return validate(status);
        }
        read_with_recovery(query_status, "无法核验设备 Wi-Fi 录音状态。").is_ok_and(
            |(peer, status)| peer.key.as_ref() == self.peer.key.as_ref() && validate(status),
        )
    }

    pub(crate) fn boot_matches(&self, boot: &str) -> bool { self.boot == boot }

    pub(crate) fn hud_supported(&self) -> bool { self.hud }

    pub(crate) fn board_id(&self) -> &str {
        &self.board_id
    }

    pub(super) fn boot_id(&self) -> &str { &self.boot }

    pub(crate) fn open(&self) -> Result<(TcpStream, Zeroizing<[u8; 16]>), String> {
        self.open_audio(self.boot.as_bytes(), false)
    }

    pub(super) fn open_voice(&self, token: u32) -> Result<(TcpStream, Zeroizing<[u8; 16]>), String> {
        let mut offer = [0; 20];
        offer[..16].copy_from_slice(self.boot.as_bytes());
        offer[16..].copy_from_slice(&token.to_be_bytes());
        self.open_audio(&offer, true)
    }

    fn open_audio(&self, body: &[u8], voice: bool) -> Result<(TcpStream, Zeroizing<[u8; 16]>), String> {
        // An admission offer never starts capture. Authenticate a fresh
        // socket once more if the first TCP handshake ended before admission.
        let offer = || {
            self.ensure_current_pair().map_err(|_| std::io::Error::new(std::io::ErrorKind::PermissionDenied, "Wi-Fi pairing changed"))?;
            exchange_link(
                &self.peer,
                MODE,
                body,
                Some(1),
                Duration::from_millis(2200),
            )
        };
        let session = retry_uncommitted_offer(offer)
            .map_err(|_| "无法建立设备 Wi-Fi 音频连接。".to_string())?;
        self.ensure_current_pair()?;
        if session.reply != [1] {
            return Err(if voice { "设备麦克风当前不可用，请重新按住说话。" }
                else { "设备音频当前不可用，请检查 Wi-Fi 后重新进入实时对话。" }.into());
        }
        session
            .stream
            .set_read_timeout(Some(Duration::from_millis(if voice { 1000 } else { 8 })))
            .and_then(|_| {
                session
                    .stream
                    .set_write_timeout(Some(Duration::from_millis(250)))
            })
            .map_err(|_| "无法配置 Wi-Fi 音频连接。".to_string())?;
        Ok((
            session.stream,
            session.subkey.ok_or("Wi-Fi 音频会话无效。")?,
        ))
    }
}

#[cfg(test)]
mod bound_identity_tests {
    use super::*;
    #[test]
    fn same_boot_repair_cannot_retarget_captured_audio_peer() {
        let old = Peer { address: "127.0.0.1:42000".parse().unwrap(), key: Zeroizing::new([17;16]) };
        let target = AudioTarget { peer: old.clone(), boot: "0123456789abcdef".into(), board_id: "p4-0123456789ab".into(), hud: true };
        assert!(target.matches_peer(Some(&old)));
        let repaired = Peer { key: Zeroizing::new([18;16]), ..old.clone() };
        assert!(!target.matches_peer(Some(&repaired)));
        assert!(!target.matches_peer(None));
        let moved = Peer { address: "127.0.0.1:42001".parse().unwrap(), ..old.clone() };
        assert!(!target.matches_peer(Some(&moved)));
        // Both an ACK retry clone and the eventual socket retain the old key.
        let retry = target.clone();
        assert!(retry.matches_peer(Some(&old)));
        assert!(!retry.matches_peer(Some(&repaired)));
        assert_eq!(retry.peer.key.as_ref(), old.key.as_ref());
        assert!(retry.hud_supported());
    }
}
