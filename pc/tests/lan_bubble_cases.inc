#[cfg(test)]
mod host_tests {
    use super::*;
    use std::sync::Arc;
    #[test]
    fn authenticated_need_busy_preserves_manifest_and_resumes_without_replaying_writes() {
        for failure in ["need_busy", "busy_boot", "busy_board", "busy_request", "need_declined", "need_unknown", "usb", "pairing"] {
            let (peer, usb, mut body, events) = setup();
            prepare(&peer, &usb, &mut body);
            sent(&peer, &body);
            let f = frame(&events);
            let metadata = body["textPixels"].clone();
            let need = json!({"boot":f.boot_id,"client":f.renderer_id,"owner":f.owner,
                "version":f.version,"id":f.id,"style":4,"page":0,"text":"中English缺字"});
            {
                let mut fake = fake().lock().unwrap();
                fake.failure = failure.into();
                // A busy reply must never dispatch even an attached need.
                fake.need = Some(need.clone());
                fake.usb = failure == "usb";
                fake.revoked = failure == "pairing";
            }
            controller().lock().unwrap().session.as_mut().unwrap().polled = Instant::now()-Duration::from_secs(2);
            // Production passes a fresh ordinary snapshot on every tick.
            body.as_object_mut().unwrap().remove("textPixels");
            prepare(&peer, &usb, &mut body);
            assert!(!events.lock().unwrap().iter().any(|e| e["topic"] == "device/need"));
            assert_eq!(current(&f), failure == "need_busy", "{failure}");
            if failure == "need_busy" {
                assert_eq!(body["textPixels"], metadata);
                {
                    let mut fake = fake().lock().unwrap();
                    fake.failure.clear(); fake.need = Some(need);
                }
                controller().lock().unwrap().session.as_mut().unwrap().polled = Instant::now()-Duration::from_secs(2);
                prepare(&peer, &usb, &mut body);
                assert!(events.lock().unwrap().iter().any(|e| e["topic"] == "device/need"));
                assert_eq!(body["textPixels"], metadata);
                let fake = fake().lock().unwrap();
                assert_eq!(fake.calls.iter().filter(|p|p["phase"]=="capabilities").count(), 1);
                assert!(!fake.calls.iter().any(|p|matches!(p["phase"].as_str(),Some("begin"|"chunk"|"end"))));
            } else {
                assert!(body.get("textPixels").is_none(), "{failure}");
            }
        }
    }
    #[test]
    fn body_prefetch_waits_for_applied_owner_and_never_accepts_stale_readback() {
        let(peer,usb,mut body,events)=setup();
        {let mut f=fake().lock().unwrap();f.glyph_cells=true;f.body_masks=true;}
        prepare(&peer,&usb,&mut body);
        assert_eq!(body["textPixels"]["bodyMasks"],true);
        assert_eq!(body["textPixels"]["glyphCells"],true);
        assert!(events.lock().unwrap().iter().all(|e|e["demandOnly"]==true));
        sent(&peer,&body);
        assert!(events.lock().unwrap().iter().all(|e|e["demandOnly"]==true),"mailbox ACK alone must not prefetch");
        {let mut f=fake().lock().unwrap();f.owner_ready=true;f.failure="stale_status".into();}
        controller().lock().unwrap().session.as_mut().unwrap().polled=Instant::now()-Duration::from_secs(2);
        prepare(&peer,&usb,&mut body);
        assert!(events.lock().unwrap().iter().all(|e|e["demandOnly"]==true),"stale owner ACK must not prefetch");
        fake().lock().unwrap().failure.clear();
        controller().lock().unwrap().session.as_mut().unwrap().polled=Instant::now()-Duration::from_secs(2);
        prepare(&peer,&usb,&mut body);
        let e=events.lock().unwrap();
        assert_eq!(e.iter().filter(|e|e["prefetchBody"]==true&&e["demandOnly"]==false).count(),1);
        drop(e);
        let f=fake().lock().unwrap();
        assert!(f.calls.iter().any(|p|p["phase"]=="status"));
        assert!(!f.calls.iter().any(|p|p["phase"]=="begin"||p["phase"]=="chunk"));
    }
    fn setup() -> (Peer, UsbSerialManager, Value, Arc<Mutex<Vec<Value>>>) {
        *fake().lock().unwrap() = Fake::default();
        *controller().lock().unwrap() = Controller::default();
        let events = Arc::new(Mutex::new(Vec::new()));
        let capture = events.clone();
        set_renderer(
            &"a".repeat(32),
            Some(Arc::new(move |v| capture.lock().unwrap().push(v))),
        );
        (
            load_peer().unwrap(),
            UsbSerialManager,
            json!({"source":"codex","status":{"displayTitle":"Agent","displayContent":"中English缺字"}}),
            events,
        )
    }
    fn frame(events: &Arc<Mutex<Vec<Value>>>) -> BubbleFrame {
        let events = events.lock().unwrap();
        let e = events.iter().find(|v| v["topic"] == "state/lan").unwrap();
        let metadata = controller()
            .lock()
            .unwrap()
            .bridge
            .metadata("state/lan", "1122334455667788");
        let asset = metadata["owners"][0]["assets"]
            .as_array()
            .unwrap()
            .iter()
            .find(|v| v["style"] == 4)
            .unwrap();
        let data = vec![15u8; 496 * 54 / 16];
        let checksum = data.iter().fold(2166136261u32, |h, b| {
            (h ^ u32::from(*b)).wrapping_mul(16777619)
        });
        serde_json::from_value(json!({"wifi":true,"boardDeviceId":"board","connectionId":e["connectionId"],
            "bootId":e["bootId"],"rendererId":e["rendererId"],"owner":e["owner"],"version":e["version"],
            "id":asset["id"],"style":4,"pageSizes":[data.len()],"pageIndex":0,"pageCount":1,
            "checksum":format!("{checksum:08x}"),"data":base64::engine::general_purpose::STANDARD.encode(data)})).unwrap()
    }
    #[test]
    fn production_lan_controller_admission_delivery_and_ack_failures() {
        // One test owns global fake/config state. No parallel-test shared IO.
        for failure in [
            "", "cached", "unknown", "stale", "offset", "usb", "pairing", "audio",
        ] {
            let (peer, usb, mut body, events) = setup();
            prepare(&peer, &usb, &mut body);
            sent(&peer, &body);
            assert!(body["textPixels"].is_object());
            assert!(events
                .lock()
                .unwrap()
                .iter()
                .all(|v| v["demandOnly"] == true && v["wifi"] == true));
            let f = frame(&events);
            fake().lock().unwrap().failure = failure.into();
            let result = send(&usb, f);
            assert_eq!(
                result.is_ok(),
                matches!(failure, "" | "cached"),
                "{failure}"
            );
            let state = fake().lock().unwrap();
            let calls = &state.calls;
            assert_eq!(calls.iter().filter(|v| v["phase"] == "begin").count(), 1);
            assert_eq!(
                calls.iter().filter(|v| v["phase"] == "end").count(),
                usize::from(failure.is_empty())
            );
            if !matches!(failure, "" | "cached") {
                assert_eq!(calls.iter().filter(|v| v["phase"] == "chunk").count(), 1);
            }
            if failure == "cached" {
                assert!(!calls.iter().any(|v| v["phase"] == "chunk"));
            }
            for index in 2..calls.len() {
                let cost = calls[index - 1].to_string().len() + 17 + 16 + 3 + 16;
                assert!(
                    state.times[index].duration_since(state.times[index - 1])
                        >= Duration::from_secs_f64(cost as f64 / RATE as f64)
                );
            }
        }
    }
    #[test]
    fn production_lan_legacy_revocation_and_authenticated_need() {
        // Run with --test-threads=1 (controller is process-global in the app).
        for setting in ["legacy", "untrusted", "usb"] {
            let (peer, usb, mut body, _) = setup();
            {
                let mut f = fake().lock().unwrap();
                f.legacy = setting == "legacy";
                f.untrusted = setting == "untrusted";
                f.usb = setting == "usb";
            }
            prepare(&peer, &usb, &mut body);
            prepare(&peer, &usb, &mut body);
            assert!(body.get("textPixels").is_none());
            assert_eq!(
                fake().lock().unwrap().calls.len(),
                usize::from(setting == "legacy")
            );
        }
        let (peer, usb, mut body, events) = setup();
        prepare(&peer, &usb, &mut body);
        sent(&peer, &body);
        let f = frame(&events);
        assert!(current(&f));
        assert!(admit(&f).is_ok());
        {
            let _held = controller().lock().unwrap();
            assert!(admit(&f).unwrap_err().starts_with("BUBBLE_BUSY:"));
        }
        fake().lock().unwrap().need = Some(json!({"boot":f.boot_id,"client":f.renderer_id,
            "owner":f.owner,"version":f.version,"id":f.id,"style":4,"page":0,"text":"中English缺字"}));
        controller()
            .lock()
            .unwrap()
            .session
            .as_mut()
            .unwrap()
            .polled = Instant::now() - Duration::from_secs(2);
        prepare(&peer, &usb, &mut body);
        assert!(events
            .lock()
            .unwrap()
            .iter()
            .any(|v| v["topic"] == "device/need" && v["wifi"] == true));
        // New text invalidates the old image before another physical write.
        body["status"]["displayContent"] = json!("new text");
        prepare(&peer, &usb, &mut body);
        assert!(!current(&f));
        assert!(send(&usb, f).is_err());
        assert!(!fake()
            .lock()
            .unwrap()
            .calls
            .iter()
            .any(|v| v["phase"] == "begin"));
        let f = frame(&events);
        reset();
        assert!(!current(&f));
    }

    #[test]
    fn optional_metadata_does_not_break_full_snapshot_and_expired_lease_does_not_send() {
        let (peer, usb, mut body, events) = setup();
        prepare(&peer, &usb, &mut body);
        sent(&peer, &body);
        let f = frame(&events);
        controller().lock().unwrap().session.as_mut().unwrap().sent =
            Some(Instant::now() - Duration::from_secs(10));
        assert!(!current(&f));
        assert!(send(&usb, f).is_err());
        body["status"]["padding"] = json!("x".repeat(MAX_BODY));
        prepare(&peer, &usb, &mut body);
        assert!(body.get("textPixels").is_none());
        assert!(controller().lock().unwrap().session.is_none());
    }
}
