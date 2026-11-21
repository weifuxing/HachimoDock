//! Pure bounded event admission. No transport, app handles, credentials or IO.
use std::time::{Duration, Instant};
pub const TTL_MS: u64 = 3000;
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Reply { pub seq: u32, pub context: u8, pub ttl: u16, pub ack: u32 }
pub fn decode(bytes: &[u8]) -> Result<Option<Reply>, &'static str> {
    if bytes.len() != 12 { return Err("invalid realtime key reply"); }
    if bytes[0] == 0 { return if bytes.iter().all(|b| *b == 0) { Ok(None) } else { Err("invalid denial") }; }
    let seq = u32::from_be_bytes(bytes[4..8].try_into().map_err(|_| "sequence")?);
    let ttl = u16::from_be_bytes([bytes[2], bytes[3]]);
    if bytes[0] != 1 || bytes[1] > 2 || u64::from(ttl) > TTL_MS ||
        (seq == 0 && (ttl != 0 || bytes[1] != 0)) || (seq != 0 && ttl == 0) { return Err("invalid event"); }
    Ok(Some(Reply { seq, context: bytes[1], ttl, ack: u32::from_be_bytes(bytes[8..12].try_into().map_err(|_| "ack")?) }))
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Token { pub board: String, pub boot: String, pub seq: u32 }
/// Capture once before ACK (and keep across lost-ACK retries).
#[derive(Clone)]
pub enum Action<T> { Start(T), Stop(String) }
impl<T> Action<T> {
    pub fn accepts_session(&self, current: Option<&str>) -> bool {
        match self {
            Self::Start(_) => current.is_none(),
            Self::Stop(expected) => current == Some(expected.as_str()),
        }
    }
}
#[derive(Default)]
pub struct Admission { pub offer: Option<(Token, Instant, u8)>, pub claiming: bool, pub settled: u32 }
impl Admission {
    pub fn observe(&mut self, token: Token, reply: Reply, request_started: Instant) {
        // Keep an accepted-but-unconfirmed ACK retryable until its ORIGINAL TTL.
        if self.claiming || self.offer.as_ref().is_some_and(|(old, until, _)| old == &token || Instant::now() < *until) { return; }
        if reply.seq == 0 || reply.seq <= self.settled { return; }
        self.offer = Some((token, request_started + Duration::from_millis(u64::from(reply.ttl)), reply.context));
    }
    pub fn ready(&self, token: &Token, now: Instant) -> bool {
        !self.claiming && self.offer.as_ref().is_some_and(|(old, until, _)| old == token && now < *until && old.seq > self.settled)
    }
    pub fn claim(&mut self, token: &Token, now: Instant) -> bool {
        if !self.ready(token, now) { return false; }
        self.claiming = true; true
    }
    pub fn finish(&mut self, token: &Token, delivered: bool) {
        if self.offer.as_ref().is_some_and(|(old, _, _)| old == token) {
            self.claiming = false;
            if delivered { self.settled = self.settled.max(token.seq); self.offer = None; }
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    fn token(seq:u32)->Token {Token{board:"p4-0123456789ab".into(),boot:"0123456789abcdef".into(),seq}}
    fn reply(seq:u32)->Reply {Reply{seq,context:0,ttl:3000,ack:0}}
    #[test] fn ack_cannot_turn_start_into_stop() {
        let action = Action::Start(());
        assert!(action.accepts_session(None));
        assert!(!action.accepts_session(Some("new-session-during-ack")));
        assert!(!action.clone().accepts_session(Some("new-session-on-ack-retry")));
    }
    #[test] fn ack_cannot_stop_successor_or_restart_ended_session() {
        let action = Action::<()>::Stop("original-session".into());
        assert!(action.accepts_session(Some("original-session")));
        assert!(!action.accepts_session(Some("successor-same-board-and-boot")));
        assert!(!action.accepts_session(None));
        assert!(!action.clone().accepts_session(Some("successor-on-ack-retry")));
    }
    #[test] fn duplicate_claim_and_boot_are_rejected() {
        let mut s=Admission::default();let t=Instant::now();s.observe(token(1),reply(1),t);
        let mut wrong=token(1);wrong.boot="other".into();assert!(!s.claim(&wrong,t));
        assert!(s.claim(&token(1),t));assert!(!s.claim(&token(1),t));s.finish(&token(1),true);
        s.observe(token(1),reply(1),t);assert!(!s.claim(&token(1),t));
    }
    #[test] fn failed_ack_retries_but_does_not_extend_ttl() {
        let mut s=Admission::default();let t=Instant::now();s.observe(token(2),reply(2),t);
        assert!(s.claim(&token(2),t));s.finish(&token(2),false);
        s.observe(token(2),reply(2),t+Duration::from_secs(2));
        assert!(s.claim(&token(2),t+Duration::from_secs(2)));s.finish(&token(2),false);
        assert!(!s.claim(&token(2),t+Duration::from_secs(3)));
        s.observe(token(2),reply(2),t+Duration::from_secs(4));
        assert!(!s.claim(&token(2),t+Duration::from_secs(4)));
    }
    #[test] fn reply_shapes_fail_closed() {
        assert_eq!(decode(&[0;12]),Ok(None));assert!(decode(&[0;11]).is_err());
        let mut b=[0;12];b[0]=1;assert!(decode(&b).unwrap().is_some());
        b[7]=1;assert!(decode(&b).is_err());b[2]=11;b[3]=184;assert_eq!(decode(&b).unwrap().unwrap().ttl,3000);
        b[3]=185;assert!(decode(&b).is_err());b[3]=184;b[1]=3;assert!(decode(&b).is_err());
    }
    #[test] fn takeover_clears_pending_and_claim() {
        let mut s=Admission::default();let t=Instant::now();s.observe(token(1),reply(1),t);s.claim(&token(1),t);
        s=Admission::default();assert!(!s.ready(&token(1),t));s.finish(&token(1),true);assert_eq!(s.settled,0);
    }
}
