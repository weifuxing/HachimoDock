//! PetUI local source builds and package publishing.
use crate::{clawpkg, component_library};
use serde::Deserialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    env,
    ffi::{OsStr, OsString},
    fs,
    io::{self, Read, Write},
    path::{Component, Path, PathBuf},
    process::{Command, Stdio},
    sync::Mutex,
    time::{Duration, Instant},
};

#[path = "petui_sprite.rs"]
mod sprite;
#[path = "petui_data_link.rs"]
mod data_link;

static PREPARE: Mutex<()> = Mutex::new(());
const MAX_PACKAGE_BYTES: usize = 7 * 1024 * 1024;
const WASM: &str = "runtime/app.wasm";
const WIDGET: &str = "runtime/widget.json";
const PROBE_SOURCE: &str = r#"#define HACHI_NATIVE_CANVAS 1
#include "hachi.h"
void hachi_init(void) { hachi_canvas640(); }
void hachi_event(int a, int b, int c) { (void)a; (void)b; (void)c; }
void hachi_frame(int ms) { (void)ms; hachi_rect(0, 0, 20, 20, 0xff00ff); }
"#;

#[derive(Deserialize)]
struct Manifest {
    version: String,
    target: String,
    files: BTreeMap<String, String>,
}

struct Compiler {
    clang: PathBuf,
    linker: PathBuf,
    skill: PathBuf,
}

fn target() -> &'static str {
    match (env::consts::OS, env::consts::ARCH) {
        ("macos", "aarch64") => "macos-arm64",
        ("macos", "x86_64") => "macos-x64",
        ("windows", "x86_64") => "windows-x64",
        _ => "",
    }
}

fn local_resources() -> Result<PathBuf, String> {
    #[cfg(test)]
    if let Some(resources) = env::var_os("PETUI_TEST_RESOURCES") {
        return Ok(PathBuf::from(resources));
    }
    let exe = env::current_exe().map_err(|e| e.to_string())?;
    let dir = exe.parent().ok_or("无法找到 PetUI 安装目录")?;
    Ok(if cfg!(target_os = "macos") {
        dir.join("../Resources")
    } else {
        dir.to_owned()
    })
}

fn development_resource(path: PathBuf, fallback: &str) -> PathBuf {
    if cfg!(debug_assertions) && !path.is_dir() {
        return Path::new(env!("CARGO_MANIFEST_DIR")).join(fallback);
    }
    path
}

fn sha(path: &Path) -> io::Result<String> {
    let mut digest = Sha256::new();
    io::copy(&mut fs::File::open(path)?, &mut digest)?;
    Ok(format!("{:x}", digest.finalize()))
}

impl Compiler {
    fn open(resources: &Path, skill: &Path) -> Result<Self, String> {
        let root = development_resource(
            resources.join("authoring-runtime"),
            "authoring-runtime-bundle",
        );
        let bytes =
            fs::read(root.join("manifest.json")).map_err(|_| "安装包缺少 PetUI 创作工具")?;
        let manifest: Manifest =
            serde_json::from_slice(&bytes).map_err(|_| "PetUI 工具清单无法读取")?;
        let clang = format!("bin/clang{}", env::consts::EXE_SUFFIX);
        let linker = format!("bin/wasm-ld{}", env::consts::EXE_SUFFIX);
        let checker = format!("bin/petui-wasm-check{}", env::consts::EXE_SUFFIX);
        if target().is_empty()
            || manifest.target != target()
            || !manifest.version.starts_with("wasi27-v4-")
            || ![&clang, &linker, &checker]
                .into_iter()
                .all(|name| manifest.files.contains_key(name))
        {
            return Err("PetUI 工具版本或系统架构不匹配".into());
        }
        for (name, hash) in manifest.files {
            let relative = Path::new(&name);
            if relative
                .components()
                .any(|part| !matches!(part, Component::Normal(_)))
            {
                return Err("PetUI 工具清单含无效路径".into());
            }
            let path = root.join(relative);
            if !fs::symlink_metadata(&path).is_ok_and(|meta| meta.is_file())
                || !sha(&path).is_ok_and(|actual| actual == hash)
            {
                return Err("安装包内的 PetUI 创作工具校验失败，请重新安装客户端".into());
            }
        }
        Ok(Self {
            clang: root.join(clang),
            linker: root.join(linker),
            skill: skill.to_owned(),
        })
    }

    fn compile(&self, code: &Path) -> Result<Vec<u8>, String> {
        self.compile_with(code, local_python().as_deref())
    }

