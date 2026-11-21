use super::{
    exchange_connection, exchange_response_with_timeout, query_status, read_before,
    read_with_recovery, Peer, IO_TIMEOUT,
};
use crate::usb_serial::{ComponentCenterTitle, UsbSerialManager};
use ring::aead;
use sha2::{Digest, Sha256};
use std::{
    collections::HashMap,
    io::Write,
    net::TcpStream,
    path::Path,
    time::{Duration, Instant},
};

const LEGACY_MODE: u8 = 9;
const MODE: u8 = 14;
const CHUNK: usize = 4096;
const MAX_RECORDS: u32 = 4352;
const FILES: [(&str, usize); 2] = [("buttons.json", 2047), ("runtime/widget.json", 4095)];

#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct ComponentPreview {
    id: String,
    digest: String,
}

struct Package {
    preview: ComponentPreview,
    stage: tempfile::TempDir,
    version_hash: String,
    name: String,
}

impl Package {
    fn read(path: &Path) -> Result<Self, String> {
        let stage = tempfile::tempdir().map_err(|e| e.to_string())?;
        let package = stage.path().join("package");
        let metadata = std::fs::symlink_metadata(path).map_err(|e| e.to_string())?;
        let archive_digest = if metadata.is_dir() {
            // The shared package reader bounds allocations and rejects unsafe paths.
            let files = crate::clawpkg::collect_clawpkg_directory_files(path)?;
            for directory in ["assets", "runtime"] {
                if path.join(directory).is_dir() {
                    std::fs::create_dir_all(package.join(directory)).map_err(|e| e.to_string())?;
                }
            }
            for (name, bytes) in files {
                let file = package.join(name);
                std::fs::create_dir_all(file.parent().ok_or("无效组件路径")?).map_err(|e| e.to_string())?;
                std::fs::write(file, bytes).map_err(|e| e.to_string())?;
            }
            None
        } else {
            let bytes = crate::clawpkg::read_clawpkg_source_file(
                path, crate::clawpkg::CLAWPKG_MAX_ARCHIVE_BYTES as u64,
            )?;
            let archive = stage.path().join("verified.hachiapp");
            std::fs::write(&archive, &bytes).map_err(|e| e.to_string())?;
            crate::component_library::extract_zip_safe(&archive, &package)?;
            Some(format!("{:x}", Sha256::digest(&bytes)))
        };
        let verified = crate::clawpkg::validate_clawpkg_at_path(&package)?;
        if !verified.ok {
            return Err(verified.errors.join("、"));
        }
        let manifest = verified.manifest.ok_or("组件缺少清单。")?;
        if let Some(reason) = manifest.sync_requirements.blocked_reason {
            return Err(reason.into());
        }
        let id = manifest.id;
        if !valid_id(&id, 47) {
            return Err("无线安装要求组件 ID 为 1–47 位小写 ASCII 字符。".into());
        }
        let version_hash = crate::component_library::package_content_hash(&package)?;
        let digest = archive_digest.unwrap_or_else(|| version_hash.clone());
        Ok(Self {
            preview: ComponentPreview { id, digest },
            stage,
            version_hash,
            name: manifest.name,
        })
    }

    fn prepare(
        &self,
        binding_overrides: &HashMap<String, String>,
        center_title: Option<&ComponentCenterTitle>,
    ) -> Result<Vec<(String, Vec<u8>)>, String> {
        // USB and Wi-Fi use the same native package compiler.
        let package = self.stage.path().join("package");
        let mut files = crate::usb_serial::UsbSerialManager::prepare_widget_delivery(
            &self.preview.id,
            &package,
            binding_overrides,
            center_title,
            center_title.map(|_| serde_json::json!({"widgetCenterTitle": "rle4-v1"})).as_ref(),
        )?;
        files.retain(|(_, bytes)| !bytes.is_empty());
        files.sort_unstable_by(|a, b| a.0.cmp(&b.0));
        if files.iter().any(|(path, _)| !valid_wire_path(path)) {
            return Err("组件含有设备安装器不支持的文件。".into());
        }
        Ok(files)
    }
}

