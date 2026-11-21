//! Authenticated, frontend-accepted realtime-key delivery. Never log peer secrets or payloads.
use super::{exchange_response, load_peer, query_status, Peer};
use super::press_state::{self, Action, Admission, Token};
use super::realtime::AudioTarget;
use crate::{realtime_chat::{RealtimeChatStartInput, RealtimeChatStatus}, usb_serial::UsbSerialManager};
use serde::{Deserialize, Serialize};
use std::{sync::{Mutex, OnceLock}, time::{Duration, Instant}};
use tauri::Emitter;
const MODE: u8 = 16;
#[derive(Clone, Deserialize, Serialize)]
#[serde(rename_all="camelCase")]
pub(crate) struct PressToken { board_device_id: String, boot_id: String, seq: u32 }
impl PressToken {
    fn token(&self)->Token {Token{board:self.board_device_id.clone(),boot:self.boot_id.clone(),seq:self.seq}}
}
#[derive(Clone)]
struct Binding { peer: Peer, board: String, boot: String, owner: [u8;16] }
impl Binding {
    fn same(&self, other:&Self)->bool {self.owner==other.owner && self.boot==other.boot && self.board==other.board && same_peer(&self.peer,&other.peer)}
    fn request(&self, ack:u32)->Vec<u8> {
        let mut body=self.boot.as_bytes().to_vec();body.extend_from_slice(&self.owner);body.extend_from_slice(&ack.to_be_bytes());body
    }
    fn exchange(&self, ack:u32)->Result<Option<press_state::Reply>,String> {
        let bytes=exchange_response(&self.peer,MODE,&self.request(ack),Some(12)).map_err(|_|"Wi-Fi 按键确认暂不可用，请重试".to_string())?;
        press_state::decode(&bytes).map_err(|_|"Wi-Fi 按键响应无效".into())
    }
}
fn same_peer(a:&Peer,b:&Peer)->bool {a.address==b.address && a.key.as_ref()==b.key.as_ref()}
#[derive(Default)]
struct Inbox { binding:Option<Binding>, admission:Admission, action:Option<(Token,Action<AudioTarget>)> }
fn inbox()->&'static Mutex<Inbox> {static INBOX:OnceLock<Mutex<Inbox>>=OnceLock::new();INBOX.get_or_init(Default::default)}
fn current(i:&Inbox,b:&Binding)->bool {i.binding.as_ref().is_some_and(|old|old.same(b))}
fn invalidate() {if let Ok(mut i)=inbox().lock(){*i=Inbox::default();}}

pub(super) struct Poller { next_status:Instant, peer:Option<Peer> }
impl Default for Poller {fn default()->Self{Self{next_status:Instant::now(),peer:None}}}
impl Poller {
    pub(super) fn reset(&mut self) {invalidate();self.peer=None;self.next_status=Instant::now();}
    pub(super) fn tick(&mut self,app:&tauri::AppHandle,usb:&UsbSerialManager,peer:&Peer)->bool {
        if usb.status().connected {self.reset();return false;}
        if !self.peer.as_ref().is_some_and(|p|same_peer(p,peer)) {self.reset();self.peer=Some(peer.clone());}
        if Instant::now()>=self.next_status {
            self.next_status=Instant::now()+Duration::from_secs(5);
            let Ok(status)=query_status(peer) else {return false;};
            if (status.rtc_key & 3)!=1 || !status.control_trusted || !status.wifi_connected ||
                status.boot_id.len()!=16 || !status.boot_id.bytes().all(|b|b.is_ascii_hexdigit()) || status.board_device_id.len()!=15 {
                invalidate();return false;
            }
            if let Ok(mut i)=inbox().lock() {
                let same=i.binding.as_ref().is_some_and(|b|b.boot==status.boot_id && b.board==status.board_device_id && same_peer(&b.peer,peer));
                if !same {
                    *i=Inbox{binding:Some(Binding{peer:peer.clone(),boot:status.boot_id,board:status.board_device_id,owner:*uuid::Uuid::new_v4().as_bytes()}),..Inbox::default()};
                }
            }
        }
        let binding=inbox().lock().ok().and_then(|i|i.binding.clone());
        let Some(binding)=binding else{return false;};
        let began=Instant::now();
        let reply=match binding.exchange(0) {
            Ok(Some(reply))=>reply,
            Ok(None)=>{invalidate();self.next_status=Instant::now();return false;},
            Err(_)=>return false, // Peek failed: no ACK, no device dequeue.
        };
        if usb.status().connected {self.reset();return false;}
        let event={
            let Ok(mut i)=inbox().lock() else{return false;};
            if !current(&i,&binding){return false;}
            i.admission.observe(Token{board:binding.board.clone(),boot:binding.boot.clone(),seq:reply.seq},reply,began);
            i.admission.offer.as_ref().and_then(|(token,_,_)|i.admission.ready(token,Instant::now()).then(||
                PressToken{board_device_id:token.board.clone(),boot_id:token.boot.clone(),seq:token.seq}))
        };
        // Repeated offers are intentional: emit is not frontend acceptance.
        if let Some(event)=event {let _=app.emit("lan-realtime-press",event);}
        true
    }
}

