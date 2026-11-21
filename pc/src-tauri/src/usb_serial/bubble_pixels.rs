//! Main-renderer-owned, bounded optional alpha assets. Tokens correlate
//! lifecycle; they do not authenticate a remote peer. Latest text wins.
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::VecDeque;
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
use std::time::{Duration, Instant};
pub(crate) type RenderSink = Arc<dyn Fn(Value) + Send + Sync>;
pub(crate) const SCHEMA: &str = "p4-text-alpha-v2";
pub(crate) const MAX_BYTES: usize = 32 * 1024;
pub(crate) const CHUNK_BYTES: usize = 512;
pub(crate) const DEADLINE_MS: u64 = 10_000;
fn scope(topic: &str) -> &str {
    if topic.starts_with("state/") || topic == "speech/text" {
        "bubble"
    } else {
        topic
    }
}
pub(crate) fn is_text_topic(topic: &str) -> bool {
    topic.starts_with("state/")
        || matches!(
            topic,
            "session/current" | "speech/text" | "ui/conversation" | "audio/play_subtitle"
        )
}
fn hex(s: &str, n: usize) -> bool {
    s.len() == n
        && s.bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
pub(crate) fn valid_boot(s: &str) -> bool {
    hex(s, 16) && s != "0000000000000000"
}
fn text(v: Option<&Value>, limit: usize) -> String {
    let s = v
        .and_then(Value::as_str)
        .unwrap_or("")
        .split('\0')
        .next()
        .unwrap_or("");
    let mut end = s.len().min(limit);
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    s[..end].to_owned()
}
fn hash(bytes: impl IntoIterator<Item = u8>) -> String {
    let h = bytes.into_iter().fold(14695981039346656037u64, |h, b| {
        (h ^ u64::from(b)).wrapping_mul(1099511628211)
    });
    format!("{h:016x}")
}
fn asset(style: u8, s: &str) -> String {
    hash(
        b"p4-text-alpha-v2:rle4:1:"
            .iter()
            .copied()
            .chain([style])
            .chain(s.bytes()),
    )
}
#[derive(Clone)]
struct Desired {
    identity: String,
    owner: String,
    version: String,
    topic: String,
    payload: Value,
    assets: Vec<(u8, String)>,
}
#[derive(Default)]
pub(crate) struct RenderBridge {
    sink: Option<RenderSink>,
    instance: String,
    peer: String,
    sequence: u64,
    owner_sequence: u64,
    recent: VecDeque<Desired>,
    last_need: Option<Instant>,
    refresh_renderer: bool,
    glyph_cells: bool,
    body_masks: bool,
    title_masks: bool,
}
impl RenderBridge {
    pub fn set_format(&mut self, glyph_cells: bool, body_masks: bool, title_masks: bool) {
        if self.glyph_cells != glyph_cells
            || self.body_masks != body_masks
            || self.title_masks != title_masks
        {
            self.clear_peer();
            self.glyph_cells = glyph_cells;
            self.body_masks = body_masks;
            self.title_masks = title_masks;
        }
    }
    fn body_mask(&self, style: u8) -> bool {
        self.body_masks && matches!(style, 2 | 4)
    }
    fn uses_cells(&self, style: u8) -> bool {
        self.glyph_cells && !self.body_mask(style) && !(style == 1 && self.title_masks)
    }
    pub fn prefetch_probe(&self, event: &Value) -> Option<Value> {
        let d = self
            .recent
            .iter()
            .find(|d| event["owner"] == d.owner && event["version"] == d.version)?;
        let (style, id) = d.assets.iter().find(|(style, _)| {
            self.body_mask(*style) || (*style == 1 && event["prefetchTitle"] == true)
        })?;
        Some(
            json!({"phase":"status","boot":event["bootId"],"client":self.instance,
            "owner":d.owner,"version":d.version,"id":id,"style":style,
            "transfer":uuid::Uuid::new_v4().simple().to_string()}),
        )
    }
    // Compare-and-clear: an old WebView disposer cannot retire its successor.
    pub fn set_sink(&mut self, instance: &str, sink: Option<RenderSink>) -> bool {
        if !hex(instance, 32) || (sink.is_none() && self.instance != instance) {
            return false;
        }
        self.refresh_renderer = sink.is_some() && !self.recent.is_empty();
        self.sink = sink;
        self.instance = if self.sink.is_some() {
            instance.to_owned()
        } else {
            String::new()
        };
        self.last_need = None;
        true
    }
    pub fn clear_peer(&mut self) {
        self.recent.clear();
        self.peer.clear();
        self.last_need = None;
        self.refresh_renderer = false;
    }
    pub fn sink(&self) -> Option<RenderSink> {
        self.sink.clone()
    }
    pub fn current(&self, f: &BubbleFrame) -> bool {
        self.sink.is_some()
            && self.instance == f.renderer_id
            && f.body_masks == self.body_mask(f.style)
            && f.glyph_cells == self.uses_cells(f.style)
            && self.peer == format!("{}:{}:{}", f.board_device_id, f.connection_id, f.boot_id)
            && self.recent.iter().any(|d| {
                d.owner == f.owner
                    && d.version == f.version
                    && d.assets.iter().any(|(s, k)| *s == f.style && *k == f.id)
            })
    }
    pub fn need(&mut self, p: &Value, generation: u64, board: &str) -> Vec<Value> {
        let boot = p["boot"].as_str().unwrap_or("");
        if !valid_boot(boot)
            || p["client"] != self.instance
            || self.peer != format!("{board}:{generation}:{boot}")
            || self.sink.is_none()
            || self.refresh_renderer
            || self
                .last_need
                .map(|n| n.elapsed() < Duration::from_millis(900))
                .unwrap_or(false)
        {
            return Vec::new();
        }
        let page = match p["page"].as_u64() {
            Some(n) if n < 16 => n,
            _ => return Vec::new(),
        };
        let mut events = Vec::new();
        for d in &self.recent {
            for (style, id) in &d.assets {
                if p["style"] != *style || p["id"] != *id || (*style != 2 && page != 0) {
                    continue;
                }
                if (p["bodyMasks"] == true) != self.body_mask(*style)
                    || (p["glyphCells"] == true) != self.uses_cells(*style)
                {
                    continue;
                }
                let exact_text = p["text"].as_str().filter(|text| {
                    !text.is_empty()
                        && text.len() <= 511
                        && !text.contains('\0')
                        && p["owner"] == d.owner
                        && p["version"] == d.version
                        && asset(*style, text) == *id
                });
                let (topic, payload) = match exact_text {
                    Some(text) => ("device/need", json!({"text":text})),
                    None => (d.topic.as_str(), d.payload.clone()),
                };
                events.push(json!({"topic":topic,"payload":payload,"connectionId":generation.to_string(),
                    "boardDeviceId":board,"bootId":boot,"rendererId":self.instance,"owner":d.owner,
                    "version":d.version,"bodyMasks":self.body_masks,"need":{"style":style,"id":id,"page":page,
                    "bodyMasks":self.body_mask(*style),"glyphCells":p["glyphCells"]==true}}));
                break;
            }
        }
        if !events.is_empty() {
            self.last_need = Some(Instant::now());
        }
        events
    }
    pub fn current_wire(&self, p: &Value, generation: u64, board: &str, boot: &str) -> bool {
        self.sink.is_some()
            && p["client"] == self.instance
            && p["boot"] == boot
            && self.peer == format!("{board}:{generation}:{boot}")
            && self.recent.iter().any(|d| {
                p["owner"] == d.owner
                    && p["version"] == d.version
                    && d.assets
                        .iter()
                        .any(|(style, key)| p["style"] == *style && p["id"] == *key)
            })
    }
    pub fn metadata(&self, _topic: &str, boot: &str) -> Value {
        if self.sink.is_none() || !valid_boot(boot) {
            return Value::Bool(false);
        }
        let mut metadata = json!({"boot":boot,"client":self.instance,"owners":self.recent.iter()
            .map(|d|json!({"id":d.owner,"version":d.version,"assets":d.assets.iter().map(|(style,id)|json!({"style":style,"id":id})).collect::<Vec<_>>()})).collect::<Vec<_>>()});
        if self.title_masks {
            metadata["titleMasks"] = true.into();
        }
        if self.glyph_cells {
            metadata["glyphCells"] = true.into();
        }
        if self.body_masks {
            metadata["bodyMasks"] = true.into();
        }
        metadata
    }
    pub fn observe(
        &mut self,
        topic: &str,
        payload: &Value,
        generation: u64,
        board: &str,
        boot: &str,
    ) -> Vec<Value> {
        if !is_text_topic(topic) || !valid_boot(boot) || self.sink.is_none() {
            return Vec::new();
        }
        let peer = format!("{board}:{generation}:{boot}");
        if self.peer != peer {
            self.clear_peer();
            self.peer = peer;
        }
        let mut candidates = Vec::new();
        if topic == "session/current" {
            if let Some(items) = payload["sessions"].as_array() {
                let items = &items[..items.len().min(8)];
                let selected = items
                    .iter()
                    .position(|i| i["id"] == payload["sessionId"])
                    .unwrap_or(0);
                let start = selected
                    .saturating_sub(1)
                    .min(items.len().saturating_sub(3));
                for item in items.iter().skip(start).take(3) {
                    let raw_id = item.get("id").and_then(Value::as_str).unwrap_or("");
                    let id = text(item.get("id"), 128);
                    if id.is_empty() || raw_id.len() > 128 || raw_id.contains('\0') {
                        continue;
                    }
                    let title = text(item.get("title"), 191);
                    let content = text(item.get("content"), 383);
                    let state = text(item.get("state"), 383);
                    candidates.push((format!("session:{id}"),json!({"sessionId":id,"sessions":[{"id":id,"title":title,"content":content,"state":state}]})));
                }
            }
        } else {
            let mut p = serde_json::Map::new();
            for k in [
                "displayTitle",
                "sessionName",
                "title",
                "agent",
                "displayContent",
                "text",
                "message",
                "speech",
                "summary",
                "statusText",
                "name",
                "role",
                "sessionId",
                "conversationId",
            ] {
                if payload[k].is_string() {
                    p.insert(
                        k.into(),
                        Value::String(text(
                            payload.get(k),
                            if matches!(k, "name" | "role") {
                                63
                            } else {
                                511
                            },
                        )),
                    );
                }
            }
            let raw_owner = payload
                .get("sessionId")
                .or_else(|| payload.get("conversationId"))
                .and_then(Value::as_str)
                .unwrap_or("");
            if raw_owner.len() > 128 || raw_owner.contains('\0') {
                return Vec::new();
            }
            let owner = text(
                payload
                    .get("sessionId")
                    .or_else(|| payload.get("conversationId")),
                128,
            );
            candidates.push((format!("{}:{owner}", scope(topic)), Value::Object(p)));
        }
        let old: Vec<_> = self.recent.iter().cloned().collect();
        let replacing: Vec<_> = candidates
            .iter()
            .map(|(identity, _)| identity.clone())
            .collect();
        self.recent.retain(|d| {
            if topic == "session/current" {
                d.topic != topic
            } else {
                !replacing.contains(&d.identity)
            }
        });
        let mut events = Vec::new();
        for (identity, payload) in candidates {
            let owner = if let Some(d) = old.iter().find(|d| d.identity == identity) {
                d.owner.clone()
            } else {
                match self.owner_sequence.checked_add(1) {
                    Some(n) => {
                        self.owner_sequence = n;
                        format!("{n:016x}")
                    }
                    None => {
                        self.clear_peer();
                        return Vec::new();
                    }
                }
            };
            let version = if let Some(d) = old
                .iter()
                .find(|d| d.identity == identity && d.topic == topic && d.payload == payload)
            {
                d.version.clone()
            } else {
                match self.sequence.checked_add(1) {
                    Some(n) => {
                        self.sequence = n;
                        format!("{n:016x}")
                    }
                    None => {
                        self.clear_peer();
                        return Vec::new();
                    }
                }
            };
            let assets = assets_for(topic, &payload);
            let mut event = json!({"topic":topic,"payload":payload,"connectionId":generation.to_string(),"boardDeviceId":board,
                "bootId":boot,"rendererId":self.instance,"owner":owner,"version":version});
            if !old.iter().any(|d| d.owner == owner && d.version == version) {
                if self.title_masks {
                    let previous = old.iter().find(|d| d.owner == owner);
                    let changed = assets.iter().any(|asset| {
                        asset.0 == 1 && !previous.is_some_and(|d| d.assets.contains(asset))
                    });
                    event["prefetchTitle"] = changed.into();
                }
                events.push(event);
            }
            self.recent.push_back(Desired {
                identity,
                owner,
                version,
                topic: topic.into(),
                payload,
                assets,
            });
        }
        for d in old {
            if !self.recent.iter().any(|n| n.owner == d.owner) {
                events.push(json!({"retired":true,"owner":d.owner,"version":d.version,"topic":topic,
            "connectionId":generation.to_string(),"boardDeviceId":board,"bootId":boot,"rendererId":self.instance}));
            }
        }
        while self.recent.len() > 12 {
            self.recent.pop_front();
        }
        if std::mem::take(&mut self.refresh_renderer) {
            events.retain(|event| event["retired"] == true);
            for d in &self.recent {
                events.push(json!({"topic":d.topic,"payload":d.payload,"connectionId":generation.to_string(),
                    "boardDeviceId":board,"bootId":boot,"rendererId":self.instance,"owner":d.owner,"version":d.version,"prefetchTitle":self.title_masks}));
            }
        }
        if self.body_masks && topic == "session/current" {
            // The selected card's first body precedes neighboring prefetches.
            events.sort_by_key(|event| event["payload"]["sessionId"] != payload["sessionId"]);
        }
        for event in &mut events {
            if self.body_masks {
                event["bodyMasks"] = true.into();
                event["prefetchBody"] = true.into();
            } else if self.glyph_cells {
                event["demandOnly"] = true.into();
            }
        }
        events
    }
}
fn assets_for(topic: &str, p: &Value) -> Vec<(u8, String)> {
    let mut out = Vec::new();
    let mut add = |style: u8, s: String| {
        if !s.is_empty() {
            out.push((style, asset(style, &s)));
        }
    };
    if topic == "session/current" {
        if let Some(a) = p["sessions"].as_array() {
            for s in a {
                add(1, text(s.get("title"), 191));
                add(
                    3,
                    text(
                        if s["content"].as_str().unwrap_or("").is_empty() {
                            s.get("state")
                        } else {
                            s.get("content")
                        },
                        511,
                    ),
                );
                // The compact bubble falls back to the title when the body is empty.
                add(3, text(s.get("title"), 511));
                add(
                    4,
                    if s["content"].as_str().unwrap_or("").is_empty() {
                        text(s.get("state"), 383)
                    } else {
                        text(s.get("content"), 383)
                    },
                );
            }
        }
    } else if topic == "ui/conversation" {
        let name = if p["name"].as_str().unwrap_or("").is_empty() {
            "宠物".into()
        } else {
            text(p.get("name"), 63)
        };
        add(1, name.clone());
        add(
            1,
            if p["role"] == "user" {
                "你".into()
            } else {
                name
            },
        );
        add(2, text(p.get("text"), 511));
        add(3, text(p.get("text"), 511));
        add(3, text(p.get("name"), 511));
        // Status hints the device renders as the bubble body.
        for hint in ["我在听", "想一想", "准备回答", "准备中"] {
            add(2, hint.to_string());
        }
    } else if topic == "audio/play_subtitle" {
        add(2, text(p.get("text"), 511));
        add(3, text(p.get("text"), 511));
    } else {
        let title = ["displayTitle", "sessionName", "title", "agent"]
            .iter()
            .find_map(|k| p[*k].as_str().filter(|s| !s.is_empty()))
            .unwrap_or("pet");
        add(1, text(Some(&Value::String(title.into())), 127));
        let body = [
            "displayContent",
            "text",
            "message",
            "speech",
            "summary",
            "statusText",
        ]
        .iter()
        .find_map(|k| p[*k].as_str().filter(|s| !s.is_empty()))
        .unwrap_or("");
        for (style, limit) in [(4, 383), (3, 511)] {
            add(style, text(Some(&Value::String(body.into())), limit));
        }
    }
    out
}
#[derive(Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub(crate) struct BubbleFrame {
    #[serde(default)]
    pub body_masks: bool,
    #[serde(default)]
    pub glyph_cells: bool,
    #[serde(default)]
    pub wifi: bool,
    pub board_device_id: String,
    pub connection_id: String,
    pub boot_id: String,
    pub renderer_id: String,
    pub owner: String,
    pub version: String,
    pub id: String,
    pub style: u8,
    pub page_sizes: Vec<usize>,
    pub page_index: u8,
    pub page_count: u8,
    pub checksum: String,
    pub data: String,
}
pub(crate) fn validate_frame(f: &BubbleFrame, bytes: &[u8]) -> Result<(), String> {
    if !matches!(f.style, 1..=4)
        || (f.body_masks && (f.glyph_cells || !matches!(f.style, 2 | 4)))
        || !hex(&f.id, 16)
        || !hex(&f.checksum, 8)
        || !valid_boot(&f.boot_id)
        || !hex(&f.renderer_id, 32)
        || !hex(&f.owner, 16)
        || !hex(&f.version, 16)
        || f.connection_id
            .parse::<u64>()
            .ok()
            .filter(|n| *n > 0)
            .is_none()
        || bytes.is_empty()
        || bytes.len() > MAX_BYTES
        || f.page_sizes != vec![bytes.len()]
        || f.page_count == 0
        || f.page_count > 16
        || f.page_index >= f.page_count
        || (f.style != 2 && (f.page_count != 1 || f.page_index != 0))
    {
        return Err("BUBBLE_FALLBACK: invalid metadata".into());
    }
    let pixels = match f.style {
        1 => 474 * 38,
        2 | 4 => 496 * 54,
        _ => 348 * 38,
    };
    if f.glyph_cells {
        if f.page_index != 0 || f.page_count != 1 || bytes.len() % 40 != 0 || bytes.len() > 511 * 40
        {
            return Err("BUBBLE_FALLBACK: invalid glyph cells".into());
        }
        let mut previous = 0;
        for record in bytes.chunks_exact(40) {
            let cp = u32::from_le_bytes(record[..4].try_into().unwrap());
            if cp < 0x20 || cp <= previous || char::from_u32(cp).is_none() {
                return Err("BUBBLE_FALLBACK: invalid glyph codepoint".into());
            }
            previous = cp;
        }
    } else if bytes.iter().map(|b| usize::from(b & 15) + 1).sum::<usize>() != pixels {
        return Err("BUBBLE_FALLBACK: invalid alpha runs".into());
    }
    let sum = bytes.iter().fold(2166136261u32, |h, b| {
        (h ^ u32::from(*b)).wrapping_mul(16777619)
    });
    if format!("{sum:08x}") != f.checksum {
        return Err("BUBBLE_FALLBACK: checksum".into());
    }
    Ok(())
}
/// Common USB/LAN transaction. An ambiguous ACK terminates, never replays.
pub(crate) fn transfer_frame(
    frame: &BubbleFrame,
    bytes: &[u8],
    mut phase: impl FnMut(&str, Value) -> Result<Value, String>,
) -> Result<(), String> {
    use base64::Engine;
    let mut metadata = json!({"pageSizes":frame.page_sizes,"pageIndex":frame.page_index,
        "pageCount":frame.page_count,"checksum":frame.checksum,"totalBytes":bytes.len()});
    if frame.glyph_cells {
        metadata["glyphCells"] = true.into();
    }
    if frame.body_masks {
        metadata["bodyMasks"] = true.into();
    }
    let begin = phase("begin", metadata)?;
    if begin["cached"] == true {
        return Ok(());
    }
    if begin["offset"] != 0 {
        return Err("BUBBLE_FALLBACK: invalid READY".into());
    }
    for (index, chunk) in bytes.chunks(CHUNK_BYTES).enumerate() {
        let reply = phase(
            "chunk",
            json!({"offset":index*CHUNK_BYTES,
            "data":base64::engine::general_purpose::STANDARD.encode(chunk)}),
        )?;
        if reply["offset"] != index * CHUNK_BYTES + chunk.len() {
            return Err("BUBBLE_FALLBACK: offset ACK".into());
        }
    }
    phase("end", json!({}))?;
    Ok(())
}

