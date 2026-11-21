//! Keep the physical tty drained while protocol dispatch is busy.
use std::{
    collections::VecDeque,
    io::{self, Read},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Condvar, Mutex,
    },
    thread::{self, JoinHandle},
    time::Duration,
};

const MAX_PENDING_BYTES: usize = 256 * 1024;
const WAIT: Duration = Duration::from_millis(40);

#[derive(Default)]
struct Pending {
    bytes: VecDeque<u8>,
    end: Option<io::Result<()>>,
}

#[derive(Default)]
struct Shared {
    pending: Mutex<Pending>,
    ready: Condvar,
    closed: AtomicBool,
}

pub(super) struct ReceiveBuffer {
    shared: Arc<Shared>,
    worker: Option<JoinHandle<()>>,
}

impl ReceiveBuffer {
    pub(super) fn start(
        mut source: impl Read + Send + 'static,
        cancel: Arc<AtomicBool>,
    ) -> io::Result<Self> {
        let shared = Arc::new(Shared::default());
        let input = shared.clone();
        let worker = thread::Builder::new()
            .name("p4-serial-physical-rx".into())
            .spawn(move || {
                let mut block = [0; 8192];
                let end = loop {
                    if input.closed.load(Ordering::Acquire) || cancel.load(Ordering::Acquire) {
                        break Ok(());
                    }
                    match source.read(&mut block) {
                        Ok(0) => break Ok(()),
                        Ok(n) => {
                            let mut pending = input.pending.lock().unwrap();
                            if n > MAX_PENDING_BYTES - pending.bytes.len() {
                                break Err(io::Error::new(
                                    io::ErrorKind::InvalidData,
                                    "serial receive backlog exceeded",
                                ));
                            }
                            pending.bytes.extend(&block[..n]);
                            input.ready.notify_one();
                        }
                        Err(e) => match super::serial_read_recovery_delay(&e) {
                            Some(delay) => {
                                if !delay.is_zero() {
                                    thread::sleep(delay);
                                }
                            }
                            None => break Err(e),
                        },
                    }
                };
                // Publish EOF only after closing this generation's OS handle.
                drop(source);
                input.pending.lock().unwrap().end = Some(end);
                input.ready.notify_one();
            })?;
        Ok(Self {
            shared,
            worker: Some(worker),
        })
    }
}

impl Read for ReceiveBuffer {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        if out.is_empty() {
            return Ok(0);
        }
        let pending = self.shared.pending.lock().unwrap();
        let (mut pending, _) = self
            .shared
            .ready
            .wait_timeout_while(pending, WAIT, |state| {
                state.bytes.is_empty() && state.end.is_none()
            })
            .unwrap();
        let n = out.len().min(pending.bytes.len());
        if n != 0 {
            let (first, second) = pending.bytes.as_slices();
            let split = first.len().min(n);
            out[..split].copy_from_slice(&first[..split]);
            out[split..n].copy_from_slice(&second[..n - split]);
            pending.bytes.drain(..n);
            return Ok(n);
        }
        match pending.end.take() {
            Some(end) => end.map(|()| 0),
            None => Err(io::ErrorKind::TimedOut.into()),
        }
    }
}

impl Drop for ReceiveBuffer {
    fn drop(&mut self) {
        self.shared.closed.store(true, Ordering::Release);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn drains_before_the_parser_starts_and_preserves_byte_order() {
        let data: Vec<_> = (0..96_000).map(|n| (n % 251) as u8).collect();
        let mut rx = ReceiveBuffer::start(
            io::Cursor::new(data.clone()),
            Arc::new(AtomicBool::new(false)),
        )
        .unwrap();
        rx.worker.take().unwrap().join().unwrap();
        let mut actual = Vec::new();
        rx.read_to_end(&mut actual).unwrap();
        assert_eq!(actual, data);
    }

    #[test]
    fn backlog_overflow_terminates_instead_of_skipping_bytes() {
        let data = vec![b'a'; MAX_PENDING_BYTES + 8192];
        let mut rx =
            ReceiveBuffer::start(io::Cursor::new(data), Arc::new(AtomicBool::new(false))).unwrap();
        rx.worker.take().unwrap().join().unwrap();
        let mut actual = Vec::new();
        let error = rx.read_to_end(&mut actual).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidData);
        assert_eq!(actual.len(), MAX_PENDING_BYTES);
    }
}
