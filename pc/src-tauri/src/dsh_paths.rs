//! DeepSeek-only path contract, matching dsh-home-paths and Node os.homedir.
use std::ffi::OsStr;
use std::path::{Path, PathBuf};

pub(crate) fn user_home(windows: bool, home: Option<&OsStr>, profile: Option<&OsStr>) -> Option<PathBuf> {
    // A Git Bash HOME must not redirect a Windows desktop profile away from
    // the USERPROFILE used by the bundled Node installer/capability writer.
    (if windows { profile } else { home }).filter(|p| !p.is_empty()).map(PathBuf::from)
}

pub(crate) fn profile_dir(home: Option<&Path>, configured: Option<&OsStr>, cwd: &Path) -> Option<PathBuf> {
    let root = match configured.filter(|s| !s.to_string_lossy().trim().is_empty()) {
        Some(s) if s == OsStr::new("~") => home?.to_path_buf(),
        Some(s) => {
            let text = s.to_string_lossy();
            if text.starts_with("~/") || text.starts_with("~\\") {
                home?.join(&text[2..])
            } else { PathBuf::from(s) }
        }
        None => home?.join(".dsh"),
    };
    let root = if root.is_absolute() { root } else { cwd.join(root) };
    Some(root.join("profiles").join("desktop"))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn windows_uses_userprofile_not_git_bash_home() {
        assert_eq!(user_home(true, Some(OsStr::new("git-home")), Some(OsStr::new("用户 Profile"))), Some(PathBuf::from("用户 Profile")));
        assert_eq!(user_home(false, Some(OsStr::new("unix-home")), Some(OsStr::new("profile"))), Some(PathBuf::from("unix-home")));
        assert_eq!(user_home(true, Some(OsStr::new("do-not-guess")), None), None);
    }
    #[test]
    fn default_override_blank_and_tilde_match_installer() {
        let cwd = std::env::temp_dir().join("synthetic-cwd");
        let home = cwd.join("用户 Home");
        let suffix = Path::new("profiles").join("desktop");
        for configured in [None, Some(OsStr::new("  "))] {
            assert_eq!(profile_dir(Some(&home), configured, &cwd), Some(home.join(".dsh").join(&suffix)));
        }
        for configured in ["~/Harness 数据", "~\\Harness 数据"] {
            assert_eq!(profile_dir(Some(&home), Some(OsStr::new(configured)), &cwd), Some(home.join("Harness 数据").join(&suffix)));
        }
        let explicit = cwd.join("custom");
        assert_eq!(profile_dir(Some(&home), Some(explicit.as_os_str()), &cwd), Some(explicit.join(&suffix)));
        assert_eq!(profile_dir(Some(&home), Some(OsStr::new("relative")), &cwd), Some(cwd.join("relative").join(&suffix)));
        assert_eq!(profile_dir(Some(&home), Some(OsStr::new("~")), &cwd), Some(home.join(&suffix)));
        assert_eq!(profile_dir(None, None, &cwd), None);
    }
}