    fn compile_with(&self, code: &Path, python: Option<&Path>) -> Result<Vec<u8>, String> {
        let code = code.canonicalize().map_err(|e| e.to_string())?;
        let work = tempfile::tempdir().map_err(|e| e.to_string())?;
        let object = work.path().join("guest.o");
        let wasm = work.path().join("guest.wasm");
        let sdk = self.skill.join("sdk");
        if let Some(python) = python {
            let mut child = Command::new(python);
            child
                .args(["-I", "-B", "-X", "utf8"])
                .arg(self.skill.join("scripts/compile_wasm.py"))
                .args([&code, &sdk, &self.clang, &self.linker, &wasm]);
            run_tool(child, 125, "PetUI 编译")?;
        } else {
            let mut clang = Command::new(&self.clang);
            clang
                .args([
                    "--no-default-config",
                    "--target=wasm32-unknown-unknown",
                    "-mcpu=mvp",
                    "-I",
                ])
                .arg(sdk)
                .args([
                    "-std=c11",
                    "-Os",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-ffreestanding",
                    "-fno-builtin",
                    "-c",
                ])
                .arg(code)
                .arg("-o")
                .arg(&object);
            run_tool(clang, 60, "PetUI C 编译")?;

            for memory in [131072, 262144] {
                let mut linker = Command::new(&self.linker);
                linker
                    .args(["--no-entry", "--export-memory", "--strip-all"])
                    .arg(format!("--initial-memory={memory}"))
                    .arg(format!("--max-memory={memory}"))
                    .args(["-z", "stack-size=16384"])
                    .arg(&object)
                    .arg("-o")
                    .arg(&wasm);
                match run_tool(linker, 30, "PetUI WASM 链接") {
                    Ok(()) => break,
                    Err(error)
                        if memory == 131072 && error.contains("initial memory too small") => {}
                    Err(error) => return Err(error),
                }
            }
        }
        clawpkg::read_clawpkg_source_file(&wasm, clawpkg::HACHI_MAX_MODULE_BYTES as u64)
    }
}

pub(crate) fn checker() -> Result<PathBuf, String> {
    let root = development_resource(
        local_resources()?.join("authoring-runtime"),
        "authoring-runtime-bundle",
    );
    let checker = root.join(format!("bin/petui-wasm-check{}", env::consts::EXE_SUFFIX));
    if !checker.is_file() {
        return Err("安装包缺少固件同源 WASM 检查器，请更新 Pet Manager".into());
    }
    Ok(checker)
}

fn run_wasm_check(module: &[u8], trace: Option<&Path>) -> Result<(), String> {
    let checker = checker()?;
    let work = tempfile::tempdir().map_err(|e| e.to_string())?;
    let path = work.path().join("app.wasm");
    fs::write(&path, module).map_err(|e| e.to_string())?;
    for seed in ["1939960805","1234567","987654321"] {
        let mut command = Command::new(&checker);
        command.arg(&path).env("PETUI_TEST_SEED",seed);
        run_tool(command, 15, "组件 WASM 运行检查")?;
    }
    if let Some(trace) = trace {
        let mut command = Command::new(&checker);
        command.arg(&path).arg(trace);
        run_tool(command, 60, "组件 WASM 交互回放")?;
    }
    Ok(())
}

pub(super) fn validate_wasm(module: &[u8]) -> Result<(), String> {
    run_wasm_check(module, None)
}

fn write_sprite(source: &Path, output: &Path) -> Result<String, String> {
    let image = sprite::encode(source)?;
    let work = tempfile::tempdir().map_err(|e| e.to_string())?;
    let candidate = work.path().join("sprite.bin");
    fs::write(&candidate, &image.bytes).map_err(|e| e.to_string())?;
    let mut command = Command::new(checker()?);
    command
        .arg("--sprite")
        .arg(image.width.to_string())
        .arg(image.height.to_string())
        .arg(candidate);
    run_tool(command, 10, &format!("{} 精灵解码检查", source.display()))?;
    let parent = output
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    let mut staged = tempfile::NamedTempFile::new_in(parent).map_err(|e| e.to_string())?;
    if output.extension().is_some_and(|ext| ext == "h") {
        let name = output
            .file_stem()
            .and_then(OsStr::to_str)
            .filter(|name| {
                name.as_bytes()
                    .first()
                    .is_some_and(|byte| byte.is_ascii_alphabetic())
                    && name
                        .bytes()
                        .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
            })
            .ok_or("精灵头文件名必须是有效的 C 标识符")?;
        writeln!(
            staged,
            "#pragma once\n#include <stdint.h>\nenum {{ {name}_width = {}, {name}_height = {} }};",
            image.width, image.height
        )
        .map_err(|e| e.to_string())?;
        writeln!(staged, "static const uint8_t {name}[] = {{").map_err(|e| e.to_string())?;
        for row in image.bytes.chunks(16) {
            write!(staged, "    ").map_err(|e| e.to_string())?;
            for byte in row {
                write!(staged, "{byte},").map_err(|e| e.to_string())?;
            }
            writeln!(staged).map_err(|e| e.to_string())?;
        }
        writeln!(staged, "}};").map_err(|e| e.to_string())?;
    } else {
        staged.write_all(&image.bytes).map_err(|e| e.to_string())?;
    }
    staged.persist(output).map_err(|e| e.to_string())?;
    Ok(json!({"width":image.width,"height":image.height,"bytes":image.bytes.len()}).to_string())
}

