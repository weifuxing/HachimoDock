//! OpenAI-compatible TTS client (e.g. GPUStack /v1/audio/speech).
//! Synthesizes text to 16 kHz mono 16-bit PCM and feeds it through the same
//! on_pcm callback pipeline as the built-in provider.
use crate::doubao_tts::DoubaoTtsConfig;

const TARGET_RATE: u32 = 16_000;

pub async fn synthesize<F>(
    cfg: &DoubaoTtsConfig,
    text: &str,
    mut on_pcm: F,
) -> Result<usize, String>
where
    F: FnMut(&[u8]),
{
    let base = crate::voice_chat_settings::custom_speech_base_url(&cfg.tts_base_url)?;
    let url = format!("{base}/v1/audio/speech");
    let client = reqwest::Client::builder()
        .timeout(std::time::Duration::from_secs(60))
        .redirect(reqwest::redirect::Policy::none())
        .build()
        .map_err(|e| format!("无法创建语音合成客户端: {e}"))?;
    let body = serde_json::json!({
        "model": cfg.tts_model,
        "input": text,
        "voice": cfg.tts_voice,
        "response_format": "wav",
    });
    let mut request = client.post(url).json(&body);
    if !cfg.api_key.trim().is_empty() {
        request = request.bearer_auth(cfg.api_key.trim());
    }
    let resp = request.send().await.map_err(|_| "语音合成请求失败")?;
    let bytes =
        crate::voice_chat_settings::read_custom_speech_response(resp, 8 * 1024 * 1024).await?;
    let pcm =
        parse_wav_to_pcm16mono16k(&bytes).map_err(|e| format!("解析语音合成音频失败: {e}"))?;
    let mut emitted = 0usize;
    for chunk in pcm.chunks(8000) {
        on_pcm(chunk);
        emitted += chunk.len();
    }
    Ok(emitted)
}

/// Parse an RIFF/WAV byte stream into 16 kHz mono 16-bit little-endian PCM.
fn parse_wav_to_pcm16mono16k(bytes: &[u8]) -> Result<Vec<u8>, String> {
    if bytes.len() < 44 || &bytes[0..4] != b"RIFF" || &bytes[8..12] != b"WAVE" {
        return Err("响应不是 WAV 音频".into());
    }
    let mut format = None;
    let mut offset = 12usize;
    let mut data: Option<&[u8]> = None;
    while offset + 8 <= bytes.len() {
        let id = &bytes[offset..offset + 4];
        let size = u32::from_le_bytes([
            bytes[offset + 4],
            bytes[offset + 5],
            bytes[offset + 6],
            bytes[offset + 7],
        ]) as usize;
        let payload = offset + 8;
        if payload.saturating_add(size) > bytes.len() {
            return Err("WAV 数据块不完整".into());
        }
        let chunk = &bytes[payload..payload.saturating_add(size)];
        if id == b"fmt " {
            if chunk.len() < 16 || u16::from_le_bytes([chunk[0], chunk[1]]) != 1 {
                return Err("WAV 必须使用 PCM 编码".into());
            }
            format = Some((
                u16::from_le_bytes([chunk[2], chunk[3]]),
                u32::from_le_bytes([chunk[4], chunk[5], chunk[6], chunk[7]]),
                u16::from_le_bytes([chunk[14], chunk[15]]),
            ));
        } else if id == b"data" {
            data = Some(chunk);
        }
        offset = payload.saturating_add(size).saturating_add(size % 2);
    }
    let data = data.ok_or("WAV 缺少 data 块")?;
    let (channels, sample_rate, bits) = format.ok_or("WAV 缺少 fmt 块")?;
    if bits != 16 {
        return Err(format!("不支持的位深: {bits}"));
    }
    if !(8_000..=96_000).contains(&sample_rate) || !matches!(channels, 1 | 2) {
        return Err("WAV 采样率异常".into());
    }
    if data.len() % (2 * usize::from(channels)) != 0 {
        return Err("WAV 包含不完整的采样".into());
    }
    // Mono-ize (average channels), then linear-resample to 16 kHz.
    let samples: Vec<i16> = if channels == 1 {
        data.chunks_exact(2)
            .map(|b| i16::from_le_bytes([b[0], b[1]]))
            .collect()
    } else {
        let ch = channels as usize;
        let frames = data.len() / (2 * ch);
        (0..frames)
            .map(|i| {
                let sum: i32 = (0..ch)
                    .map(|c| {
                        i16::from_le_bytes([data[(i * ch + c) * 2], data[(i * ch + c) * 2 + 1]])
                            as i32
                    })
                    .sum();
                (sum / ch as i32) as i16
            })
            .collect()
    };
    if samples.is_empty() {
        return Err("音频内容为空".into());
    }
    let out = resample_linear(&samples, sample_rate, TARGET_RATE);
    if out.is_empty() {
        return Err("音频内容为空".into());
    }
    let mut v = Vec::with_capacity(out.len() * 2);
    for s in out {
        v.extend_from_slice(&s.to_le_bytes());
    }
    Ok(v)
}

fn resample_linear(src: &[i16], from: u32, to: u32) -> Vec<i16> {
    if from == to {
        return src.to_vec();
    }
    let ratio = from as f64 / to as f64;
    let n = (src.len() as f64 / ratio) as usize;
    (0..n)
        .map(|i| {
            let pos = i as f64 * ratio;
            let lo = pos.floor() as usize;
            let hi = (lo + 1).min(src.len() - 1);
            let frac = pos - lo as f64;
            let a = f64::from(src[lo]);
            let b = f64::from(src[hi]);
            (a + (b - a) * frac).round() as i16
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resamples_between_pcm_samples() {
        assert_eq!(
            resample_linear(&[0, 1000], 8000, 16000),
            [0, 500, 1000, 1000]
        );
    }

    #[test]
    fn rejects_non_pcm_format_and_partial_samples() {
        let mut wav = b"RIFF&\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\x80>\0\0\0}\0\0\x02\0\x10\0data\x02\0\0\0\x01\0".to_vec();
        assert_eq!(parse_wav_to_pcm16mono16k(&wav).unwrap(), [1, 0]);
        wav[20] = 3;
        assert!(parse_wav_to_pcm16mono16k(&wav).is_err());
        wav[20] = 1;
        wav.pop();
        assert!(parse_wav_to_pcm16mono16k(&wav).is_err());
    }
}
