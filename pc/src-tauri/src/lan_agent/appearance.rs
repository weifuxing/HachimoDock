//! Trusted LAN delivery of the same prepared appearance packs used by USB.
use super::{
    exchange_response_with_timeout, query_status, read_with_recovery, require_no_usb, Peer,
};
use crate::usb_serial::{
    appearance_transaction::{
        asset_checksum_hex, build_asset_file_commit_payload, p4_builtin_slot,
        parse_p4_appearance_slot_state, verified_p4_payload_snapshot, P4AppearanceSlot,
        P4AppearanceSlotState, P4_BUILTIN_APPEARANCE_SLOT, P4_RAW_APPEARANCE_SLOT,
    },
    prepared_p4_lan_files, transfer_log, UsbSerialManager,
};
use base64::{engine::general_purpose::STANDARD, Engine as _};
use serde_json::{json, Value};
use std::io::Read;
use std::path::Path;
use std::time::{Duration, Instant};
use tauri::{Emitter, Manager};

const MODE: u8 = 18;
const CHUNK_BYTES: usize = 8192;
const REPLY_MAX: usize = 3072;
const RESULT_UNKNOWN: &str = "Wi-Fi 未能确认形象切换结果，请先查看设备当前形象。";
const NOT_READY: &str = "设备暂未接受无线形象操作，请确认设备空闲后再试。";
const PREPARED_ASSET_ERROR: &str = "无法完整读取已准备的形象素材，设备形象未改动。";

#[derive(Debug, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) enum AppearanceSyncOutcome {
    AlreadyActive,
    Activated,
    Uploaded,
}

#[derive(Debug, PartialEq, Eq)]
enum AppearancePlan<'a> {
    Keep,
    Activate(&'a P4AppearanceSlot),
    Upload,
}

fn appearance_plan<'a>(
    slots: &'a P4AppearanceSlotState,
    custom_pack: Option<&str>,
) -> Result<AppearancePlan<'a>, String> {
    let target = match custom_pack {
        None => Some(p4_builtin_slot(slots)?),
        Some(pack) => slots
            .slots
            .iter()
            .find(|slot| slot.slot == P4_RAW_APPEARANCE_SLOT && slot.pack_id == pack),
    };
    Ok(match target {
        Some(slot) if slots.active_slot == Some(slot.slot) => AppearancePlan::Keep,
        Some(slot) => AppearancePlan::Activate(slot),
        None => AppearancePlan::Upload,
    })
}

fn confirmed_active_pack(slots: &P4AppearanceSlotState, slot: u32, pack_id: &str) -> bool {
    slots.active_slot == Some(slot)
        && slots
            .slots
            .iter()
            .any(|candidate| candidate.slot == slot && candidate.pack_id == pack_id)
}

#[derive(Clone, Copy)]
#[repr(u8)]
enum Phase {
    Slots,
    Activate,
    Begin,
    Chunk,
    File,
    Commit,
}

impl Phase {
    fn label(self) -> &'static str {
        match self {
            Self::Slots => "slot-query",
            Self::Activate => "activate",
            Self::Begin => "begin",
            Self::Chunk => "chunk",
            Self::File => "file",
            Self::Commit => "commit",
        }
    }
}

struct Transfer<'a> {
    peer: &'a Peer,
    board: &'a str,
    boot: &'a str,
    capacity: u32,
    id: String,
    usb: &'a UsbSerialManager,
}

