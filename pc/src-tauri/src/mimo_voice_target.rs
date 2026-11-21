//! Bounded MiMo Desktop AX readiness policy; no UI or device access in tests.

pub(super) const DESKTOP_BUNDLE_ID: &str = "com.xiaomi.mimo.desktop";
const CAPTURE_ATTEMPTS: usize = 31; // At most 30 x 25ms waits.

pub(super) fn capture_when_ready<T>(
    mut capture: impl FnMut() -> Result<T, String>,
    mut still_active: impl FnMut() -> bool,
    mut wait: impl FnMut(),
) -> Result<T, String> {
    for attempt in 0..CAPTURE_ATTEMPTS {
        if !still_active() {
            return Err("MiMo Desktop 前台应用已变化，本次语音未写入".into());
        }
        match capture() {
            Ok(target) => return Ok(target),
            Err(error) if attempt + 1 == CAPTURE_ATTEMPTS
                || error.contains("[focus_changed]") || error.contains("[composer_ambiguous]")
                || error.contains("[prepare_cancelled]") || error.contains("[tree_limit]") => return Err(error),
            Err(_) => wait(),
        }
    }
    unreachable!("nonzero capture attempts")
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ready_caret_does_not_wait() {
        assert_eq!(capture_when_ready(|| Ok(7), || true, || panic!("unexpected wait")), Ok(7));
    }
    #[test]
    fn cold_electron_tree_can_become_ready() {
        let mut attempts = 0;
        let mut waits = 0;
        let result = capture_when_ready(|| {
            attempts += 1;
            if attempts < 3 { Err("AXGroup".into()) } else { Ok(7) }
        }, || true, || waits += 1);
        assert_eq!(result, Ok(7));
        assert_eq!((attempts, waits), (3, 2));
    }
    #[test]
    fn unreadable_tree_has_bounded_wait_and_preserves_error() {
        let mut waits = 0;
        let result: Result<(), _> = capture_when_ready(|| Err("not editable".into()), || true, || waits += 1);
        assert_eq!(result, Err("not editable".into()));
        assert_eq!(waits, 30);
    }
    #[test]
    fn switching_apps_during_warmup_never_captures_the_other_app() {
        let mut checks = 0;
        let mut captures = 0;
        let result: Result<(), _> = capture_when_ready(|| {
            captures += 1;
            Err("AXGroup".into())
        }, || { checks += 1; checks == 1 }, || {});
        assert!(result.unwrap_err().contains("前台应用已变化"));
        assert_eq!(captures, 1);
    }
}

/// Desktop wins even if a CLI terminal happens to be active. Never fall back to
/// arbitrary focused text after Desktop preparation fails.
pub(super) fn use_desktop(desktop_available: bool, verified_cli: bool) -> Result<bool, &'static str> {
    if desktop_available { Ok(true) }
    else if verified_cli { Ok(false) }
    else { Err("mimo_target_unavailable") }
}

pub(super) fn unique_candidate(count: usize) -> Result<usize, &'static str> {
    match count { 1 => Ok(0), 0 => Err("composer_missing"), _ => Err("composer_ambiguous") }
}

pub(super) fn append_guard(same_focus: bool, original: &str, current: &str,
    selection: Option<(isize, isize)>) -> bool {
    same_focus && original == current
        && selection == isize::try_from(original.encode_utf16().count()).ok().map(|end| (end, 0))
}

#[cfg(test)]
mod desktop_tests {
    use super::*;
    #[test]
    fn mimo_desktop_takes_priority_and_never_uses_unverified_focus() {
        assert_eq!(use_desktop(true, false), Ok(true)); // background Desktop
        assert_eq!(use_desktop(true, true), Ok(true)); // Desktop + CLI
        assert_eq!(use_desktop(false, true), Ok(false));
        assert!(use_desktop(false, false).is_err());
    }
    #[test]
    fn mimo_auto_focus_requires_exactly_one_visible_composer() {
        assert_eq!(unique_candidate(1), Ok(0)); // AXWebArea focus doesn't prevent selection
        assert_eq!(unique_candidate(0), Err("composer_missing"));
        assert_eq!(unique_candidate(2), Err("composer_ambiguous"));
    }
    #[test]
    fn mimo_append_preserves_draft_and_rejects_changed_focus_or_selection() {
        assert!(append_guard(true, "已有😀", "已有😀", Some((4, 0))));
        assert!(!append_guard(false, "old", "old", Some((3, 0))));
        assert!(!append_guard(true, "old", "new", Some((3, 0))));
        assert!(!append_guard(true, "old", "old", Some((0, 3))));
        assert!(!append_guard(true, "old", "old", None));
    }
}

#[cfg(test)]
mod fatal_tests {
    use super::*;
    #[test]
    fn mimo_focus_change_or_ambiguity_is_not_retried() {
        for code in ["focus_changed", "composer_ambiguous"] {
            let result: Result<(), String> = capture_when_ready(
                || Err(format!("MiMo Desktop / MiMoCode [{code}]")), || true,
                || panic!("must not retry uncertain target"));
            assert!(result.is_err());
        }
    }
}
