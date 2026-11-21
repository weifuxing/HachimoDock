//! Manifest checks for device-owned Wi-Fi audio. The firmware checks DNS results
//! and pins the verified public IPv4 address when it connects.
use super::wifi_endpoint::valid as endpoint_valid;
use serde_json::Value;

const MAX_TRACKS: usize = 16;
const MAX_TITLE_BYTES: usize = 90;
// Matches the firmware's fixed buffer and cJSON's five-byte print allowance.
const MAX_PLAYLIST_BYTES: usize = 7168 - 5;

pub(crate) fn validate_playlist(playlist: &Value) -> Result<(), String> {
    let tracks = playlist
        .as_array()
        .filter(|rows| (1..=MAX_TRACKS).contains(&rows.len()))
        .ok_or("media.playlist must contain 1–16 tracks")?;
    for (index, track) in tracks.iter().enumerate() {
        let track = track
            .as_object()
            .ok_or_else(|| format!("media.playlist[{index}] must be an object"))?;
        if track
            .keys()
            .any(|key| !matches!(key.as_str(), "title" | "url" | "live"))
        {
            return Err(format!("media.playlist[{index}] has an unknown field"));
        }
        let title_ok = track
            .get("title")
            .and_then(Value::as_str)
            .is_some_and(|title| {
                !title.is_empty()
                    && title.len() <= MAX_TITLE_BYTES
                    && title.bytes().all(|byte| byte >= b' ' && byte != 127)
            });
        if !title_ok {
            return Err(format!("media.playlist[{index}].title must contain 1–90 UTF-8 bytes without ASCII controls"));
        }
        if !track
            .get("url")
            .and_then(Value::as_str)
            .is_some_and(endpoint_valid)
        {
            return Err(format!(
                "media.playlist[{index}].url must be a supported HTTPS URL with a DNS hostname"
            ));
        }
        if track.get("live").is_some_and(|live| !live.is_boolean()) {
            return Err(format!("media.playlist[{index}].live must be a boolean"));
        }
    }
    if playlist.to_string().len() > MAX_PLAYLIST_BYTES {
        return Err("media.playlist exceeds the device's serialized size limit".into());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn accepts_finite_and_live_tracks() {
        assert!(validate_playlist(&json!([
            {"title":"歌曲", "url":"https://Audio.example/song.wav?name=hello%20world"},
            {"title":"Radio", "url":"https://audio.example?station=1", "live":true}
        ]))
        .is_ok());
    }

    #[test]
    fn rejects_invalid_playlists_and_targets() {
        assert!(validate_playlist(&json!([])).is_err());
        let track = json!({"title":"Track","url":"https://audio.example/song.mp3"});
        assert!(validate_playlist(&json!(vec![track; MAX_TRACKS + 1])).is_err());
        assert!(validate_playlist(&json!([
            {"title":"Track", "url":"https://audio.example/track", "live":null}
        ]))
        .is_err());
        assert!(validate_playlist(&json!([
            {"title":"NUL\u{0}", "url":"https://audio.example/track"}
        ]))
        .is_err());
        let largest = json!({
            "title": "\"".repeat(90),
            "url": format!("https://{}.example/{}", "a".repeat(63), "p".repeat(254)),
        });
        assert!(validate_playlist(&json!(vec![largest; MAX_TRACKS])).is_err());
        assert!(validate_playlist(&json!([
            {"title":"Track", "url":"https://audio.example/track", "format":"mp3"}
        ]))
        .is_err());
        for url in [
            "http://audio.example/track",
            "https://user@audio.example/track",
            "https://audio.example:443/track",
            "https://127.0.0.1/track",
            "https://[::1]/track",
            "https://audio.example//other",
            "https://audio.example/a#b",
            "https://audio.example/a\\b",
            "https://audio.example/a\r\nb",
        ] {
            assert!(!endpoint_valid(url), "{url}");
        }
    }
}
