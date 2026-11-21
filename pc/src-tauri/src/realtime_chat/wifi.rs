use super::{
    lan_audio::{LanAudio, Playback, Record, HEADER, MAX_RECORD, PCM},
    request_failure, Input, Session,
};
use crate::lan_agent::realtime::AudioTarget;
use std::{
    collections::VecDeque,
    io::{self, Read, Write},
    net::TcpStream,
    sync::{
        atomic::{AtomicBool, AtomicU32, AtomicU8, Ordering},
        mpsc, Arc, Mutex,
    },
    thread,
    time::{Duration, Instant},
};
use zeroize::Zeroizing;

const IDLE: u8 = 0;
const RUNNING: u8 = 1;
const FAILED: u8 = 2;
const STOPPED: u8 = 3;
const KEEPALIVE: Duration = Duration::from_millis(300);
const RECEIVE_TIMEOUT: Duration = Duration::from_millis(2200);
const STOP_TIMEOUT: Duration = Duration::from_millis(1800);
const WRITE_TIMEOUT: Duration = Duration::from_millis(1500);
pub(super) const PLAYBACK_LEAD_MS: u64 = 540;
const PLAYBACK_QUEUE_FRAMES: usize = 128;

struct PendingPlayback {
    kind: Playback,
    turn: u32,
    pcm: Zeroizing<Vec<u8>>,
}

#[derive(Default)]
struct Turn {
    number: u32,
    id: String,
}

pub(super) struct Link {
    target: AudioTarget,
    state: AtomicU8,
    stop: AtomicBool,
    activate: AtomicBool,
    hud: Mutex<VecDeque<Zeroizing<Vec<u8>>>>,
    terminal_hud: Mutex<Option<Zeroizing<Vec<u8>>>>,
    retired: AtomicU32,
    sender: Mutex<Option<mpsc::SyncSender<PendingPlayback>>>,
    turn: Mutex<Turn>,
}

impl Link {
    pub(super) fn new(target: AudioTarget) -> Self {
        Self {
            target,
            state: AtomicU8::new(IDLE),
            stop: AtomicBool::new(false),
            activate: AtomicBool::new(false),
            hud: Mutex::new(VecDeque::new()),
            terminal_hud: Mutex::new(None),
            retired: AtomicU32::new(0),
            sender: Mutex::new(None),
            turn: Mutex::new(Turn::default()),
        }
    }

    pub(super) fn start(session: &Arc<Session>) -> Result<(), String> {
        Self::launch(session)?;
        session.wifi.as_ref().unwrap().activate.store(true, Ordering::Release);
        Ok(())
    }

    pub(super) fn hud(session: &Session, payload: &serde_json::Value) -> Result<(), String> {
        let link = session.wifi.as_ref().ok_or("Wi-Fi 音频连接无效。")?;
        if !link.target.hud_supported() { return Ok(()); } // Never send kind 7 to old peers.
        if link.stop.load(Ordering::Acquire) || !link.current()
            || payload["sessionId"].as_str() != Some(session.id.as_str()) {
            return Err("实时对话显示会话已结束或已变化".into());
        }
        let body = super::lan_audio::hud_body(payload)?;
        {
            let mut pending = link.hud.lock().map_err(|_| "实时对话显示队列不可用")?;
            if pending.len() == 8 { return Err("实时对话显示队列繁忙".into()); }
            pending.push_back(body);
        }
        let owned = super::active_session().filter(|s| std::ptr::eq(s.as_ref(), session))
            .ok_or("实时对话显示所有者已变化")?;
        Self::launch(&owned)
    }

    fn launch(session: &Arc<Session>) -> Result<(), String> {
        let link = session.wifi.as_ref().ok_or("Wi-Fi 音频连接无效。")?;
        match link.state.compare_exchange(IDLE, RUNNING, Ordering::AcqRel, Ordering::Acquire) {
            Ok(_) => {},
            Err(RUNNING) => return Ok(()),
            Err(_) => return Err("Wi-Fi 音频会话已结束。".into()),
        }
        let (tx, rx) = mpsc::sync_channel(PLAYBACK_QUEUE_FRAMES);
        *link.sender.lock().map_err(|_| "Wi-Fi 音频队列不可用。")? = Some(tx);
        let owned = session.clone();
        if thread::Builder::new()
            .name("realtime-wifi".into())
            .spawn(move || run(owned, rx))
            .is_err()
        {
            link.state.store(STOPPED, Ordering::Release);
            return Err("Wi-Fi 音频线程启动失败。".into());
        }
        Ok(())
    }

