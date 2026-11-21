use super::wifi_endpoint;
use serde_json::Value;

pub(crate) const CAPABILITY: &str = "p4-private-feed-v1";

#[derive(Debug)]
pub(crate) struct Feed<'a> {
    pub(crate) url: &'a str,
    pub(crate) topic: Option<&'a str>,
}

fn mqtt_url(url: &str) -> bool {
    let Some(authority) = url.strip_prefix("mqtts://") else {
        return false;
    };
    if authority.contains(['/', '?', '#', '@']) {
        return false;
    }
    let host = match authority.split_once(':') {
        Some((host, port)) => {
            if port.len() > 5
                || !port.bytes().all(|b| b.is_ascii_digit())
                || !port.parse::<u16>().is_ok_and(|value| value != 0)
            {
                return false;
            }
            host
        }
        None => authority,
    };
    wifi_endpoint::valid(&format!("https://{host}"))
}

pub(crate) fn parse(data: &Value) -> Result<Feed<'_>, &'static str> {
    const INVALID: &str = "设备私有数据源需要受控的 HTTPS Bearer 或 MQTT TLS 配置";
    let url = data["url"].as_str().ok_or(INVALID)?;
    let auth = data["connection"].as_object().ok_or(INVALID)?;
    if data["source"] != "wifi.feed" || data.get("http").is_some() {
        return Err(INVALID);
    }
    let topic = match auth.get("type").and_then(Value::as_str) {
        Some("bearer") if auth.len() == 1 && !url.contains('?') && wifi_endpoint::valid(url) => {
            None
        }
        Some("mqtt" | "mqtt-public") if auth.len() == 2 && mqtt_url(url) => {
            let topic = auth.get("topic").and_then(Value::as_str).ok_or(INVALID)?;
            if topic.is_empty()
                || topic.len() > 127
                || topic.starts_with('$')
                || topic
                    .bytes()
                    .any(|b| !(33..=126).contains(&b) || matches!(b, b'+' | b'#'))
            {
                return Err(INVALID);
            }
            Some(topic)
        }
        _ => return Err(INVALID),
    };
    Ok(Feed { url, topic })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn declarations_cannot_carry_credentials_or_wildcard_subscriptions() {
        let mut feed = json!({"source":"wifi.feed","url":"https://api.example.org/data",
            "connection":{"type":"bearer"}});
        assert!(parse(&feed).is_ok());
        feed["connection"]["token"] = json!("package-secret");
        assert!(parse(&feed).is_err());
        feed["connection"] = json!({"type":"bearer"});
        feed["url"] = json!("https://api.example.org/data?key=secret");
        assert!(parse(&feed).is_err());
        feed["url"] = json!("mqtts://broker.example.org:9443");
        feed["connection"] = json!({"type":"mqtt","topic":"sensors/room"});
        assert_eq!(parse(&feed).unwrap().topic, Some("sensors/room"));
        feed["connection"]["topic"] = json!("sensors/+");
        assert!(parse(&feed).is_err());
    }
}
