//! Remember a confirmed OTA rollback for this device and this exact image.
use sha2::{Digest, Sha256};
use std::collections::HashSet;
use std::fs::{self, OpenOptions};
use std::io;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard, OnceLock};

pub(crate) const ROLLED_BACK: &str = "ota_rollback";
pub(crate) const IMAGE_BLOCKED: &str = "firmware_image_rolled_back";
const RECORD_UNAVAILABLE: &str = "firmware_rollback_record_unavailable";

// Serialize OTA across transports as well as within each transport.
static UPDATE: Mutex<()> = Mutex::new(());
static IN_MEMORY: OnceLock<Mutex<HashSet<PathBuf>>> = OnceLock::new();

pub(crate) fn start_update() -> Result<MutexGuard<'static, ()>, String> {
    UPDATE
        .try_lock()
        .map_err(|_| "another firmware transfer is active".into())
}

pub(crate) fn directory(app_data: &Path) -> PathBuf {
    app_data.join("firmware-rollbacks-v1")
}

fn marker(directory: &Path, board_id: &str, sha256: &[u8; 32]) -> PathBuf {
    let mut key = Sha256::new();
    key.update(board_id.as_bytes());
    key.update(sha256);
    directory.join(format!("{:x}", key.finalize()))
}

pub(crate) fn is_blocked(
    directory: &Path,
    board_id: &str,
    sha256: &[u8; 32],
) -> Result<bool, String> {
    let path = marker(directory, board_id, sha256);
    if IN_MEMORY
        .get()
        .is_some_and(|records| records.lock().unwrap_or_else(|e| e.into_inner()).contains(&path))
    {
        return Ok(true);
    }
    match path.try_exists() {
        Ok(exists) => Ok(exists),
        Err(_) => Err(RECORD_UNAVAILABLE.into()),
    }
}

pub(crate) fn ensure_allowed(
    directory: &Path,
    board_id: &str,
    sha256: &[u8; 32],
) -> Result<(), String> {
    if is_blocked(directory, board_id, sha256)? {
        return Err(IMAGE_BLOCKED.into());
    }
    Ok(())
}

pub(crate) fn record(directory: &Path, board_id: &str, sha256: &[u8; 32]) {
    let path = marker(directory, board_id, sha256);
    // Preserve the guard in this app even if the disk is full.
    IN_MEMORY
        .get_or_init(Mutex::default)
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .insert(path.clone());
    let result = fs::create_dir_all(directory).and_then(|()| {
        OpenOptions::new().write(true).create_new(true).open(path)
    });
    if result.is_err_and(|error| error.kind() != io::ErrorKind::AlreadyExists) {
        eprintln!("could not persist confirmed firmware rollback");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rollback_survives_a_new_lookup_and_only_blocks_the_exact_device_and_image() {
        let temp = tempfile::tempdir().unwrap();
        let dir = directory(temp.path());
        let image = [1; 32];
        assert_eq!(ensure_allowed(&dir, "board-a", &image), Ok(()));
        record(&dir, "board-a", &image);
        record(&dir, "board-a", &image);
        assert!(IN_MEMORY.get().unwrap().lock().unwrap().remove(&marker(&dir, "board-a", &image)));
        assert_eq!(ensure_allowed(&dir, "board-a", &image), Err(IMAGE_BLOCKED.into()));
        assert_eq!(ensure_allowed(&dir, "board-b", &image), Ok(()));
        assert_eq!(ensure_allowed(&dir, "board-a", &[2; 32]), Ok(()));
    }

    #[test]
    fn rollback_still_blocks_before_restart_if_storage_is_unavailable() {
        let temp = tempfile::tempdir().unwrap();
        let dir = directory(temp.path());
        fs::write(&dir, []).unwrap();
        let image = [1; 32];
        record(&dir, "board-a", &image);
        assert_eq!(ensure_allowed(&dir, "board-a", &image), Err(IMAGE_BLOCKED.into()));
    }
}
