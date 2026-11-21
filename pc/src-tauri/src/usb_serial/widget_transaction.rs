/*
 * [Input] Widget package paths/JSON bytes, bounded PNG sprite sheets, device capabilities, and chunk metadata.
 * [Output] P4-safe widget payloads with legacy package-navigation bindings removed,
 *          frame-contiguous RGB565-alpha sprite compilation, capability gates, path checks,
 *          and OTA timing policy.
 * [Pos] Pure component/widget transaction contract beneath usb_serial.rs.
 * [Sync] If this file changes, update `pc/.folder.md`.
 */

use super::UsbConnectionStatus;
use crate::clawpkg::validate_p4_vars_object_capped;
use serde_json::Value;
use std::path::Path;
use std::time::Duration;

pub(super) const WIDGET_BEGIN_ACK_TIMEOUT: Duration = Duration::from_secs(2);
pub(super) const WIDGET_CHUNK_ACK_TIMEOUT: Duration = Duration::from_secs(2);
// P4 may spend several seconds syncing its flash-backed widget catalog and
// refreshing the active mini-app before the final acknowledgement reaches the host.
// A/B catalog + image generations can trigger SPIFFS GC during durable commit.
// Real full-resolution app packages approach/exceed 15s after repeated updates.
// Wait for the original transaction only; never retransmit an uncertain commit.
pub(super) const WIDGET_COMMIT_ACK_TIMEOUT: Duration = Duration::from_secs(45);
pub(super) const WIDGET_DELETE_ACK_TIMEOUT: Duration = Duration::from_secs(15);
pub(super) const WIDGET_CHUNK_MAX_ATTEMPTS: usize = 3;
pub(super) const P4_WIDGET_JSON_MAX_BYTES: usize = 4095;
const P4_BUTTONS_JSON_MAX_BYTES: usize = 2047;
const P4_SCENE_MAX_SPRITES: usize = 4;
const P4_SCENE_MAX_SPRITE_PIXELS: usize = 4096;
const P4_SCENE_MAX_SPRITE_SOURCE_BYTES: usize = 128 * 1024;
const P4_SCENE_SPRITE_MAGIC: &[u8; 4] = b"P4S1";
const WIDGET_DELETE_CAPABILITY: &str = "widgetDelete";
const WIDGET_INVENTORY_CAPABILITY: &str = "widgetInventory";

pub(super) fn build_widget_begin_payload(transfer_id:&str,widget_id:&str,repair:bool,caps:&Value)->Value{
    let mut payload=serde_json::json!({"transferId":transfer_id,"widgetId":widget_id,"repairResources":repair});
    if !repair&&caps["widgetPreserveUnchanged"]=="p4-preserve-unchanged-v1"{
        payload["preserveUnchanged"]=serde_json::json!(true);
    }
    payload
}

#[test]
fn unchanged_install_begin_requires_exact_negotiation_and_never_changes_repairs(){
    let legacy=serde_json::json!({"transferId":"transfer","widgetId":"owned","repairResources":false});
    for caps in [Value::Null,serde_json::json!({}),serde_json::json!({"widgetPreserveUnchanged":true}),serde_json::json!({"widgetPreserveUnchanged":"other-engine"})]{
        assert_eq!(build_widget_begin_payload("transfer","owned",false,&caps),legacy);
    }
    let caps=serde_json::json!({"widgetPreserveUnchanged":"p4-preserve-unchanged-v1"});
    let mut opted=legacy.clone();opted["preserveUnchanged"]=serde_json::json!(true);
    assert_eq!(build_widget_begin_payload("transfer","owned",false,&caps),opted);
    let mut repair=legacy;repair["repairResources"]=serde_json::json!(true);
    assert_eq!(build_widget_begin_payload("transfer","owned",true,&caps),repair);
}