fn local_python() -> Option<PathBuf> {
    let names: &[&str] = if cfg!(windows) {
        &["python3.exe", "python.exe"]
    } else {
        &["python3"]
    };
    let search = env::var_os("PATH").unwrap_or_default();
    let mut dirs: Vec<_> = env::split_paths(&search).collect();
    if cfg!(target_os = "macos") {
        dirs.extend(
            [
                "/opt/homebrew/bin",
                "/usr/local/bin",
                "/Library/Developer/CommandLineTools/usr/bin",
            ]
            .into_iter()
            .map(PathBuf::from),
        );
    }
    dirs.into_iter()
        .filter(|dir| dir.is_absolute())
        .filter(|dir| {
            !dir.components()
                .any(|part| part.as_os_str().eq_ignore_ascii_case("WindowsApps"))
        })
        .flat_map(|dir| names.iter().map(move |name| dir.join(name)))
        .filter_map(|path| path.canonicalize().ok())
        .filter(|path| {
            path != Path::new("/usr/bin/python3")
                && path.is_file()
                && !path.components().any(|part| {
                    part.as_os_str().eq_ignore_ascii_case(".claw-pet")
                        || part.as_os_str().eq_ignore_ascii_case("WindowsApps")
                })
        })
        .find(|path| {
            let mut command = Command::new(path);
            command.args([
                "-I",
                "-B",
                "-c",
                "import sys, subprocess, tempfile; sys.exit(sys.version_info < (3, 9))",
            ]);
            run_tool(command, 3, "本机 Python 检查").is_ok()
        })
}

fn run_tool(mut command: Command, timeout_secs: u64, label: &str) -> Result<(), String> {
    let mut log = tempfile::tempfile().map_err(|e| e.to_string())?;
    command
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(log.try_clone().map_err(|e| e.to_string())?);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000);
    }
    let mut child = command.spawn().map_err(|e| format!("{label}：{e}"))?;
    let deadline = Instant::now() + Duration::from_secs(timeout_secs);
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status,
            Ok(None) if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(20)),
            result => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(match result {
                    Err(e) => format!("{label}：{e}"),
                    _ => format!("{label}超时"),
                });
            }
        }
    };
    if status.success() {
        return Ok(());
    }
    use std::io::{Seek, SeekFrom};
    let mut message = Vec::new();
    let _ = log
        .seek(SeekFrom::Start(0))
        .and_then(|_| log.take(3000).read_to_end(&mut message));
    Err(format!(
        "{label}失败（{status}）\n{}",
        String::from_utf8_lossy(&message).trim()
    ))
}

fn write_if_changed(path: &Path, bytes: &[u8], executable: bool) -> Result<(), String> {
    let unchanged = fs::read(path).is_ok_and(|old| old == bytes);
    #[cfg(unix)]
    let executable_ok = {
        use std::os::unix::fs::PermissionsExt;
        !executable || fs::metadata(path).is_ok_and(|meta| meta.permissions().mode() & 0o111 != 0)
    };
    #[cfg(not(unix))]
    let executable_ok = {
        let _ = executable;
        true
    };
    if unchanged && executable_ok {
        return Ok(());
    }
    let mut file = tempfile::NamedTempFile::new_in(path.parent().ok_or("PetUI 配置路径无效")?)
        .map_err(|e| e.to_string())?;
    file.write_all(bytes).map_err(|e| e.to_string())?;
    #[cfg(unix)]
    if executable {
        use std::os::unix::fs::PermissionsExt;
        file.as_file()
            .set_permissions(fs::Permissions::from_mode(0o755))
            .map_err(|e| e.to_string())?;
    }
    file.persist(path).map_err(|e| e.to_string())?;
    Ok(())
}

pub(super) fn install_launcher(home: &Path, skill: &Path) -> Result<PathBuf, String> {
    checker()?;
    let _lock = PREPARE.lock().map_err(|_| "PetUI 创作工具正在更新")?;
    let workspace = home.join(".claw-pet/authoring");
    fs::create_dir_all(&workspace).map_err(|e| e.to_string())?;
    let name = if cfg!(windows) { "petui.cmd" } else { "petui" };
    let launcher = fs::read(skill.join("scripts").join(name)).map_err(|e| e.to_string())?;
    let executable = env::current_exe().map_err(|e| e.to_string())?;
    let executable = executable
        .to_str()
        .filter(|path| !path.contains(['\r', '\n']))
        .ok_or("PetUI 客户端路径无法写入启动器")?;
    write_if_changed(
        &workspace.join("desktop-path"),
        format!(
            "{executable}
"
        )
        .as_bytes(),
        false,
    )?;
    let command = workspace.join(name);
    write_if_changed(&command, &launcher, true)?;
    Ok(command)
}