pub(crate) struct WorkerLease(Arc<AtomicBool>);
impl WorkerLease {
    pub fn acquire(gate: Arc<AtomicBool>) -> Result<Self, String> {
        gate.compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .map_err(|_| "BUBBLE_BUSY: worker occupied")?;
        Ok(Self(gate))
    }
}
impl Drop for WorkerLease {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}
pub(crate) fn wire_rate(baud: u32) -> usize {
    ((baud as usize / 10) * 8 / 100).clamp(1, 12288)
}
pub(crate) fn estimated_wire_bytes(bytes: usize) -> usize {
    bytes.div_ceil(3) * 4 + bytes.div_ceil(CHUNK_BYTES) * 320 + 2048
}

#[cfg(test)]
mod tests {
    use super::*;
    fn ready() -> RenderBridge {
        let mut b = RenderBridge::default();
        assert!(b.set_sink(&"a".repeat(32), Some(Arc::new(|_| {}))));
        b
    }
    fn frame(e: &Value) -> BubbleFrame {
        let bytes = vec![15u8; 496 * 54 / 16];
        let sum = bytes.iter().fold(2166136261u32, |h, b| {
            (h ^ u32::from(*b)).wrapping_mul(16777619)
        });
        BubbleFrame {
            body_masks: false,
            glyph_cells: false,
            wifi: false,
            board_device_id: e["boardDeviceId"].as_str().unwrap().into(),
            connection_id: e["connectionId"].as_str().unwrap().into(),
            boot_id: e["bootId"].as_str().unwrap().into(),
            renderer_id: e["rendererId"].as_str().unwrap().into(),
            owner: e["owner"].as_str().unwrap().into(),
            version: e["version"].as_str().unwrap().into(),
            id: asset(4, "body"),
            style: 4,
            page_sizes: vec![bytes.len()],
            page_index: 0,
            page_count: 1,
            checksum: format!("{sum:08x}"),
            data: String::new(),
        }
    }