pub(super) fn build_widget_chunk_payload(
    transfer_id: &str,
    relative_path: &str,
    data_base64: &str,
    index: u32,
    decoded_size: usize,
    checksum: &str,
) -> Value {
    serde_json::json!({
        "transferId": transfer_id,
        "path": relative_path,
        "data": data_base64,
        "index": index.to_string(),
        "decodedSize": decoded_size,
        "checksum": checksum,
    })
}

pub(super) const WIDGET_FILE_TRANSFER: &str = "p4-widget-files-v2";
pub(super) const WIDGET_FILE_CHUNK_BYTES: usize = 4096;
const P4_WIDGET_LINE_MAX: usize = 32767;

// R203 accepts the sorted v2 wire manifest but persists images and sprites
// by declaration index. Use the ordered legacy path when sorting would move them.
pub(super) fn supports_sorted_widget_transfer(caps: &Value, entries: &[(String, Vec<u8>)]) -> bool {
    if caps["widgetTransfer"] != WIDGET_FILE_TRANSFER {
        return false;
    }
    if caps["widgetResourceOrder"] == "p4-widget-resource-id-v1" {
        return true;
    }
    let mut prior = "";
    for (path, _) in entries {
        if path.starts_with("runtime/images/") || path.starts_with("runtime/sprites/") {
            if path.as_str() <= prior {
                return false;
            }
            prior = path;
        }
    }
    true
}

#[test]
fn r203_keeps_nonlexical_image_and_keyframe_order_without_changing_the_package() {
    let entries = ["runtime/images/bg.p4i", "runtime/images/gold.p4i", "runtime/images/pink.p4i",
        "runtime/images/envelope.p4i"].map(|path| (path.to_owned(), vec![0; 40000]));
    let old = serde_json::json!({"widgetTransfer": WIDGET_FILE_TRANSFER});
    assert!(!supports_sorted_widget_transfer(&old, &entries));
    assert!(check_legacy_widget_frames("widget-install-12345678-1234-1234-1234-123456789abc", &entries).is_ok());
    assert!(supports_sorted_widget_transfer(&old, &entries[..3]));
    let new = serde_json::json!({"widgetTransfer": WIDGET_FILE_TRANSFER,
        "widgetResourceOrder": "p4-widget-resource-id-v1"});
    assert!(supports_sorted_widget_transfer(&new, &entries));
    let keyframes = ["runtime/images/bg.p4i", "runtime/images/gold.p4i", "runtime/images/bg.2.p4i"]
        .map(|path| (path.to_owned(), vec![0; 1]));
    assert!(!supports_sorted_widget_transfer(&old, &keyframes));
}

pub(super) fn check_wasm_device_budget(bytes: &[u8], capabilities: &Value) -> Result<(), String> {
    let (_, _, pages, _, audio) = crate::clawpkg::wasm_profile_details(bytes)
        .ok_or("WASM 文件或固定内存无效")?;
    if audio && (capabilities["widgetWasmAudio"] != crate::wasm_audio::PROFILE
        || capabilities["widgetTransfer"] != WIDGET_FILE_TRANSFER
        || capabilities["widgetResourceOrder"] != "p4-widget-resource-id-v1")
    {
        return Err("该组件需要支持 PetUI 离线语音的固件，请先更新固件".into());
    }
    let max_bytes = capabilities["widgetWasmMaxBytes"].as_u64().unwrap_or(64 * 1024);
    let max_pages = capabilities["widgetWasmMemoryPages"].as_u64().unwrap_or(2);
    if bytes.len() as u64 > max_bytes || u64::from(pages) > max_pages {
        return Err(format!(
            "此组件需要 {} KiB 的 WASM 文件空间和 {} KiB 的固定内存，设备目前支持 {} KiB 和 {} KiB。请先更新固件。",
            bytes.len().div_ceil(1024), u64::from(pages) * 64,
            max_bytes / 1024, max_pages.saturating_mul(64)
        ));
    }
    Ok(())
}