    pub(super) fn current(&self) -> bool {
        matches!(self.state.load(Ordering::Acquire), IDLE | RUNNING)
    }

    pub(super) fn boot_matches(&self, boot: &str) -> bool { self.target.boot_matches(boot) }

    pub(super) fn stop(&self) {
        self.stop.store(true, Ordering::Release);
        // A session cancelled before starting capture never opened its microphone.
        let _ = self
            .state
            .compare_exchange(IDLE, STOPPED, Ordering::AcqRel, Ordering::Acquire);
    }

    pub(super) fn finish_hud(&self, payload: &serde_json::Value) {
        if self.target.hud_supported() && self.current() {
            if let (Ok(body), Ok(mut terminal)) =
                (super::lan_audio::hud_body(payload), self.terminal_hud.lock()) {
                *terminal = Some(body);
            }
        }
        self.stop();
    }

    pub(super) fn stopped(&self) -> bool {
        self.state.load(Ordering::Acquire) == STOPPED
    }

    pub(super) fn observe_idle(&self) -> bool {
        if self.stopped() {
            return true;
        }
        if self.state.load(Ordering::Acquire) != FAILED {
            return false;
        }
        if self.target.is_idle() {
            self.state.store(STOPPED, Ordering::Release);
            return true;
        }
        false
    }

    pub(super) fn playback(&self, kind: Playback, id: &str, bytes: &[u8]) -> Result<(), String> {
        // A half-duplex greeting may precede begin_capture. Its validated TTS
        // can activate the already admitted passive HUD connection now.
        if self.target.hud_supported() { self.activate.store(true, Ordering::Release); }
        let mut turn = self.turn.lock().map_err(|_| "Wi-Fi 播放状态不可用。")?;
        match kind {
            Playback::Begin => {
                turn.number = turn.number.checked_add(1).ok_or("Wi-Fi 播放轮次超限。")?;
                turn.id = id.to_owned();
            }
            Playback::Flush if turn.number != 0 => {
                self.retired.store(turn.number, Ordering::Release)
            }
            Playback::Flush => return Ok(()),
            _ if turn.number == 0 || turn.id != id => return Err("Wi-Fi 播放轮次已变化。".into()),
            _ => {}
        }
        let sender = self.sender.lock().map_err(|_| "Wi-Fi 音频队列不可用。")?;
        let sender = sender.as_ref().ok_or("Wi-Fi 音频连接已关闭。")?;
        let send = |pcm: &[u8]| {
            sender
                .try_send(PendingPlayback {
                    kind,
                    turn: turn.number,
                    pcm: Zeroizing::new(pcm.to_vec()),
                })
                .map_err(|_| "Wi-Fi 音频发送队列繁忙或设备已断开。".to_string())
        };
        if matches!(kind, Playback::Pcm) {
            for frame in bytes.chunks(PCM) {
                if frame.len() == PCM {
                    send(frame)?;
                } else {
                    let mut tail = Zeroizing::new([0; PCM]);
                    tail[..frame.len()].copy_from_slice(frame);
                    send(&tail[..])?;
                }
            }
            Ok(())
        } else {
            send(&[])
        }
    }

    fn note_drained(&self, session: &Session, turn: u32) {
        let Ok(active) = self.turn.lock() else {
            return;
        };
        if active.number == turn && self.retired.load(Ordering::Acquire) < turn {
            if let Ok(mut completed) = session.completed_playback.lock() {
                completed.clone_from(&active.id);
            }
        }
    }
}

struct Reader {
    frame: Zeroizing<Vec<u8>>,
    expected: usize,
}

impl Reader {
    fn new() -> Self {
        Self {
            frame: Zeroizing::new(Vec::with_capacity(MAX_RECORD)),
            expected: HEADER,
        }
    }

    // TCP reads can split both the header and body. Keep partial data across
    // the polling timeout, with the caller enforcing a deadline on full records.
    fn receive(
        &mut self,
        stream: &mut TcpStream,
        codec: &mut LanAudio,
    ) -> io::Result<Option<Record>> {
        let mut scratch = Zeroizing::new([0; MAX_RECORD]);
        let remaining = self.expected - self.frame.len();
        match stream.read(&mut scratch[..remaining]) {
            Ok(0) => return Err(io::ErrorKind::UnexpectedEof.into()),
            Ok(count) => self.frame.extend_from_slice(&scratch[..count]),
            Err(err)
                if matches!(
                    err.kind(),
                    io::ErrorKind::WouldBlock
                        | io::ErrorKind::TimedOut
                        | io::ErrorKind::Interrupted
                ) =>
            {
                return Ok(None)
            }
            Err(err) => return Err(err),
        }
        if self.frame.len() < self.expected {
            return Ok(None);
        }
        if self.expected == HEADER {
            let header: [u8; HEADER] = self.frame[..].try_into().unwrap();
            self.expected = codec
                .record_size(&header)
                .ok_or(io::ErrorKind::InvalidData)?;
            return Ok(None);
        }
        codec
            .open(&mut self.frame)
            .map(Some)
            .ok_or(io::ErrorKind::InvalidData.into())
    }