fn is_package_metadata(path: &str) -> bool {
    matches!(
        path,
        "component.json" | "negative-screen.json" | "share.json"
    )
}

fn valid_wire_path(path: &str) -> bool {
    if path.len() >= 96 || !path.is_ascii() {
        return false;
    }
    is_package_metadata(path)
        || matches!(
            path,
            "buttons.json"
                | "runtime/widget.json"
                | "runtime/app.wasm"
                | "runtime/center-title.rle"
                | crate::wasm_audio::DEVICE_PATH
        )
        || [
            ("runtime/images/", ".p4i"),
            ("runtime/sprites/", ".p4s"),
            ("runtime/samples/", ".pcm"),
            ("runtime/clips/", ".h264"),
            ("runtime/big/", ".p4t"),
        ]
        .iter()
        .any(|(prefix, suffix)| {
            path.strip_prefix(prefix)
                .and_then(|name| name.strip_suffix(suffix))
                .is_some_and(|id| {
                    !id.is_empty()
                        && id
                            .bytes()
                            .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'_' | b'-' | b'.'))
                })
        })
}

pub(super) fn valid_id(id: &str, max: usize) -> bool {
    let mut bytes = id.bytes();
    id.len() <= max
        && bytes.next().is_some_and(|b| b.is_ascii_lowercase())
        && bytes.all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-' || b == b'_')
}

fn legacy_body(id: &str, boot: &str, files: &[(String, Vec<u8>)]) -> Result<Vec<u8>, String> {
    let legacy_files: Vec<_> = files
        .iter()
        .filter(|(path, _)| !is_package_metadata(path))
        .collect();
    if legacy_files.len() != FILES.len() || !valid_id(id, 32) {
        return Err("设备固件仅支持旧版无线组件安装，请先升级。".into());
    }
    let mut body = Vec::from(boot.as_bytes());
    body.push(id.len() as u8);
    for ((path, data), (expected, max)) in legacy_files.iter().copied().zip(FILES) {
        if path != expected || data.len() > max {
            return Err("设备固件仅支持旧版无线组件安装，请先升级。".into());
        }
        body.extend_from_slice(&(data.len() as u16).to_be_bytes());
    }
    body.extend_from_slice(id.as_bytes());
    for (_, data) in legacy_files {
        body.extend_from_slice(data);
    }
    Ok(body)
}

fn offer(id: &str, files: &[(String, Vec<u8>)]) -> Result<Vec<u8>, String> {
    let (digest, size) = crate::usb_serial::widget_package_digest(files)?;
    let mut bytes = vec![1, id.len() as u8, files.len() as u8];
    bytes.extend_from_slice(&(size as u32).to_be_bytes());
    for pair in digest.as_bytes().chunks_exact(2) {
        let hex = std::str::from_utf8(pair).map_err(|e| e.to_string())?;
        bytes.push(u8::from_str_radix(hex, 16).map_err(|e| e.to_string())?);
    }
    bytes.extend_from_slice(id.as_bytes());
    Ok(bytes)
}

fn chunk_record(file: u8, path: &str, offset: usize, file_bytes: usize, data: &[u8]) -> Vec<u8> {
    let mut record = vec![2, file];
    record.extend_from_slice(&(offset as u32).to_be_bytes());
    record.extend_from_slice(&(file_bytes as u32).to_be_bytes());
    record.push(path.len() as u8);
    record.extend_from_slice(path.as_bytes());
    record.extend_from_slice(data);
    record
}

fn record_nonce(direction: u8, sequence: u32) -> aead::Nonce {
    let mut bytes = [0; 12];
    bytes[0] = direction;
    bytes[8..].copy_from_slice(&sequence.to_be_bytes());
    aead::Nonce::assume_unique_for_key(bytes)
}

struct Link {
    stream: TcpStream,
    key: aead::LessSafeKey,
    sequence: u32,
}