#[test]
fn expanded_wasm_requires_both_advertised_device_budgets() {
    let mut module = b"\0asm\x01\0\0\0\x05\x04\x01\x01\x02\x02".to_vec();
    let legacy = serde_json::json!({});
    let expanded = serde_json::json!({"widgetWasmMaxBytes": 192 * 1024, "widgetWasmMemoryPages": 4});
    assert!(check_wasm_device_budget(&module, &legacy).is_ok());
    module[12..14].copy_from_slice(&[4, 4]);
    assert!(check_wasm_device_budget(&module, &legacy).is_err());
    assert!(check_wasm_device_budget(&module, &expanded).is_ok());
    module[12..14].copy_from_slice(&[2, 2]);
    // Custom section of 65,536 bytes, including an empty section name.
    module.extend([0, 0x80, 0x80, 0x04]);
    module.resize(module.len() + 65536, 0);
    assert!(check_wasm_device_budget(&module, &legacy).is_err());
    assert!(check_wasm_device_budget(&module, &expanded).is_ok());
}

#[test]
fn speech_module_requires_audio_and_the_file_transaction() {
    let module = b"\0asm\x01\0\0\0\x02\x10\x01\x05hachi\x06sample\0\0\x05\x04\x01\x01\x02\x02";
    let mut capabilities = serde_json::json!({"widgetWasmAudio": crate::wasm_audio::PROFILE});
    assert!(check_wasm_device_budget(module, &capabilities).is_err());
    capabilities["widgetTransfer"] = serde_json::json!(WIDGET_FILE_TRANSFER);
    assert!(check_wasm_device_budget(module, &capabilities).is_err());
    capabilities["widgetResourceOrder"] = serde_json::json!("p4-widget-resource-id-v1");
    assert!(check_wasm_device_budget(module, &capabilities).is_ok());
    capabilities["widgetWasmAudio"] = serde_json::json!("other");
    assert!(check_wasm_device_budget(module, &capabilities).is_err());
}

pub(super) fn legacy_chunk_bytes(path: &str, size: usize) -> usize {
    if matches!(path, "runtime/app.wasm" | "runtime/center-title.rle") {
        2046
    } else if ["runtime/images/", "runtime/samples/", "runtime/clips/", "runtime/big/"]
        .iter().any(|prefix| path.starts_with(prefix))
    {
        WIDGET_FILE_CHUNK_BYTES
    } else {
        size.max(1)
    }
}

pub(super) fn check_widget_wire_frame(path: &str, payload: &Value, encoded_bytes: usize) -> Result<(), String> {
    let line = serde_json::to_vec(&serde_json::json!({"topic":"widget/chunk","payload":payload}))
        .map_err(|e| e.to_string())?;
    if line.len() + encoded_bytes > P4_WIDGET_LINE_MAX {
        return Err(format!(
            "组件资源 {path} 的 USB 消息需要 {} 字节，超过当前协议允许的 {} 字节。请升级支持分块的设备固件，或减小该资源。",
            line.len() + encoded_bytes, P4_WIDGET_LINE_MAX
        ));
    }
    Ok(())
}

pub(super) fn check_legacy_widget_frames(transfer_id: &str, entries: &[(String, Vec<u8>)]) -> Result<(), String> {
    for (path, bytes) in entries {
        if bytes.is_empty() {
            continue;
        }
        let limit = legacy_chunk_bytes(path, bytes.len());
        let size = bytes.len().min(limit);
        let index = (bytes.len().div_ceil(limit) - 1) as u32;
        let payload = build_widget_chunk_payload(transfer_id, path, "", index, size, "0000000000000000");
        check_widget_wire_frame(path, &payload, size.div_ceil(3) * 4)?;
    }
    Ok(())
}