fn toggle(app:&tauri::AppHandle,usb:&UsbSerialManager,token:PressToken,input:Option<RealtimeChatStartInput>)->Result<Option<RealtimeChatStatus>,String>{
    let key=token.token();
    if usb.status().connected {return Err("USB 已接管，已忽略旧 Wi-Fi 按键".into());}
    let (binding,pinned)={let i=inbox().lock().map_err(|_|"Wi-Fi 按键状态不可用")?;
        if !i.admission.ready(&key,Instant::now()){return Err("Wi-Fi 按键已过期或已处理".into());}
        (i.binding.clone().ok_or("Wi-Fi 按键连接已变化")?,i.action.as_ref().filter(|(token,_)|token==&key).map(|(_,action)|action.clone()))};
    if !load_peer().is_some_and(|p|same_peer(&p,&binding.peer)){return Err("Wi-Fi 配对已变化".into());}
    let status=query_status(&binding.peer).map_err(|_|"无法核验 Wi-Fi 设备身份")?;
    if (status.rtc_key & 3)!=1 || !status.control_trusted || !status.wifi_connected || status.boot_id!=key.boot || status.board_device_id!=key.board {
        return Err("Wi-Fi 设备已重启或由 USB 接管".into());
    }
    let action=if let Some(action)=pinned {action} else {
        let active=crate::realtime_chat::status();
        if active.active {
            if active.board_device_id!=key.board{return Err("另一台设备正在实时对话".into());}
            Action::Stop(active.session_id)
        } else {
            if status.wireless_realtime_busy!=Some(false){return Err("设备音频已被其他会话使用".into());}
            Action::Start(AudioTarget::from_bound(binding.peer.clone(),status,&key.board,&key.boot)?)
        }
    };
    if input.as_ref().is_some_and(|i|i.board_device_id!=key.board){return Err("按键与形象目标设备不一致".into());}
    let action={let mut i=inbox().lock().map_err(|_|"Wi-Fi 按键状态不可用")?;
        if !current(&i,&binding)||!i.admission.ready(&key,Instant::now()){return Err("Wi-Fi 按键已过期或已处理".into());}
        // Preserve intent across persona loading and lost-ACK retries as well.
        if !i.action.as_ref().is_some_and(|(token,_)|token==&key) {i.action=Some((key.clone(),action));}
        let action=i.action.as_ref().unwrap().1.clone();
        if matches!(&action,Action::Start(_)) && input.is_none(){return Ok(None);}
        if !i.admission.claim(&key,Instant::now()){return Err("Wi-Fi 按键已过期或已处理".into());}
        action};
    // Acceptance is a real frontend command, not event emission. ACK loss is
    // retryable with the same seq; firmware remembers the ACK within the lease.
    let ack=binding.exchange(key.seq);
    let confirmed=matches!(ack,Ok(Some(reply)) if reply.ack==key.seq);
    let valid={let mut i=inbox().lock().map_err(|_|"Wi-Fi 按键状态不可用")?;
        let valid=current(&i,&binding)&&i.admission.offer.as_ref().is_some_and(|(old,until,_)|old==&key&&Instant::now()<*until);
        i.admission.finish(&key,confirmed);valid};
    if !confirmed{return Err("Wi-Fi 按键确认未完成，请重试".into());}
    if !valid || usb.status().connected || !load_peer().is_some_and(|p|same_peer(&p,&binding.peer)) {
        return Err("连接已变化，未执行旧 Wi-Fi 按键".into());
    }
    let current=crate::realtime_chat::status();
    if !action.accepts_session(current.active.then_some(current.session_id.as_str())) {
        return Err("对话已变化，未执行旧 Wi-Fi 按键".into());
    }
    match action {
        Action::Stop(session_id)=>crate::realtime_chat::stop_wifi_bound(&key.board,&key.boot,&session_id).map(Some),
        Action::Start(target)=>crate::realtime_chat::start_wifi_bound(app,input.ok_or("请重新长按开始")?,target).map(Some),
    }
}