impl Link {
    fn request(&mut self, mut record: Vec<u8>, committing: bool) -> Result<(), String> {
        if self.sequence >= MAX_RECORDS {
            return Err("组件分块数量超出无线安装上限。".into());
        }
        let sequence = self.sequence;
        self.sequence += 1;
        let uncertain = || {
            if committing {
                "无线安装提交结果未确认，请查看设备组件列表。".to_owned()
            } else {
                "无线组件传输已中断，请检查连接。".to_owned()
            }
        };
        let mut length = (record.len() as u16).to_be_bytes();
        self.key
            .seal_in_place_append_tag(
                record_nonce(2, sequence),
                aead::Aad::from(length),
                &mut record,
            )
            .map_err(|_| uncertain())?;
        let timeout = if committing {
            Duration::from_secs(58)
        } else {
            Duration::from_secs(6)
        };
        let deadline = Instant::now() + timeout;
        self.stream
            .write_all(&length)
            .and_then(|_| self.stream.write_all(&record))
            .and_then(|_| read_before(&mut self.stream, &mut length, deadline))
            .map_err(|_| uncertain())?;
        if length != [0, 1] {
            return Err(uncertain());
        }
        let mut reply = [0; 17];
        read_before(&mut self.stream, &mut reply, deadline).map_err(|_| uncertain())?;
        match self
            .key
            .open_in_place(
                record_nonce(3, sequence),
                aead::Aad::from(length),
                &mut reply,
            )
            .map_err(|_| uncertain())?
        {
            [1] => Ok(()),
            [0] => Err("设备拒绝组件安装。请检查空闲状态、槽位和组件资源。".into()),
            _ => Err(uncertain()),
        }
    }
}

fn install_bundle(
    peer: &Peer,
    boot: &str,
    id: &str,
    files: &[(String, Vec<u8>)],
    expected_installed_version: Option<&str>,
    require_lan: impl Fn() -> Result<(), String>,
) -> Result<(), String> {
    let mut begin = offer(id, files)?;
    if let Some(version) = expected_installed_version {
        if version.is_empty() || version.len() > 63 || version.as_bytes().contains(&0) {
            return Err("无法确认当前设备组件的版本。".into());
        }
        begin[0] = 4;
        begin.extend_from_slice(version.as_bytes());
    }
    require_lan()?;
    let (stream, key, ack) = super::retry_uncommitted_offer(|| {
        require_lan()
            .map_err(|message| std::io::Error::new(std::io::ErrorKind::Interrupted, message))?;
        exchange_connection(peer, MODE, boot.as_bytes(), Some(1), IO_TIMEOUT)
    })
    .map_err(|error| {
        if error.kind() == std::io::ErrorKind::Interrupted {
            error.to_string()
        } else {
            "无法建立无线组件传输。".into()
        }
    })?;
    if ack != [1] {
        return Err("设备未接受当前启动身份或无线控制信任。".into());
    }
    let mut link = Link {
        stream,
        key,
        sequence: 0,
    };
    require_lan()?;
    link.request(begin, false)?;
    for (index, (path, data)) in files.iter().enumerate() {
        for (part, chunk) in data.chunks(CHUNK).enumerate() {
            require_lan()?;
            link.request(
                chunk_record(index as u8, path, part * CHUNK, data.len(), chunk),
                false,
            )?;
        }
    }
    require_lan()?;
    link.request(vec![3], true)
}

#[tauri::command]
pub(crate) async fn lan_component_preview(path: String) -> Result<ComponentPreview, String> {
    tauri::async_runtime::spawn_blocking(move || Package::read(Path::new(&path)).map(|p| p.preview))
        .await
        .map_err(|_| "组件校验任务未完成。")?
}