pub(crate) fn widget_package_digest(entries: &[(String, Vec<u8>)]) -> Result<(String, usize), String> {
    use sha2::{Digest, Sha256};

    let total: usize = entries.iter().map(|(_, bytes)| bytes.len()).sum();
    if entries.is_empty() || entries.len() > 128 || total > 16 * 1024 * 1024 {
        return Err("组件下发清单超过设备支持的文件数或总大小".into());
    }
    let mut previous = "";
    let mut hash = Sha256::new();
    for (path, bytes) in entries {
        if path.as_str() <= previous || path.len() >= 96 || bytes.is_empty() {
            return Err(format!("组件下发清单含无效或重复资源 {path}"));
        }
        previous = path;
        hash.update(path.as_bytes());
        hash.update([0]);
        hash.update((bytes.len() as u32).to_le_bytes());
        hash.update(bytes);
    }
    Ok((format!("{:x}", hash.finalize()), total))
}

#[test]
fn widget_wire_preflight_rejects_large_legacy_sprites_but_accepts_chunked_images() {
    let data = vec![0; 8 + 64 * 64 * 2 * 3];
    let id = "widget-install-12345678-1234-1234-1234-123456789abc";
    let entries = vec![("runtime/sprites/letter.p4s".to_owned(), data.clone())];
    let error = check_legacy_widget_frames(id, &entries).unwrap_err();
    assert!(error.contains("runtime/sprites/letter.p4s") && error.contains("32767"));
    assert!(check_legacy_widget_frames(id, &[("runtime/images/letter.p4i".to_owned(), data)]).is_ok());
}

#[test]
fn widget_package_digest_matches_the_wire_manifest_and_rejects_duplicate_paths() {
    let entries = vec![("a".to_owned(), vec![1, 2]), ("b".to_owned(), vec![3])];
    let (digest, size) = widget_package_digest(&entries).unwrap();
    assert_eq!(size, 3);
    assert_eq!(digest, "e0ee70a90bd0b67a166f761a28b65568e7a7492970f8061a5dd12ad15c91259d");
    assert!(widget_package_digest(&[entries[0].clone(), entries[0].clone()]).is_err());
}

pub(super) fn format_widget_ack_timeout(transfer_id: &str, phase: &str) -> String {
    format!(
        "未收到板端组件 OTA 确认: transferId={} phase={}",
        transfer_id, phase
    )
}

pub(super) fn ensure_widget_delete_supported(status: &UsbConnectionStatus) -> Result<(), String> {
    ensure_p4_widget_capability(status, WIDGET_DELETE_CAPABILITY, "组件删除")
}

pub(super) fn ensure_widget_inventory_supported(
    status: &UsbConnectionStatus,
) -> Result<(), String> {
    ensure_p4_widget_capability(status, WIDGET_INVENTORY_CAPABILITY, "组件清单")
}

fn ensure_p4_widget_capability(
    status: &UsbConnectionStatus,
    capability: &str,
    capability_label: &str,
) -> Result<(), String> {
    if !status.runtime.eq_ignore_ascii_case("esp-p4") {
        return Ok(());
    }
    let advertised = status
        .capabilities
        .get(capability)
        .and_then(Value::as_bool)
        .or_else(|| {
            status
                .capabilities
                .get("features")
                .and_then(|features| features.get(capability))
                .and_then(Value::as_bool)
        });
    if advertised == Some(true) {
        return Ok(());
    }
    Err(format!(
        "当前 ESP32-P4 板端固件 {} 未声明{}能力；请先在设备操作中升级板端固件并重新连接，再重试。",
        if status.firmware.trim().is_empty() {
            "<未知版本>"
        } else {
            status.firmware.trim()
        },
        capability_label,
    ))
}

pub(super) fn widget_ota_relative_path(root: &Path, path: &Path) -> Result<String, String> {
    let rel = path.strip_prefix(root).map_err(|error| error.to_string())?;
    let parts = rel
        .components()
        .map(|component| match component {
            std::path::Component::Normal(value) => value
                .to_str()
                .map(str::to_string)
                .ok_or_else(|| format!("widget path is not UTF-8: {}", path.display())),
            _ => Err(format!("unsafe widget path: {}", path.display())),
        })
        .collect::<Result<Vec<_>, _>>()?;
    Ok(parts.join("/"))
}