pub(super) fn ensure_healthy(skill: &Path) -> Result<(), String> {
    static HEALTH: std::sync::OnceLock<Mutex<Option<String>>> = std::sync::OnceLock::new();
    let resources = local_resources()?;
    let root = development_resource(resources.join("authoring-runtime"), "authoring-runtime-bundle");
    let key = format!("{}:{}:{}", env::current_exe().map_err(|e| e.to_string())?.display(),
        sha(&root.join("manifest.json")).map_err(|e|e.to_string())?, env!("CARGO_PKG_VERSION"));
    let mut health = HEALTH.get_or_init(Default::default).lock().map_err(|_| "PetUI health busy")?;
    if health.as_ref() == Some(&key) { return Ok(()); }
    let compiler = Compiler::open(&resources, skill)?;
    let work = tempfile::tempdir().map_err(|e|e.to_string())?;
    let code = work.path().join("probe.c");
    fs::write(&code, PROBE_SOURCE).map_err(|e|e.to_string())?;
    validate_wasm(&compiler.compile(&code)?)?;
    *health = Some(key);
    Ok(())
}

pub(super) fn build_project(
    resources: &Path,
    skill: &Path,
    project: &Path,
    output: &Path,
) -> Result<(), String> {
    let code = project.join("app.c");
    let module = match fs::symlink_metadata(&code) {
        Ok(meta) if meta.is_file() => Some(Compiler::open(resources, skill)?.compile(&code)?),
        Ok(_) => return Err("app.c 必须是普通文件".into()),
        Err(e) if e.kind() == io::ErrorKind::NotFound => None,
        Err(e) => return Err(e.to_string()),
    };
    let trace = project.join("game.trace");
    pack_with_trace(
        &project.join("package"),
        module,
        output,
        trace.is_file().then_some(trace.as_path()),
    )
}

fn pack(source: &Path, compiled: Option<Vec<u8>>, output: &Path) -> Result<(), String> {
    pack_with_trace(source, compiled, output, None)
}

fn pack_with_trace(
    source: &Path,
    compiled: Option<Vec<u8>>,
    output: &Path,
    trace: Option<&Path>,
) -> Result<(), String> {
    if !output
        .extension()
        .and_then(OsStr::to_str)
        .is_some_and(|s| s.eq_ignore_ascii_case("hachiapp"))
    {
        return Err("输出必须为 .hachiapp".into());
    }
    if !fs::symlink_metadata(source).is_ok_and(|meta| meta.is_dir()) {
        return Err("组件源必须是本地普通目录".into());
    }
    let mut files: BTreeMap<_, _> = clawpkg::collect_clawpkg_directory_files(source)?
        .into_iter()
        .collect();
    if files.len() > 120 || files.values().map(Vec::len).sum::<usize>() > MAX_PACKAGE_BYTES {
        return Err("组件超过打包大小或文件数量上限".into());
    }
    for name in files.keys() {
        let suffix = name
            .rsplit_once('.')
            .map(|(_, ext)| ext.to_ascii_lowercase())
            .unwrap_or_default();
        if component_library::FORBIDDEN_COMPONENT_SUFFIXES.contains(&suffix.as_str())
            || (suffix == "wasm" && name != WASM)
        {
            return Err(format!("组件内不允许该文件：{name}"));
        }
    }
    if !files.contains_key(clawpkg::HACHI_COVER_PATH) {
        return Err("交付包缺少封面 pc/cover.png".into());
    }
    let compiled_source = compiled.is_some();
    let module = compiled.or_else(|| files.remove(WASM));
    let parse = |name: &str| -> Result<Value, String> {
        serde_json::from_slice(files.get(name).ok_or_else(|| format!("缺少 {name}"))?)
            .map_err(|e| format!("{name}：{e}"))
    };
    let component = parse("component.json")?;
    let mut share = parse("share.json")?;
    let share_fields = share.as_object_mut().ok_or("share.json 必须是对象")?;
    let missing_title = share_fields
        .get("title")
        .is_none_or(|value| value.as_str().is_some_and(|title| title.trim().is_empty()));
    let share_default = component["name"].as_str()
        .filter(|name| missing_title && !name.trim().is_empty());
    if let Some(name) = share_default {
        share_fields.insert("title".into(), json!(name));
    }
    let widget = parse(WIDGET)?;
    let mut widget = widget
        .as_object()
        .cloned()
        .ok_or("runtime/widget.json 必须是对象")?;
    let id = component["id"].as_str().ok_or("组件 ID 不合法")?;
    if widget.get("media").is_some_and(|media| media["source"] == "audio.player") {
        return Err(clawpkg::LEGACY_AUDIO_SYNC_BLOCK.into());
    }
    clawpkg::complete_p4_runtime_containers(&mut widget);
    clawpkg::validate_p4_runtime_shell(&Value::Object(widget.clone()))?;
    let require_game = compiled_source && component["kind"] == "game";
    if let Some(module) = module {
        let (canvas, host, _, _, audio) =
            clawpkg::wasm_profile_details(&module).ok_or("WASM 格式、宿主导入或固定内存无效")?;
        let buttons = parse("buttons.json")?;
        let bindings = crate::wasm_input_contract::bindings(id, &buttons)?;
        let require_touch = compiled_source && component["wasmTouch"] == true;
        if (require_game || require_touch) && trace.is_none() {
            return Err("新编译的 WASM 小游戏或触屏组件需要 game.trace，覆盖开局和关键交互".into());
        }
        let staged_trace = trace
            .map(|path| stage_input_trace(path, &bindings, require_game, require_touch))
            .transpose()?;
        run_wasm_check(
            &module,
            staged_trace.as_ref().map(|trace| trace.path()),
        )?;
        for (key, enabled, profile) in [
            ("hachiCanvas", canvas, clawpkg::HACHI_NATIVE_CANVAS),
            ("hachiHost", host, clawpkg::HACHI_HOST_PROFILE),
            ("hachiAudio", audio, crate::wasm_audio::PROFILE),
        ] {
            if enabled {
                widget.insert(key.into(), json!(profile));
            } else {
                widget.remove(key);
            }
        }
        if audio {
            let bank = files.get(crate::wasm_audio::ASSET).ok_or("缺少 WASM 语音资源 assets/speech.hab")?;
            crate::wasm_audio::validate(bank)?;
            widget.insert("hachiAudioSha256".into(), json!(format!("{:x}", Sha256::digest(bank))));
        } else {
            widget.remove("hachiAudioSha256");
        }
        widget.insert(
            "hachiWasm".into(),
            json!(format!("{:x}", Sha256::digest(&module))),
        );
        files.insert(WASM.into(), module);
    }
    if share_default.is_some() {
        files.insert("share.json".into(), serde_json::to_vec(&share).map_err(|e| e.to_string())?);
    }
    files.insert(WIDGET.into(), serde_json::to_vec(&widget).map_err(|e| e.to_string())?);
    files.insert(
        "hachiapp.json".into(),
        serde_json::to_vec(&json!({"format":"hachiapp/1","id":id})).map_err(|e| e.to_string())?,
    );
    let parent = output
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    let mut staged = tempfile::NamedTempFile::new_in(parent).map_err(|e| e.to_string())?;
    let mut zip = zip::ZipWriter::new(staged.as_file_mut());
    let options =
        zip::write::SimpleFileOptions::default().compression_method(zip::CompressionMethod::Stored);
    zip.add_directory("assets/", options)
        .map_err(|e| e.to_string())?;
    for (name, bytes) in files {
        zip.start_file(name, options).map_err(|e| e.to_string())?;
        zip.write_all(&bytes).map_err(|e| e.to_string())?;
    }
    zip.finish().map_err(|e| e.to_string())?;
    let staged = staged.into_temp_path();
    validate(&staged)?;
    staged.persist(output).map_err(|e| e.to_string())?;
    Ok(())
}

