use super::*;
use std::net::TcpListener;

fn accept(listener: &TcpListener) -> TcpStream {
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        match listener.accept() {
            Ok((stream, _)) => { stream.set_nonblocking(false).unwrap(); stream.set_read_timeout(Some(Duration::from_secs(3))).unwrap(); return stream; }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock && Instant::now() < deadline => std::thread::sleep(Duration::from_millis(5)),
            Err(e) => panic!("test peer did not connect: {e}"),
        }
    }
}

fn confirmed_roundtrip(drop_ack: bool) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    listener.set_nonblocking(true).unwrap();
    let address = listener.local_addr().unwrap();
    let server = std::thread::spawn(move || {
        let mut stream = accept(&listener);
        let mut cn = [0;16]; stream.read_exact(&mut cn).unwrap();
        let sn = [0x31;16]; stream.write_all(&sn).unwrap();
        let mut header = [0;3]; stream.read_exact(&mut header).unwrap(); assert_eq!(header,[5,1,128]);
        let mut public_a = [0;SRP_BYTES]; stream.read_exact(&mut public_a).unwrap();
        // Independent server side of SRP-6a, with a deterministic fixture salt/private b.
        let n = BigUint::parse_bytes(SRP_PRIME.as_bytes(),16).unwrap();
        let g = BigUint::from(5u8); let b = BigUint::from(37u8); let salt = [0x23;16];
        let mut pg = [0;SRP_BYTES]; pg[SRP_BYTES-1]=5;
        let identity = sha512(&[b"Hachimodock:Hachimodock-auto-v1"]);
        let x = BigUint::from_bytes_be(sha512(&[&salt,identity.as_ref()]).as_ref());
        let v = g.modpow(&x,&n);
        let k = BigUint::from_bytes_be(sha512(&[&n.to_bytes_be(),&pg]).as_ref());
        let public_b = (k * &v + g.modpow(&b,&n)) % &n;
        let pb = public_b.to_bytes_be(); let mut padded_b = [0;SRP_BYTES]; padded_b[SRP_BYTES-pb.len()..].copy_from_slice(&pb);
        let u = BigUint::from_bytes_be(sha512(&[&public_a,&padded_b]).as_ref());
        let shared = (BigUint::from_bytes_be(&public_a) * v.modpow(&u,&n)).modpow(&b,&n);
        let secret = sha512(&[&shared.to_bytes_be()]);
        stream.write_all(&salt).unwrap(); stream.write_all(&(pb.len() as u16).to_be_bytes()).unwrap(); stream.write_all(&pb).unwrap();
        let mut proof = [0;64]; stream.read_exact(&mut proof).unwrap();
        let mut group_hash = sha512(&[&n.to_bytes_be()]);
        for (a,b) in group_hash.iter_mut().zip(sha512(&[&pg]).iter()) {*a ^= *b;}
        let expected = sha512(&[group_hash.as_ref(),sha512(&[b"Hachimodock"]).as_ref(),&salt,&public_a,&pb,secret.as_ref()]);
        assert_eq!(proof.as_slice(),expected.as_ref());
        let master = hmac::Key::new(hmac::HMAC_SHA256,secret.as_ref()); let mut transcript=hmac::Context::with_key(&master);
        transcript.update(b"P4LAN1");transcript.update(&cn);transcript.update(&sn);
        let mut stored=[0;16];stored.copy_from_slice(&transcript.sign().as_ref()[..16]);
        if !drop_ack {stream.write_all(sha512(&[&public_a,&proof,secret.as_ref()]).as_ref()).unwrap();}
        drop(stream);
        // Client must authenticate a fresh read-only probe before returning success.
        let mut stream = accept(&listener); let mut cn=[0;16]; stream.read_exact(&mut cn).unwrap(); let sn=[0x41;16];stream.write_all(&sn).unwrap();
        let mut header=[0;3];stream.read_exact(&mut header).unwrap();assert_eq!(header,[PROBE,0,0]);
        let key=session_key(&stored,&cn,&sn);let mut cipher=[0;16];stream.read_exact(&mut cipher).unwrap();
        assert!(key.open_in_place(nonce(0),aead::Aad::from(header),&mut cipher).unwrap().is_empty());
        let mut ack=Vec::new();key.seal_in_place_append_tag(nonce(1),aead::Aad::from(header),&mut ack).unwrap();stream.write_all(&ack).unwrap();
        stored
    });
    let peer=pair_exchange(address,b"Hachimodock-auto-v1",true).unwrap();
    assert_eq!(*peer.key,server.join().unwrap());
}

