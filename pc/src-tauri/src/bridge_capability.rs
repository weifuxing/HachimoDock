//! Owner-only local capability bootstrap shared by approval and voice callers.
//! Credentials are never obtained from HTTP, URLs, redirects, or diagnostics.
use serde_json::Value;
use std::io::Read;
use std::path::PathBuf;
use std::time::Duration;

#[derive(Clone)]
pub(crate) struct Capability {
    pub base_url: String,

    pub token: String,
    pub epoch: String,
}

#[cfg(windows)]
pub(crate) mod managed {
    use super::Capability;
    use rand::{rngs::OsRng, RngCore};
    use serde::Serialize;
    use std::sync::{Mutex, OnceLock};

    #[derive(Serialize)]
    struct Tokens {
        read: String,
        control: String,
        hook: String,
    }

    #[derive(Serialize)]
    pub(crate) struct Bootstrap {
        epoch: String,
        tokens: Tokens,
    }

    fn active() -> &'static Mutex<Option<(u32, Bootstrap)>> {
        static ACTIVE: OnceLock<Mutex<Option<(u32, Bootstrap)>>> = OnceLock::new();
        ACTIVE.get_or_init(|| Mutex::new(None))
    }

    pub(crate) fn fresh() -> Result<Bootstrap, String> {
        fn secret() -> Result<String, String> {
            let mut bytes = [0u8; 32];
            OsRng.try_fill_bytes(&mut bytes).map_err(|_| "bridge random source unavailable")?;
            Ok(bytes.iter().map(|byte| format!("{byte:02x}")).collect())
        }
        Ok(Bootstrap {
            epoch: secret()?,
            tokens: Tokens { read: secret()?, control: secret()?, hook: secret()? },
        })
    }

    pub(crate) fn set(pid: u32, bootstrap: Bootstrap) -> Result<(), String> {
        *active().lock().map_err(|_| "bridge capability state unavailable")? = Some((pid, bootstrap));
        Ok(())
    }

    pub(crate) fn clear() {
        if let Ok(mut current) = active().lock() {
            current.take();
        }
    }

    pub(super) fn load(role: &str) -> Option<Capability> {
        let current = active().lock().ok()?;
        let (pid, bootstrap) = current.as_ref()?;
        if !crate::managed_runtime::is_live(*pid) {
            return None;
        }
        let (port, token) = match role {
            "read" => (crate::DEFAULT_AGENT_BUS_PORT, &bootstrap.tokens.read),
            "control" => (crate::DEFAULT_BRIDGE_PORT, &bootstrap.tokens.control),
            "hook" => (crate::DEFAULT_BRIDGE_PORT, &bootstrap.tokens.hook),
            _ => return None,
        };
        Some(Capability {
            base_url: format!("http://127.0.0.1:{port}"),
            token: token.clone(),
            epoch: bootstrap.epoch.clone(),
        })
    }
}

pub(crate) fn load(role: &str) -> Result<Capability, String> {
    #[cfg(windows)]
    if let Some(capability) = managed::load(role) {
        return Ok(capability);
    }
    let name = if role == "hook" { "hook.json" } else { "client.json" };
    let file = std::env::var_os("HACHIMODOCK_BRIDGE_CAPABILITY_DIR")
        .map(|p| PathBuf::from(p).join(name))
        .or_else(|| {
            std::env::var_os(if cfg!(windows) { "USERPROFILE" } else { "HOME" })
                .map(|h| PathBuf::from(h).join(".claw-pet").join("bridge-runtime").join(name))
        })
        .ok_or("bridge capability unavailable")?;
    let dir = file.parent().ok_or("bridge capability unavailable")?;
    let dm = std::fs::symlink_metadata(dir).map_err(|_| "bridge unpaired")?;
    if !dm.is_dir() || dm.file_type().is_symlink() {
        return Err("unsafe bridge capability directory".into());
    }
    let mut options = std::fs::OpenOptions::new();
    options.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
        if dm.uid() != unsafe { libc::geteuid() } || dm.mode() & 0o077 != 0 {
            return Err("unsafe bridge capability permissions".into());
        }
        options.custom_flags(libc::O_NOFOLLOW);
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::{MetadataExt, OpenOptionsExt};
        if dm.file_attributes() & 0x400 != 0 {
            return Err("unsafe bridge reparse directory".into());
        }
        crate::dsh_windows_acl::verify_file(&file)?;
        // Open the reparse entry itself rather than following it, and prohibit
        // sharing writes/deletes while the protected descriptor is being read.
        options.custom_flags(0x00200000).share_mode(1);
    }
    #[cfg(not(any(unix, windows)))]
    {
        return Err("bridge capability ACL verification unavailable on this platform".into());
    }
    #[cfg(any(unix, windows))]
    {
        let mut f = options.open(&file).map_err(|_| "bridge unpaired")?;
        let m = f.metadata().map_err(|_| "bridge unpaired")?;
        if !m.is_file() || m.len() > 4096 {
            return Err("unsafe bridge capability file".into());
        }
        #[cfg(unix)]
        {
            use std::os::unix::fs::MetadataExt;
            if m.uid() != unsafe { libc::geteuid() } || m.mode() & 0o077 != 0 {
                return Err("unsafe bridge capability file".into());
            }
        }
        #[cfg(windows)]
        {
            use std::os::windows::fs::MetadataExt;
            if m.file_attributes() & 0x400 != 0 {
                return Err("unsafe bridge reparse file".into());
            }
            crate::dsh_windows_acl::verify_file(&file)?;
        }
        let mut raw = String::new();
        f.by_ref()
            .take(4097)
            .read_to_string(&mut raw)
            .map_err(|_| "bridge unpaired")?;
        let v: Value = serde_json::from_str(&raw).map_err(|_| "invalid bridge capability")?;
        let port_key = if matches!(role, "control" | "hook") { "hookPort" } else if role == "read" { "busPort" } else { return Err("invalid bridge role".into()); };
        let port = v[port_key].as_u64().filter(|p| *p > 0 && *p <= 65535).ok_or("bridge endpoint unavailable")?;
        if v["version"] != 1 { return Err("invalid bridge descriptor".into()); }
        let epoch = v["epoch"].as_str().ok_or("invalid bridge epoch")?;
        let token = v["tokens"][role]
            .as_str()
            .ok_or("invalid bridge capability role")?;
        if ![epoch, token]
            .iter()
            .all(|s| s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit()))
        {
            return Err("invalid bridge capability".into());
        }
        Ok(Capability {
            base_url: format!("http://127.0.0.1:{port}"),
            token: token.into(),
            epoch: epoch.into(),
        })
    }
}