pub(super) fn widget_ota_should_skip_path(path: &str) -> bool {
    path.starts_with("pc/") || path.starts_with("licenses/")
        || matches!(path, "LICENSE.txt" | "THIRD_PARTY_NOTICES.txt" | "hachiapp.json")
        || path.split('/').next_back() == Some(".keep")
}

pub(super) fn prepare_p4_widget_sprite_files(
    widget_root: &Path,
    widget_bytes: &[u8],
) -> Result<Vec<(String, Vec<u8>)>, String> {
    let widget: Value = serde_json::from_slice(widget_bytes)
        .map_err(|error| format!("runtime/widget.json is invalid JSON: {error}"))?;
    if let Some(images)=widget["surface"]["images"].as_array() {
        if widget["surface"]["engine"]=="p4-app-surface-v1" {
            let base=crate::component_capability_ext::strip(&widget);
            crate::component_extension::validate_app_surface(&base["surface"],Some(&base))?;
        } else {
            crate::component_extension::validate_surface(&widget["surface"])?;
        }
        let limits=crate::app_graphics::limits(Some(&widget));
        let mut compiled=Vec::new();let mut total=0;let mut frames_total=0;
        for im in images {
            let id=im["id"].as_str().ok_or("image id")?;
            let bytes=std::fs::read(widget_root.join(im["asset"].as_str().ok_or("image asset")?)).map_err(|e|e.to_string())?;
            let image=crate::component_extension::decode_surface_image(im,&bytes)?;
            let encoded=if im["encoding"]=="jpeg" {crate::surface_image::encode_background(&image)?}else{crate::surface_image::encode(&image)};
            total+=encoded.len();frames_total+=if im["encoding"]=="jpeg"{encoded.len()}else{0};
            if encoded.len()>100000||total>limits.image_bytes{return Err(format!("组件图片超过 {} KiB 设备存储预算",limits.image_bytes/1024));}
            compiled.push((format!("runtime/images/{id}.p4i"),encoded));
        }
        // p4-keyframe-bg-v1: frames 2..N of the JPEG background follow the declared images as "<id>.k".
        if let Some((frames,_))=crate::app_graphics::keyframes(&widget["surface"]){
            let im=&images[0];let id=im["id"].as_str().ok_or("image id")?;let asset=im["asset"].as_str().ok_or("image asset")?;
            for k in 2..=frames{
                let source=crate::app_graphics::keyframe_asset(asset,k);
                let bytes=std::fs::read(widget_root.join(&source)).map_err(|e|format!("{source}: {e}"))?;
                let image=crate::component_extension::decode_surface_image(im,&bytes)?;
                let encoded=crate::surface_image::encode_background(&image)?;
                frames_total+=encoded.len();
                if encoded.len()>crate::app_graphics::KEYFRAME_BYTES||frames_total>crate::app_graphics::KEYFRAME_SET_BYTES{return Err("关键帧背景超过每帧 100 KB / 整组 400 KB 的传输预算".into());}
                compiled.push((format!("runtime/images/{}.p4i",crate::app_graphics::keyframe_id(id,k)),encoded));
            }
        }
        return Ok(compiled);
    }
    let Some(sprites) = widget
        .get("scene")
        .and_then(|scene| scene.get("sprites"))
        .and_then(Value::as_array)
    else {
        return Ok(Vec::new());
    };
    let sprite_limits=crate::app_graphics::limits(Some(&widget));
    if sprites.len() > sprite_limits.scene_sprites {
        return Err(format!(
            "runtime/widget.json scene.sprites exceeds {} entries",
            sprite_limits.scene_sprites
        ));
    }
    let mut total_pixels = 0usize;
    let mut compiled = Vec::with_capacity(sprites.len());
    for sprite in sprites {
        let id = sprite
            .get("id")
            .and_then(Value::as_str)
            .ok_or_else(|| "scene sprite is missing id".to_string())?;
        let asset = sprite
            .get("asset")
            .and_then(Value::as_str)
            .ok_or_else(|| format!("scene sprite {id} is missing asset"))?;
        let frame_width = sprite
            .get("frame_width")
            .and_then(Value::as_u64)
            .and_then(|value| u8::try_from(value).ok())
            .ok_or_else(|| format!("scene sprite {id} has invalid frame_width"))?;
        let frame_height = sprite
            .get("frame_height")
            .and_then(Value::as_u64)
            .and_then(|value| u8::try_from(value).ok())
            .ok_or_else(|| format!("scene sprite {id} has invalid frame_height"))?;
        let frames = sprite
            .get("frames")
            .and_then(Value::as_u64)
            .and_then(|value| u8::try_from(value).ok())
            .ok_or_else(|| format!("scene sprite {id} has invalid frames"))?;
        let fps = sprite
            .get("fps")
            .and_then(Value::as_u64)
            .and_then(|value| u8::try_from(value).ok())
            .ok_or_else(|| format!("scene sprite {id} has invalid fps"))?;
        let source = widget_root.join(asset);
        let source_bytes = std::fs::read(&source)
            .map_err(|error| format!("read scene sprite {} failed: {error}", source.display()))?;
        if source_bytes.len() > P4_SCENE_MAX_SPRITE_SOURCE_BYTES {
            return Err(format!(
                "scene sprite {id} exceeds the 128 KiB source limit"
            ));
        }
        let image = image::load_from_memory_with_format(&source_bytes, image::ImageFormat::Png)
            .map_err(|error| format!("decode scene sprite {id} failed: {error}"))?
            .to_rgba8();
        let expected_width = u32::from(frame_width) * u32::from(frames);
        if image.width() != expected_width || image.height() != u32::from(frame_height) {
            return Err(format!(
                "scene sprite {id} must be {}x{}, got {}x{}",
                expected_width,
                frame_height,
                image.width(),
                image.height()
            ));
        }
        let pixels = usize::from(frame_width) * usize::from(frame_height) * usize::from(frames);
        total_pixels = total_pixels
            .checked_add(pixels)
            .ok_or_else(|| "scene sprite pixel count overflow".to_string())?;
        if total_pixels > sprite_limits.sprite_pixels as usize {
            return Err(format!(
                "scene sprites exceed {} decoded pixels",
                sprite_limits.sprite_pixels
            ));
        }
        let mut encoded = Vec::with_capacity(8 + pixels * 3);
        encoded.extend_from_slice(P4_SCENE_SPRITE_MAGIC);
        encoded.extend_from_slice(&[frame_width, frame_height, frames, fps]);
        // Source sheets are horizontal, but firmware advances frames as
        // contiguous frame-sized blocks. Repack frame-by-frame instead of
        // preserving whole-sheet scanline order, which would interleave all
        // frame columns on every row and render each animation frame as a
        // flickering mixture of its neighbours.
        for frame in 0..u32::from(frames) {
            for y in 0..u32::from(frame_height) {
                for x in 0..u32::from(frame_width) {
                    let pixel = image.get_pixel(frame * u32::from(frame_width) + x, y);
                    let red = u16::from(pixel[0]);
                    let green = u16::from(pixel[1]);
                    let blue = u16::from(pixel[2]);
                    let rgb565 = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
                    encoded.extend_from_slice(&rgb565.to_le_bytes());
                    encoded.push(pixel[3]);
                }
            }
        }
        compiled.push((format!("runtime/sprites/{id}.p4s"), encoded));
    }
    Ok(compiled)
}

