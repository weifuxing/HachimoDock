use reqwest::blocking::{Client, Response};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    cmp::Reverse,
    collections::HashMap,
    fmt,
    io::Read,
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Arc, Mutex, OnceLock,
    },
    thread::{self, Thread},
    time::Duration,
};

const CONFIG_FILE_NAME: &str = "devin-cloud.secure.json";
const CONFIG_PURPOSE: &str = "devin-cloud";
const API_BASE: &str = "https://api.devin.ai";
const POLL_INTERVAL: Duration = Duration::from_secs(15);
const MAX_BACKOFF: Duration = Duration::from_secs(5 * 60);
const MAX_SESSIONS: usize = 20;
const MAX_RESPONSE_BYTES: u64 = 4 * 1024 * 1024;
const MAX_DISPLAY_CHARS: usize = 320;
const MAX_TITLE_CHARS: usize = 160;

static CLOUD_ENABLED: AtomicBool = AtomicBool::new(false);

#[derive(Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct CloudConfig {
    token: String,
    org_id: String,
    enabled: bool,
}

impl fmt::Debug for CloudConfig {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("CloudConfig")
            .field("token", &"[REDACTED]")
            .field("org_id", &self.org_id)
            .field("enabled", &self.enabled)
            .finish()
    }
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ConfigResponse {
    pub has_token: bool,
    pub org_id: String,
    pub enabled: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct TestResponse {
    pub ok: bool,
    pub message: String,
}

#[derive(Debug, Clone, Deserialize)]
struct DevinIdentity {
    principal_type: String,
    user_id: String,
}

#[derive(Debug, Clone, Deserialize)]
struct DevinSessionPage {
    #[serde(default)]
    items: Vec<DevinSession>,
    #[serde(default)]
    has_next_page: bool,
    end_cursor: Option<String>,
}

#[derive(Debug, Clone, Deserialize)]
struct DevinSession {
    session_id: String,
    status: String,
    status_detail: Option<String>,
    title: Option<String>,
    user_id: Option<String>,
    is_archived: Option<bool>,
    created_at: Option<i64>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct SessionPresentation {
    state: &'static str,
    title: String,
}

#[derive(Default)]
struct BridgeDelivery {
    epoch: String,
    sessions: HashMap<String, SessionPresentation>,
}

impl BridgeDelivery {
    fn publish(
        &mut self,
        epoch: &str,
        sessions: Vec<DevinSession>,
        is_current: impl Fn() -> bool,
        mut post: impl FnMut(&DevinSession, &SessionPresentation) -> bool,
    ) {
        // The owner-private hook descriptor is renewed whenever the bridge
        // starts. Even a restart entirely between polls invalidates old ACKs.
        if self.epoch != epoch {
            self.epoch = epoch.to_owned();
            self.sessions.clear();
        }
        for session in sessions {
            if !is_current() {
                break;
            }
            let Some(presentation) = session_presentation(&session) else {
                continue;
            };
            if self.sessions.get(&session.session_id) != Some(&presentation)
                && post(&session, &presentation)
            {
                self.sessions.insert(session.session_id, presentation);
            }
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PollError {
    Unauthorized,
    Forbidden,
    Retryable,
    Other,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PollAction {
    Continue,
    StopAuthentication,
    Stop,
}

#[derive(Default)]
struct Poller {
    stop: Option<Arc<AtomicBool>>,
    thread: Option<Thread>,
}

static POLLER: OnceLock<Mutex<Poller>> = OnceLock::new();
static LAST_AUTH_ERROR: OnceLock<Mutex<Option<String>>> = OnceLock::new();
static POLL_GENERATION: AtomicU64 = AtomicU64::new(0);

fn poller() -> &'static Mutex<Poller> {
    POLLER.get_or_init(|| Mutex::new(Poller::default()))
}

fn last_auth_error() -> &'static Mutex<Option<String>> {
    LAST_AUTH_ERROR.get_or_init(|| Mutex::new(None))
}

fn config_path(app_data_dir: &Path) -> PathBuf {
    app_data_dir.join(CONFIG_FILE_NAME)
}

fn read_config(path: &Path) -> Result<Option<CloudConfig>, String> {
    crate::secure_config::read(path, CONFIG_PURPOSE)?
        .map(|raw| {
            serde_json::from_str::<CloudConfig>(&raw).map_err(|_| "Devin 配置格式无效".to_string())
        })
        .transpose()
}

fn write_config(path: &Path, config: &CloudConfig) -> Result<(), String> {
    let raw = serde_json::to_vec(config).map_err(|_| "Devin 配置编码失败".to_string())?;
    crate::secure_config::write(path, CONFIG_PURPOSE, &raw)
}

pub fn config_enabled(app_data_dir: &Path) -> bool {
    read_config(&config_path(app_data_dir))
        .ok()
        .flatten()
        .is_some_and(|config| config.enabled)
}

pub fn enabled_for_bridge() -> bool {
    CLOUD_ENABLED.load(Ordering::SeqCst)
}

pub fn get_config(app_data_dir: &Path) -> Result<ConfigResponse, String> {
    let config = read_config(&config_path(app_data_dir))?.unwrap_or(CloudConfig {
        token: String::new(),
        org_id: String::new(),
        enabled: false,
    });
    let error = last_auth_error()
        .lock()
        .map_err(|_| "Devin 状态读取失败".to_string())?
        .clone();
    Ok(ConfigResponse {
        has_token: !config.token.trim().is_empty(),
        org_id: config.org_id,
        enabled: config.enabled,
        error,
    })
}

pub fn save_config(
    app_data_dir: &Path,
    token: Option<String>,
    org_id: String,
    enabled: bool,
) -> Result<(), String> {
    let path = config_path(app_data_dir);
    let mut config = read_config(&path)?.unwrap_or(CloudConfig {
        token: String::new(),
        org_id: String::new(),
        enabled: false,
    });
    if let Some(token) = token.filter(|token| !token.trim().is_empty()) {
        if token.len() > 4096 {
            return Err("Devin PAT 长度无效".to_string());
        }
        config.token = token.trim().to_string();
    }
    let org_id = org_id.trim();
    if org_id.len() > 128
        || !org_id
            .chars()
            .all(|character| character.is_ascii_alphanumeric() || character == '-')
    {
        return Err("Devin 组织 ID 格式无效".to_string());
    }
    config.org_id = org_id.to_string();
    config.enabled = enabled;
    write_config(&path, &config)?;
    if let Ok(mut error) = last_auth_error().lock() {
        *error = None;
    }
    restart_poller(path);
    Ok(())
}

pub fn start_polling(app_data_dir: &Path) {
    restart_poller(config_path(app_data_dir));
}

pub fn stop_polling() {
    if let Ok(mut current) = poller().lock() {
        if let Some(stop) = current.stop.take() {
            stop.store(true, Ordering::SeqCst);
        }
        if let Some(thread) = current.thread.take() {
            thread.unpark();
        }
    }
}

fn restart_poller(path: PathBuf) {
    stop_polling();
    let Ok(Some(config)) = read_config(&path) else {
        CLOUD_ENABLED.store(false, Ordering::SeqCst);
        return;
    };
    let enabled = config.enabled && !config.token.trim().is_empty();
    CLOUD_ENABLED.store(enabled, Ordering::SeqCst);
    if !enabled || config.org_id.trim().is_empty() {
        return;
    }

    if let Ok(mut error) = last_auth_error().lock() {
        *error = None;
    }
    let generation = POLL_GENERATION.fetch_add(1, Ordering::SeqCst) + 1;
    let stop = Arc::new(AtomicBool::new(false));
    let thread_stop = Arc::clone(&stop);
    if let Ok(handle) = thread::Builder::new()
        .name("devin-cloud-poller".into())
        .spawn(move || poll_loop(path, generation, thread_stop))
    {
        if let Ok(mut current) = poller().lock() {
            current.stop = Some(stop);
            current.thread = Some(handle.thread().clone());
        }
    }
}

fn poll_loop(path: PathBuf, generation: u64, stop: Arc<AtomicBool>) {
    let Ok(Some(config)) = read_config(&path) else {
        return;
    };
    let Ok(bridge) = crate::bridge_capability::blocking_client(Duration::from_secs(2)) else {
        return;
    };
    let mut delivery = BridgeDelivery::default();
    let is_current =
        || !stop.load(Ordering::SeqCst) && POLL_GENERATION.load(Ordering::SeqCst) == generation;
    let mut backoff = POLL_INTERVAL;
    while is_current() {
        match poll_once(|| fetch_sessions(&config)) {
            Ok(sessions) => {
                backoff = POLL_INTERVAL;
                if is_current() {
                    publish_sessions(&bridge, &mut delivery, sessions, is_current);
                }
            }
            Err(error) => match poll_action(&error) {
                PollAction::StopAuthentication => {
                    let message = if error == PollError::Unauthorized {
                        "Devin PAT 无效或已过期，请检查连接配置。"
                    } else {
                        "Devin API 权限不足，请检查 PAT 的访问权限。"
                    };
                    if let Ok(mut auth_error) = last_auth_error().lock() {
                        *auth_error = Some(message.to_string());
                    }
                    return;
                }
                PollAction::Continue => {
                    thread::park_timeout(backoff);
                    backoff = (backoff * 2).min(MAX_BACKOFF);
                    continue;
                }
                PollAction::Stop => backoff = POLL_INTERVAL,
            },
        }

        if !is_current() {
            break;
        }
        thread::park_timeout(backoff);
    }
}

fn poll_once(
    fetch: impl FnOnce() -> Result<Vec<DevinSession>, PollError>,
) -> Result<Vec<DevinSession>, PollError> {
    fetch()
}

fn poll_action(error: &PollError) -> PollAction {
    match error {
        PollError::Unauthorized | PollError::Forbidden => PollAction::StopAuthentication,
        PollError::Retryable => PollAction::Continue,
        PollError::Other => PollAction::Stop,
    }
}

fn api_client() -> Result<Client, PollError> {
    Client::builder()
        .timeout(Duration::from_secs(12))
        .build()
        .map_err(|_| PollError::Retryable)
}

fn request_json<T: for<'de> Deserialize<'de>>(
    client: &Client,
    token: &str,
    url: &str,
    query: &[(String, String)],
) -> Result<T, PollError> {
    let mut response = client
        .get(url)
        .bearer_auth(token)
        .query(query)
        .send()
        .map_err(|_| PollError::Retryable)?;
    check_response_status(&response)?;
    let mut body = Vec::new();
    response
        .by_ref()
        .take(MAX_RESPONSE_BYTES + 1)
        .read_to_end(&mut body)
        .map_err(|_| PollError::Retryable)?;
    if body.len() as u64 > MAX_RESPONSE_BYTES {
        return Err(PollError::Other);
    }
    serde_json::from_slice(&body).map_err(|_| PollError::Other)
}

fn check_response_status(response: &Response) -> Result<(), PollError> {
    if response.status().as_u16() == 401 {
        Err(PollError::Unauthorized)
    } else if response.status().as_u16() == 403 {
        Err(PollError::Forbidden)
    } else if response.status().is_server_error() || response.status().as_u16() == 429 {
        Err(PollError::Retryable)
    } else if response.status().is_success() {
        Ok(())
    } else {
        Err(PollError::Other)
    }
}

fn fetch_identity(client: &Client, token: &str) -> Result<DevinIdentity, PollError> {
    let identity: DevinIdentity = request_json(client, token, &format!("{API_BASE}/v3/self"), &[])?;
    if identity.principal_type != "pat_user" || identity.user_id.trim().is_empty() {
        return Err(PollError::Other);
    }
    Ok(identity)
}

fn fetch_sessions(config: &CloudConfig) -> Result<Vec<DevinSession>, PollError> {
    let client = api_client()?;
    let identity = fetch_identity(&client, &config.token)?;
    // Devin v3 SessionResponse timestamps are integer Unix seconds:
    // https://docs.devin.ai/api-reference/v3/sessions/get-organizations-session
    let updated_after = (chrono::Utc::now().timestamp() - 24 * 60 * 60).to_string();
    let url = format!("{API_BASE}/v3/organizations/{}/sessions", config.org_id);
    let mut sessions = Vec::new();
    let mut cursor: Option<String> = None;

    while sessions.len() < MAX_SESSIONS {
        let mut query = vec![
            ("first".to_string(), MAX_SESSIONS.to_string()),
            ("is_archived".to_string(), "false".to_string()),
            ("updated_after".to_string(), updated_after.clone()),
            ("user_ids".to_string(), identity.user_id.clone()),
        ];
        if let Some(after) = cursor.take() {
            query.push(("after".to_string(), after));
        }
        let page: DevinSessionPage = request_json(&client, &config.token, &url, &query)?;
        sessions.extend(page.items.into_iter().filter(|session| {
            session
                .user_id
                .as_deref()
                .map_or(true, |user_id| user_id == identity.user_id)
                && session.is_archived != Some(true)
        }));
        if sessions.len() >= MAX_SESSIONS || !page.has_next_page {
            break;
        }
        cursor = page.end_cursor;
        if cursor.is_none() {
            break;
        }
    }

    sessions.sort_by_key(|session| Reverse(session.created_at.unwrap_or_default()));
    sessions.truncate(MAX_SESSIONS);
    Ok(sessions)
}

pub fn test_connection(app_data_dir: &Path) -> TestResponse {
    let config = match read_config(&config_path(app_data_dir)) {
        Ok(Some(config)) if !config.token.trim().is_empty() => config,
        Ok(_) => {
            return TestResponse {
                ok: false,
                message: "请先保存 Devin PAT。".to_string(),
            }
        }
        Err(_) => {
            return TestResponse {
                ok: false,
                message: "Devin 配置无法读取，请重新保存配置。".to_string(),
            }
        }
    };
    let result = api_client().and_then(|client| fetch_identity(&client, &config.token).map(|_| ()));
    match result {
        Ok(()) => {
            if let Ok(mut auth_error) = last_auth_error().lock() {
                *auth_error = None;
            }
            TestResponse {
                ok: true,
                message: "Devin 连接成功。".to_string(),
            }
        }
        Err(PollError::Unauthorized) => {
            if let Ok(mut auth_error) = last_auth_error().lock() {
                *auth_error = Some("Devin PAT 无效或已过期，请检查连接配置。".to_string());
            }
            TestResponse {
                ok: false,
                message: "Devin PAT 无效或已过期，请检查后重试。".to_string(),
            }
        }
        Err(PollError::Forbidden) => {
            if let Ok(mut auth_error) = last_auth_error().lock() {
                *auth_error = Some("Devin API 权限不足，请检查 PAT 的访问权限。".to_string());
            }
            TestResponse {
                ok: false,
                message: "Devin API 权限不足，请检查 PAT 的访问权限。".to_string(),
            }
        }
        Err(PollError::Retryable) => TestResponse {
            ok: false,
            message: "暂时无法连接 Devin API，请检查网络后重试。".to_string(),
        },
        Err(PollError::Other) => TestResponse {
            ok: false,
            message: "Devin API 返回了无法识别的响应。".to_string(),
        },
    }
}

fn session_presentation(session: &DevinSession) -> Option<SessionPresentation> {
    let state = match session.status.as_str() {
        "new" | "claimed" | "resuming" => "working",
        "running" => match session.status_detail.as_deref() {
            Some("waiting_for_user" | "waiting_for_approval") => "waiting_user",
            Some("finished") => "done",
            _ => "working",
        },
        "exit" => "done",
        "error" => "error",
        "suspended" if session.status_detail.as_deref() == Some("error") => "error",
        "suspended" => "sleeping",
        _ => return None,
    };
    let title = session
        .title
        .as_deref()
        .filter(|title| !title.trim().is_empty())
        .unwrap_or("Devin 会话");
    Some(SessionPresentation {
        state,
        title: truncate_chars(title, MAX_TITLE_CHARS),
    })
}

fn status_text(state: &str) -> &'static str {
    match state {
        "working" => "正在工作",
        "waiting_user" => "等待你操作",
        "done" => "已完成",
        "error" => "执行失败",
        "sleeping" => "已暂停",
        _ => "Devin 会话",
    }
}

fn truncate_chars(value: &str, max_chars: usize) -> String {
    value.chars().take(max_chars).collect()
}

fn build_state_body(session: &DevinSession, presentation: &SessionPresentation) -> Value {
    let title = if presentation.title.trim().is_empty() {
        "Devin 会话".to_string()
    } else {
        presentation.title.clone()
    };
    let content = truncate_chars(
        &format!("{} · {}", status_text(presentation.state), title),
        MAX_DISPLAY_CHARS,
    );
    json!({
        "agent_id": "devin",
        "session_id": session.session_id,
        "event": "CloudStatus",
        "state": presentation.state,
        "session_title": title,
        "session_title_explicit": session.title.as_deref().is_some_and(|value| !value.trim().is_empty()),
        "display_content": content,
    })
}

fn publish_sessions(
    client: &Client,
    delivery: &mut BridgeDelivery,
    sessions: Vec<DevinSession>,
    is_current: impl Fn() -> bool,
) {
    let Ok(capability) = crate::bridge_capability::load_hook() else {
        return;
    };
    let endpoint = format!("{}/state", capability.base_url);
    delivery.publish(
        &capability.epoch,
        sessions,
        is_current,
        |session, presentation| {
            client
                .post(&endpoint)
                .bearer_auth(&capability.token)
                .json(&build_state_body(session, presentation))
                .send()
                .is_ok_and(|response| response.status().is_success())
        },
    );
}

#[cfg(test)]
mod tests {
    use super::*;

    fn session(status: &str, detail: Option<&str>, title: Option<&str>) -> DevinSession {
        DevinSession {
            session_id: "session-1".to_string(),
            status: status.to_string(),
            status_detail: detail.map(str::to_string),
            title: title.map(str::to_string),
            user_id: None,
            is_archived: None,
            created_at: None,
        }
    }

    #[test]
    fn replays_unchanged_sessions_once_after_bridge_restart() {
        let mut delivery = BridgeDelivery::default();
        let mut calls = 0;
        for epoch in ["first", "first", "second", "second"] {
            delivery.publish(
                epoch,
                vec![session("exit", None, None)],
                || true,
                |_, _| {
                    calls += 1;
                    true
                },
            );
        }
        assert_eq!(calls, 2);
    }

    #[test]
    fn retries_failed_replay_and_stops_before_sending_after_disable() {
        let mut delivery = BridgeDelivery::default();
        let mut calls = 0;
        for (current, acknowledged) in [(false, false), (true, false), (true, true), (true, true)] {
            delivery.publish(
                "new-bridge",
                vec![session("running", None, None)],
                || current,
                |_, _| {
                    calls += 1;
                    acknowledged
                },
            );
        }
        assert_eq!(calls, 2);
    }

    #[test]
    fn maps_devin_session_states() {
        for status in ["new", "claimed", "resuming"] {
            assert_eq!(
                session_presentation(&session(status, None, None))
                    .unwrap()
                    .state,
                "working"
            );
        }
        assert_eq!(
            session_presentation(&session("running", Some("working"), None))
                .unwrap()
                .state,
            "working"
        );
        for detail in [None, Some("unrecognized")] {
            assert_eq!(
                session_presentation(&session("running", detail, None))
                    .unwrap()
                    .state,
                "working"
            );
        }
        for detail in ["waiting_for_user", "waiting_for_approval"] {
            assert_eq!(
                session_presentation(&session("running", Some(detail), None))
                    .unwrap()
                    .state,
                "waiting_user"
            );
        }
        assert_eq!(
            session_presentation(&session("running", Some("finished"), None))
                .unwrap()
                .state,
            "done"
        );
        assert_eq!(
            session_presentation(&session("exit", None, None))
                .unwrap()
                .state,
            "done"
        );
        assert_eq!(
            session_presentation(&session("error", None, None))
                .unwrap()
                .state,
            "error"
        );
        assert_eq!(
            session_presentation(&session("suspended", Some("error"), None))
                .unwrap()
                .state,
            "error"
        );
        assert_eq!(
            session_presentation(&session("suspended", Some("inactivity"), None))
                .unwrap()
                .state,
            "sleeping"
        );
        assert_eq!(
            session_presentation(&session("running", Some("inactivity"), None))
                .unwrap()
                .state,
            "working"
        );
    }

    #[test]
    fn builds_bounded_cloud_status_body_without_reply_text() {
        let mut source = session("running", Some("working"), Some(&"会".repeat(400)));
        source.session_id = "session-2".to_string();
        let presentation = session_presentation(&source).unwrap();
        let body = build_state_body(&source, &presentation);
        assert_eq!(body["agent_id"], "devin");
        assert_eq!(body["event"], "CloudStatus");
        assert_eq!(
            body["session_title"].as_str().unwrap().chars().count(),
            MAX_TITLE_CHARS
        );
        assert!(body["display_content"].as_str().unwrap().chars().count() <= MAX_DISPLAY_CHARS);
        assert_eq!(body["session_title_explicit"], true);
        assert!(body.get("last_assistant_message").is_none());
    }

    #[test]
    fn debug_and_errors_never_include_the_pat() {
        let token = "cog_do_not_leak_this";
        let config = CloudConfig {
            token: token.to_string(),
            org_id: "org-example".to_string(),
            enabled: true,
        };
        assert!(!format!("{config:?}").contains(token));
        assert!(!format!("{:?}", PollError::Unauthorized).contains(token));
    }

    #[test]
    fn unauthorized_fetch_stops_polling_after_one_call() {
        let calls = Mutex::new(0);
        let result = poll_once(|| {
            *calls.lock().unwrap() += 1;
            Err(PollError::Unauthorized)
        });
        assert!(matches!(result, Err(PollError::Unauthorized)));
        assert_eq!(*calls.lock().unwrap(), 1);
        assert_eq!(
            poll_action(&PollError::Unauthorized),
            PollAction::StopAuthentication
        );
        assert_eq!(
            poll_action(&PollError::Forbidden),
            PollAction::StopAuthentication
        );
        assert_eq!(poll_action(&PollError::Retryable), PollAction::Continue);
        assert_eq!(poll_action(&PollError::Other), PollAction::Stop);
    }
}
