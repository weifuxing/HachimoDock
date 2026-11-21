//! Record codec for a separately authenticated, trusted LAN chat socket.
use ring::aead::{self, Aad, LessSafeKey, Nonce, UnboundKey};
use zeroize::{Zeroize, Zeroizing};

pub(crate) const HEADER: usize = 8;
const TAG: usize = 16;
pub(crate) const PCM: usize = 640;
pub(super) const MAX_RECORD: usize = HEADER + PCM + TAG;
const DOWN: u8 = 1;
const UP: u8 = 2;
const AUDIO: u8 = 1;
const KEEPALIVE: u8 = 2;
const CLOSE: u8 = 3;
const PLAY_BEGIN: u8 = 4;
const PLAY_END: u8 = 5;
const PLAY_FLUSH: u8 = 6;
pub(super) const HUD_BYTES: usize = 644;
const HUD: u8 = 7;

/// Fixed UTF-8 fields have exactly the same byte limits as the device's USB
/// conversation state. Never serialize arbitrary JSON into the audio lane.
pub(super) fn hud_body(payload: &serde_json::Value) -> Result<Zeroizing<Vec<u8>>, String> {
    let phase = match payload["state"].as_str().unwrap_or("") {
        "preparing" => 0, "listening" => 1, "thinking" => 2,
        "speaking" => 3, "ended" => 4, "error" => 5,
        _ => return Err("无效的实时对话显示状态".into()),
    };
    let id = payload["sessionId"].as_str().unwrap_or("");
    if id.is_empty() || id.len() >= 64 || id.contains('\0') {
        return Err("无效的实时对话显示会话".into());
    }
    let mut body = Zeroizing::new(vec![0; HUD_BYTES]);
    body[0] = phase;
    body[1] = u8::from(payload["role"] == "user") | (u8::from(payload["final"] == true) << 1);
    for (key, offset, capacity) in [("sessionId", 4, 64), ("name", 68, 64), ("text", 132, 512)] {
        let text = payload[key].as_str().unwrap_or("").split('\0').next().unwrap_or("");
        let mut length = text.len().min(capacity - 1);
        while !text.is_char_boundary(length) { length -= 1; }
        body[offset..offset + length].copy_from_slice(&text.as_bytes()[..length]);
    }
    Ok(body)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Record {
    Audio { vad: Option<bool> },
    Keepalive,
    Close,
    PlaybackDrained,
    VoiceReady,
}

#[derive(Clone, Copy)]
pub(super) enum Playback {
    Begin,
    Pcm,
    End,
    Flush,
}

fn nonce(direction: u8, sequence: u32) -> Nonce {
    let mut bytes = [b'R', b'T', b'C', 1, direction, 0, 0, 0, 0, 0, 0, 0];
    bytes[8..].copy_from_slice(&sequence.to_be_bytes());
    Nonce::assume_unique_for_key(bytes)
}

fn header(record: Record, sequence: u32) -> [u8; HEADER] {
    let (kind, flags, length) = match record {
        Record::Audio { vad } => (AUDIO, vad.map_or(0, |speech| 2 + u8::from(speech)), PCM),
        Record::Keepalive => (KEEPALIVE, 0, 0),
        Record::Close => (CLOSE, 0, 0),
        Record::PlaybackDrained => (PLAY_END, 0, 4),
        Record::VoiceReady => (8, 0, 1),
    };
    let mut bytes = [kind, flags, (length >> 8) as u8, length as u8, 0, 0, 0, 0];
    bytes[4..].copy_from_slice(&sequence.to_be_bytes());
    bytes
}

fn parse_uplink(bytes: &[u8; HEADER], sequence: u32) -> Option<(Record, usize)> {
    let record = match (bytes[0], bytes[1]) {
        (AUDIO, 0) => Record::Audio { vad: None },
        (AUDIO, 2) => Record::Audio { vad: Some(false) },
        (AUDIO, 3) => Record::Audio { vad: Some(true) },
        (KEEPALIVE, 0) => Record::Keepalive,
        (CLOSE, 0) => Record::Close,
        (PLAY_END, 0) => Record::PlaybackDrained,
        (8, 0) => Record::VoiceReady,
        _ => return None,
    };
    if sequence == u32::MAX || *bytes != header(record, sequence) {
        return None;
    }
    let length = u16::from_be_bytes([bytes[2], bytes[3]]) as usize;
    Some((record, HEADER + length + TAG))
}

pub(crate) struct LanAudio {
    key: Option<LessSafeKey>,
    outgoing: u32,
    incoming: u32,
}

impl LanAudio {
    /// A single-use audio-only key from the paired handshake on this socket.
    /// Construct only after authenticating the device's admission acknowledgment.
    pub(crate) fn new(key: Zeroizing<[u8; 16]>) -> Self {
        Self {
            key: UnboundKey::new(&aead::AES_128_GCM, key.as_ref())
                .ok()
                .map(LessSafeKey::new),
            outgoing: 0,
            incoming: 0,
        }
    }

    /// Records are never retried. Invoke this on timeout, short write or socket loss.
    pub(super) fn end(&mut self) {
        self.key = None;
    }

    pub(crate) fn seal(&mut self, record: Record, pcm: &[u8]) -> Option<Zeroizing<Vec<u8>>> {
        match (record, pcm.len()) {
            (Record::Audio { vad: None }, PCM) | (Record::Keepalive | Record::Close, 0) => {}
            _ => return None,
        }
        let kind = header(record, 0)[0];
        let wire = self.seal_wire(kind, pcm);
        if record == Record::Close {
            self.end();
        }
        wire
    }

    pub(super) fn request_close(&mut self) -> Option<Zeroizing<Vec<u8>>> {
        self.seal_wire(CLOSE, &[])
    }

    pub(super) fn seal_hud(&mut self, body: &[u8]) -> Option<Zeroizing<Vec<u8>>> {
        if body.len() != HUD_BYTES { return None; }
        self.seal_wire(HUD, body)
    }

    pub(super) fn seal_playback(
        &mut self,
        action: Playback,
        turn: u32,
        pcm: &[u8],
    ) -> Option<Zeroizing<Vec<u8>>> {
        if turn == 0
            || (matches!(action, Playback::Pcm) && pcm.len() != PCM)
            || (!matches!(action, Playback::Pcm) && !pcm.is_empty())
        {
            return None;
        }
        let kind = match action {
            Playback::Begin => PLAY_BEGIN,
            Playback::Pcm => AUDIO,
            Playback::End => PLAY_END,
            Playback::Flush => PLAY_FLUSH,
        };
        let mut body = Zeroizing::new(Vec::with_capacity(4 + pcm.len()));
        body.extend_from_slice(&turn.to_be_bytes());
        body.extend_from_slice(pcm);
        self.seal_wire(kind, &body)
    }

    fn seal_wire(&mut self, kind: u8, bytes: &[u8]) -> Option<Zeroizing<Vec<u8>>> {
        if self.outgoing == u32::MAX {
            self.end();
        }
        let key = self.key.as_ref()?;
        let len = (bytes.len() as u16).to_be_bytes();
        let seq = self.outgoing.to_be_bytes();
        let h = [kind, 0, len[0], len[1], seq[0], seq[1], seq[2], seq[3]];
        let mut wire = Zeroizing::new(Vec::with_capacity(HEADER + bytes.len() + TAG));
        wire.extend_from_slice(&h);
        wire.extend_from_slice(bytes);
        let tag = key.seal_in_place_separate_tag(
            nonce(DOWN, self.outgoing),
            Aad::from(h),
            &mut wire[HEADER..],
        );
        self.outgoing += 1;
        if tag.is_err() {
            self.end();
        }
        wire.extend_from_slice(tag.ok()?.as_ref());
        Some(wire)
    }

    /// Size check before a bounded socket read. The header is still unauthenticated.
    pub(crate) fn record_size(&mut self, h: &[u8; HEADER]) -> Option<usize> {
        self.key.as_ref()?;
        if let Some((_, size)) = parse_uplink(h, self.incoming) {
            return Some(size);
        }
        self.end();
        None
    }

    /// On success the PCM remains in the supplied zeroizing buffer.
    pub(crate) fn open(&mut self, wire: &mut Zeroizing<Vec<u8>>) -> Option<Record> {
        let result = self.open_record(wire);
        if result.is_none() || result == Some(Record::Close) {
            wire.zeroize();
            self.end();
        }
        result
    }

    fn open_record(&mut self, wire: &mut Vec<u8>) -> Option<Record> {
        let key = self.key.as_ref()?;
        let h: [u8; HEADER] = wire.get(..HEADER)?.try_into().ok()?;
        let (record, size) = parse_uplink(&h, self.incoming)?;
        if size != wire.len() {
            return None;
        }
        let length = key
            .open_in_place(nonce(UP, self.incoming), Aad::from(h), &mut wire[HEADER..])
            .ok()?
            .len();
        wire.truncate(HEADER + length);
        self.incoming += 1;
        Some(record)
    }
}

#[cfg(test)]
#[path = "lan_audio_tests.rs"]
mod tests;

#[cfg(test)]
mod hud_tests {
    use super::*;
    #[test]
    fn hud_utf8_limits_and_authenticated_record_lifecycle() {
        let payload = serde_json::json!({"sessionId":"rtc-one", "state":"listening",
            "name":"蛙".repeat(30), "text":"蛙".repeat(300), "role":"user", "final":true});
        let body = hud_body(&payload).unwrap();
        assert_eq!(body.len(), 644);
        assert_eq!(&body[..4], &[1,3,0,0]);
        assert_eq!(body[131], 0);
        assert_eq!(body[642], 0);
        assert!(std::str::from_utf8(&body[132..642]).is_ok());
        let mut codec = LanAudio::new(Zeroizing::new([42;16]));
        let wire = codec.seal_hud(&body).unwrap();
        assert_eq!(wire.len(), 668);
        assert_eq!(&wire[..8], &[7,0,2,132,0,0,0,0]);
        let key = LessSafeKey::new(UnboundKey::new(&aead::AES_128_GCM, &[42;16]).unwrap());
        let mut decoded = wire[8..].to_vec();
        assert_eq!(key.open_in_place(nonce(DOWN,0), Aad::from(&wire[..8]), &mut decoded).unwrap(), &body[..]);
        let mut corrupt = wire[8..].to_vec();corrupt[0] ^= 1;
        assert!(key.open_in_place(nonce(DOWN,0), Aad::from(&wire[..8]), &mut corrupt).is_err());
        assert!(codec.seal_hud(&body[..643]).is_none());
        assert_eq!(codec.request_close().unwrap()[7], 1);
        codec.end();assert!(codec.seal_hud(&body).is_none());
    }
    #[test]
    fn hud_accepts_only_existing_usb_states_and_bounded_session() {
        for (phase, number) in ["preparing","listening","thinking","speaking","ended","error"].iter().zip(0..) {
            assert_eq!(hud_body(&serde_json::json!({"sessionId":"rtc", "state":phase})).unwrap()[0],number);
        }
        for id in ["".to_owned(), "x".repeat(64), "rtc\0other".to_owned()] {
            assert!(hud_body(&serde_json::json!({"sessionId":id,"state":"preparing"})).is_err());
        }
        assert!(hud_body(&serde_json::json!({"sessionId":"rtc","state":"unknown"})).is_err());
    }
}