pub(crate) fn load_hook() -> Result<Capability, String> {
    load("hook")
}

// Renderer accepts only these local routes; it never receives a credential or
// chooses an origin. Do not add authentication to generic http_request_text.
fn operation(route: &str, body: &Option<Value>) -> Result<(&'static str, reqwest::Url), String> {
    if route.len() > 4096 || !route.starts_with('/') || route.starts_with("//") {
        return Err("invalid bridge operation".into());
    }
    let url = reqwest::Url::parse(&format!("http://127.0.0.1{route}")).map_err(|_| "invalid bridge route")?;
    if url.fragment().is_some() || url.host_str() != Some("127.0.0.1") { return Err("invalid bridge route".into()); }
    match url.path() {
        "/agent/status" | "/agent/sessions" | "/agent/session-events" if body.is_none() => {
            if url.query_pairs().any(|(k,v)| !["agentId","limit","cursor","streamId"].contains(&k.as_ref()) || v.len() > 1024) {
                return Err("invalid bridge query".into());
            }
            Ok(("read", url))
        }
        "/mock-button-inject" if url.query().is_none() && body.as_ref().is_some_and(|v| v.is_object() && v.to_string().len() <= 65536) => Ok(("control", url)),
        _ => Err("invalid bridge operation".into()),
    }
}

pub(crate) async fn request(route: String, body: Option<Value>) -> Result<Value, String> {
    let (role, path) = operation(&route, &body)?;
    // ACL validation may invoke the bundled Windows helper; keep it off the UI executor.
    let cap = tokio::task::spawn_blocking(move || load(role)).await.map_err(|_| "bridge unavailable")??;
    let client = reqwest::Client::builder().no_proxy().redirect(reqwest::redirect::Policy::none())
        .timeout(Duration::from_secs(if role == "control" { 150 } else { 3 }))
        .build().map_err(|_| "bridge unavailable")?;
    let mut url = reqwest::Url::parse(&cap.base_url).map_err(|_| "invalid bridge endpoint")?;
    url.set_path(path.path()); url.set_query(path.query());
    let req = if let Some(body) = body { client.post(url).json(&body) } else { client.get(url) };
    let mut response = req.bearer_auth(&cap.token).send().await.map_err(|_| "bridge request failed; not resent")?;
    if !response.status().is_success() { return Err(format!("bridge rejected request (HTTP {})", response.status().as_u16())); }
    let mut bytes = Vec::new();
    while let Some(chunk) = response.chunk().await.map_err(|_| "bridge response unavailable")? {
        if bytes.len() + chunk.len() > 1024 * 1024 { return Err("bridge response too large".into()); }
        bytes.extend_from_slice(&chunk);
    }
    serde_json::from_slice(&bytes).map_err(|_| "invalid bridge response".into())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn limits_renderer_to_fixed_operations() {
        for route in ["http://example.test/agent/status", "//example.test/agent/status", "/agent/inject", "/agent/status#fragment", "/agent/status?token=secret", "/mock-button-inject?url=x"] {
            assert!(operation(route, &None).is_err());
        }
        assert!(operation("/agent/sessions?agentId=codex&limit=20", &None).is_ok());
        assert!(operation("/mock-button-inject", &Some(serde_json::json!({"text":"test"}))).is_ok());
        assert!(operation("/agent/status", &Some(serde_json::json!({}))).is_err());
        assert!(operation("/mock-button-inject", &Some(Value::String("x".repeat(65537)))).is_err());
    }
}

pub(crate) fn blocking_client(timeout: Duration) -> Result<reqwest::blocking::Client, String> {
    reqwest::blocking::Client::builder().no_proxy().redirect(reqwest::redirect::Policy::none())
        .timeout(timeout).build().map_err(|_| "bridge unavailable".into())
}