    fn clear(&mut self) {
        use zeroize::Zeroize;
        self.frame.zeroize();
        self.expected = HEADER;
    }
}

fn write_record(stream: &mut TcpStream, bytes: Option<Zeroizing<Vec<u8>>>) -> io::Result<()> {
    stream.write_all(&bytes.ok_or(io::ErrorKind::InvalidData)?)
}

struct WireWrite {
    record: Zeroizing<Vec<u8>>,
    offset: usize,
    deadline: Instant,
}

impl WireWrite {
    fn new(record: Option<Zeroizing<Vec<u8>>>, now: Instant) -> io::Result<Self> {
        Ok(Self {
            record: record.ok_or(io::ErrorKind::InvalidData)?,
            offset: 0,
            deadline: now + WRITE_TIMEOUT,
        })
    }

    fn advance(&mut self, stream: &mut impl Write, now: Instant) -> io::Result<bool> {
        if now >= self.deadline {
            return Err(io::ErrorKind::TimedOut.into());
        }
        match stream.write(&self.record[self.offset..]) {
            Ok(0) => return Err(io::ErrorKind::WriteZero.into()),
            Ok(bytes) => self.offset += bytes,
            Err(error)
                if matches!(
                    error.kind(),
                    io::ErrorKind::WouldBlock
                        | io::ErrorKind::TimedOut
                        | io::ErrorKind::Interrupted
                ) => {}
            Err(error) => return Err(error),
        }
        Ok(self.offset == self.record.len())
    }
}

fn pump(
    session: &Session,
    link: &Link,
    rx: mpsc::Receiver<PendingPlayback>,
    mut stream: TcpStream,
    mut codec: LanAudio,
) -> io::Result<()> {
    let mut reader = Reader::new();
    let mut last_sent = Instant::now();
    let mut last_read = last_sent;
    let mut stop_deadline = None;
    let mut close_queued = false;
    let mut outgoing: Option<WireWrite> = None;
    let mut sent_turn = 0;
    let mut queued: Option<PendingPlayback> = None;
    let mut next_pcm = last_sent;
    let mut activated = !link.target.hud_supported();
    let mut last_hud: Option<Zeroizing<Vec<u8>>> = None;

    // The offer alone is passive. This authenticated Start record activates capture.
    if link.stop.load(Ordering::Acquire) {
        return Ok(());
    }
    if activated { write_record(&mut stream, codec.seal(Record::Keepalive, &[]))?; }
    // A send window stall must leave time to drain microphone records.
    stream.set_write_timeout(Some(Duration::from_millis(8)))?;
    loop {
        let now = Instant::now();
        if link.stop.load(Ordering::Acquire) && stop_deadline.is_none() {
            stop_deadline = Some(now + STOP_TIMEOUT);
        }
        if stop_deadline.is_some_and(|deadline| now >= deadline)
            || (stop_deadline.is_none() && now.duration_since(last_read) >= RECEIVE_TIMEOUT)
        {
            return Err(io::ErrorKind::TimedOut.into());
        }
        if outgoing.is_none() && stop_deadline.is_some() && !close_queued {
            let terminal = link.terminal_hud.lock().map_err(|_| io::ErrorKind::Other)?.take();
            if let Some(body) = terminal.filter(|_| last_hud.is_some()) {
                outgoing = Some(WireWrite::new(codec.seal_hud(&body), now)?);
            } else {
                outgoing = Some(WireWrite::new(codec.request_close(), now)?);
                close_queued = true;
            }
        } else if outgoing.is_none() && stop_deadline.is_none() {
            if let Some(body) = link.hud.lock().map_err(|_| io::ErrorKind::Other)?.pop_front() {
                outgoing = Some(WireWrite::new(codec.seal_hud(&body), now)?);
                last_hud = Some(body);
            } else if !activated && link.activate.load(Ordering::Acquire) {
                outgoing = Some(WireWrite::new(codec.seal(Record::Keepalive, &[]), now)?);
                activated = true;
            }
            let is_current = |command: &PendingPlayback| match command.kind {
                Playback::Flush => sent_turn == command.turn,
                _ => link.retired.load(Ordering::Acquire) < command.turn,
            };
            if outgoing.is_none() && activated {
            if let Some(command) = queued
                .take()
                .filter(is_current)
                .or_else(|| rx.try_iter().find(is_current))
            {
                let pcm = matches!(command.kind, Playback::Pcm);
                if pcm && now < next_pcm {
                    queued = Some(command);
                } else {
                    outgoing = Some(WireWrite::new(
                        codec.seal_playback(command.kind, command.turn, &command.pcm),
                        now,
                    )?);
                    if pcm {
                        // Cap packet bursts independently of TTS/player bursts.
                        // Let playback build its device-side jitter buffer.
                        next_pcm = (next_pcm + Duration::from_millis(20))
                            .max(now + Duration::from_millis(10));
                    }
                    if matches!(command.kind, Playback::Begin) {
                        sent_turn = command.turn;
                        next_pcm = now;
                    }
                }
            }
            }
            if outgoing.is_none() && now.duration_since(last_sent) >= KEEPALIVE {
                let record = if activated { codec.seal(Record::Keepalive, &[]) }
                    else { last_hud.as_ref().and_then(|body| codec.seal_hud(body)) };
                outgoing = Some(WireWrite::new(record, now)?);
            }
        }
        if let Some(record) = &mut outgoing {
            if record.advance(&mut stream, now)? {
                outgoing = None;
                last_sent = Instant::now();
            }
        }
        if let Some(record) = reader.receive(&mut stream, &mut codec)? {
            last_read = Instant::now();
            match record {
                Record::Close if stop_deadline.is_some() => return Ok(()),
                Record::Close => return Err(io::ErrorKind::ConnectionAborted.into()),
                Record::Audio { vad: Some(speech) } if stop_deadline.is_none() => {
                    session.capture_ready.store(true, Ordering::Release);
                    session
                        .input
                        .try_send(Input::Pcm(reader.frame[HEADER..].to_vec(), Some(speech)))
                        .map_err(|_| io::ErrorKind::OutOfMemory)?;
                }
                Record::Audio { vad: None } | Record::VoiceReady => {
                    return Err(io::ErrorKind::InvalidData.into());
                }
                Record::PlaybackDrained => {
                    let turn = u32::from_be_bytes(
                        reader.frame[HEADER..]
                            .try_into()
                            .map_err(|_| io::ErrorKind::InvalidData)?,
                    );
                    link.note_drained(session, turn);
                }
                _ => {}
            }
            reader.clear();
        }
    }
}

