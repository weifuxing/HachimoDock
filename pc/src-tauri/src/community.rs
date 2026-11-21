//! Native community boundary: fixed public origin, OS/private credentials, safe packages.
//! [Sync] Update pc/.folder.md when changing publication/download behavior.
use base64::Engine;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
mod network;
use std::{
    collections::BTreeMap,
    fs,
    io::{Cursor, Read, Write},
    path::{Path, PathBuf},
    sync::{Mutex, OnceLock},
};

// This is the intentionally public community endpoint, not an internal API.
const ORIGIN: &str = "https://115.191.67.48";
const MAX_PACKAGE: usize = 8 * 1024 * 1024;
const MAX_RESPONSE: u64 = 8 * 1024 * 1024;
const RESERVED: &[&str] = &[
    "stock-watchlist",
    "wooden-fish",
    "music-player",
    "city-radio",
    "upcoming-todos",
    "computer-status",
    "fruit-slice",
    "fresh-fruit-slice",
    "touch-slice",
    "two-key-pong",
    "tomato-clock",
    "snake-turn",
    "drink-reminder",
    "bloomfrog_companion",
    "token-usage",
    "block-combo",
    "flappy-bird",
];
static MUTATION: Mutex<()> = Mutex::new(());
static IMAGE_CACHE: OnceLock<Mutex<BTreeMap<String, String>>> = OnceLock::new();

#[derive(Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct LocalState {
    #[serde(default)]
    downloaded: BTreeMap<String, Value>,
    #[serde(default)]
    owned: BTreeMap<String, Value>,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct CommunityInput {
    pub action: String,
    pub id: Option<String>,
    pub query: Option<String>,
    pub category: Option<String>,
    pub order: Option<String>,
    pub metric: Option<String>,
    pub window: Option<String>,
    pub offset: Option<u32>,
    pub path: Option<String>,
    pub active: Option<bool>,
    pub name: Option<String>,
    pub text: Option<String>,
    pub request_id: Option<String>,
    pub metadata: Option<Value>,
    pub covers: Option<Vec<String>>,
    pub expected_content_hash: Option<String>,
    // Accepted for older clients; publication no longer requires or fabricates acknowledgments.
    pub rights_confirmed: Option<bool>,
    pub privacy_confirmed: Option<bool>,
}

fn root(home: &Path) -> PathBuf {
    home.join(".claw-pet/components/community")
}
fn state(home: &Path) -> Result<LocalState, String> {
    let path = root(home).join("state.json");
    match fs::read(path) {
        Ok(bytes) if bytes.len() <= 2 * 1024 * 1024 => serde_json::from_slice(&bytes)
            .map_err(|_| "社区本地来源记录损坏，请勿覆盖，联系开发者处理".into()),
        Ok(_) => Err("社区来源记录过大".into()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(LocalState::default()),
        Err(_) => Err("无法读取社区来源记录".into()),
    }
}
fn save(home: &Path, state: &LocalState) -> Result<(), String> {
    let dir = root(home);
    fs::create_dir_all(&dir).map_err(|_| "无法创建社区目录")?;
    let mut temp = tempfile::NamedTempFile::new_in(&dir).map_err(|_| "无法保存社区来源记录")?;
    temp.write_all(&serde_json::to_vec(state).map_err(|_| "来源记录无效")?)
        .and_then(|_| temp.as_file().sync_all())
        .map_err(|_| "无法保存社区来源记录")?;
    temp.persist(dir.join("state.json"))
        .map_err(|_| "无法替换社区来源记录")?;
    Ok(())
}
fn digest(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
fn identity(home: &Path, purpose: &str) -> Result<String, String> {
    let secret = crate::community_credentials::load_or_create(&root(home))?;
    Ok(digest(format!("{purpose}:{ORIGIN}:{secret}").as_bytes()))
}
fn safe_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 44
        && value.as_bytes()[0].is_ascii_lowercase()
        && value
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || matches!(b, b'-' | b'_'))
}
fn reserved(id: &str) -> bool {
    RESERVED.contains(&id) || id.starts_with("official-") || id.starts_with("hachimo-")
}
fn response_bytes(mut response: reqwest::blocking::Response, max: u64) -> Result<Vec<u8>, String> {
    let status = response.status();
    if response.content_length().is_some_and(|n| n > max) {
        return Err("社区响应超过大小限制".into());
    }
    let mut bytes = Vec::new();
    (&mut response)
        .take(max + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| "社区响应未完整接收")?;
    if bytes.len() as u64 > max {
        return Err("社区响应超过大小限制".into());
    }
    if !status.is_success() {
        let detail = serde_json::from_slice::<Value>(&bytes)
            .ok()
            .and_then(|v| {
                v["error"]
                    .as_str()
                    .map(|s| s.chars().take(600).collect::<String>())
            })
            .unwrap_or_else(|| format!("社区服务暂不可用（HTTP {}）", status.as_u16()));
        return Err(detail);
    }
    Ok(bytes)
}
fn request(
    home: &Path,
    path: &str,
    body: Option<Value>,
    owner: bool,
    identify: bool,
) -> Result<Value, String> {
    if !path.starts_with("/api/v1/") || path.contains('#') {
        return Err("社区请求路径无效".into());
    }
    let url = format!("{ORIGIN}{path}");
    let mut headers=reqwest::header::HeaderMap::new();
    if identify {
        headers.insert("X-Community-Client",identity(home,"client")?.parse().map_err(|_|"社区身份格式无效")?);
    }
    if owner {
        headers.insert(reqwest::header::AUTHORIZATION,format!("Bearer {}",identity(home,"owner")?).parse().map_err(|_|"社区身份格式无效")?);
    }
    let bytes = response_bytes(network::send(home,&url,body,headers)?, MAX_RESPONSE)?;
    serde_json::from_slice(&bytes).map_err(|_| "社区响应格式无效".into())
}
fn with_query(path: &str, pairs: &[(&str, String)]) -> String {
    let mut url = reqwest::Url::parse(&format!("{ORIGIN}{path}")).expect("fixed public origin");
    for (k, v) in pairs {
        if !v.is_empty() {
            url.query_pairs_mut().append_pair(k, v);
        }
    }
    format!(
        "{}{}",
        url.path(),
        url.query().map(|s| format!("?{s}")).unwrap_or_default()
    )
}
fn valid_hash(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase())
}
const WASM_MODULE: &str = "runtime/app.wasm";

