//! Launch handoff between signed or locally built copies of the macOS app.
use objc2_app_kit::{NSApplicationActivationOptions, NSRunningApplication};
use objc2_foundation::{NSBundle, NSString};
use semver::Version;
use std::{
    io, thread,
    time::{Duration, Instant},
};

fn bundle_version(bundle: &NSBundle) -> Option<Version> {
    let version = bundle
        .objectForInfoDictionaryKey(&NSString::from_str("CFBundleShortVersionString"))?
        .downcast::<NSString>()
        .ok()?;
    Version::parse(&version.to_string()).ok()
}

pub(crate) fn prepare_launch() -> bool {
    let bundle = NSBundle::mainBundle();
    let Some(identifier) = bundle.bundleIdentifier() else {
        return true;
    };
    if identifier.to_string() != "com.petmanager.desktop" {
        return true;
    }
    let Some(current_version) = bundle_version(&bundle) else {
        return true;
    };
    let apps = NSRunningApplication::runningApplicationsWithBundleIdentifier(&identifier);
    let mut older = Vec::new();
    for app in apps.iter() {
        let pid = app.processIdentifier();
        if pid <= 0 || pid as u32 == std::process::id() || app.isTerminated() {
            continue;
        }
        let version = app
            .bundleURL()
            .and_then(|url| NSBundle::bundleWithURL(&url))
            .and_then(|bundle| bundle_version(&bundle));
        if !version.is_some_and(|version| version < current_version) {
            let _ = app.activateWithOptions(NSApplicationActivationOptions::ActivateAllWindows);
            return false;
        }
        older.push(app);
    }

    // Send the ordinary macOS Quit request. The old process retains ownership
    // until it actually exits, including when it delays or refuses the request.
    for app in &older {
        let _ = app.terminate();
    }
    let deadline = Instant::now() + Duration::from_secs(12);
    while older
        .iter()
        .any(|app| process_exists(app.processIdentifier()))
    {
        if Instant::now() >= deadline {
            eprintln!("[desktop-launch] previous instance has not exited");
            return false;
        }
        thread::sleep(Duration::from_millis(100));
    }
    true
}

fn process_exists(pid: libc::pid_t) -> bool {
    // Signal zero checks existence without changing the target process.
    unsafe {
        libc::kill(pid, 0) == 0 || io::Error::last_os_error().raw_os_error() != Some(libc::ESRCH)
    }
}