#[test]
fn automatic_pairing_proofs_and_readonly_probe_succeed() { confirmed_roundtrip(false); }
#[test]
fn lost_final_ack_is_recovered_only_by_authenticated_probe() { confirmed_roundtrip(true); }

#[test]
fn address_recovery_requires_original_key_and_uses_only_read_only_probes() {
    fn mock_peer(master: [u8; 16]) -> (SocketAddr, std::thread::JoinHandle<()>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let address = listener.local_addr().unwrap();
        let worker = std::thread::spawn(move || {
            let mut stream = accept(&listener);
            let mut client = [0; 16];
            stream.read_exact(&mut client).unwrap();
            let server = [0x53; 16];
            stream.write_all(&server).unwrap();
            let mut header = [0; 3];
            stream.read_exact(&mut header).unwrap();
            assert_eq!(header, [PROBE, 0, 0]);
            let mut body = [0; 16];
            stream.read_exact(&mut body).unwrap();
            let key = session_key(&master, &client, &server);
            if key.open_in_place(nonce(0), aead::Aad::from(header), &mut body).is_ok() {
                let mut ack = Vec::new();
                key.seal_in_place_append_tag(nonce(1), aead::Aad::from(header), &mut ack).unwrap();
                stream.write_all(&ack).unwrap();
            }
        });
        (address, worker)
    }

    let previous = Peer { address: ([192, 168, 1, 10], PORT).into(), key: Zeroizing::new([7; 16]) };
    let (unrelated, unrelated_thread) = mock_peer([2; 16]);
    let (moved, moved_thread) = mock_peer([7; 16]);
    let recovered = find_paired_peer(&previous, [previous.address, unrelated, moved], |peer| {
        exchange(peer, PROBE, &[]).is_ok()
    }).unwrap();
    assert_eq!(recovered.address, moved);
    assert_eq!(*recovered.key, *previous.key);
    unrelated_thread.join().unwrap();
    moved_thread.join().unwrap();
}

#[test]
fn volume_acceptance_and_result_use_authenticated_frames() {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    listener.set_nonblocking(true).unwrap();
    let master = [0x73; 16];
    let peer = Peer { address: listener.local_addr().unwrap(), key: Zeroizing::new(master) };
    let id = [0x25; 16];
    let fixture = volume_request_body(id, "0123456789abcdef", 64).unwrap();
    let worker = std::thread::spawn(move || {
        for (mode, request, result) in [(VOLUME_REQUEST, fixture.as_slice(), 3),
                                        (VOLUME_RESULT, id.as_slice(), 5)] {
            let mut stream = accept(&listener);
            let mut client = [0; 16];
            stream.read_exact(&mut client).unwrap();
            let server = [mode; 16];
            stream.write_all(&server).unwrap();
            let mut header = [0; 3];
            stream.read_exact(&mut header).unwrap();
            assert_eq!(header, [mode, 0, request.len() as u8]);
            let mut body = vec![0; request.len() + 16];
            stream.read_exact(&mut body).unwrap();
            let key = session_key(&master, &client, &server);
            assert_eq!(key.open_in_place(nonce(0), aead::Aad::from(header), &mut body).unwrap(), request);
            let mut reply = vec![result];
            let aad = [mode, 0, request.len() as u8, 0, 1];
            key.seal_in_place_append_tag(nonce(1), aead::Aad::from(aad), &mut reply).unwrap();
            stream.write_all(&aad[3..]).unwrap();
            stream.write_all(&reply).unwrap();
        }
    });
    let accepted = exchange_response(&peer, VOLUME_REQUEST, &fixture, Some(1)).unwrap();
    assert_eq!(volume_result(&accepted).unwrap(), LanVolumeResult::Applying);
    let outcome = exchange_response(&peer, VOLUME_RESULT, &id, Some(1)).unwrap();
    assert_eq!(volume_result(&outcome).unwrap(), LanVolumeResult::Applied);
    worker.join().unwrap();
}