pub(crate) fn prepare_p4_widget_file(
    widget_id: &str,
    path: &str,
    bytes: &[u8],
) -> Result<Vec<u8>, String> {
    let max_bytes = match path {
        "runtime/widget.json" => P4_WIDGET_JSON_MAX_BYTES,
        "buttons.json" => P4_BUTTONS_JSON_MAX_BYTES,
        _ => return Ok(bytes.to_vec()),
    };
    let mut value: Value = serde_json::from_slice(bytes)
        .map_err(|error| format!("{} is invalid JSON: {}", path, error))?;
    let max_bytes=if path=="runtime/widget.json" {
        match value["quota"].as_str() {
            Some("p4-quota-v3") => 12287,
            Some("p4-quota-v2") => 8191,
            _ if value["surface"]["graphics"]=="p4-app-graphics-v1" => 8191,
            _ => max_bytes,
        }
    } else { max_bytes };

    if path == "runtime/widget.json" {
        let object = value
            .as_object_mut()
            .ok_or_else(|| "runtime/widget.json 必须是 JSON 对象，无法下发到 P4".to_string())?;

        crate::clawpkg::complete_p4_runtime_containers(object);
        if !object["vars"].is_object() {
            return Err("runtime/widget.json 的 vars 必须是以变量名为键的 JSON 对象；无变量时请使用 {}".into());
        }
        let quota_v3 = object.get("quota").is_some_and(|q| q == "p4-quota-v3");
        let quota_v2 = object.get("quota").is_some_and(|q| q == "p4-quota-v2");
        let vars=object.get("vars").and_then(Value::as_object).expect("normalized vars");
        // Reuse the shared structural validator without imposing its historical
        // 63-byte string limit on explicit v3. Only the validation copy changes;
        // the exact user strings remain in the outgoing payload.
        let mut checked=vars.clone();
        if quota_v3 {
            for (name, declaration) in &mut checked {
                if declaration["type"]=="string" {
                    if let Some(init)=declaration.get("init") {
                        let text=init.as_str().filter(|s|s.len()<=127&&!s.contains('\0'))
                            .ok_or_else(||format!("runtime/widget.json.vars.{name}.init 必须是最多 127 个 UTF-8 字节的字符串"))?;
                        let _=text;
                        declaration["init"]=Value::String(String::new());
                    }
                }
            }
        }
        validate_p4_vars_object_capped(
            &checked,
            if quota_v3 { 32 } else if quota_v2 { 16 } else { 8 },
        )?;

        if let Some(pages) = value.get_mut("pages").and_then(Value::as_array_mut) {
            for page in pages {
                if let Some(page) = page.as_object_mut() {
                    page.remove("label");
                }
            }
        }
    }

    if path == "buttons.json" {
        let bindings = value
            .as_array_mut()
            .ok_or_else(|| "buttons.json 必须是 JSON 数组，无法下发到 P4".to_string())?;
        bindings.retain(|binding| {
            !matches!(
                binding.get("action").and_then(Value::as_str),
                Some(
                    "page_toggle"
                        | "page_enter"
                        | "page_back"
                        | "page_main"
                        | "page_app"
                        | "component_center"
                )
            )
        });
    }

    if widget_id == "token-usage" && path == "runtime/widget.json" {
        let object = value
            .as_object_mut()
            .ok_or_else(|| "token-usage runtime/widget.json must be an object".to_string())?;
        object.remove("readers");
        object.remove("fetchers");
    }

    let compact = serde_json::to_vec(&value).map_err(|error| error.to_string())?;
    if compact.len() > max_bytes {
        return Err(format!(
            "{} is {} bytes after JSON compaction; P4 limit is {} bytes",
            path,
            compact.len(),
            max_bytes
        ));
    }
    if path == "runtime/widget.json" {
        crate::clawpkg::validate_p4_runtime_shell(&value)?;
    }
    Ok(compact)
}

