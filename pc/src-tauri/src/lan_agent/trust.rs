use super::{query_status, read_with_recovery};
use ring::hmac;
use zeroize::Zeroizing;

const CHALLENGE_BYTES: usize = 36;

fn challenge_proof(key: &[u8], encoded: &str, boot_id: &str) -> Result<Zeroizing<String>, String> {
    if encoded.len() != CHALLENGE_BYTES * 2 || !encoded.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err("USB 返回的授权挑战无效。".into());
    }
    let mut challenge = [0; CHALLENGE_BYTES];
    for (i, byte) in challenge.iter_mut().enumerate() {
        *byte =
            u8::from_str_radix(&encoded[2 * i..2 * i + 2], 16).map_err(|_| "USB 授权挑战无效。")?;
    }
    if &challenge[16..32] != boot_id.as_bytes() {
        return Err("USB 上的设备与当前 Wi-Fi 配对目标不一致。".into());
    }
    let key = hmac::Key::new(hmac::HMAC_SHA256, key);
    let tag = hmac::sign(&key, &challenge);
    Ok(Zeroizing::new(
        tag.as_ref().iter().map(|b| format!("{b:02x}")).collect(),
    ))
}

#[tauri::command]
pub(crate) async fn lan_trust_usb(
    usb_manager: tauri::State<'_, crate::usb_serial::UsbSerialManager>,
) -> Result<(), String> {
    let usb = usb_manager.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let (board, connection_id) = usb
            .approval_connection()
            .ok_or("请用 USB 连接当前 Wi-Fi 配对的设备。")?;
        let (peer, status) = read_with_recovery(query_status, "无法读取已配对的 Wi-Fi 设备。")?;
        if status.control_trusted {
            return Ok(());
        }
        if !status.reboot_supported || !status.wifi_connected {
            return Err("设备未开放 USB 授权的无线控制。".into());
        }
        usb.with_asset_transfer_guard(|| {
            let challenge = usb.lan_trust_request(&board, connection_id, None)?;
            let encoded = challenge["challenge"]
                .as_str()
                .ok_or("USB 授权挑战缺失。")?;
            let proof = challenge_proof(peer.key.as_ref(), encoded, &status.boot_id)?;
            let reply = usb.lan_trust_request(&board, connection_id, Some(proof.as_str()));
            // Read back using the same key even if confirmation was lost.
            if query_status(&peer)
                .is_ok_and(|live| live.boot_id == status.boot_id && live.control_trusted)
            {
                return Ok(());
            }
            reply?;
            Err("无线控制授权未能确认，请重新读取设备状态。".into())
        })
    })
    .await
    .map_err(|_| "USB 授权任务未完成。")?
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn proof_binds_nonce_boot_generation_and_key() {
        let key = [0x71; 16];
        let boot = "0123456789abcdef";
        let challenge = "000000000000000000000000000000003031323334353637383961626364656600000001";
        let expected = challenge_proof(&key, challenge, boot).unwrap();
        assert_eq!(expected.len(), 64);
        assert!(challenge_proof(&key, challenge, "1123456789abcdef").is_err());
        assert_ne!(
            *expected,
            *challenge_proof(&[0x72; 16], challenge, boot).unwrap()
        );
        let next = format!("{}2", &challenge[..71]);
        assert_ne!(*expected, *challenge_proof(&key, &next, boot).unwrap());
    }
}