fn validate(path: &Path) -> Result<(), String> {
    let result = clawpkg::validate_clawpkg_at_path(path)?;
    if result.ok {
        Ok(())
    } else {
        Err(format!("组件包校验失败：{}", result.errors.join("；")))
    }
}

fn stage_input_trace(
    path: &Path,
    bindings: &BTreeMap<&str, u8>,
    require_game: bool,
    require_touch: bool,
) -> Result<tempfile::NamedTempFile, String> {
    let bytes = clawpkg::read_clawpkg_source_file(path, 512 * 1024)?;
    let replay = std::str::from_utf8(&bytes).map_err(|_| "game.trace 必须使用 UTF-8")?;
    let resolved =
        crate::wasm_input_contract::resolve_trace(replay, bindings, require_game, require_touch)?;
    let mut staged = tempfile::NamedTempFile::new().map_err(|e| e.to_string())?;
    staged
        .write_all(resolved.as_bytes())
        .map_err(|e| e.to_string())?;
    Ok(staged)
}

fn check_package_wasm(path: &Path, trace: Option<&Path>, require_wasm: bool) -> Result<(), String> {
    validate(path)?;
    let files = if path.is_dir() {
        clawpkg::collect_clawpkg_directory_files(path)?
    } else {
        let mut archive = zip::ZipArchive::new(fs::File::open(path).map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
        let mut files = std::collections::HashMap::new();
        for name in [WASM, "component.json", "buttons.json"] {
            match archive.by_name(name) {
                Ok(mut entry) => {
                    let max = clawpkg::HACHI_MAX_MODULE_BYTES as u64;
                    if entry.size() > max {
                        return Err(format!("{name} 超过容量限制"));
                    }
                    let mut bytes = Vec::new();
                    (&mut entry)
                        .take(max + 1)
                        .read_to_end(&mut bytes)
                        .map_err(|e| e.to_string())?;
                    if bytes.len() as u64 > max {
                        return Err(format!("{name} 超过容量限制"));
                    }
                    files.insert(name.to_owned(), bytes);
                }
                Err(zip::result::ZipError::FileNotFound) => {}
                Err(e) => return Err(e.to_string()),
            }
        }
        files
    };
    let Some(module) = files.get(WASM) else {
        return if require_wasm {
            Err("组件包缺少 runtime/app.wasm".into())
        } else {
            Ok(())
        };
    };
    let staged_trace = trace
        .map(|trace| {
            let json = |name| -> Result<Value, String> {
                serde_json::from_slice(files.get(name).ok_or_else(|| format!("缺少 {name}"))?)
                    .map_err(|e| e.to_string())
            };
            let component = json("component.json")?;
            let buttons = json("buttons.json")?;
            let bindings = crate::wasm_input_contract::bindings(
                component["id"].as_str().unwrap_or_default(),
                &buttons,
            )?;
            stage_input_trace(trace, &bindings, false, component["wasmTouch"] == true)
        })
        .transpose()?;
    run_wasm_check(
        module,
        staged_trace.as_ref().map(|trace| trace.path()),
    )
}

pub(super) fn cli(args: &[OsString]) -> Result<String, String> {
    match args {
        [command, source] if command == "publish-json" => {
            let home=crate::get_home_dir()?;
            let entry=component_library::publish_source(&home,Path::new(source))?;
            serde_json::to_string(&entry).map_err(|e|e.to_string())
        }
        [command] if command == "tool-info" => Ok(json!({"version":env!("CARGO_PKG_VERSION"),"protocol":2,"target":target()}).to_string()),
        [command, args @ ..] if command == "data-link" => {
            let [runtime, report, scope @ ..] = args else { return Err(data_link::USAGE.into()); };
            data_link::run(Path::new(runtime), Path::new(report), scope)
        }
        [command, args @ ..] if command == "feed-credential" => crate::lan_agent::private_feed::cli(args),
        [command] if command == "doctor" => {
            let resources = local_resources()?;
            let skill = development_resource(resources.join("skills/petui"), "../skills/petui");
            let work = tempfile::tempdir().map_err(|e| e.to_string())?;
            let code = work.path().join("probe.c");
            fs::write(&code, PROBE_SOURCE).map_err(|e| e.to_string())?;
            let module = Compiler::open(&resources, &skill)?.compile(&code)?;
            if clawpkg::wasm_profile(&module) != Some((true, false, 2)) {
                return Err("PetUI 创作工具链自检未生成原生画布 WASM".into());
            }
            validate_wasm(&module)?;
            Ok(json!({"ok":true, "wasmBytes":module.len()}).to_string())
        }
        [command, source, output] if command == "pack" => {
            pack(Path::new(source), None, Path::new(output))?;
            Ok(Path::new(output).display().to_string())
        }
        [command, source, output] if command == "sprite" => write_sprite(Path::new(source), Path::new(output)),
        [command, package] if command == "verify-wasm" => {
            check_package_wasm(Path::new(package), None, true)?;
            Ok("组件 WASM 运行检查通过".into())
        }
        [command, package, flag, trace] if command == "verify-wasm" && flag == "--trace" => {
            check_package_wasm(Path::new(package), Some(Path::new(trace)), true)?;
            Ok("组件 WASM 交互回放通过".into())
        }
        [command, code, source, output, rest @ ..] if command == "build" => {
            let code = Path::new(code);
            let project_trace = code.with_file_name("game.trace");
            let trace = match rest {
                [] => project_trace.is_file().then_some(project_trace.as_path()),
                [flag, file] if flag == "--trace" => Some(Path::new(file)),
                _ => return Err("用法：petui build <app.c> <package> <输出.hachiapp> [--trace <game.trace>]".into()),
            };
            let resources = local_resources()?;
            let skill = development_resource(resources.join("skills/petui"), "../skills/petui");
            let module = Compiler::open(&resources, &skill)?.compile(code)?;
            pack_with_trace(Path::new(source), Some(module), Path::new(output), trace)?;
            Ok(Path::new(output).display().to_string())
        }
        [command, package] if command == "validate" => {
            check_package_wasm(Path::new(package), None, false)?;
            Ok("组件包校验通过".into())
        }
        [command, package] if command == "publish" => {
            let path = Path::new(package);
            let entry = crate::get_home_dir()
                .and_then(|home| component_library::import_hachiapp(&home, path))
                .map_err(|error| format!(
                    "发布到本地组件库失败：{error}。请保留 {}，在 Pet Manager「组件中心 → 导入组件」中导入。正式目录由客户端生成，请勿手动写入。",
                    path.display()
                ))?;
            let mut result = format!("已发布到本地组件库：{} {}（{}）", entry.name, entry.version, entry.version_hash);
            if let Some(reason) = entry.sync_requirements.blocked_reason {
                result.push_str(&format!("\n该包暂不可同步：{reason}"));
            } else if entry.sync_requirements.requires_usb_capabilities() {
                result.push_str("\n同步前请在组件中心连接目标设备，由 USB 握手核对所需服务能力。");
            }
            Ok(result)
        }
        _ => Err(format!("用法：petui doctor | build <app.c> <package> <输出.hachiapp> | pack <package> <输出.hachiapp> | sprite <源.png> <精灵.bin> | verify-wasm <组件包> [--trace <game.trace>] | validate <组件包> | publish <组件包>\n{}", data_link::USAGE)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn source(root: &Path) -> PathBuf {
        let source = root.join("package");
        fs::create_dir_all(source.join("runtime")).unwrap();
        fs::create_dir_all(source.join("pc")).unwrap();
        for (name, value) in [
            (
                "component.json",
                json!({"id":"rust-authoring-test","name":"Rust PetUI","version":"1.0.0"}),
            ),
            ("negative-screen.json", json!({})),
            ("buttons.json", json!([])),
            ("runtime/widget.json", json!({"schema_version":1,"states":["ready"],"initial_state":"ready","dashboard":{}})),
            ("share.json", json!({})),
        ] {
            fs::write(source.join(name), serde_json::to_vec(&value).unwrap()).unwrap();
        }
        image::RgbImage::new(320, 240)
            .save(source.join("pc/cover.png"))
            .unwrap();
        source
    }

    #[test]
    fn pack_validate_and_publish_into_an_empty_library() {
        let scratch = tempfile::tempdir().unwrap();
        let source = source(scratch.path());
        let output = scratch.path().join("game.hachiapp");
        pack(&source, None, &output).unwrap();
        let home = scratch.path().join("test-user");
        let entry = component_library::import_hachiapp(&home, &output).unwrap();
        assert_eq!(entry.id, "rust-authoring-test");
        assert!(Path::new(&entry.path).join("pc/cover.png").is_file());
        let share: Value = serde_json::from_slice(
            &fs::read(Path::new(&entry.path).join("share.json")).unwrap(),
        ).unwrap();
        assert_eq!(share["title"], "Rust PetUI");
        assert_eq!(fs::read(source.join("share.json")).unwrap(), b"{}");
        let again = component_library::import_hachiapp(&home, &output).unwrap();
        assert_eq!(entry.path, again.path);

        let original = fs::read(&output).unwrap();
        let runtime_path = source.join(WIDGET);
        let original_runtime = fs::read(&runtime_path).unwrap();
        let mut legacy: Value = serde_json::from_slice(&original_runtime).unwrap();
        legacy["media"] = json!({"source":"audio.player"});
        fs::write(&runtime_path, serde_json::to_vec(&legacy).unwrap()).unwrap();
        assert_eq!(pack(&source, None, &output).unwrap_err(), clawpkg::LEGACY_AUDIO_SYNC_BLOCK);
        assert_eq!(fs::read(&output).unwrap(), original);
        fs::write(&runtime_path, original_runtime).unwrap();
        let widget_path = source.join(WIDGET);
        let widget_before = fs::read(&widget_path).unwrap();
        let mut widget: Value = serde_json::from_slice(&widget_before).unwrap();
        widget["tick"] = json!({});
        fs::write(&widget_path, serde_json::to_vec(&widget).unwrap()).unwrap();
        assert!(pack(&source, None, &output).unwrap_err().contains("runtime/widget.json.tick"));
        assert_eq!(fs::read(&output).unwrap(), original);
        fs::write(&widget_path, widget_before).unwrap();
        fs::write(source.join("pc/cover.png"), b"invalid PNG").unwrap();
        assert!(pack(&source, None, &output).is_err());
        assert_eq!(fs::read(&output).unwrap(), original);
    }

    #[test]
    fn malformed_sprite_is_rejected_before_publication_and_on_interaction() {
        let Ok(resources) = env::var("PETUI_TEST_RESOURCES") else {
            return;
        };
        let scratch = tempfile::tempdir().unwrap();
        let root = scratch.path();
        let package = source(root);
        let code = root.join("app.c");
        let output = root.join("game.hachiapp");
        let png = root.join("sample.png");
        let header = root.join("sample.h");
        // Indexed PNG: one opaque red entry and one transparent entry.
        let indexed = [
            137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 2, 0, 0, 0, 2,
            1, 3, 0, 0, 0, 72, 120, 159, 103, 0, 0, 0, 6, 80, 76, 84, 69, 255, 0, 0, 0, 0, 0, 65,
            163, 18, 3, 0, 0, 0, 2, 116, 82, 78, 83, 255, 0, 229, 183, 48, 74, 0, 0, 0, 12, 73, 68,
            65, 84, 120, 156, 99, 112, 96, 112, 0, 0, 1, 4, 0, 129, 104, 13, 2, 175, 0, 0, 0, 0,
            73, 69, 78, 68, 174, 66, 96, 130,
        ];
        fs::write(&png, indexed).unwrap();
        write_sprite(&png, &header).unwrap();
        let fixture = |bytes: &str, condition: &str| {
            format!(
                r#"
#include "hachi.h"
#include "sample.h"
static int pressed;
{bytes}
void hachi_init(void) {{ hachi_canvas640(); }}
void hachi_event(int a, int b, int c) {{ (void)a; (void)b; (void)c; pressed = 1; }}
void hachi_frame(int ms) {{ (void)ms; if ({condition}) hachi_sprite(0, 0, sample_width, sample_height, sample, sizeof(sample)); }}
"#
            )
        };
        let skill = Path::new(env!("CARGO_MANIFEST_DIR")).join("../skills/petui");
        let compiler = Compiler::open(Path::new(&resources), &skill).unwrap();
        fs::write(&code, fixture("", "1")).unwrap();
        let wasm = compiler.compile_with(&code, None).unwrap();
        pack(&package, Some(wasm), &output).unwrap();
        let valid = fs::read(&output).unwrap();
        fs::write(&header, "enum { sample_width=2, sample_height=2 }; static const unsigned char sample[]={1,0,0,131,3};").unwrap();
        let wasm = compiler.compile_with(&code, None).unwrap();
        assert!(pack(&package, Some(wasm), &output)
            .unwrap_err()
            .contains("invalid sprite palette index"));
        assert_eq!(fs::read(&output).unwrap(), valid);

        // Simulate a package from a different authoring tool with a correct
        // SHA-256 over a broken module. The atomic import must reject it.
        let malformed = compiler.compile_with(&code, None).unwrap();
        let external = root.join("external.hachiapp");
        let mut input = zip::ZipArchive::new(io::Cursor::new(&valid)).unwrap();
        let mut archive = zip::ZipWriter::new(fs::File::create(&external).unwrap());
        let options = zip::write::SimpleFileOptions::default();
        for i in 0..input.len() {
            let mut entry = input.by_index(i).unwrap();
            if entry.is_dir() {
                archive.add_directory(entry.name().unwrap(), options).unwrap();
                continue;
            }
            let mut bytes = Vec::new();
            entry.read_to_end(&mut bytes).unwrap();
            if entry.name().unwrap() == WASM {
                bytes = malformed.clone();
            } else if entry.name().unwrap() == WIDGET {
                let mut widget: Value = serde_json::from_slice(&bytes).unwrap();
                widget["hachiWasm"] = json!(format!("{:x}", Sha256::digest(&malformed)));
                bytes = serde_json::to_vec(&widget).unwrap();
            }
            archive.start_file(entry.name().unwrap(), options).unwrap();
            archive.write_all(&bytes).unwrap();
        }
        archive.finish().unwrap();
        validate(&external).unwrap();
        let home = root.join("test-user");
        let installed = component_library::import_hachiapp(&home, &output).unwrap();
        let error = component_library::import_hachiapp(&home, &external).unwrap_err();
        assert!(error.contains("invalid sprite palette index"), "{error}");
        assert_eq!(
            component_library::list(&home).unwrap().components[0].version_hash,
            installed.version_hash
        );

        fs::write(&code, fixture("", "pressed")).unwrap();
        let trace = root.join("game.trace");
        fs::write(&trace, "frame 33\nevent 4 5 0\nframe 33\n").unwrap();
        let wasm = compiler.compile_with(&code, None).unwrap();
        assert!(pack_with_trace(&package, Some(wasm), &output, Some(&trace))
            .unwrap_err()
            .contains("invalid sprite palette index"));
        assert_eq!(fs::read(&output).unwrap(), valid);
    }

    #[test]
    fn pinned_compiler_produces_the_same_package_with_or_without_python() {
        let Ok(runtime) = env::var("PETUI_TEST_RESOURCES") else {
            return;
        };
        let scratch = tempfile::tempdir().unwrap();
        let source = source(scratch.path());
        let code = scratch.path().join("game.c");
        fs::write(&code, PROBE_SOURCE).unwrap();
        let skill = Path::new(env!("CARGO_MANIFEST_DIR")).join("../skills/petui");
        let compiler = Compiler::open(Path::new(&runtime), &skill).unwrap();
        let rust_module = compiler.compile_with(&code, None).unwrap();
        let output = scratch.path().join("rust.hachiapp");
        pack(&source, Some(rust_module.clone()), &output).unwrap();
        if let Some(python) = local_python() {
            let python_module = compiler.compile_with(&code, Some(&python)).unwrap();
            assert_eq!(rust_module, python_module);
        }
        let home = scratch.path().join("test-user");
        let entry = component_library::import_hachiapp(&home, &output).unwrap();
        let widget = fs::read(Path::new(&entry.path).join(WIDGET)).unwrap();
        let widget: Value = serde_json::from_slice(&widget).unwrap();
        assert_eq!(widget["hachiCanvas"], clawpkg::HACHI_NATIVE_CANVAS);
        assert_eq!(widget["vars"], json!({}));
        assert_eq!(widget["transitions"], json!([]));
        assert_eq!(widget["tick"], json!([]));
        assert_eq!(
            widget["hachiWasm"],
            format!("{:x}", Sha256::digest(&rust_module))
        );
    }
}