impl Transfer<'_> {
    fn request(&self, phase: Phase, payload: Value) -> Result<Value, String> {
        let reply = self.exchange_phase(phase, payload);
        if reply.is_err() {
            transfer_log::record(
                "appearance",
                "phase_send_failed",
                json!({
                    "transport": "wifi", "phase": phase.label(),
                }),
            );
        } else if matches!(phase, Phase::Activate | Phase::Begin | Phase::Commit) {
            transfer_log::record(
                "appearance",
                "phase_ack",
                json!({
                    "transport": "wifi", "phase": phase.label(),
                }),
            );
        }
        reply
    }

    fn exchange_phase(&self, phase: Phase, payload: Value) -> Result<Value, String> {
        require_no_usb(self.usb)?;
        self.usb.ensure_appearance_sync_not_cancelled(None)?;
        let mut body = self.boot.as_bytes().to_vec();
        body.push(phase as u8);
        serde_json::to_writer(&mut body, &payload).map_err(|_| NOT_READY)?;
        let timeout = match phase {
            Phase::Begin | Phase::Activate | Phase::Commit => Duration::from_secs(58),
            _ => Duration::from_secs(6),
        };
        let reason = match phase {
            Phase::Activate | Phase::Commit => RESULT_UNKNOWN,
            Phase::Slots => "无法确认设备形象槽位，尚未发送素材。",
            _ => "Wi-Fi 形象传输已中断，尚未提交新形象。",
        };
        let bytes =
            exchange_response_with_timeout(self.peer, MODE, &body, Some(REPLY_MAX), timeout)
                .map_err(|_| reason)?;
        if bytes == [0] {
            return Err(match phase {
                Phase::Activate | Phase::Commit => RESULT_UNKNOWN,
                _ => NOT_READY,
            }
            .into());
        }
        let reply: Value = serde_json::from_slice(&bytes).map_err(|_| reason)?;
        let ack = &reply["payload"];
        if reply["topic"] != "asset/ack"
            || ack["phase"] != phase.label()
            || ack["transferId"] != self.id
            || (matches!(phase, Phase::Chunk | Phase::File) && ack["path"] != payload["path"])
            || (matches!(phase, Phase::Chunk) && ack["index"] != payload["index"])
        {
            return Err(reason.into());
        }
        if !matches!(phase, Phase::Slots) && ack["ok"] != true {
            return Err(if ack["errorCode"] == "appearance_query_outcome_unknown"
                || ack["error"]
                    .as_str()
                    .is_some_and(|error| error.contains("unknown"))
            {
                RESULT_UNKNOWN
            } else {
                NOT_READY
            }
            .into());
        }
        Ok(ack.clone())
    }

    fn slots(&self) -> Result<P4AppearanceSlotState, String> {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            let nonce = uuid::Uuid::new_v4().to_string();
            let ack = self.request(
                Phase::Slots,
                json!({
                    "transferId": self.id, "queryNonce": nonce, "expectedBoardDeviceId": self.board,
                    "expectedBootId": self.boot, "observationSchema": "p4-appearance-quiescent-v1",
                }),
            )?;
            if ack["serialized"] != true
                || ack["queryNonce"] != nonce
                || ack["boardDeviceId"] != self.board
                || ack["bootId"] != self.boot
                || ack["observationSchema"] != "p4-appearance-quiescent-v1"
            {
                return Err(NOT_READY.into());
            }
            if ack["errorCode"] == "appearance_query_outcome_unknown" {
                return Err(RESULT_UNKNOWN.into());
            }
            if ack["ok"] == true && ack["quiescent"] == true && ack["snapshotChanged"] == false {
                return parse_p4_appearance_slot_state(&ack).map_err(|_| NOT_READY.into());
            }
            // Retry only read-only, scoped reports that explicitly indicate transient work.
            let busy = ack["errorCode"] == "appearance_query_busy"
                || (ack["ok"] == true && ack["quiescent"] == false);
            if !busy || Instant::now() >= deadline {
                return Err(NOT_READY.into());
            }
            std::thread::sleep(Duration::from_millis(200));
        }
    }

    fn confirm_active(&self, slot: u32, pack_id: &str) -> Result<(), String> {
        let observed = self.slots().map_err(|_| RESULT_UNKNOWN)?;
        if !confirmed_active_pack(&observed, slot, pack_id) {
            return Err(RESULT_UNKNOWN.into());
        }
        Ok(())
    }

    fn activate(&self, slot: &P4AppearanceSlot) -> Result<(), String> {
        match self.request(
            Phase::Activate,
            json!({ "transferId": self.id, "slot": slot.slot, "packId": slot.pack_id }),
        ) {
            Ok(_) => self.confirm_active(slot.slot, &slot.pack_id),
            // A reply can be lost after publication. Re-read the device, never resend.
            Err(error) if error == RESULT_UNKNOWN => self.confirm_active(slot.slot, &slot.pack_id),
            Err(error) => Err(error),
        }
    }

    fn sync(
        &self,
        dir: &Path,
        builtin: bool,
        app: &tauri::AppHandle,
    ) -> Result<AppearanceSyncOutcome, String> {
        let prepared = if builtin {
            None
        } else {
            Some(prepared_p4_lan_files(dir)?)
        };
        let slots = self.slots()?;
        match appearance_plan(
            &slots,
            prepared.as_ref().map(|(pack, _)| pack.pack_id.as_str()),
        )? {
            AppearancePlan::Keep => return Ok(AppearanceSyncOutcome::AlreadyActive),
            AppearancePlan::Activate(slot) => {
                let _ = app.emit("wifi-appearance-progress", json!({ "stage": "activating" }));
                self.activate(slot)?;
                return Ok(AppearanceSyncOutcome::Activated);
            }
            AppearancePlan::Upload => {}
        }

        let (pack, files) = prepared.ok_or(NOT_READY)?;
        let factory = p4_builtin_slot(&slots)?;
        let _ = app.emit("wifi-appearance-progress", json!({ "stage": "preparing" }));
        let capacity = u64::from(self.capacity);
        let mut total = 0_u64;
        let mut raw = 0_u64;
        let mut payloads = Vec::with_capacity(files.len());
        // Fully read the upload before activating the factory fallback or erasing slot 1.
        for (path, file) in files {
            self.usb.ensure_appearance_sync_not_cancelled(None)?;
            let input = std::fs::File::open(file).map_err(|_| PREPARED_ASSET_ERROR)?;
            let length = input.metadata().map_err(|_| PREPARED_ASSET_ERROR)?.len();
            total = total.checked_add(length).ok_or(NOT_READY)?;
            if path.ends_with(".h264") || path.ends_with(".mjpg") {
                raw = raw
                    .next_multiple_of(4)
                    .checked_add(length)
                    .ok_or(NOT_READY)?;
            }
            if length == 0 || raw > capacity || total > capacity + 2 * 1024 * 1024 {
                return Err("形象素材超出设备的可用空间，未发送素材。".into());
            }
            let mut bytes = Vec::new();
            input
                .take(length + 1)
                .read_to_end(&mut bytes)
                .map_err(|_| PREPARED_ASSET_ERROR)?;
            if bytes.len() as u64 != length {
                return Err(PREPARED_ASSET_ERROR.into());
            }
            payloads.push((path, bytes));
        }
        if total != pack.byte_count || !verified_p4_payload_snapshot(&payloads, &pack.pack_id) {
            return Err(PREPARED_ASSET_ERROR.into());
        }
        if slots.active_slot != Some(P4_BUILTIN_APPEARANCE_SLOT) {
            self.activate(factory)?;
        }
        transfer_log::record(
            "appearance",
            "transaction_started",
            json!({
                "transport": "wifi", "totalBytes": total, "rawBytes": raw, "fileCount": payloads.len(),
            }),
        );
        self.request(
            Phase::Begin,
            json!({ "transferId": self.id, "totalBytes": total, "rawBytes": raw }),
        )?;
        let mut sent = 0;
        for (file_no, (path, data)) in payloads.iter().enumerate() {
            for (index, chunk) in data.chunks(CHUNK_BYTES).enumerate() {
                self.request(
                    Phase::Chunk,
                    json!({
                        "transferId": self.id, "path": path, "index": index.to_string(),
                        "size": chunk.len(), "data": STANDARD.encode(chunk),
                    }),
                )?;
                sent += chunk.len() as u64;
                let _ = app.emit(
                    "wifi-appearance-progress",
                    json!({
                        "currentFile": file_no + 1, "totalFiles": payloads.len(),
                        "bytesSent": sent, "bytesTotal": total,
                    }),
                );
            }
            self.request(
                Phase::File,
                build_asset_file_commit_payload(
                    &self.id,
                    path,
                    data.len() as u64,
                    &asset_checksum_hex(data),
                    data.len().div_ceil(CHUNK_BYTES) as u64,
                ),
            )?;
        }
        match self.request(
            Phase::Commit,
            json!({ "transferId": self.id, "fileCount": payloads.len(), "totalBytes": total }),
        ) {
            Ok(_) => {}
            Err(error) if error == RESULT_UNKNOWN => {}
            Err(error) => return Err(error),
        }
        self.confirm_active(P4_RAW_APPEARANCE_SLOT, &pack.pack_id)?;
        transfer_log::record(
            "appearance",
            "transaction_committed",
            json!({
                "transport": "wifi", "bytes": total, "files": payloads.len(),
            }),
        );
        Ok(AppearanceSyncOutcome::Uploaded)
    }
}