fn file_limit(name: &str) -> u64 {
    if name == crate::wasm_audio::ASSET {
        crate::wasm_audio::MAX_BYTES as u64
    } else if name == WASM_MODULE {
        crate::clawpkg::HACHI_MAX_MODULE_BYTES as u64
    } else if name == crate::component_extension::CATALOG_PATH {
        crate::component_extension::CATALOG_MAX_BYTES as u64
    } else {
        4 * 1024 * 1024
    }
}

fn allowed_file(name: &str) -> bool {
    if [
        "component.json",
        "negative-screen.json",
        "buttons.json",
        "runtime/widget.json",
        WASM_MODULE,
        crate::wasm_audio::ASSET,
        "hachiapp.json",
        "share.json",
        "pc/extension.json",
        "pc/application.json",
        "assets/.keep",
        "LICENSE.txt",
        "THIRD_PARTY_NOTICES.txt",
    ]
    .contains(&name)
    {
        return true;
    }
    let parts = name.split('/').collect::<Vec<_>>();
    if parts.len() != 2 {
        return false;
    }
    let safe = parts[1]
        .bytes()
        .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'-' | b'.'))
        && !parts[1].starts_with('.');
    let public_asset = parts[1].rsplit_once('.').is_some_and(|(stem, ext)| {
        matches!(ext, "png" | "wav" | "mp4")
            && !stem.is_empty()
            && stem.bytes().all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'-'))
    });
    safe && ((parts[0] == "assets" && public_asset)
        || (parts[0] == "licenses" && (parts[1].ends_with(".txt") || parts[1].ends_with(".md"))))
}

