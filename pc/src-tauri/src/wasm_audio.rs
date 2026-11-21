//! Bounded offline audio bank shared by PetUI packaging and device sync.
pub(crate) const PROFILE: &str = "hachi-audio-v2";
pub(crate) const ASSET: &str = "assets/speech.hab";
pub(crate) const DEVICE_PATH: &str = "runtime/speech.hab";
pub(crate) const MAX_BYTES: usize = 512 * 1024;
const BLOCK_BYTES: usize = 166;
const MAX_CUE_BYTES: usize = 400 * BLOCK_BYTES;

pub(crate) fn validate(bytes: &[u8]) -> Result<(), String> {
    let invalid = || "WASM 语音资源格式、数量或时长无效".to_owned();
    if bytes.len() < 16 || bytes.len() > MAX_BYTES || &bytes[..4] != b"HAB1" {
        return Err(invalid());
    }
    let word = |data: &[u8]| u32::from_le_bytes(data.try_into().unwrap()) as usize;
    let count = word(&bytes[4..8]);
    if !(1..=64).contains(&count) || 8 + count * 8 > bytes.len() {
        return Err(invalid());
    }
    let mut cursor = 8 + count * 8;
    for entry in bytes[8..cursor].chunks_exact(8) {
        let offset = word(&entry[..4]);
        let length = word(&entry[4..]);
        if offset != cursor || length == 0 || length > MAX_CUE_BYTES
            || length % BLOCK_BYTES != 0 || length > bytes.len() - cursor
        {
            return Err(invalid());
        }
        if bytes[cursor..cursor + length].chunks_exact(BLOCK_BYTES).any(|block| {
            block[2] > 88 || block[3] != 0 || block[4..6] != [0x40, 0x01] || block[165] & 0xf0 != 0
        }) {
            return Err(invalid());
        }
        cursor += length;
    }
    if cursor != bytes.len() {
        return Err(invalid());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn checks_contiguous_index_and_every_ima_block() {
        let mut bank = b"HAB1\x01\0\0\0\x10\0\0\0\xa6\0\0\0".to_vec();
        bank.resize(16 + BLOCK_BYTES, 0);
        bank[20..22].copy_from_slice(&[0x40, 0x01]);
        assert!(validate(&bank).is_ok());
        for (offset, value) in [(8, 17), (12, 0), (18, 89), (19, 1), (20, 0), (181, 16)] {
            let mut damaged = bank.clone();
            damaged[offset] = value;
            assert!(validate(&damaged).is_err());
        }
        assert!(validate(&bank[..bank.len() - 1]).is_err());
        bank.push(0);
        assert!(validate(&bank).is_err());
    }
}