#[tauri::command]
pub(crate) async fn lan_realtime_toggle(app_handle:tauri::AppHandle,usb_manager:tauri::State<'_,UsbSerialManager>,token:PressToken,input:Option<RealtimeChatStartInput>)->Result<Option<RealtimeChatStatus>,String>{
    let usb=usb_manager.inner().clone();
    tauri::async_runtime::spawn_blocking(move||toggle(&app_handle,&usb,token,input)).await.map_err(|_|"Wi-Fi 按键任务异常".to_string())?
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{io::{Read,Write},net::TcpListener};
    use ring::aead;
    use crate::lan_agent::{session_digest,key_from_digest,nonce};
    fn fixture(corrupt:bool)->(Binding,std::thread::JoinHandle<()>) {
        let listener=TcpListener::bind("127.0.0.1:0").unwrap();
        let peer=Peer{address:listener.local_addr().unwrap(),key:zeroize::Zeroizing::new([42;16])};
        let binding=Binding{peer,board:"p4-0123456789ab".into(),boot:"0123456789abcdef".into(),owner:[7;16]};
        let copy=binding.clone();
        let task=std::thread::spawn(move||{
            for ack in if corrupt {vec![0]} else {vec![0,1]} {
                let (mut stream,_)=listener.accept().unwrap();stream.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
                let mut client=[0;16];stream.read_exact(&mut client).unwrap();let server=[9;16];stream.write_all(&server).unwrap();
                let mut header=[0;3];stream.read_exact(&mut header).unwrap();assert_eq!(header,[MODE,0,36]);
                let key=key_from_digest(&session_digest(copy.peer.key.as_ref(),&client,&server));
                let mut body=vec![0;52];stream.read_exact(&mut body).unwrap();
                let plain=key.open_in_place(nonce(0),aead::Aad::from(header),&mut body).unwrap();assert_eq!(plain,copy.request(ack));
                let mut response=vec![1,0,0,0,0,0,0,0,0,0,0,0];
                if ack==0 {response[2]=11;response[3]=184;response[7]=1;}else{response[11]=1;}
                key.seal_in_place_append_tag(nonce(1),aead::Aad::from([MODE,0,36,0,12]),&mut response).unwrap();
                if corrupt {*response.last_mut().unwrap()^=1;}
                stream.write_all(&[0,12]).unwrap();stream.write_all(&response).unwrap();
            }
        });(binding,task)
    }
    #[test] fn authenticated_peek_then_exact_ack() {
        let (binding,task)=fixture(false);
        assert_eq!(binding.exchange(0).unwrap().unwrap().seq,1);
        let ack=binding.exchange(1).unwrap().unwrap();assert_eq!(ack.ack,1);assert_eq!(ack.seq,0);task.join().unwrap();
    }
    #[test] fn tampered_authenticated_response_never_delivers() {
        let (binding,task)=fixture(true);assert!(binding.exchange(0).is_err());task.join().unwrap();
    }
}
