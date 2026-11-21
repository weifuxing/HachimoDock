//! Paired provisioning for a dedicated on-device credential. Never print secret input.
use super::{exchange_response, load_peer, query_status};
use crate::clawpkg::private_feed::{parse, Feed};
use std::{
    ffi::OsString,
    io::{self, IsTerminal, Read},
    path::Path,
};
use zeroize::Zeroizing;

const MODE: u8 = 21;
const USAGE: &str = "用法：petui feed-credential <set|status|forget> <板卡 ID> <组件 ID> <runtime/widget.json>。set 从标准输入读取凭证。";

fn read_fields() -> Result<Zeroizing<Vec<u8>>, String> {
    let input = io::stdin();
    if input.is_terminal() {
        return Err("请用密码管理器或权限受限的输入文件将凭证送入标准输入。".into());
    }
    let mut fields = Zeroizing::new(Vec::with_capacity(325));
    input
        .lock()
        .take(325)
        .read_to_end(&mut fields)
        .map_err(|_| "无法读取凭证")?;
    if fields.len() > 324 {
        return Err("凭证超过长度限制。".into());
    }
    Ok(fields)
}

fn strip_final_newline(bytes: &[u8]) -> &[u8] {
    bytes
        .strip_suffix(b"\r\n")
        .or_else(|| bytes.strip_suffix(b"\n"))
        .unwrap_or(bytes)
}

fn request(
    boot: &str,
    op: u8,
    id: &str,
    feed: Feed<'_>,
    fields: &[u8],
) -> Result<Zeroizing<Vec<u8>>, String> {
    if boot.len() != 16
        || !boot.bytes().all(|b| b.is_ascii_hexdigit())
        || id.is_empty()
        || id.len() > 47
        || !id
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b"-_.".contains(&b))
    {
        return Err("设备或组件标识无效。".into());
    }
    let data = strip_final_newline(fields);
    let (user, secret) = match (op, feed.topic) {
        (1, Some(_)) => {
            let index = data
                .iter()
                .position(|b| *b == b'\n')
                .ok_or("MQTT 凭证需要两行，依次是用户名和密码。无需认证时可留空。")?;
            (strip_final_newline(&data[..=index]), &data[index + 1..])
        }
        (1, None) => {
            if data.is_empty()
                || !data
                    .iter()
                    .all(|b| b.is_ascii_alphanumeric() || b"-._~+/=".contains(b))
            {
                return Err("HTTPS 需要一行有效的 Bearer Token。".into());
            }
            (&[][..], data)
        }
        _ => (&[][..], &[][..]),
    };
    if user.len() > 64
        || secret.len() > 256
        || (feed.topic.is_some() && user.is_empty() && !secret.is_empty())
        || user.iter().chain(secret).any(|b| !(32..=126).contains(b))
    {
        return Err("凭证格式或长度无效。".into());
    }
    let url = feed.url.as_bytes();
    let topic = feed.topic.unwrap_or_default().as_bytes();
    let mut body = Zeroizing::new(Vec::with_capacity(
        25 + id.len() + url.len() + topic.len() + user.len() + secret.len(),
    ));
    body.extend_from_slice(boot.as_bytes());
    body.extend_from_slice(&[op, if feed.topic.is_some() { 2 } else { 1 }, id.len() as u8]);
    body.extend_from_slice(&(url.len() as u16).to_be_bytes());
    body.extend_from_slice(&[topic.len() as u8, user.len() as u8]);
    body.extend_from_slice(&(secret.len() as u16).to_be_bytes());
    body.extend_from_slice(id.as_bytes());
    body.extend_from_slice(url);
    body.extend_from_slice(topic);
    body.extend_from_slice(user);
    body.extend_from_slice(secret);
    Ok(body)
}

pub(crate) fn cli(args: &[OsString]) -> Result<String, String> {
    let [action, board, id, path] = args else {
        return Err(USAGE.into());
    };
    let operation = match action.to_str() {
        Some("status") => 0,
        Some("set") => 1,
        Some("forget") => 2,
        _ => return Err(USAGE.into()),
    };
    let bytes = std::fs::File::open(Path::new(path)).map_err(|_| "无法打开组件 runtime")?;
    let manifest: serde_json::Value =
        serde_json::from_reader(bytes.take(128 * 1024)).map_err(|_| "组件 runtime 格式无效")?;
    let feed = parse(&manifest["data"])?;
    if manifest["data"]["connection"]["type"] == "mqtt-public" {
        return Err("公开订阅打开即可使用。".into());
    }
    let peer = load_peer().ok_or("请先配对设备并授予控制权限。")?;
    let status = query_status(&peer).map_err(|_| "无法确认设备身份，请检查 Wi-Fi。")?;
    if Some(status.board_device_id.as_str()) != board.to_str()
        || !status.wifi_connected
        || !status.control_trusted
    {
        return Err("配对设备身份或控制权限不匹配。".into());
    }
    if !status.widget_credential_storage_ready {
        return Err("设备尚未开放硬件保护的凭证存储。".into());
    }
    let fields = if operation == 1 {
        read_fields()?
    } else {
        Zeroizing::new(Vec::new())
    };
    let body = request(
        &status.boot_id,
        operation,
        id.to_str().ok_or(USAGE)?,
        feed,
        &fields,
    )?;
    if !load_peer().is_some_and(|current| {
        current.address == peer.address && current.key.as_ref() == peer.key.as_ref()
    }) {
        return Err("Wi-Fi 配对已变化。".into());
    }
    let reply = exchange_response(&peer, MODE, &body, Some(1))
        .map_err(|_| "设备未确认操作，请用 status 核对后再操作。")?;
    match reply.as_slice() {
        [1] => Ok("已配置设备凭证".into()),
        [2] => Ok("设备没有此数据源的凭证".into()),
        [3] => Err("设备安全存储不可用。".into()),
        [4] => Err("设备凭证存储已满或发生读写错误。".into()),
        [5] => Err("设备正在保存另一份凭证，请稍后重试。".into()),
        _ => Err("设备拒绝了本次凭证操作。".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn credentials_are_not_part_of_declaration_and_have_checked_boundaries() {
        let feed = || Feed {
            url: "https://feed.example.org/read",
            topic: None,
        };
        let boot = "abcdef0123456789";
        let body = request(boot, 1, "example", feed(), b"fixture-token\n").unwrap();
        assert_eq!(&body[..16], boot.as_bytes());
        assert_eq!(&body[16..25], &[1, 1, 7, 0, 29, 0, 0, 0, 13]);
        assert_eq!(
            *body,
            *request(boot, 1, "example", feed(), b"fixture-token\r\n").unwrap()
        );
        assert!(request(boot, 1, "example", feed(), b"invalid\r\nvalue").is_err());
        assert!(request(boot, 0, "../example", feed(), b"").is_err());
        let mqtt = || Feed {
            url: "mqtts://broker.example.org",
            topic: Some("temperature"),
        };
        assert_eq!(
            *request(boot, 1, "example", mqtt(), b"name\r\npassword\r\n").unwrap(),
            *request(boot, 1, "example", mqtt(), b"name\npassword\n").unwrap(),
        );
    }
}
