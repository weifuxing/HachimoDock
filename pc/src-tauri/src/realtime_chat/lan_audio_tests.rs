use super::*;
use std::io::{Read, Write};
use std::process::{Command, Stdio};

fn key() -> Zeroizing<[u8; 16]> {
    let mut key = Zeroizing::new([0; 16]);
    key[0] = 42;
    key
}

fn uplink(record: Record, seq: u32) -> Zeroizing<Vec<u8>> {
    let h = header(record, seq);
    let body = match record {
        Record::Audio { .. } => vec![11; PCM],
        Record::VoiceReady => vec![1],
        _ => Vec::new(),
    };
    let sender = LessSafeKey::new(UnboundKey::new(&aead::AES_128_GCM, key().as_ref()).unwrap());
    let mut wire = Zeroizing::new(h.to_vec());
    wire.extend(&body);
    let tag = sender
        .seal_in_place_separate_tag(nonce(UP, seq), Aad::from(h), &mut wire[HEADER..])
        .unwrap();
    wire.extend(tag.as_ref());
    wire
}

#[test]
fn authenticated_order_bounds_and_vad() {
    let mut channel = LanAudio::new(key());
    let vad = Record::Audio { vad: Some(false) };
    let mut audio = uplink(vad, 0);
    assert_eq!(
        channel.record_size(audio[..HEADER].try_into().unwrap()),
        Some(MAX_RECORD)
    );
    assert_eq!(channel.open(&mut audio), Some(vad));
    assert_eq!(&audio[HEADER..], &[11; PCM]);
    assert!(channel.open(&mut uplink(vad, 0)).is_none());
    assert!(channel.seal(Record::Keepalive, &[]).is_none());

    let mut channel = LanAudio::new(key());
    assert!(channel.seal(vad, &[0; PCM]).is_none());
    assert!(channel
        .seal(Record::Audio { vad: None }, &[0; PCM - 1])
        .is_none());
    assert_eq!(channel.seal(Record::Keepalive, &[]).unwrap()[7], 0);
    let mut too_long = header(vad, 0);
    too_long[2] = 0xff;
    assert!(channel.record_size(&too_long).is_none());
    assert!(channel.open(&mut uplink(vad, 0)).is_none());
}

#[test]
fn voice_ready_carries_one_authenticated_byte_and_precedes_pcm() {
    let mut channel = LanAudio::new(key());
    let mut ready = uplink(Record::VoiceReady, 0);
    assert_eq!(channel.record_size(ready[..HEADER].try_into().unwrap()), Some(HEADER + TAG + 1));
    assert_eq!(channel.open(&mut ready), Some(Record::VoiceReady));
    assert_eq!(&ready[HEADER..], &[1]);
    let audio = Record::Audio { vad: None };
    assert_eq!(channel.open(&mut uplink(audio, 1)), Some(audio));
    assert_eq!(channel.open(&mut uplink(Record::Close, 2)), Some(Record::Close));
    assert!(channel.open(&mut uplink(audio, 3)).is_none());
}

#[test]
fn tag_failure_and_close_retire_both_directions() {
    let mut channel = LanAudio::new(key());
    let mut tampered = uplink(Record::Audio { vad: None }, 0);
    tampered[HEADER] ^= 1;
    assert!(channel.open(&mut tampered).is_none());
    assert!(tampered.is_empty());
    assert!(channel.open(&mut uplink(Record::Keepalive, 0)).is_none());

    let mut channel = LanAudio::new(key());
    assert_eq!(
        channel.open(&mut uplink(Record::Close, 0)),
        Some(Record::Close)
    );
    assert!(channel.seal(Record::Keepalive, &[]).is_none());
}

#[test]
#[ignore = "requires the firmware C test executable linked against mbedcrypto"]
fn interop_with_firmware_mbedtls() {
    let mut child = Command::new(std::env::var_os("PET_LAN_AUDIO_C_PEER").unwrap())
        .arg("--stdio")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    let mut input = child.stdin.take().unwrap();
    let mut output = child.stdout.take().unwrap();
    let mut desktop = LanAudio::new(key());
    let pcm: Vec<u8> = (0..PCM).map(|i| i as u8).collect();
    input
        .write_all(&desktop.seal(Record::Audio { vad: None }, &pcm).unwrap())
        .unwrap();
    let mut wire = Zeroizing::new(vec![0; MAX_RECORD]);
    output.read_exact(&mut wire).unwrap();
    assert_eq!(
        desktop.open(&mut wire),
        Some(Record::Audio { vad: Some(true) })
    );
    assert_eq!(&wire[HEADER..], pcm);
    input
        .write_all(&desktop.seal(Record::Close, &[]).unwrap())
        .unwrap();
    assert!(desktop.seal(Record::Keepalive, &[]).is_none());
    drop(input);
    assert!(child.wait().unwrap().success());
}
