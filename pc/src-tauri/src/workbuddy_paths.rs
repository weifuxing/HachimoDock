//! WorkBuddy's config root follows its Node runtime, without changing other
//! Agents' legacy HOME handling. No filesystem reads or environment mutations.
use std::path::{Path, PathBuf};

fn resolve(
    windows: bool,
    override_dir: Option<&str>,
    user_profile: Option<&Path>,
    system_home: Option<&Path>,
    legacy_home: &Path,
) -> Option<PathBuf> {
    if let Some(dir) = override_dir.map(str::trim).filter(|s| !s.is_empty()) {
        return Some(PathBuf::from(dir));
    }
    let home = if windows {
        user_profile
            .filter(|p| !p.as_os_str().is_empty())
            .or(system_home)
    } else {
        Some(legacy_home)
    }?;
    if home.as_os_str().is_empty() {
        return None;
    }
    Some(home.join(".workbuddy"))
}

pub(crate) fn config_dir(legacy_home: &Path) -> Result<PathBuf, String> {
    let override_dir = std::env::var("WORKBUDDY_CONFIG_DIR").ok();
    let profile = std::env::var_os("USERPROFILE").map(PathBuf::from);
    // Since Rust 1.85, Windows home_dir ignores shell HOME and uses
    // USERPROFILE or GetUserProfileDirectory, matching Node os.homedir().
    let system_home = if cfg!(windows) {
        std::env::home_dir()
    } else {
        None
    };
    resolve(
        cfg!(windows),
        override_dir.as_deref(),
        profile.as_deref(),
        system_home.as_deref(),
        legacy_home,
    )
    .ok_or_else(|| "无法解析 WorkBuddy 用户目录；未修改其配置或 Skill".into())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn windows_ignores_shell_home_for_profile() {
        let profile = Path::new("C:/Users/测试用户");
        assert_eq!(
            resolve(
                true,
                None,
                Some(profile),
                Some(Path::new("C:/system")),
                Path::new("D:/shell-home")
            ),
            Some(profile.join(".workbuddy"))
        );
    }

    #[test]
    fn windows_without_profile_uses_system_home_not_shell_home() {
        for profile in [None, Some(Path::new(""))] {
            assert_eq!(
                resolve(
                    true,
                    Some("  "),
                    profile,
                    Some(Path::new("C:/system")),
                    Path::new("D:/shell-home")
                ),
                Some(Path::new("C:/system").join(".workbuddy"))
            );
        }
    }

    #[test]
    fn missing_windows_profile_never_creates_relative_or_shell_config() {
        assert_eq!(
            resolve(true, None, None, None, Path::new("D:/shell-home")),
            None
        );
        assert_eq!(
            resolve(
                true,
                None,
                None,
                Some(Path::new("")),
                Path::new("D:/shell-home")
            ),
            None
        );
    }

    #[test]
    fn explicit_workbuddy_override_wins_on_both_platforms() {
        for windows in [false, true] {
            assert_eq!(
                resolve(
                    windows,
                    Some("  D:/WorkBuddy profile  "),
                    None,
                    None,
                    Path::new("/other")
                ),
                Some(PathBuf::from("D:/WorkBuddy profile"))
            );
        }
    }

    #[test]
    fn non_windows_keeps_callers_home_and_empty_home_fails_closed() {
        assert_eq!(
            resolve(
                false,
                None,
                Some(Path::new("C:/profile")),
                None,
                Path::new("/synthetic/home")
            ),
            Some(PathBuf::from("/synthetic/home/.workbuddy"))
        );
        assert_eq!(resolve(false, None, None, None, Path::new("")), None);
    }
}