#[tauri::command]
pub(crate) async fn lan_component_install(
    path: String,
    id: String,
    digest: String,
    expected_board_device_id: String,
    expected_boot_id: String,
    expected_installed_version: Option<String>,
    expected_version_hash: Option<String>,
    binding_overrides: Option<HashMap<String, String>>,
    center_title: Option<ComponentCenterTitle>,
    usb_manager: tauri::State<'_, UsbSerialManager>,
) -> Result<serde_json::Value, String> {
    let usb = usb_manager.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let require_lan = || super::require_no_usb(&usb);
        require_lan()?;
        let package = Package::read(Path::new(&path))?;
        if package.preview.id != id || package.preview.digest != digest
            || expected_version_hash.as_ref().is_some_and(|expected| expected != &package.version_hash)
        {
            return Err("组件内容在预览后变化，请重新选择文件。".into());
        }
        let (peer, status) = read_with_recovery(query_status, "设备当前不可达，安装请求未发送。")?;
        if status.board_device_id != expected_board_device_id || status.boot_id != expected_boot_id {
            return Err("设备身份已变化，请重新确认组件同步目标。".into());
        }
        let center_title = center_title.as_ref().filter(|_| status.panel_control && status.component_bundle);
        let files = package.prepare(&binding_overrides.unwrap_or_default(), center_title)?;
        if !status.input_double_press {
            if let Some((_, bytes)) = files.iter().find(|(path, _)| path == "buttons.json") {
                let buttons: Vec<serde_json::Value> = serde_json::from_slice(bytes).map_err(|e| e.to_string())?;
                if buttons.iter().any(|button| button["event"].as_str()
                    .is_some_and(|event| event.ends_with(".double_press")))
                {
                    return Err("组件使用实体键双击，请先升级设备固件。".into());
                }
            }
        }
        let boot = &status.boot_id;
        if !status.component_delivery || !status.wifi_connected || !status.control_trusted {
            return Err("无线安装需要通过 USB 授权当前已配对的设备。".into());
        }
        if boot.len() != 16 || !boot.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err("无法确认目标设备的本次启动身份。".into());
        }
        let result = serde_json::json!({"ok": true, "manifest": {"name": package.name}});
        if expected_installed_version.is_some() && (!status.component_bundle || !status.panel_control) {
            return Err("当前固件未开放 Wi-Fi 组件更新。".into());
        }
        if status.component_bundle {
            install_bundle(&peer, boot, &id, &files, expected_installed_version.as_deref(), require_lan)?;
            return Ok(result);
        }
        let body = legacy_body(&id, boot, &files)?;
        require_lan()?;
        match exchange_response_with_timeout(
            &peer,
            LEGACY_MODE,
            &body,
            Some(1),
            Duration::from_secs(90),
        )
        .as_deref()
        {
            Ok([1]) => Ok(result),
            Ok([0]) => Err("设备拒绝旧版无线组件安装。".into()),
            _ => Err("安装结果未确认。请查看设备组件列表，再决定是否重试。".into()),
        }
    })
    .await
    .map_err(|_| "无线安装任务未完成，请查看设备组件列表。")?
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[ignore = "installs the selected package on an explicitly selected USB-trusted board"]
    fn hil_install_bundle() {
        let approved_board = std::env::var("HACHIMO_LAN_HIL_BOARD_ID")
            .expect("set HACHIMO_LAN_HIL_BOARD_ID to the target device ID");
        assert!(
            approved_board.strip_prefix("p4-").is_some_and(|id|
                id.len() == 12 && id.bytes().all(|b| b.is_ascii_hexdigit())
            ),
            "expected a p4 device ID"
        );
        let path = std::env::var_os("HACHIMO_LAN_HIL_PACKAGE")
            .expect("set HACHIMO_LAN_HIL_PACKAGE to the exact .hachiapp file to install");
        let package = Package::read(Path::new(&path)).expect("validate the selected archive");
        let files = package
            .prepare(&HashMap::new(), None)
            .expect("prepare device payload via USB's package compiler");
        let (peer, status) =
            read_with_recovery(query_status, "authenticate live status over Wi-Fi")
                .expect("desktop must already be paired and USB-trusted");
        assert!(
            status.board_device_id == approved_board,
            "refusing a different device"
        );
        assert!(
            status.wifi_connected && status.control_trusted && status.component_bundle,
            "target must be online and advertise USB-trusted mode 14"
        );
        assert!(
            status.boot_id.len() == 16 && status.boot_id.bytes().all(|b| b.is_ascii_hexdigit()),
            "target must supply a valid boot ID"
        );

        let total_bytes: usize = files.iter().map(|(_, bytes)| bytes.len()).sum();
        eprintln!(
            "Installing {} via mode 14: {} device files, {} bytes",
            package.preview.id,
            files.len(),
            total_bytes
        );
        for (name, bytes) in &files {
            eprintln!("  {name}: {} bytes", bytes.len());
        }
        // The explicitly selected HIL test exercises LAN, independent of the
        // desktop command's transport selection.
        install_bundle(&peer, &status.boot_id, &package.preview.id, &files, None, || {
            Ok(())
        })
        .expect("mode 14 must return an authenticated commit success");
        eprintln!(
            "Committed {}. This HIL test leaves it installed.",
            package.preview.id
        );
    }

    #[test]
    fn sequential_record_binds_file_identity_offset_and_opaque_bytes() {
        assert_eq!(
            chunk_record(1, "runtime/app.wasm", 4096, 4099, &[0, 255, 8]),
            b"\x02\x01\0\0\x10\0\0\0\x10\x03\x10runtime/app.wasm\0\xff\x08"
        );
        assert!(valid_wire_path("runtime/images/game.2.p4i"));
        assert!(!valid_wire_path("runtime/images/../file.p4i"));
        assert!(!valid_wire_path("pc/application.json"));
        for path in ["component.json", "negative-screen.json", "share.json"] {
            assert!(valid_wire_path(path));
        }
        assert!(!valid_wire_path("runtime/component.json"));
        assert!(!valid_wire_path("project.json"));
        assert!(!valid_id("demo/other", 47));
    }

    #[test]
    fn legacy_peers_never_receive_a_binary_bundle() {
        let boot = "0123456789abcdef";
        let mut files = vec![
            ("buttons.json".into(), b"[]".to_vec()),
            ("runtime/widget.json".into(), b"{}".to_vec()),
        ];
        assert_eq!(
            legacy_body("demo", boot, &files).unwrap(),
            b"0123456789abcdef\x04\0\x02\0\x02demo[]{}"
        );
        files.insert(1, ("component.json".into(), b"{}".to_vec()));
        files.insert(2, ("negative-screen.json".into(), b"{}".to_vec()));
        files.push(("share.json".into(), b"{}".to_vec()));
        assert_eq!(
            legacy_body("demo", boot, &files).unwrap(),
            b"0123456789abcdef\x04\0\x02\0\x02demo[]{}"
        );
        files.insert(1, ("runtime/app.wasm".into(), b"\0asm".to_vec()));
        assert!(legacy_body("demo", boot, &files).is_err());
    }

    #[test]
    fn bundle_offer_authenticates_all_native_metadata_files() {
        let mut files = vec![
            ("buttons.json".into(), b"[]".to_vec()),
            ("component.json".into(), br#"{"version":"1.2.3"}"#.to_vec()),
            ("negative-screen.json".into(), b"{}".to_vec()),
            ("runtime/widget.json".into(), b"{}".to_vec()),
            ("share.json".into(), b"{}".to_vec()),
        ];
        let original = offer("demo", &files).unwrap();
        assert_eq!(original[2], 5);
        for index in [1, 2, 4] {
            files[index].1.push(b' ');
            let changed = offer("demo", &files).unwrap();
            assert_ne!(&original[7..39], &changed[7..39]);
            files[index].1.pop();
        }
    }

    #[test]
    fn dropped_commit_reply_is_uncertain_and_does_not_open_another_connection() {
        exercise_bundle_end(false);
    }

    #[test]
    fn usb_connecting_before_commit_closes_lan_without_sending_commit() {
        exercise_bundle_end(true);
    }

    fn exercise_bundle_end(usb_connects: bool) {
        use std::cell::Cell;
        use std::io::Read;
        use std::net::TcpListener;
        use zeroize::Zeroizing;

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let peer = Peer {
            address: listener.local_addr().unwrap(),
            key: Zeroizing::new([7; 16]),
        };
        let boot = "0123456789abcdef";
        let files = vec![
            ("buttons.json".into(), b"[]".to_vec()),
            ("runtime/app.wasm".into(), b"\0asm\x01\0\0\0".to_vec()),
            ("runtime/widget.json".into(), b"{}".to_vec()),
        ];
        let mut expected = vec![offer("demo", &files).unwrap()];
        for (index, (path, bytes)) in files.iter().enumerate() {
            expected.push(chunk_record(index as u8, path, 0, bytes.len(), bytes));
        }
        if !usb_connects {
            expected.push(vec![3]);
        }
        let server = std::thread::spawn(move || {
            let (mut socket, _) = listener.accept().unwrap();
            socket
                .set_read_timeout(Some(Duration::from_secs(3)))
                .unwrap();
            let mut client = [0; 16];
            socket.read_exact(&mut client).unwrap();
            let server_nonce = [5; 16];
            socket.write_all(&server_nonce).unwrap();
            let mut header = [0; 3];
            socket.read_exact(&mut header).unwrap();
            assert_eq!(header, [MODE, 0, 16]);
            let key = super::super::session_key(&[7; 16], &client, &server_nonce);
            let mut hello = [0; 32];
            socket.read_exact(&mut hello).unwrap();
            assert_eq!(
                key.open_in_place(super::super::nonce(0), aead::Aad::from(header), &mut hello)
                    .unwrap(),
                boot.as_bytes()
            );
            let mut accepted = vec![1];
            key.seal_in_place_append_tag(
                super::super::nonce(1),
                aead::Aad::from([MODE, 0, 16, 0, 1]),
                &mut accepted,
            )
            .unwrap();
            socket.write_all(&[0, 1]).unwrap();
            socket.write_all(&accepted).unwrap();
            for (sequence, record) in expected.into_iter().enumerate() {
                let mut length = [0; 2];
                socket.read_exact(&mut length).unwrap();
                let mut incoming = vec![0; u16::from_be_bytes(length) as usize + 16];
                socket.read_exact(&mut incoming).unwrap();
                assert_eq!(
                    key.open_in_place(
                        record_nonce(2, sequence as u32),
                        aead::Aad::from(length),
                        &mut incoming
                    )
                    .unwrap(),
                    record
                );
                if record == [3] {
                    break;
                }
                let mut reply = vec![1];
                key.seal_in_place_append_tag(
                    record_nonce(3, sequence as u32),
                    aead::Aad::from([0, 1]),
                    &mut reply,
                )
                .unwrap();
                socket.write_all(&[0, 1]).unwrap();
                socket.write_all(&reply).unwrap();
            }
            if usb_connects {
                assert_eq!(socket.read(&mut [0]).unwrap(), 0, "commit must not be sent");
            }
            drop(socket);
            listener.set_nonblocking(true).unwrap();
            std::thread::sleep(Duration::from_millis(30));
            assert_eq!(
                listener.accept().unwrap_err().kind(),
                std::io::ErrorKind::WouldBlock
            );
        });
        // The guard runs before opening the link, in the connection attempt,
        // before the offer, and before each chunk. Attach USB at commit.
        let checks_before_commit = 3 + files.len();
        let checks = Cell::new(0);
        let require_lan = || {
            checks.set(checks.get() + 1);
            if usb_connects && checks.get() > checks_before_commit {
                Err("USB connected".into())
            } else {
                Ok(())
            }
        };
        let failure = install_bundle(&peer, boot, "demo", &files, None, require_lan).unwrap_err();
        if usb_connects {
            assert_eq!(failure, "USB connected");
        } else {
            assert!(failure.contains("提交结果未确认"));
        }
        server.join().unwrap();
    }
}