#[test]
fn widget_delivery_rejects_missing_and_unknown_transition_sources() {
    let mut widget = serde_json::json!({
        "schema_version": 1, "states": ["garden"], "initial_state": "garden", "dashboard": {},
        "transitions": [{"on": "move", "from": "*"}, {"on": "dig", "to": "garden"}]
    });
    let prepare = |widget: &Value| {
        prepare_p4_widget_file("garden", "runtime/widget.json", &serde_json::to_vec(widget).unwrap())
    };
    for source in [None, Some(Value::Null), Some(serde_json::json!("outside"))] {
        let rule = widget["transitions"][1].as_object_mut().unwrap();
        if let Some(source) = source {
            rule.insert("from".into(), source);
        } else {
            rule.remove("from");
        }
        let error = prepare(&widget).unwrap_err();
        assert!(error.contains("runtime/widget.json.transitions[1].from"), "{error}");
    }
    widget["transitions"][1]["from"] = serde_json::json!("garden");
    let sent: Value = serde_json::from_slice(&prepare(&widget).unwrap()).unwrap();
    assert_eq!(sent["transitions"], widget["transitions"]);
}

#[test]
fn widget_delivery_completes_absent_containers_and_rejects_bad_runtime_shapes() {
    let source = serde_json::json!({
        "schema_version": 1, "engine": "p4-bounded-runtime-v4",
        "states": ["ready"], "initial_state": "ready", "dashboard": {},
        "hachiWasm": "a".repeat(64)
    });
    let prepare = |value: &Value| {
        prepare_p4_widget_file("postcard-puzzle", "runtime/widget.json", &serde_json::to_vec(value).unwrap())
    };
    let sent: Value = serde_json::from_slice(&prepare(&source).unwrap()).unwrap();
    assert_eq!(sent["vars"], serde_json::json!({}));
    assert_eq!(sent["transitions"], serde_json::json!([]));
    assert_eq!(sent["tick"], serde_json::json!([]));
    assert_eq!(sent["hachiWasm"], source["hachiWasm"]);
    for (field, value) in [
        ("transitions", Value::Null), ("tick", serde_json::json!({})),
        ("states", serde_json::json!(["ready", "ready"])),
        ("initial_state", serde_json::json!("missing")),
        ("schema_version", serde_json::json!("1")), ("dashboard", Value::Null),
        ("pages", serde_json::json!([])),
    ] {
        let mut bad = source.clone();
        bad[field] = value;
        let error = prepare(&bad).unwrap_err();
        assert!(error.contains(&format!("runtime/widget.json.{field}")), "{error}");
    }
}