fn validate_public_application(files: &BTreeMap<String, Vec<u8>>) -> Result<(), String> {
    let Some(application) = files.get("pc/application.json") else { return Ok(()); };
    let runtime = files.get("runtime/widget.json")
        .and_then(|bytes| serde_json::from_slice(bytes).ok())
        .ok_or("PC 应用缺少有效的运行时声明")?;
    crate::app_services::parse(application, &runtime).map(|_| ())
}
fn sensitive_text(text: &str) -> bool {
    let lower = text.to_ascii_lowercase();
    if [
        "begin private key",
        "begin rsa private key",
        "begin openssh private key",
        "/users/",
        "/home/",
        "/root/",
        "/etc/",
        ":\\users\\",
        ":/users/",
    ]
    .iter()
    .any(|x| lower.contains(x))
    {
        return true;
    }
    let urls = regex::Regex::new(r#"https?://[^\s\"'<>\\)]+"#).expect("static regex");
    for item in urls.find_iter(text) {
        if let Ok(url) = reqwest::Url::parse(item.as_str()) {
            let host = url.host_str().unwrap_or("");
            if !url.username().is_empty()
                || url.password().is_some()
                || host == "localhost"
                || [".internal", ".local", ".lan", ".localhost"]
                    .iter()
                    .any(|s| host.ends_with(s))
            {
                return true;
            }
            if let Ok(ip) = host.trim_matches(['[', ']']).parse::<std::net::IpAddr>() {
                if !crate::media_network::public_ip(ip) {
                    return true;
                }
            }
        }
    }
    regex::Regex::new(r"\b(sk-|gh[pousr]_|github_pat_)[A-Za-z0-9_-]{20,}")
        .expect("static regex")
        .is_match(text)
}
fn sensitive_json(value: &Value) -> bool {
    match value {
        Value::Object(map) => map.iter().any(|(k, v)| {
            let key = k.to_lowercase().replace(['_', '-'], "");
            ([
                "apikey",
                "accesstoken",
                "refreshtoken",
                "password",
                "secret",
                "clientsecret",
                "authorization",
                "privatekey",
            ]
            .contains(&key.as_str())
                && !v.is_null()
                && v != ""
                && v != false)
                || sensitive_json(v)
        }),
        Value::Array(values) => values.iter().any(sensitive_json),
        Value::String(s) => sensitive_text(s),
        _ => false,
    }
}

fn collect(
    root: &Path,
    current: &Path,
    has_wasm: bool,
    include_catalog: bool,
    files: &mut BTreeMap<String, Vec<u8>>,
    total: &mut usize,
) -> Result<(), String> {
    for entry in fs::read_dir(current).map_err(|_| "无法读取组件文件")? {
        let entry = entry.map_err(|_| "无法读取组件目录")?;
        let ty = entry.file_type().map_err(|_| "无法检查组件文件")?;
        if ty.is_symlink() {
            return Err("资源包不允许符号链接".into());
        }
        let path = entry.path();
        let name = path
            .strip_prefix(root)
            .map_err(|_| "组件路径越界")?
            .to_string_lossy()
            .replace('\\', "/");
        if ty.is_dir() {
            if !["runtime", "assets", "pc", "licenses"].contains(&name.as_str()) {
                return Err("资源包中包含非发布目录，请移出私人文件后重试".into());
            }
            collect(root, &path, has_wasm, include_catalog, files, total)?;
        } else if ty.is_file() {
            // Keep the legacy declarative archive and send the cover separately.
            if name == crate::clawpkg::HACHI_COVER_PATH || name == "hachiapp.json" && !has_wasm {
                continue;
            }
            if name == WASM_MODULE && !has_wasm {
                return Err("组件在准备发布期间发生变化，请重试".into());
            }
            if !(allowed_file(&name) || include_catalog && name == crate::component_extension::CATALOG_PATH) {
                return Err(format!("不可发布的文件：{name}"));
            }
            if files.len() >= 128
                || entry.metadata().map_err(|_| "无法检查资源大小")?.len() > file_limit(&name)
            {
                return Err("组件资源超出发布预算".into());
            }
            let bytes = crate::clawpkg::read_clawpkg_source_file(&path, file_limit(&name))?;
            *total += bytes.len();
            if *total > 16 * 1024 * 1024 {
                return Err("组件展开大小超过 16 MiB".into());
            }
            if name.ends_with(".json") {
                let v: Value = serde_json::from_slice(&bytes).map_err(|_| "组件 JSON 无效")?;
                if sensitive_json(&v) {
                    return Err(format!("{name} 可能含密钥、内部地址或个人路径，请检查"));
                }
            } else if name.ends_with(".txt") || name.ends_with(".md") {
                if sensitive_text(std::str::from_utf8(&bytes).map_err(|_| "声明文件需为 UTF-8")?)
                {
                    return Err(format!("{name} 可能含敏感信息，请检查"));
                }
            }
            files.insert(name, bytes);
        } else {
            return Err("资源包含特殊文件".into());
        }
    }
    Ok(())
}
fn public_content_hash(path:&Path)->Result<String,String>{
    let mut files=BTreeMap::new();let mut total=0;
    collect(path,path,path.join(WASM_MODULE).is_file(),true,&mut files,&mut total)?;
    let mut hash=Sha256::new();for (name,bytes) in files{hash.update(name.as_bytes());hash.update([0]);hash.update(bytes);hash.update([0]);}
    Ok(format!("{:x}",hash.finalize()))
}
fn cache_cover(home:&Path,id:&str,work:&Value)->Result<(),String>{
    let Some(hash)=work["covers"].as_array().and_then(|v|v.first()).and_then(Value::as_str).filter(|v|valid_hash(v)) else{return Ok(());};
    let bytes=response_bytes(network::send(home,&format!("{ORIGIN}/api/v1/images/{hash}"),None,Default::default())?,2*1024*1024)?;
    if digest(&bytes)!=hash{return Err("Community cover digest mismatch".into());}
    let mut reader=image::ImageReader::new(Cursor::new(&bytes)).with_guessed_format().map_err(|e|e.to_string())?;
    let mut limits=image::Limits::default();limits.max_image_width=Some(2048);limits.max_image_height=Some(2048);limits.max_alloc=Some(24*1024*1024);reader.limits(limits);
    if reader.format()!=Some(image::ImageFormat::Jpeg){return Err("Invalid community cover".into());}
    reader.decode().map_err(|e|e.to_string())?;
    let dir=root(home).join("covers");fs::create_dir_all(&dir).map_err(|e|e.to_string())?;
    let content=work["contentHash"].as_str().filter(|v|valid_hash(v)).ok_or("Invalid content hash")?;
    let mut file=tempfile::NamedTempFile::new_in(&dir).map_err(|e|e.to_string())?;file.write_all(&bytes).map_err(|e|e.to_string())?;
    file.persist(dir.join(format!("{id}-{}.jpg",&content[..16]))).map_err(|e|e.to_string())?;Ok(())
}
struct Prepared {
    entry: crate::component_library::ComponentLibraryEntry,
    files: BTreeMap<String, Vec<u8>>,
    hash: String,
    archive: Vec<u8>,
    expanded: usize,
}
// Validate the complete downloaded content before the library is mutated. The
// archive digest authenticates transport; this binds its manifest and every file
// to the published identity, including the full (not display-truncated) hash.
fn verify_download(bytes: &[u8], work: &Value) -> Result<bool, String> {
    let mut archive = zip::ZipArchive::new(Cursor::new(bytes)).map_err(|_| "组件压缩包无效")?;
    if archive.has_duplicate_names() {
        return Err("组件包含重复路径".into());
    }
    if archive.len() > 128 {
        return Err("资源包文件过多".into());
    }
    let mut files = BTreeMap::new();
    let mut names = std::collections::BTreeSet::new();
    let mut total = 0;
    for index in 0..archive.len() {
        let mut file = archive.by_index(index).map_err(|_| "无法读取组件压缩包")?;
        let name = file.name().map_err(|_| "组件压缩包文件名无效")?.into_owned();
        let limit = file_limit(&name);
        if file.is_dir()
            || !(allowed_file(&name) || work["official"] == true && name == crate::component_extension::CATALOG_PATH)
            || !names.insert(name.to_lowercase())
            || file.unix_mode().is_some_and(|m| m & 0o170000 == 0o120000)
            || file.size() > limit
        {
            return Err("组件包含不允许的文件、重复路径或链接".into());
        }
        let mut data = Vec::new();
        (&mut file)
            .take(limit + 1)
            .read_to_end(&mut data)
            .map_err(|_| "组件包读取失败")?;
        total += data.len();
        if data.len() as u64 > limit || total > 16 * 1024 * 1024 {
            return Err("组件包展开超出预算".into());
        }
        if name.ends_with(".json")
            && sensitive_json(
                &serde_json::from_slice::<Value>(&data).map_err(|_| "组件 JSON 无效")?,
            )
        {
            return Err("组件包含敏感数据，已停止导入".into());
        }
        files.insert(name, data);
    }
    let manifest: Value =
        serde_json::from_slice(files.get("component.json").ok_or("缺少组件清单")?)
            .map_err(|_| "组件清单无效")?;
    if manifest["id"] != work["id"] || manifest["version"] != work["version"] {
        return Err("组件身份或版本不匹配，未导入".into());
    }
    let has_wasm = files.contains_key(WASM_MODULE);
    if has_wasm != files.contains_key("hachiapp.json") {
        return Err("WASM 文件或包清单不完整，未导入".into());
    }
    let catalog = crate::component_extension::CATALOG_PATH;
    let extension = files.get("pc/extension.json")
        .map(|data| crate::component_extension::parse(data))
        .transpose()?;
    let declaration = extension.as_ref().and_then(|value| value.search.catalog.as_deref());
    match (declaration, files.get(catalog)) {
        (Some(path), Some(bytes)) if path == catalog => {
            crate::component_extension::Extension::validate_catalog_bytes(bytes)?;
        }
        (None, None) => {}
        _ => return Err("包内电台目录与 PC 扩展声明不匹配".into()),
    }
    validate_public_application(&files)?;
    let mut hash = Sha256::new();
    for (name, data) in files {
        hash.update(name.as_bytes());
        hash.update([0]);
        hash.update(data);
        hash.update([0]);
    }
    if work["contentHash"].as_str() != Some(format!("{:x}", hash.finalize()).as_str()) {
        return Err("组件文件摘要不匹配，未导入".into());
    }
    Ok(has_wasm)
}
fn install_download(
    home: &Path, id: &str, version: &str, hash: &str, work: &Value, bytes: &[u8],
    before_import: impl FnOnce() -> Result<(), String>,
) -> Result<crate::component_library::ComponentLibraryEntry, String> {
    if digest(bytes) != hash || work["packageBytes"].as_u64() != Some(bytes.len() as u64) {
        return Err("下载内容完整性校验失败，未导入组件".into());
    }
    let has_wasm = verify_download(bytes, work)?;
    let temp = tempfile::tempdir().map_err(|_| "无法创建下载暂存区")?;
    let archive = temp.path().join(if has_wasm {
        "package.hachiapp"
    } else {
        "package.clawpkg"
    });
    fs::write(&archive, bytes).map_err(|_| "无法暂存下载")?;
    // Canonical server ZIPs store files only, so an empty assets directory
    // can be absent. Recreate it after verification without changing any bytes.
    let staged = temp.path().join("package");
    crate::component_library::extract_zip_safe(&archive, &staged)?;
    fs::create_dir_all(staged.join("assets")).map_err(|_| "无法暂存组件素材目录")?;
    let validation = crate::clawpkg::validate_clawpkg_at_path(&staged)?;
    if !validation.ok {
        return Err(format!("下载的组件未通过本地 PetUI 校验：{}", validation.errors.join("；")));
    }
    if validation.manifest.as_ref().is_none_or(|manifest| manifest.id != id) {
        return Err("下载的组件清单与作品不匹配".into());
    }
    // Preserve provenance before the library entry becomes visible.
    before_import()?;
    let entry = crate::component_library::publish_source(home, &staged)?;
    if entry.version_hash
        != work["contentHash"]
            .as_str()
            .unwrap_or("")
            .get(..16)
            .unwrap_or("")
        || entry.version != version
    {
        return Err("导入后的组件版本不匹配，请勿安装".into());
    }
    Ok(entry)
}
fn prepare(home: &Path, path: &str) -> Result<Prepared, String> {
    let source = PathBuf::from(path);
    let canonical = source.canonicalize().map_err(|_| "组件源不存在")?;
    let library = crate::component_library::library_root(home)
        .canonicalize()
        .map_err(|_| "正式本地组件库不可用")?;
    if !canonical.starts_with(&library)
        || canonical
            .strip_prefix(&library)
            .map_err(|_| "组件源无效")?
            .components()
            .count()
            != 2
        || source
            .symlink_metadata()
            .map_err(|_| "无法检查组件源")?
            .file_type()
            .is_symlink()
    {
        return Err("只能发布正式本地组件库中的完整组件".into());
    }
    let entry = crate::component_library::inspect(&canonical)?;
    if !entry.valid {
        return Err(format!(
            "组件校验失败：{}",
            entry.validation_errors.join("；")
        ));
    }
    if entry.media_source.as_deref() == Some("audio.player") {
        return Err("当前版本未开放此媒体音源，不能通过社区启用".into());
    }
    if !safe_id(&entry.id) || reserved(&entry.id) {
        return Err("预置组件 ID 不能作为个人作品发布，请为原创作品使用独立 ID".into());
    }
    let local = state(home)?;
    if local.downloaded.contains_key(&entry.id) && !local.owned.contains_key(&entry.id) {
        return Err(
            "下载的作品不能直接冒充自己的创作；派生作品需使用新 ID 并注明来源与授权".into(),
        );
    }
    let version =
        semver::Version::parse(&entry.version).map_err(|_| "公开发布需使用 1.0.0 格式版本")?;
    if !version.pre.is_empty() || !version.build.is_empty() {
        return Err("公开发布请使用不含预发布或构建后缀的版本".into());
    }
    let mut files = BTreeMap::new();
    let mut expanded = 0;
    let has_wasm = entry.wasm_preview.is_some();
    collect(&canonical, &canonical, has_wasm, false, &mut files, &mut expanded)?;
    validate_public_application(&files)?;
    if has_wasm && (!files.contains_key(WASM_MODULE) || !files.contains_key("hachiapp.json")) {
        return Err("组件在准备发布期间发生变化，请重试".into());
    }
    let mut hash = Sha256::new();
    let mut archive = zip::ZipWriter::new(Cursor::new(Vec::new()));
    for (name, bytes) in &files {
        hash.update(name.as_bytes());
        hash.update([0]);
        hash.update(bytes);
        hash.update([0]);
        archive
            .start_file(
                name,
                zip::write::SimpleFileOptions::default()
                    .compression_method(zip::CompressionMethod::Deflated)
                    .unix_permissions(0o644),
            )
            .map_err(|_| "无法打包组件")?;
        archive.write_all(bytes).map_err(|_| "无法打包组件")?;
    }
    let archive = archive.finish().map_err(|_| "无法完成资源包")?.into_inner();
    if archive.len() > MAX_PACKAGE {
        return Err("压缩组件包超过 8 MiB".into());
    }
    Ok(Prepared {
        entry,
        files,
        hash: format!("{:x}", hash.finalize()),
        archive,
        expanded,
    })
}

fn check_server_publication(home: &Path, prepared: &Prepared) -> Result<bool, String> {
    let config = request(home, "/api/v1/config", None, false, false)?;
    if config["publicationPreflight"] != true {
        return Ok(false);
    }
    let result = request(
        home,
        "/api/v1/works/preflight",
        Some(json!({"package": base64::engine::general_purpose::STANDARD.encode(&prepared.archive)})),
        true,
        false,
    )?;
    if result["ok"] != true || result["id"] != prepared.entry.id
        || result["contentHash"] != prepared.hash
    {
        return Err("社区预检结果与组件包不一致，请重新检查".into());
    }
    Ok(true)
}

pub fn execute(home: &Path, input: CommunityInput) -> Result<Value, String> {
    let id = input.id.as_deref().unwrap_or("");
    if !id.is_empty() && !safe_id(id) && !(input.action == "image" && valid_hash(id)) {
        return Err("组件标识无效".into());
    }
    match input.action.as_str() {
        "browse" => request(
            home,
            &with_query(
                "/api/v1/works",
                &[
                    (
                        "q",
                        input.query.unwrap_or_default().chars().take(80).collect(),
                    ),
                    ("category", input.category.unwrap_or_default()),
                    ("order", input.order.unwrap_or_else(|| "featured".into())),
                    ("offset", input.offset.unwrap_or(0).to_string()),
                ],
            ),
            None,
            false,
            false,
        ),
        "rankings" => request(
            home,
            &with_query(
                "/api/v1/rankings",
                &[
                    ("metric", input.metric.unwrap_or_else(|| "downloads".into())),
                    ("window", input.window.unwrap_or_else(|| "7d".into())),
                ],
            ),
            None,
            false,
            false,
        ),
        "detail" => request(home, &format!("/api/v1/works/{id}"), None, false, true),
        "comments" => request(
            home,
            &with_query(
                &format!("/api/v1/works/{id}/comments"),
                &[("offset", input.offset.unwrap_or(0).to_string())],
            ),
            None,
            false,
            false,
        ),
        "config" => Ok(
            json!({"baseUrl":ORIGIN,"contactUrl":"https://github.com/YizhengWw/HachimoDock/issues","schemaVersion":1}),
        ),
        "local" => {
            let mut value =
                serde_json::to_value(state(home)?).map_err(|_| "社区来源状态不可读取")?;
            value["reservedIds"] = json!(RESERVED);
            Ok(value)
        }
        "mine" => {
            let _lock = MUTATION.lock().map_err(|_| "社区操作繁忙")?;
            let result = request(home, "/api/v1/mine", None, true, false)?;
            let mut local = state(home)?;
            if let Some(items) = result["items"].as_array() {
                for work in items {
                    if let Some(id) = work["id"].as_str().filter(|s| safe_id(s)) {
                        local.owned.insert(id.into(), work.clone());
                    }
                }
            }
            save(home, &local)?;
            Ok(result)
        }
        "image" => {
            if !valid_hash(id) {
                return Err("截图标识无效".into());
            }
            let cache = IMAGE_CACHE.get_or_init(|| Mutex::new(BTreeMap::new()));
            if let Ok(cache) = cache.lock() {
                if let Some(value) = cache.get(id) {
                    return Ok(json!({"dataUrl":value}));
                }
            }
            let bytes = response_bytes(
                network::send(home,&format!("{ORIGIN}/api/v1/images/{id}"),None,Default::default())?,
                2 * 1024 * 1024,
            )?;
            if digest(&bytes) != id {
                return Err("截图完整性校验失败".into());
            }
            let mut reader = image::ImageReader::new(Cursor::new(&bytes))
                .with_guessed_format()
                .map_err(|_| "截图格式无效")?;
            let mut limits = image::Limits::default();
            limits.max_image_width = Some(2048);
            limits.max_image_height = Some(2048);
            limits.max_alloc = Some(24 * 1024 * 1024);
            reader.limits(limits);
            if reader.format() != Some(image::ImageFormat::Jpeg) {
                return Err("社区截图格式不受支持".into());
            }
            reader.decode().map_err(|_| "截图解码失败")?;
            let value = format!(
                "data:image/jpeg;base64,{}",
                base64::engine::general_purpose::STANDARD.encode(bytes)
            );
            if let Ok(mut cache) = cache.lock() {
                if cache.len() >= 40 {
                    cache.pop_first();
                }
                cache.insert(id.into(), value.clone());
            }
            Ok(json!({"dataUrl":value}))
        }
        "prepare" => {
            let prepared = prepare(home, input.path.as_deref().ok_or("请选择本地组件")?)?;
            let server_checked = check_server_publication(home, &prepared)?;
            let license = prepared
                .files
                .get("LICENSE.txt")
                .is_some_and(|b| !b.is_empty());
            Ok(
                json!({"id":prepared.entry.id,"name":prepared.entry.name,"version":prepared.entry.version,"description":prepared.entry.description,"kind":prepared.entry.kind,"contentHash":prepared.hash,"packageBytes":prepared.archive.len(),"expandedBytes":prepared.expanded,"files":prepared.files.keys().collect::<Vec<_>>(),"hasLicense":license,"canPublish":true,"coverDataUrl":prepared.entry.cover_data_url,"warning":if server_checked {"社区预检通过。提交后进入审核。"} else {"本地校验通过。社区会在提交时校验。"}}),
            )
        }
        "publish" => {
            let _lock = MUTATION.lock().map_err(|_| "社区操作繁忙")?;
            let prepared = prepare(home, input.path.as_deref().ok_or("请选择本地组件")?)?;
            if input.expected_content_hash.as_deref() != Some(prepared.hash.as_str()) {
                return Err("组件文件在预览后发生变化，请重新检查再发布".into());
            }
            let covers = input.covers.ok_or("请选择真实截图")?;
            if covers.is_empty()
                || covers.len() > 3
                || covers.iter().any(|s| s.len() > 3 * 1024 * 1024)
            {
                return Err("请选择 1–3 张不超过 2 MiB 的截图".into());
            }
            let metadata = input.metadata.ok_or("缺少作品介绍")?;
            if sensitive_json(&metadata) {
                return Err("作品介绍可能含内部地址或敏感信息，请检查".into());
            }
            let result = request(
                home,
                "/api/v1/works",
                Some(
                    json!({"requestId":input.request_id.ok_or("缺少发布请求编号")?,"metadata":metadata,"package":base64::engine::general_purpose::STANDARD.encode(&prepared.archive),"covers":covers}),
                ),
                true,
                true,
            )?;
            if result["id"] != prepared.entry.id || result["contentHash"] != prepared.hash {
                return Err("服务端发布回执不匹配，请刷新我的创作核对，不要重复创建作品".into());
            }
            let mut local = state(home)?;
            local.owned.insert(prepared.entry.id, result.clone());
            save(home, &local)?;
            Ok(result)
        }
        "download" => {
            let _lock = MUTATION.lock().map_err(|_| "社区操作繁忙")?;
            if !safe_id(id) {
                return Err("此社区组件标识不可下载".into());
            }
            let mut local = state(home)?;
            let existing = crate::component_library::list(home)?
                .components
                .into_iter()
                .find(|c| c.id == id);
            let ticket = request(
                home,
                &format!("/api/v1/works/{id}/download"),
                Some(json!({})),
                false,
                true,
            )?;
            let work = &ticket["work"];
            if let Some(expected) = input.expected_content_hash.as_deref() {
                if work["contentHash"].as_str() != Some(expected) {
                    return Err("社区版本已变化，请刷新详情并重新确认下载".into());
                }
            }
            if reserved(id) && work["official"] != true {
                return Err("保留组件 ID 只接受社区服务器认证的维护者作品".into());
            }
            if work["requirements"]["media"] == "audio.player" {
                return Err("此音源尚未开放，暂不支持下载".into());
            }
            let hash = work["sha256"]
                .as_str()
                .filter(|s| valid_hash(s))
                .ok_or("下载摘要无效")?;
            if work["id"] != id {
                return Err("下载回执与作品不匹配".into());
            }
            let version = work["version"].as_str().ok_or("版本无效")?;
            if let Some(existing) = &existing {
                let official_bundled = work["official"] == true && existing.is_bundled_copy;
                if !official_bundled && !local.downloaded.contains_key(id) && !local.owned.contains_key(id) {
                    return Err("本机已有同 ID 的非社区组件，已停止下载，避免覆盖你的创作".into());
                }
                let local_version = semver::Version::parse(&existing.version).map_err(|_| "本机组件版本待核对")?;
                let remote_version = semver::Version::parse(version).map_err(|_| "社区组件版本无效")?;
                if local_version > remote_version {
                    return Err("本机版本更高，已阻止降级".into());
                }
                if local_version == remote_version && !official_bundled
                    && public_content_hash(Path::new(&existing.path))?.as_str() != work["contentHash"].as_str().unwrap_or("")
                {
                    return Err("本机同版本内容不同，已停止覆盖".into());
                }
            }
            let bytes = response_bytes(
                network::send(home,&format!("{ORIGIN}/api/v1/packages/{hash}"),None,Default::default())?,
                MAX_PACKAGE as u64,
            )?;
            let entry = install_download(home, id, version, hash, work, &bytes, || {
                local.downloaded.insert(id.into(), work.clone());
                save(home, &local)
            })?;
            // The separately fetched cover is optional and does not affect installation.
            if let Err(error) = cache_cover(home, id, work) {
                eprintln!("[community] cover: {error}");
            }
            let confirmed = request(
                home,
                &format!("/api/v1/works/{id}/confirm-download"),
                Some(json!({"ticket":ticket["ticket"],"sha256":hash})),
                false,
                true,
            )
            .is_ok();
            Ok(json!({"entry":entry,"work":work,"downloadCountConfirmed":confirmed}))
        }
        "like" => request(
            home,
            &format!("/api/v1/works/{id}/like"),
            Some(json!({"active":input.active.ok_or("缺少点赞状态")?})),
            false,
            true,
        ),
        "comment" => request(
            home,
            &format!("/api/v1/works/{id}/comments"),
            Some(
                json!({"name":input.name.ok_or("请填写显示名")?,"text":input.text.ok_or("请填写评论")?,"requestId":input.request_id.ok_or("缺少评论编号")?}),
            ),
            false,
            true,
        ),
        _ => Err("未知社区操作".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn public_identity_ignores_only_nonpublished_cover_and_declarative_envelope() {
        let root=tempfile::tempdir().unwrap();
        fs::create_dir(root.path().join("pc")).unwrap();
        fs::write(root.path().join("component.json"),br#"{"id":"sample"}"#).unwrap();
        let hash=public_content_hash(root.path()).unwrap();
        fs::write(root.path().join("pc/cover.png"),b"cover").unwrap();
        fs::write(root.path().join("hachiapp.json"),br#"{"format":"hachiapp/1"}"#).unwrap();
        assert_eq!(hash,public_content_hash(root.path()).unwrap());
        fs::write(root.path().join("component.json"),br#"{"id":"changed"}"#).unwrap();
        assert_ne!(hash,public_content_hash(root.path()).unwrap());
        assert!(allowed_file("assets/intro.mp4"));
        assert!(!allowed_file("assets/intro.exe"));
    }
    #[test]
    fn public_endpoint_and_paths() {
        assert!(ORIGIN.starts_with("https://"));
        assert!(!safe_id("../x"));
        assert!(safe_id("focus-timer"));
        assert!(reserved("stock-watchlist"));
        assert!(!allowed_file("private/key.pem"));
        assert!(allowed_file("LICENSE.txt"));
        assert!(allowed_file("licenses/font-LICENSE.txt"));
        assert!(allowed_file("pc/application.json"));
        assert!(!allowed_file("pc/player.json"));
        assert!(!allowed_file("assets/a.js"));
    }
    #[test]
    fn local_music_declaration_can_be_shared_without_local_player_data() {
        let home = tempfile::tempdir().unwrap();
        let source = Path::new(env!("CARGO_MANIFEST_DIR")).join("../candidate-clawpkgs/local-music-player");
        let entry = crate::component_library::publish_source(home.path(), &source).unwrap();
        let package = prepare(home.path(), &entry.path).unwrap();
        assert!(package.files.contains_key("pc/application.json"));
        let work = json!({"id": entry.id, "version": entry.version, "contentHash": package.hash});
        assert_eq!(verify_download(&package.archive, &work), Ok(false));

        let mut files = package.files;
        files.insert("pc/application.json".into(), br#"{"service":"unsupported"}"#.to_vec());
        assert!(validate_public_application(&files).is_err());
    }
    #[test]
    fn community_archive_keeps_wasm_manifest_and_omits_desktop_cover() {
        let root = tempfile::tempdir().unwrap();
        for dir in ["pc", "runtime"] {
            fs::create_dir(root.path().join(dir)).unwrap();
        }
        for (name, data) in [
            ("component.json", b"{}".as_slice()),
            ("hachiapp.json", b"{}"),
            (crate::clawpkg::HACHI_COVER_PATH, b"cover"),
            ("runtime/widget.json", b"{}"),
        ] {
            fs::write(root.path().join(name), data).unwrap();
        }
        let mut files = BTreeMap::new();
        let mut total = 0;
        collect(root.path(), root.path(), false, false, &mut files, &mut total).unwrap();
        assert_eq!(files.keys().map(String::as_str).collect::<Vec<_>>(),
            ["component.json", "runtime/widget.json"]);
        assert_eq!(total, 4);
        fs::write(root.path().join(WASM_MODULE), b"wasm").unwrap();
        files.clear();
        collect(root.path(), root.path(), true, false, &mut files, &mut 0).unwrap();
        assert_eq!(files.keys().map(String::as_str).collect::<Vec<_>>(),
            ["component.json", "hachiapp.json", WASM_MODULE, "runtime/widget.json"]);
        assert!(!allowed_file("runtime/extra.wasm"));
        fs::write(root.path().join(WASM_MODULE), vec![0; file_limit(WASM_MODULE) as usize + 1]).unwrap();
        assert!(collect(root.path(), root.path(), true, false, &mut BTreeMap::new(), &mut 0).is_err());
    }
    #[test]
    fn blocks_private_values() {
        for value in [
            "http://127.0.0.1/",
            "https://192.168.0.1/",
            "https://fixture.internal/",
            "/Users/private/data",
            concat!("-----BEGIN ", "PRIVATE KEY-----"),
        ] {
            assert!(sensitive_text(value), "{value}");
        }
        assert!(sensitive_json(&json!({"apiKey":"abc"})));
        assert!(!sensitive_json(
            &json!({"title":"专注","source":"https://github.com/example/project"})
        ));
    }
    #[test]
    fn downloaded_manifest_and_full_hash_are_bound_before_import() {
        let data = br#"{"id":"sample","version":"1.0.0"}"#;
        let mut hash = Sha256::new();
        hash.update(b"component.json\0");
        hash.update(data);
        hash.update([0]);
        let mut zip = zip::ZipWriter::new(Cursor::new(Vec::new()));
        zip.start_file("component.json", zip::write::SimpleFileOptions::default())
            .unwrap();
        zip.write_all(data).unwrap();
        let bytes = zip.finish().unwrap().into_inner();
        let mut work =
            json!({"id":"sample","version":"1.0.0","contentHash":format!("{:x}",hash.finalize())});
        assert!(verify_download(&bytes, &work).is_ok());
        work["version"] = json!("2.0.0");
        assert!(verify_download(&bytes, &work).is_err());
        work["version"] = json!("1.0.0");
        work["contentHash"] = json!("a".repeat(64));
        assert!(verify_download(&bytes, &work).is_err());
    }
    #[test]
    #[ignore = "read-only production HTTPS check; run explicitly after deployment"]
    fn public_service_tls_read_only() {
        let home = tempfile::tempdir().unwrap();
        let result = request(home.path(), "/api/v1/health", None, false, false).unwrap();
        assert_eq!(result["ok"], true);
        let works=request(home.path(),"/api/v1/works?order=featured&offset=0",None,false,false).unwrap();
        assert!(works["items"].is_array());
        request(home.path(),"/api/v1/rankings?metric=downloads&window=7d",None,false,false).unwrap();
        assert!(!root(home.path()).join("private").exists());
    }
    #[test]
    #[ignore = "requires an explicitly staged, offline-validated official batch"]
    fn staged_community_packages_import_with_exact_content_hash() {
        let directory = PathBuf::from(std::env::var("PET_COMMUNITY_TEST_BATCH").expect("staged canonical directory"));
        let records: Vec<Value> = serde_json::from_slice(&fs::read(directory.join("catalog.json")).unwrap()).unwrap();
        assert!(!records.is_empty());
        let home = tempfile::tempdir().unwrap();
        for work in records {
            let id = work["id"].as_str().unwrap();
            assert!(safe_id(id));
            let path = directory.join(format!("{id}.clawpkg"));
            let bytes = fs::read(&path).unwrap();
            let entry = install_download(
                home.path(), id, work["version"].as_str().unwrap(), work["sha256"].as_str().unwrap(),
                &work, &bytes, || Ok(()),
            ).unwrap_or_else(|error| panic!("{id}: {error}"));
            assert!(entry.valid, "{id}: {:?}", entry.validation_errors);
            assert_eq!(Some(entry.version_hash.as_str()), work["contentHash"].as_str().map(|v| &v[..16]), "{id}");
            assert_eq!(work["version"], entry.version, "{id}");
            println!("PASS: {id} verified and imported into isolated library");
        }
    }
}