#[test]
fn device_status_requires_the_paired_key_and_authenticates_the_response() {
    fn request(tamper: bool) -> io::Result<Vec<u8>> {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let peer = Peer {
            address: listener.local_addr().unwrap(),
            key: Zeroizing::new([7; 16]),
        };
        let worker = std::thread::spawn(move || {
            let mut stream = accept(&listener);
            let mut client = [0; 16];
            stream.read_exact(&mut client).unwrap();
            let server = [0x31; 16];
            stream.write_all(&server).unwrap();
            let mut header = [0; 3];
            stream.read_exact(&mut header).unwrap();
            assert_eq!(header, [DEVICE_STATUS, 0, 0]);
            let mut request = [0; 16];
            stream.read_exact(&mut request).unwrap();
            let key = session_key(&[7; 16], &client, &server);
            assert!(key.open_in_place(nonce(0), aead::Aad::from(header), &mut request)
                .unwrap().is_empty());
            let mut response = br#"{"wifiConnected":true}"#.to_vec();
            let length = (response.len() as u16).to_be_bytes();
            let aad = [header[0], header[1], header[2], length[0], length[1]];
            key.seal_in_place_append_tag(nonce(1), aead::Aad::from(aad), &mut response).unwrap();
            if tamper {
                response[0] ^= 1;
            }
            stream.write_all(&length).unwrap();
            stream.write_all(&response).unwrap();
        });
        let result = exchange_response(&peer, DEVICE_STATUS, &[], Some(DEVICE_STATUS_MAX));
        worker.join().unwrap();
        result
    }

    assert_eq!(request(false).unwrap(), br#"{"wifiConnected":true}"#);
    assert_eq!(request(true).unwrap_err().kind(), io::ErrorKind::InvalidData);
}

#[test]
fn component_delivery_rejects_tampered_results_and_never_reopens_the_connection() {
    let fixture = b"0123456789abcdef\x04\0\x02\0\x02demo[]{}";
    for outcome in [Some(false), Some(true), None] {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let master = [0x71; 16];
        let peer = Peer { address: listener.local_addr().unwrap(), key: Zeroizing::new(master) };
        let worker = std::thread::spawn(move || {
            let mut stream = accept(&listener);
            let mut client = [0; 16];
            stream.read_exact(&mut client).unwrap();
            let server = [0x84; 16];
            stream.write_all(&server).unwrap();
            let mut header = [0; 3];
            stream.read_exact(&mut header).unwrap();
            assert_eq!(header, [9, 0, fixture.len() as u8]);
            let key = session_key(&master, &client, &server);
            let mut body = vec![0; fixture.len() + 16];
            stream.read_exact(&mut body).unwrap();
            assert_eq!(
                key.open_in_place(nonce(0), aead::Aad::from(header), &mut body).unwrap(),
                fixture
            );
            if let Some(tamper) = outcome {
                let aad = [9, 0, fixture.len() as u8, 0, 1];
                let mut reply = vec![1];
                key.seal_in_place_append_tag(nonce(1), aead::Aad::from(aad), &mut reply).unwrap();
                if tamper { reply[0] ^= 1; }
                stream.write_all(&aad[3..]).unwrap();
                stream.write_all(&reply).unwrap();
            }
            drop(stream);
            std::thread::sleep(Duration::from_millis(50));
            assert_eq!(listener.accept().unwrap_err().kind(), io::ErrorKind::WouldBlock);
        });
        let result = exchange_response_with_timeout(&peer, 9, fixture, Some(1), Duration::from_secs(1));
        if outcome == Some(false) {
            assert_eq!(result.unwrap(), [1]);
        } else {
            assert!(result.is_err());
        }
        worker.join().unwrap();
    }
}