#[test]
fn quota_v3_transaction_keeps_32_vars_127_utf8_bytes_and_exact_json_ceiling(){
    let mut vars=serde_json::Map::new();
    for i in 0..32{vars.insert(format!("v{i}"),serde_json::json!({"type":"string","init":"a".repeat(127)}));}
    let mut w=serde_json::json!({"engine":"p4-bounded-runtime-v4","quota":"p4-quota-v3","vars":vars,
        "schema_version":1,"states":["ready"],"initial_state":"ready","dashboard":{}});
    let send=|w:&Value|prepare_p4_widget_file("quota-probe","runtime/widget.json",&serde_json::to_vec(w).unwrap());
    let got:Value=serde_json::from_slice(&send(&w).unwrap()).unwrap();
    assert_eq!(got["vars"],w["vars"]);
    w["vars"]["v0"]["init"]=Value::String("界".repeat(42)+"a");assert!(send(&w).is_ok());
    w["vars"]["v0"]["init"]=Value::String("界".repeat(42)+"ab");assert!(send(&w).is_err());
    w["vars"]["v0"]["init"]=Value::String("a".repeat(127));
    w["vars"]["extra"]=serde_json::json!({"type":"int"});assert!(send(&w).is_err());
    w["vars"].as_object_mut().unwrap().remove("extra");
    w["quota"]="p4-quota-v2".into();assert!(send(&w).is_err());
    w["quota"]="p4-quota-v3".into();w["padding"]="".into();
    crate::clawpkg::complete_p4_runtime_containers(w.as_object_mut().unwrap());
    let base=serde_json::to_vec(&w).unwrap().len();w["padding"]=Value::String("x".repeat(12287-base));
    assert_eq!(send(&w).unwrap().len(),12287);
    w["padding"].as_str().map(str::to_owned).map(|s|w["padding"]=Value::String(s+"x"));
    assert!(send(&w).is_err());
    assert!(prepare_p4_widget_file("quota-probe","buttons.json",&serde_json::to_vec(&vec!["x".repeat(2048)]).unwrap()).is_err());
}