#[tauri::command]
pub(crate) async fn lan_sync_appearance(
    app: tauri::AppHandle,
    usb_manager: tauri::State<'_, UsbSerialManager>,
    appearance_id: String,
    board_device_id: String,
    boot_id: String,
) -> Result<AppearanceSyncOutcome, String> {
    let appearance_id = crate::validate_appearance_id(&appearance_id)?.to_string();
    let dir = app
        .path()
        .app_local_data_dir()
        .map_err(|_| "无法读取本机形象库。")?
        .join("custom-appearances")
        .join(&appearance_id);
    let usb = usb_manager.inner().clone();
    require_no_usb(&usb)?;
    usb.begin_appearance_sync()?;
    let worker = usb.clone();
    let result = tauri::async_runtime::spawn_blocking(move || {
        let _foreground = super::widget_data::foreground()?;
        let (peer, status) = read_with_recovery(query_status, "无法读取已配对的 Wi-Fi 设备。")?;
        if status.boot_id != boot_id
            || status.board_device_id != board_device_id
            || !status.wifi_connected
        {
            return Err("Wi-Fi 设备已变更，请确认连接后再切换形象。".into());
        }
        if status.appearance_capacity == 0 {
            return Err("当前固件需要升级后才能通过 Wi-Fi 切换形象。".into());
        }
        if !status.control_trusted {
            return Err(
                "请通过 USB 连接同一台设备一次，授权无线控制后即可通过 Wi-Fi 切换形象。".into(),
            );
        }
        Transfer {
            peer: &peer,
            board: &board_device_id,
            boot: &boot_id,
            capacity: status.appearance_capacity,
            id: uuid::Uuid::new_v4().to_string(),
            usb: &worker,
        }
        .sync(&dir, appearance_id == "builtin-terrier", &app)
    })
    .await;
    usb.finish_appearance_sync();
    result.map_err(|_| "Wi-Fi 形象同步任务未完成。")?
}