fn run(session: Arc<Session>, rx: mpsc::Receiver<PendingPlayback>) {
    let Some(link) = &session.wifi else {
        return;
    };
    let result = match link.target.open() {
        Ok((stream, key)) => pump(&session, link, rx, stream, LanAudio::new(key)),
        // Without an authenticated Start record the hardware has not opened capture.
        Err(error) => {
            link.state.store(STOPPED, Ordering::Release);
            request_failure(&session, "wifi_audio_admission_failed", Some(&error));
            return;
        }
    };
    if let Ok(mut sender) = link.sender.lock() {
        *sender = None;
    }
    if let Err(error) = result {
        link.state.store(FAILED, Ordering::Release);
        super::diagnostics::record(
            &session.id,
            "wifi_audio_transport_ended",
            serde_json::json!({"errorKind":format!("{:?}",error.kind())}),
        );
        #[cfg(test)]
        eprintln!("Wi-Fi audio transport ended: {:?}", error.kind());
        request_failure(
            &session,
            "wifi_audio_link_lost",
            Some("设备 Wi-Fi 音频连接已中断，本轮对话结束。"),
        );
    } else {
        link.state.store(STOPPED, Ordering::Release);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::realtime_chat::{
        active_slot, begin_capture, spawn_player, PlayerCmd, RealtimeChatStatus,
    };
    use crate::usb_serial::UsbSerialManager;
    use std::sync::atomic::AtomicU32;
    use tokio::sync::mpsc;

    #[test]
    fn a_short_write_and_backpressure_preserve_the_record_boundary() {
        struct ShortWriter(Vec<u8>, usize);

        impl Write for ShortWriter {
            fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
                self.1 += 1;
                if self.1 == 2 {
                    return Err(io::ErrorKind::WouldBlock.into());
                }
                let count = bytes.len().min(2);
                self.0.extend_from_slice(&bytes[..count]);
                Ok(count)
            }

            fn flush(&mut self) -> io::Result<()> {
                Ok(())
            }
        }

        let started = Instant::now();
        let mut writer = ShortWriter(Vec::new(), 0);
        let mut pending = WireWrite::new(Some(Zeroizing::new(vec![1, 2, 3, 4])), started).unwrap();
        assert!(!pending.advance(&mut writer, started).unwrap());
        assert!(!pending.advance(&mut writer, started).unwrap());
        assert_eq!(writer.0, [1, 2]);
        assert!(pending.advance(&mut writer, started).unwrap());
        assert_eq!(writer.0, [1, 2, 3, 4]);
    }

    #[tokio::test]
    #[ignore = "requires a locally paired Wi-Fi 6 board and audible test environment"]
    async fn physical_wifi_chat_capture_playback_and_stop() {
        let board = std::env::var("PET_LAN_RTC_HIL").expect("set the exact test board ID");
        let target = AudioTarget::find(&board).expect("authenticated board status");
        let (tx, mut rx) = mpsc::channel(128);
        let id = format!("rtc-hil-{}", uuid::Uuid::new_v4());
        let session = Arc::new(Session {
            id: id.clone(),
            board_device_id: board.clone(),
            transport: None,
            wifi: Some(Link::new(target)),
            capture_stream: Mutex::new(None),
            audio_requests: Arc::new(Mutex::new(Default::default())),
            audio_egress: Mutex::new(()),
            cancelled: AtomicBool::new(false),
            duplex: AtomicBool::new(true),
            capture_armed: AtomicBool::new(false),
            capture_ready: AtomicBool::new(false),
            stop_notify: tokio::sync::Notify::new(),
            input: tx,
            status: Mutex::new(RealtimeChatStatus {
                active: true,
                state: "listening".into(),
                ..Default::default()
            }),
            completed_playback: Mutex::new(String::new()),
            pending_persona: Mutex::new(None),
            playback_volume: AtomicU32::new(1.0f32.to_bits()),
            corrupt_capture_streak: AtomicU32::new(0),
        });
        *active_slot().lock().unwrap() = Some(session.clone());
        struct Cleanup(Arc<Session>);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                self.0.cancelled.store(true, Ordering::Release);
                self.0.wifi.as_ref().unwrap().stop();
            }
        }
        let _cleanup = Cleanup(session.clone());
        let usb = UsbSerialManager::new();
        begin_capture(&usb, &session, true)
            .await
            .expect("AEC PCM must reach desktop Start");

        // Use the production player and verify the device's DAC-drained receipt.
        let player = spawn_player(usb, board, session.clone());
        let turn = format!("{id}-tone");
        let pcm: Vec<u8> = (0..12_800)
            .flat_map(|i| {
                let pulse =
                    ((i as f64 * std::f64::consts::TAU * 440.0 / 16_000.0).sin() * 900.0) as i16;
                pulse.to_le_bytes()
            })
            .collect();
        player.send(PlayerCmd::Begin(turn.clone())).unwrap();
        player.send(PlayerCmd::Pcm(pcm)).unwrap();
        let (drained_tx, drained_rx) = std::sync::mpsc::channel();
        player.send(PlayerCmd::End(drained_tx)).unwrap();

        let captured = tokio::time::timeout(Duration::from_secs(7), async {
            let mut count = 0;
            let mut nonzero = 0;
            let mut drained = false;
            while count < 40 || !drained {
                if session.cancelled.load(Ordering::Acquire) {
                    let status = session.status.lock().unwrap();
                    panic!(
                        "audio session ended while verifying playback: {} ({})",
                        status.reason, status.error
                    );
                }
                drained |= drained_rx.try_recv().is_ok();
                if let Ok(Some(Input::Pcm(bytes, Some(_)))) =
                    tokio::time::timeout(Duration::from_millis(50), rx.recv()).await
                {
                    assert_eq!(bytes.len(), PCM);
                    count += 1;
                    nonzero += usize::from(bytes.iter().any(|value| *value != 0));
                }
            }
            (count, nonzero)
        })
        .await
        .expect("capture must continue through playback and the DAC must drain");
        assert!(captured.0 >= 40 && captured.1 > 0);
        assert_eq!(*session.completed_playback.lock().unwrap(), turn);
        println!(
            "Wi-Fi chat: {} AEC capture frames, {} nonzero; DAC drained test turn",
            captured.0, captured.1
        );

        let link = session.wifi.as_ref().unwrap();
        link.stop();
        tokio::time::timeout(Duration::from_secs(3), async {
            while !link.stopped() {
                tokio::time::sleep(Duration::from_millis(25)).await;
            }
        })
        .await
        .expect("authenticated close must confirm device stop");
        assert!(link.target.is_idle());
        println!("Wi-Fi chat: authenticated stop and idle readback verified");
    }
}