    #[test]
    fn glyph_cell_transaction_uses_one_native_page_and_rejects_invalid_scalars() {
        let mut f = frame(
            &json!({"boardDeviceId":"board","connectionId":"1","bootId":"1234567890abcdef",
            "rendererId":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","owner":"0000000000000001",
            "version":"0000000000000002"}),
        );
        let mut cell = [0u8; 40];
        cell[..4].copy_from_slice(&0x3400u32.to_le_bytes());
        cell[4] = 0x80;
        let sum = cell.iter().fold(2166136261u32, |h, b| {
            (h ^ u32::from(*b)).wrapping_mul(16777619)
        });
        f.glyph_cells = true;
        f.page_sizes = vec![cell.len()];
        f.page_index = 0;
        f.page_count = 1;
        f.checksum = format!("{sum:08x}");
        assert!(validate_frame(&f, &cell).is_ok());
        transfer_frame(&f, &cell, |phase, data| {
            assert_eq!(phase, "begin");
            assert_eq!(data["glyphCells"], true);
            Ok(json!({"cached":true}))
        })
        .unwrap();
        f.glyph_cells = false;
        transfer_frame(&f, &cell, |phase, data| {
            assert_eq!(phase, "begin");
            assert!(data.get("glyphCells").is_none());
            Ok(json!({"cached":true}))
        })
        .unwrap();
        f.glyph_cells = true;
        cell[..4].copy_from_slice(&0xD800u32.to_le_bytes());
        assert!(validate_frame(&f, &cell).unwrap_err().contains("codepoint"));
    }
    #[test]
    fn gray_title_is_independent_of_body_version_and_negotiated_per_peer() {
        let mut bridge = ready();
        let boot = "1234567890abcdef";
        bridge.set_format(true, true, true);
        let observe = |b: &mut RenderBridge, body: &str| {
            b.observe("speech/text", &json!({"text":body,"displayTitle":"标题"}), 1, "board", boot)
        };
        let initial = observe(&mut bridge, "A").remove(0);
        assert_eq!(initial["prefetchTitle"], true);
        assert_eq!(bridge.prefetch_probe(&initial).unwrap()["style"], 1);
        assert_eq!(bridge.metadata("speech/text", boot)["titleMasks"], true);
        let updated = observe(&mut bridge, "B").remove(0);
        assert_eq!(updated["prefetchTitle"], false);
        let mut title = frame(&updated);
        title.style = 1;
        title.id = asset(1, "标题");
        assert!(bridge.current(&title));
        let need = json!({"boot":boot,"client":title.renderer_id,"owner":title.owner,
            "version":title.version,"id":title.id,"style":1,"page":0,"text":"标题"});
        assert_eq!(bridge.need(&need, 1, "board")[0]["need"]["glyphCells"], false);
        title.glyph_cells = true;
        assert!(!bridge.current(&title));
        bridge.set_format(true, true, false);
        assert!(bridge.metadata("speech/text", boot).get("titleMasks").is_none());
        let legacy = observe(&mut bridge, "B").remove(0);
        assert!(legacy.get("prefetchTitle").is_none());
        title.owner = legacy["owner"].as_str().unwrap().into();
        title.version = legacy["version"].as_str().unwrap().into();
        assert!(bridge.current(&title));
    }
    #[test]
    fn body_masks_negotiate_per_style_and_old_mode_callbacks_cannot_revive() {
        let mut b = ready();
        b.set_format(true, true, false);
        let events = b.observe(
            "speech/text",
            &json!({"text":"body","displayTitle":"title"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let e = &events[0];
        assert_eq!(e["prefetchBody"], true);
        let metadata = b.metadata("speech/text", "1234567890abcdef");
        assert_eq!(metadata["bodyMasks"], true);
        assert_eq!(metadata["glyphCells"], true);
        let mut f = frame(e);
        f.body_masks = true;
        assert!(b.current(&f));
        f.glyph_cells = true;
        assert!(!b.current(&f));
        f.glyph_cells = false;
        let probe = b.prefetch_probe(e).unwrap();
        assert_eq!(probe["phase"], "status");
        assert_eq!(probe["id"], f.id);
        let mut need = json!({"boot":f.boot_id,"client":f.renderer_id,"owner":f.owner,
            "version":f.version,"id":f.id,"style":4,"page":0,"bodyMasks":true,"text":"body"});
        assert_eq!(b.need(&need, 1, "board").len(), 1);
        b.last_need = None;
        need["glyphCells"] = true.into();
        assert!(b.need(&need, 1, "board").is_empty());
        b.set_format(true, false, false);
        assert!(!b.current(&f));
        let old = b.observe(
            "speech/text",
            &json!({"text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        assert_eq!(old[0]["demandOnly"], true);
        assert!(old[0].get("prefetchBody").is_none());
        assert!(b
            .metadata("speech/text", "1234567890abcdef")
            .get("bodyMasks")
            .is_none());
        b.set_format(false, false, false);
        let legacy = b.observe(
            "speech/text",
            &json!({"text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        assert!(legacy[0].get("demandOnly").is_none());
    }
    #[test]
    fn gray_body_begin_is_separate_from_glyph_cells_and_keeps_original_geometry() {
        let mut b = ready();
        b.set_format(true, true, false);
        let e = b.observe(
            "speech/text",
            &json!({"text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let mut f = frame(&e[0]);
        f.body_masks = true;
        let bytes = vec![15u8; 496 * 54 / 16];
        assert!(validate_frame(&f, &bytes).is_ok());
        transfer_frame(&f, &bytes, |phase, payload| {
            assert_eq!(phase, "begin");
            assert_eq!(payload["bodyMasks"], true);
            assert!(payload.get("glyphCells").is_none());
            Ok(json!({"cached":true}))
        })
        .unwrap();
        f.glyph_cells = true;
        assert!(validate_frame(&f, &bytes).is_err());
        f.glyph_cells = false;
        f.style = 3;
        assert!(validate_frame(&f, &bytes).is_err());
    }
    #[test]
    fn selected_body_prefetch_precedes_neighbors_and_clips_to_device_key_limit() {
        let mut b = ready();
        b.set_format(true, true, false);
        let events = b.observe(
            "session/current",
            &json!({"sessionId":"b","sessions":[
            {"id":"a","title":"a","content":"left"},
            {"id":"b","title":"b","content":"selected"},
            {"id":"c","title":"c","content":"right"}]}),
            1,
            "board",
            "1234567890abcdef",
        );
        assert_eq!(events[0]["payload"]["sessionId"], "b");
        let long = "中".repeat(160);
        let event = b.observe(
            "speech/text",
            &json!({"text":long}),
            1,
            "board",
            "1234567890abcdef",
        );
        assert_eq!(
            b.prefetch_probe(&event[0]).unwrap()["id"],
            asset(4, &"中".repeat(127))
        );
    }
    #[test]
    fn incremental_same_topic_owners_do_not_erase_each_other() {
        let mut b = ready();
        let a = b.observe(
            "speech/text",
            &json!({"sessionId":"a","text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let c = b.observe(
            "speech/text",
            &json!({"sessionId":"b","text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        assert!(b.current(&frame(&a[0])) && b.current(&frame(&c[0])));
        b.observe(
            "speech/text",
            &json!({"sessionId":"a","text":"new"}),
            1,
            "board",
            "1234567890abcdef",
        );
        assert!(!b.current(&frame(&a[0])) && b.current(&frame(&c[0])));
        assert_eq!(
            b.metadata("speech/text", "1234567890abcdef")["owners"]
                .as_array()
                .unwrap()
                .len(),
            2
        );
    }
    #[test]
    fn same_connection_boot_change_and_aba_versions_never_revive() {
        let mut b = ready();
        let e = b.observe(
            "speech/text",
            &json!({"text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let old = frame(&e[0]);
        b.observe(
            "speech/text",
            &json!({"text":"different"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let current = b.observe(
            "speech/text",
            &json!({"text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        assert!(!b.current(&old));
        assert_ne!(old.version, current[0]["version"].as_str().unwrap());
        b.observe(
            "speech/text",
            &json!({"text":"body"}),
            1,
            "board",
            "ffffffffffffffff",
        );
        assert!(!b.current(&old));
    }
    #[test]
    fn high_u64_connections_are_decimal_strings_and_old_disposer_is_ignored() {
        let mut b = ready();
        let e = b.observe(
            "speech/text",
            &json!({"text":"body"}),
            u64::MAX,
            "board",
            "1234567890abcdef",
        );
        assert_eq!(e[0]["connectionId"], u64::MAX.to_string());
        assert!(b.set_sink(&"b".repeat(32), Some(Arc::new(|_| {}))));
        assert!(!b.set_sink(&"a".repeat(32), None));
        assert!(b.sink().is_some());
        assert!(!b.current(&frame(&e[0])));
    }
    #[test]
    fn descriptors_are_bounded_and_retirement_does_not_replay_old_peer() {
        let mut b = ready();
        for n in 0..100 {
            b.observe(
                "speech/text",
                &json!({"sessionId":n.to_string(),"text":"x".repeat(10000)}),
                1,
                "board",
                "1234567890abcdef",
            );
        }
        assert_eq!(b.recent.len(), 12);
        assert!(b
            .recent
            .iter()
            .all(|d| d.payload["text"].as_str().unwrap().len() == 511));
        b.clear_peer();
        assert!(b.recent.is_empty());
    }
    #[test]
    fn alpha_geometry_single_page_checksum_and_limits_fail_closed() {
        let mut b = ready();
        let e = b.observe(
            "speech/text",
            &json!({"text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let mut f = frame(&e[0]);
        let bytes = vec![15u8; 496 * 54 / 16];
        assert!(validate_frame(&f, &bytes).is_ok());
        f.page_sizes = vec![usize::MAX];
        assert!(validate_frame(&f, &bytes).is_err());
        f = frame(&e[0]);
        f.checksum = "00000000".into();
        assert!(validate_frame(&f, &bytes).is_err());
        f = frame(&e[0]);
        f.page_count = 2;
        assert!(validate_frame(&f, &bytes).is_err());
        assert!(validate_frame(&frame(&e[0]), &vec![255; MAX_BYTES + 1]).is_err());
    }
    #[test]
    fn final_wire_token_rejects_warm_boot_renderer_and_content_changes() {
        let mut b = ready();
        let e = b.observe(
            "speech/text",
            &json!({"text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let f = frame(&e[0]);
        let mut p = json!({"client":f.renderer_id,"boot":f.boot_id,"owner":f.owner,"version":f.version,"style":4,"id":f.id});
        assert!(b.current_wire(&p, 1, "board", "1234567890abcdef"));
        assert!(!b.current_wire(&p, 1, "board", "ffffffffffffffff"));
        assert!(!b.current_wire(&p, 2, "board", "1234567890abcdef"));
        p["version"] = json!("f".repeat(16));
        assert!(!b.current_wire(&p, 1, "board", "1234567890abcdef"));
    }
    #[test]
    fn device_missing_requires_current_peer_asset_and_is_rate_bounded() {
        let mut b = ready();
        let e = b.observe(
            "speech/text",
            &json!({"text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let f = frame(&e[0]);
        let mut p = json!({"boot":f.boot_id,"client":f.renderer_id,"id":f.id,"style":4,"page":0});
        assert!(b.need(&p, 2, "board").is_empty());
        p["page"] = json!(1);
        assert!(b.need(&p, 1, "board").is_empty());
        p["page"] = json!(0);
        let events = b.need(&p, 1, "board");
        assert_eq!(events.len(), 1);
        assert_eq!(events[0]["version"], f.version);
        assert!(b.need(&p, 1, "board").is_empty());
    }
    #[test]
    fn renderer_replacement_warms_only_the_current_peer_and_latest_text() {
        let mut b = ready();
        let boot = "1234567890abcdef";
        let update = |value| json!({"sessionId":"a","text":value});
        b.observe("speech/text", &update("old"), 1, "board", boot);
        let newest = b.observe("speech/text", &update("latest"), 1, "board", boot);
        let mut f = frame(&newest[0]);
        f.id = asset(4, "latest");
        let next = "b".repeat(32);
        assert!(b.set_sink(&next, Some(Arc::new(|_| {}))));
        let need = json!({"boot":boot,"client":next,"id":f.id,"style":4,"page":0,
            "owner":f.owner,"version":f.version,"text":"latest"});
        assert!(b.need(&need, 1, "board").is_empty());
        let events = b.observe("speech/text", &update("latest"), 1, "board", boot);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0]["rendererId"], next);
        assert_eq!(events[0]["payload"]["text"], "latest");
        assert_eq!(b.need(&need, 1, "board")[0]["topic"], "device/need");
        assert!(!b.current(&f));

        assert!(b.set_sink(&"c".repeat(32), Some(Arc::new(|_| {}))));
        let events = b.observe(
            "speech/text",
            &update("after reboot"),
            2,
            "board",
            "ffffffffffffffff",
        );
        assert_eq!(events.len(), 1);
        assert_eq!(events[0]["payload"]["text"], "after reboot");
        assert_eq!(events[0]["connectionId"], "2");
    }
    #[test]
    fn one_display_owner_follows_state_to_speech_without_stale_parallel_tokens() {
        let mut b = ready();
        let old = b.observe(
            "state/working",
            &json!({"sessionId":"a","text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let f = frame(&old[0]);
        let new = b.observe(
            "speech/text",
            &json!({"sessionId":"a","text":"new"}),
            1,
            "board",
            "1234567890abcdef",
        );
        assert_eq!(new[0]["owner"], f.owner);
        assert!(!b.current(&f));
        assert_eq!(
            b.metadata("state/done", "1234567890abcdef")["owners"]
                .as_array()
                .unwrap()
                .len(),
            1
        );
    }
    #[test]
    fn worker_admission_is_single_and_drop_releases_on_failure() {
        let gate = Arc::new(AtomicBool::new(false));
        let held = WorkerLease::acquire(gate.clone()).unwrap();
        assert!(WorkerLease::acquire(gate.clone()).is_err());
        drop(held);
        assert!(WorkerLease::acquire(gate).is_ok());
    }
    #[test]
    fn baud_budget_declines_assets_that_cannot_finish_and_has_a_fixed_ceiling() {
        assert_eq!(wire_rate(115200), 921);
        assert_eq!(wire_rate(4_000_000), 12288);
        assert!(estimated_wire_bytes(MAX_BYTES) > wire_rate(115200) * 10);
        assert!(estimated_wire_bytes(MAX_BYTES) < wire_rate(4_000_000) * 10);
    }
    #[test]
    fn manifest_is_the_complete_bounded_authority_snapshot_across_roles() {
        let mut b = ready();
        b.observe(
            "speech/text",
            &json!({"sessionId":"a","text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        b.observe(
            "ui/conversation",
            &json!({"conversationId":"b","text":"body"}),
            1,
            "board",
            "1234567890abcdef",
        );
        let m = b.metadata("ui/conversation", "1234567890abcdef");
        assert_eq!(m["owners"].as_array().unwrap().len(), 2);
    }
}