#[cfg(test)]
mod tests {
    use super::*;

    fn slots(active_slot: u32, has_custom: bool) -> P4AppearanceSlotState {
        let mut slots = vec![P4AppearanceSlot {
            slot: P4_BUILTIN_APPEARANCE_SLOT,
            pack_id: "factory".into(),
        }];
        if has_custom {
            slots.push(P4AppearanceSlot {
                slot: P4_RAW_APPEARANCE_SLOT,
                pack_id: "cached".into(),
            });
        }
        P4AppearanceSlotState {
            active_slot: Some(active_slot),
            slots,
        }
    }

    #[test]
    fn builtin_is_only_checked_or_activated_even_when_custom_slot_is_invalid() {
        let active = slots(P4_BUILTIN_APPEARANCE_SLOT, false);
        assert_eq!(
            appearance_plan(&active, None).unwrap(),
            AppearancePlan::Keep
        );
        let inactive = slots(P4_RAW_APPEARANCE_SLOT, true);
        assert_eq!(
            appearance_plan(&inactive, None).unwrap(),
            AppearancePlan::Activate(&inactive.slots[0])
        );
        assert!(appearance_plan(
            &P4AppearanceSlotState {
                active_slot: None,
                slots: vec![]
            },
            None
        )
        .is_err());
    }

    #[test]
    fn custom_upload_depends_on_the_device_pack_and_active_slot() {
        let active = slots(P4_RAW_APPEARANCE_SLOT, true);
        assert_eq!(
            appearance_plan(&active, Some("cached")).unwrap(),
            AppearancePlan::Keep
        );
        let inactive = slots(P4_BUILTIN_APPEARANCE_SLOT, true);
        assert_eq!(
            appearance_plan(&inactive, Some("cached")).unwrap(),
            AppearancePlan::Activate(&inactive.slots[1])
        );
        assert_eq!(
            appearance_plan(&inactive, Some("other")).unwrap(),
            AppearancePlan::Upload
        );
        assert_eq!(
            appearance_plan(&slots(P4_BUILTIN_APPEARANCE_SLOT, false), Some("cached")).unwrap(),
            AppearancePlan::Upload
        );
        assert!(!confirmed_active_pack(
            &inactive,
            P4_RAW_APPEARANCE_SLOT,
            "cached"
        ));
        assert!(!confirmed_active_pack(
            &active,
            P4_RAW_APPEARANCE_SLOT,
            "other"
        ));
        assert!(confirmed_active_pack(
            &active,
            P4_RAW_APPEARANCE_SLOT,
            "cached"
        ));
    }
}
