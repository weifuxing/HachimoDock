//! Fixed MiMo diagnostic labels. Never persist AX roles, window titles or error text.

pub(super) fn failure_reason(error: &str) -> &'static str {
    for code in ["prepare_cancelled", "tree_limit", "composer_missing", "composer_ambiguous", "window_ambiguous", "window_missing", "modal_open", "draft_changed", "focus_changed", "caret_unconfirmed", "activation_failed", "mimo_target_unavailable", "input_event_failed", "write_unconfirmed"] {
        if error.starts_with(&format!("MiMo Desktop / MiMoCode [{code}]")) { return code; }
    }
    if error.contains("需要 macOS 辅助功能权限") { "permission_required" }
    else if error.contains("当前焦点不是可输入文本的位置") { "focus_not_editable" }
    else if error.contains("当前没有可读取的文本光标") { "caret_unavailable" }
    else if error.contains("前台应用已变化") || error.contains("前台窗口或文本光标已变化") { "focus_changed" }
    else if error.contains("无法读取 macOS 当前前台应用") || error.contains("无法确认 macOS 当前前台应用进程") { "foreground_unavailable" }
    else if error.contains("剪贴板") { "clipboard_failed" }
    else { "other" }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn mimo_error_labels_do_not_expose_dynamic_details() {
        for (error, expected) in [
            ("Pet Manager 需要 macOS 辅助功能权限；fixture", "permission_required"),
            ("MiMo Desktop / MiMoCode 当前焦点不是可输入文本的位置（AXWebArea）；fixture", "focus_not_editable"),
            ("MiMo Desktop / MiMoCode 当前没有可读取的文本光标；fixture", "caret_unavailable"),
            ("MiMo Desktop 前台应用已变化，fixture", "focus_changed"),
            ("MiMo Desktop / MiMoCode 备份剪贴板期间前台窗口或文本光标已变化，fixture", "focus_changed"),
            ("无法确认 macOS 当前前台应用进程: fixture", "foreground_unavailable"),
            ("无法完整备份剪贴板类型 fixture", "clipboard_failed"),
            ("arbitrary private fixture", "other"),
        ] {
            let label = failure_reason(error);
            assert_eq!(label, expected);
            assert!(label.len() <= 64 && label.bytes().all(|b| b.is_ascii_lowercase() || b == b'_'));
            assert!(!label.contains("fixture"));
        }
    }
}

#[cfg(test)]
mod startup_tests {
    #[test]
    fn mimo_preparation_runs_parallel_to_asr_and_final_waits_for_it() {
        let source = include_str!("lib.rs");
        let startup = source.split("fn continue_device_voice_context_start(").nth(1).unwrap().split("fn ").next().unwrap();
        assert!(startup.contains("pet-mimo-composer-startup"));
        assert!(startup.find(".spawn(move ||").unwrap() < startup.find("prepare_mimo_text_target(").unwrap());
        assert!(startup.contains("else if context.target.agent_id != \"mimocode\""));
        assert!(startup.contains("StreamingSpeechRecognizer::start"));
        let finalize = source.split("fn finalize_device_voice_final(").nth(1).unwrap().split("fn ").next().unwrap();
        assert!(finalize.find("wait_timeout_while").unwrap() < finalize.find("insert_at_focused_text_target").unwrap());
        assert!(source.contains("const DSH_PENDING_PCM_BYTES: usize = 320_000;"));
    }
}
