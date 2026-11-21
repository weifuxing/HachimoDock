//! Real production authenticated exchange on loopback, synthetic key only.
use ring::{
    aead, hmac,
    rand::{SecureRandom, SystemRandom},
};
use std::{
    io::{self, Read, Write},
    net::{SocketAddr, TcpListener, TcpStream},
    time::{Duration, Instant},
};
use zeroize::Zeroizing;
const IO_TIMEOUT: Duration = Duration::from_millis(2200);
const MAX_BODY: usize = 16384;
struct Peer {
    address: SocketAddr,
    key: Zeroizing<[u8; 16]>,
}
struct AuthenticatedLink {
    stream: TcpStream,
    key: aead::LessSafeKey,
    reply: Vec<u8>,
    subkey: Option<Zeroizing<[u8; 16]>>,
}
mod realtime {
    pub const MODE: u8 = 20;
}
include!("production_auth.inc");

fn server(tamper: bool, slow: bool) -> (Peer, std::thread::JoinHandle<()>) {
    let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
    let peer = Peer {
        address: listener.local_addr().unwrap(),
        key: Zeroizing::new([7; 16]),
    };
    let handle = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(1)))
            .unwrap();
        let mut client = [0; 16];
        stream.read_exact(&mut client).unwrap();
        if slow {
            std::thread::sleep(Duration::from_millis(160));
            return;
        }
        let server = [3; 16];
        stream.write_all(&server).unwrap();
        let key = key_from_digest(&session_digest(&[7; 16], &client, &server));
        let mut header = [0; 3];
        stream.read_exact(&mut header).unwrap();
        assert_eq!(header[0], 17);
        let size = u16::from_be_bytes([header[1], header[2]]) as usize;
        let mut bytes = vec![0; size + 16];
        stream.read_exact(&mut bytes).unwrap();
        assert_eq!(
            key.open_in_place(nonce(0), aead::Aad::from(header), &mut bytes)
                .unwrap(),
            b"synthetic"
        );
        let mut reply = b"ack".to_vec();
        let length = [0, 3];
        let mut aad = header.to_vec();
        aad.extend(length);
        key.seal_in_place_append_tag(nonce(1), aead::Aad::from(aad), &mut reply)
            .unwrap();
        if tamper {
            *reply.last_mut().unwrap() ^= 1;
        }
        stream.write_all(&length).unwrap();
        stream.write_all(&reply).unwrap();
    });
    (peer, handle)
}
#[test]
fn real_aes_gcm_exchange_accepts_correlated_framing_and_rejects_modified_tag() {
    for tamper in [false, true] {
        let (peer, server) = server(tamper, false);
        let result = exchange_link_bounded(
            &peer,
            17,
            b"synthetic",
            Some(32),
            Duration::from_millis(300),
            Some(Instant::now() + Duration::from_millis(300)),
        );
        assert_eq!(result.is_ok(), !tamper);
        if let Ok(link) = result {
            assert_eq!(link.reply, b"ack");
        }
        server.join().unwrap();
    }
}
#[test]
fn asset_deadline_covers_authentication_not_only_response_wait() {
    let (peer, server) = server(false, true);
    let start = Instant::now();
    let result = exchange_link_bounded(
        &peer,
        17,
        b"synthetic",
        Some(32),
        Duration::from_millis(900),
        Some(start + Duration::from_millis(40)),
    );
    assert!(result.is_err());
    assert!(start.elapsed() < Duration::from_millis(130));
    server.join().unwrap();
}
#[test]
fn expired_asset_does_not_even_open_a_connection() {
    let listener = TcpListener::bind(("127.0.0.1", 0)).unwrap();
    listener.set_nonblocking(true).unwrap();
    let peer = Peer {
        address: listener.local_addr().unwrap(),
        key: Zeroizing::new([7; 16]),
    };
    assert!(exchange_link_bounded(
        &peer,
        17,
        b"synthetic",
        Some(32),
        IO_TIMEOUT,
        Some(Instant::now() - Duration::from_millis(1))
    )
    .is_err());
    assert!(listener.accept().is_err());
}
