"""Production RTC HUD header, mailbox scheduling, conversation and view lifecycle."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_production_hud_frames_nonblocking_state_lane_and_retirement(tmp_path):
    source = (ROOT / "main/pet_p4_lan_realtime.inc").read_text()
    checks = source[source.index("static size_t rtc_wire_downlink_bytes("):source.index("static unsigned rtc_wire_open(")]
    offer = source[source.index("static bool rtc_hud_offer("):source.index("static atomic_uint rtc_wire_last_exit;")]
    process = source[source.index("static void rtc_hud_process("):source.index("static bool rtc_hud_offer(")]
    harness = r'''
#include <assert.h>
#include <stdio.h>
#include "pet_p4_lan_hud.h"
#include "pet_p4_lan_realtime_audio.h"
#include "pet_p4_view.h"
enum { LAN_RTC_BEGIN=4, LAN_RTC_END=5, LAN_RTC_FLUSH=6, LAN_RTC_TURN_BYTES=4,
       LAN_RTC_BODY_MAX=644, LAN_TAG_SIZE=16 };
static pet_lan_hud_t rtc_hud;
static pet_p4_runtime_state_t state;
static int rtc_hud_mux;
static uint32_t rtc_hud_generation;
static uint64_t now;
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
''' + checks + offer + process + r'''
static uint8_t frame[644];
static void payload(unsigned phase, const char *id, const char *text) {
  memset(frame,0,sizeof(frame));frame[0]=phase;
  snprintf((char *)frame+4,64,"%s",id);
  snprintf((char *)frame+68,64,"%s","小西");
  snprintf((char *)frame+132,512,"%s",text);
}
int main(void) {
  uint8_t header[8];
  pet_lan_rtc_header(header,0,7,0,644);
  assert(rtc_wire_check_header(header,0)==668);
  assert(!rtc_wire_check_header(header,1));
  header[1]=1;assert(!rtc_wire_check_header(header,0));header[1]=0;
  header[3]--;assert(!rtc_wire_check_header(header,0));
  pet_lan_rtc_header(header,0,7,0,65535);assert(!rtc_wire_check_header(header,0));
  pet_lan_rtc_header(header,0,2,0,0);assert(rtc_wire_check_header(header,0)==24);
  pet_lan_rtc_header(header,UINT32_MAX,7,0,644);assert(!rtc_wire_check_header(header,UINT32_MAX));
  pet_lan_hud_begin(&rtc_hud);
  payload(1,"rtc-a","");assert(!rtc_hud_offer(frame));
  payload(0,"rtc-a","");assert(rtc_hud_offer(frame));
  payload(1,"rtc-a","用户的问题");frame[1]=3;
  assert(rtc_hud_offer(frame)); // Main has not consumed preparing: latest state coalesces safely.
  assert(!pet_p4_conversation_active(&state));
  payload(2,"rtc-a","");assert(rtc_hud_offer(frame));
  assert(!pet_p4_conversation_active(&state));
  pet_lan_hud_apply(&rtc_hud,&state,++now,false);
  pet_p4_view_model_t view;pet_p4_build_view_model(&state,&view);
  assert(view.realtime_conversation&&!strcmp(view.body,"想一想"));
  assert(state.conversation_history_count==1&&!strcmp(state.conversation_history[0].text,"用户的问题"));
  payload(3,"rtc-a","");assert(rtc_hud_offer(frame));
  pet_lan_hud_apply(&rtc_hud,&state,++now,false);
  pet_p4_build_view_model(&state,&view);assert(!strcmp(view.body,"准备回答"));
  payload(5,"rtc-a","连接失败");assert(rtc_hud_offer(frame));
  pet_lan_hud_apply(&rtc_hud,&state,++now,false);assert(state.conversation_error_until_ms==now+8000);
  payload(0,"other","");assert(!rtc_hud_offer(frame));
  payload(4,"rtc-a","");assert(rtc_hud_offer(frame));
  pet_lan_hud_apply(&rtc_hud,&state,++now,false);assert(!pet_p4_conversation_active(&state));
  payload(3,"rtc-a","late");assert(!rtc_hud_offer(frame));
  pet_lan_hud_begin(&rtc_hud);payload(0,"rtc-b","");assert(rtc_hud_offer(frame));
  payload(4,"rtc-b","");assert(rtc_hud_offer(frame)); // End before first tick never opens UI.
  pet_lan_hud_apply(&rtc_hud,&state,++now,false);assert(!pet_p4_conversation_active(&state));
  pet_lan_hud_begin(&rtc_hud);payload(0,"rtc-c","");assert(rtc_hud_offer(frame));
  pet_lan_hud_apply(&rtc_hud,&state,++now,false);
  pet_lan_hud_end(&rtc_hud);pet_lan_hud_begin(&rtc_hud); // New old-peer socket has no HUD.
  pet_lan_hud_apply(&rtc_hud,&state,++now,false);assert(!pet_p4_conversation_active(&state));
  payload(0,"rtc-d","");assert(rtc_hud_offer(frame));pet_lan_hud_apply(&rtc_hud,&state,++now,false);
  assert(pet_p4_conversation_update(&state,"usb-new","preparing","USB","new",false,false,++now));
  pet_lan_hud_apply(&rtc_hud,&state,++now,true);
  assert(!strcmp(state.conversation_id,"usb-new"));
  payload(3,"rtc-d","late");assert(!rtc_hud_offer(frame));
  pet_lan_hud_end(&rtc_hud);pet_lan_hud_begin(&rtc_hud);
  pet_lan_hud_apply(&rtc_hud,&state,++now,false);assert(!strcmp(state.conversation_id,"usb-new"));
  pet_p4_conversation_clear(&state);
  payload(0,"rtc-e","");memset(frame+132,'x',512);assert(!rtc_hud_offer(frame));
  payload(0,"rtc-e","");frame[2]=1;assert(!rtc_hud_offer(frame));
  payload(0,"rtc-e","");frame[0]=6;assert(!rtc_hud_offer(frame));
  payload(0,"rtc-e","");assert(rtc_hud_offer(frame));
  uint64_t before=now;
  for(unsigned i=0;i<10000;i++) {payload(1,"rtc-e","latest");assert(rtc_hud_offer(frame));}
  assert(now==before); // Stalled main never makes the audio task wait or allocate.
  pet_lan_hud_apply(&rtc_hud,&state,++now,false);assert(!strcmp(state.conversation_state,"listening"));
  uint64_t updated=state.last_update_ms;
  assert(rtc_hud_offer(frame));pet_lan_hud_apply(&rtc_hud,&state,++now,false);
  assert(state.last_update_ms==updated); // Duplicate heartbeat does not redraw/history-append.
  pet_lan_hud_end(&rtc_hud);pet_lan_hud_apply(&rtc_hud,&state,++now,false);
  assert(!pet_p4_conversation_active(&state));
  pet_lan_hud_begin(&rtc_hud);payload(0,"A","");assert(rtc_hud_offer(frame));
  pet_lan_hud_apply(&rtc_hud,&state,++now,false);
  pet_lan_hud_end(&rtc_hud);pet_lan_hud_begin(&rtc_hud);
  payload(0,"B","");assert(rtc_hud_offer(frame));payload(4,"B","");assert(rtc_hud_offer(frame));
  pet_lan_hud_close(&rtc_hud);pet_lan_hud_apply(&rtc_hud,&state,++now,false);
  assert(!pet_p4_conversation_active(&state)); // A -> close -> B ended before main tick.
  pet_lan_hud_begin(&rtc_hud);payload(0,"error-owner","");assert(rtc_hud_offer(frame));
  payload(5,"error-owner","设备连接失败");assert(rtc_hud_offer(frame));
  pet_lan_hud_close(&rtc_hud);pet_lan_hud_apply(&rtc_hud,&state,++now,false);
  assert(!strcmp(state.conversation_state,"error")&&state.conversation_error_until_ms==now+8000);
  pet_lan_hud_apply(&rtc_hud,&state,++now,false);assert(!strcmp(state.conversation_state,"error"));
  assert(pet_p4_conversation_update(&state,"usb-successor","preparing","USB","",false,false,++now));
  rtc_hud_generation=4;rtc_hud_process(&state,++now,false,4);
  assert(!strcmp(state.conversation_id,"usb-successor"));
  pet_p4_conversation_clear(&state);
  pet_lan_hud_begin(&rtc_hud);payload(0,"generation-4","");assert(rtc_hud_offer(frame));
  rtc_hud_process(&state,++now,true,5); // Re-pairing still trusted but generation changed.
  assert(!pet_p4_conversation_active(&state)&&!rtc_hud.live);
  rtc_hud_generation=5;pet_lan_hud_begin(&rtc_hud);payload(0,"generation-5","");assert(rtc_hud_offer(frame));
  rtc_hud_process(&state,++now,true,5);assert(pet_p4_conversation_active(&state));
  payload(1,"generation-5","pending");assert(rtc_hud_offer(frame));
  rtc_hud_process(&state,++now,false,5); // Forget/USB/offline before worker cleanup.
  assert(!pet_p4_conversation_active(&state)&&!rtc_hud.live);
  puts("HUD production frame/state lifecycle passed");
}
'''
    c = tmp_path / "hud.c"
    c.write_text(harness)
    binary = tmp_path / "hud"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", "-I", str(ROOT / "main"),
                    str(c), str(ROOT / "main/pet_p4_conversation.c"),
                    str(ROOT / "main/pet_p4_view.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)


def test_actual_pc_rtc_pump_preparation_terminal_order_and_old_peer(tmp_path):
    """Reuse local Rust dependencies; no Cargo rebuild, downloads or real device."""
    import os
    import shutil
    import pytest
    deps = Path(os.environ.get("HACHIMI_HOST_RUST_DEPS", ROOT.parents[1] / ".cache/cargo/macos-arm64/release/deps"))
    rustc = shutil.which("rustc")
    if not rustc or not deps.is_dir():
        pytest.skip("requires existing host Rust compiler/dependencies")
    pc = ROOT.parent / "pc/src-tauri/src/realtime_chat"
    wifi = (pc / "wifi.rs").read_text().split("\n#[cfg(test)]\nmod tests {", 1)[0]
    prefix = r'''
#![allow(dead_code)]
mod lan_agent { pub mod realtime {
  use std::{net::TcpStream,sync::Mutex};
  pub struct AudioTarget {pub stream:Mutex<Option<TcpStream>>,pub hud:bool}
  impl AudioTarget {
    pub fn hud_supported(&self)->bool{self.hud}
    pub fn boot_matches(&self,boot:&str)->bool{boot=="boot-test"}
    pub fn is_idle(&self)->bool{true}
    pub fn open(&self)->Result<(TcpStream,zeroize::Zeroizing<[u8;16]>),String>{
      Ok((self.stream.lock().unwrap().take().ok_or("already opened")?,zeroize::Zeroizing::new([42;16])))
    }
  }
}}
mod realtime_chat {
  use std::sync::{Arc,Mutex,OnceLock,mpsc,atomic::{AtomicBool,Ordering}};
  pub enum Input {Pcm(Vec<u8>,Option<bool>)}
  pub struct Session {pub id:String,pub wifi:Option<wifi::Link>,pub capture_ready:AtomicBool,
    pub input:mpsc::SyncSender<Input>,pub completed_playback:Mutex<String>,pub failed:AtomicBool}
  static ACTIVE:OnceLock<Mutex<Option<Arc<Session>>>>=OnceLock::new();
  fn active_session()->Option<Arc<Session>>{ACTIVE.get_or_init(||Mutex::new(None)).lock().unwrap().clone()}
  fn request_failure(s:&Session,_:&str,_:Option<&str>){s.failed.store(true,Ordering::SeqCst);}
  mod diagnostics {pub fn record(_:&str,_:&str,_:serde_json::Value){}}
'''
    suffix = r'''
#[test]
fn production_pump_hud_lifecycle() {
    use ring::aead::{self,Aad,LessSafeKey,Nonce,UnboundKey};
    use std::net::TcpListener;
    fn nonce(direction:u8,seq:u32)->Nonce {
      let mut n=[b'R',b'T',b'C',1,direction,0,0,0,0,0,0,0];n[8..].copy_from_slice(&seq.to_be_bytes());
      Nonce::assume_unique_for_key(n)
    }
    fn receive(peer:&mut TcpStream,seq:&mut u32)->(u8,Vec<u8>){
      let mut h=[0;8];peer.read_exact(&mut h).unwrap();
      assert_eq!(&h[4..],&seq.to_be_bytes());assert_eq!(h[1],0);
      let size=u16::from_be_bytes([h[2],h[3]])as usize;assert!(size<=644);
      let mut b=vec![0;size+16];peer.read_exact(&mut b).unwrap();
      let key=LessSafeKey::new(UnboundKey::new(&aead::AES_128_GCM,&[42;16]).unwrap());
      key.open_in_place(nonce(1,*seq),Aad::from(h),&mut b).unwrap();*seq+=1;b.truncate(size);(h[0],b)
    }
    fn reply(peer:&mut TcpStream,kind:u8,seq:&mut u32){
      let mut h=[kind,0,0,0,0,0,0,0];h[4..].copy_from_slice(&seq.to_be_bytes());
      let key=LessSafeKey::new(UnboundKey::new(&aead::AES_128_GCM,&[42;16]).unwrap());
      let tag=key.seal_in_place_separate_tag(nonce(2,*seq),Aad::from(h),&mut[]).unwrap();*seq+=1;
      peer.write_all(&h).unwrap();peer.write_all(tag.as_ref()).unwrap();
    }
    fn pair(cap:bool)->(Arc<Session>,TcpStream){
      let l=TcpListener::bind("127.0.0.1:0").unwrap();let stream=TcpStream::connect(l.local_addr().unwrap()).unwrap();
      stream.set_read_timeout(Some(Duration::from_millis(8))).unwrap();
      let(mut peer,_)=l.accept().unwrap();peer.set_read_timeout(Some(Duration::from_secs(3))).unwrap();
      let(tx,_rx)=std::sync::mpsc::sync_channel(128);
      let s=Arc::new(Session{id:"rtc-socket-test".into(),wifi:Some(Link::new(AudioTarget{stream:Mutex::new(Some(stream)),hud:cap})),
        capture_ready:AtomicBool::new(false),input:tx,completed_playback:Mutex::new(String::new()),failed:AtomicBool::new(false)});
      *super::ACTIVE.get_or_init(||Mutex::new(None)).lock().unwrap()=Some(s.clone());(s,peer)
    }
    fn payload(state:&str)->serde_json::Value{serde_json::json!({"sessionId":"rtc-socket-test","state":state,"name":"小西","text":"测试"})}
    for active in [false,true] {
      let(s,mut peer)=pair(true);let mut rx=0;let mut tx=0;
      Link::hud(&s,&payload("preparing")).unwrap();
      let(k,b)=receive(&mut peer,&mut rx);assert_eq!(k,7);assert_eq!(b[0],0);reply(&mut peer,2,&mut tx);
      let link=s.wifi.as_ref().unwrap();assert!(!link.activate.load(Ordering::Acquire));
      if active {
        Link::start(&s).unwrap();
        loop{let(k,b)=receive(&mut peer,&mut rx);reply(&mut peer,2,&mut tx);if k==2{break;}assert_eq!(k,7);assert_eq!(b[0],0);}
        for (phase,code) in [("listening",1),("thinking",2),("speaking",3)] {
          Link::hud(&s,&payload(phase)).unwrap();
          loop{let(k,b)=receive(&mut peer,&mut rx);reply(&mut peer,2,&mut tx);if k==7{assert_eq!(b[0],code);break;}assert_eq!(k,2);}
        }
      }
      link.finish_hud(&payload(if active{"error"}else{"ended"}));
      loop{let(k,b)=receive(&mut peer,&mut rx);if k==7{assert_eq!(b[0],if active{5}else{4});break;}assert_eq!(k,2);reply(&mut peer,2,&mut tx);}
      let(k,_)=receive(&mut peer,&mut rx);assert_eq!(k,3);reply(&mut peer,3,&mut tx);
      let until=Instant::now()+Duration::from_secs(1);while !link.stopped()&&Instant::now()<until{thread::sleep(Duration::from_millis(5));}
      assert!(link.stopped());assert!(!s.failed.load(Ordering::Acquire));
    }
    let(s,_peer)=pair(false);Link::hud(&s,&payload("preparing")).unwrap();
    let link=s.wifi.as_ref().unwrap();assert_eq!(link.state.load(Ordering::Acquire),IDLE);
    assert!(link.hud.lock().unwrap().is_empty()); // Old peer never receives unknown kind 7.
}
}}
'''
    source = tmp_path / "pump.rs"
    source.write_text(prefix + f'#[path="{pc / "lan_audio.rs"}"] mod lan_audio;\nmod wifi {{\n' + wifi + suffix)
    binary = tmp_path / "pump"
    args = [rustc, "--edition=2021", "--test", str(source), "-L", f"dependency={deps}", "-o", str(binary)]
    for name in ("ring", "zeroize", "serde_json"):
        matches = sorted(deps.glob(f"lib{name}-*.rlib"), key=lambda p: p.stat().st_mtime, reverse=True)
        if not matches:
            pytest.skip(f"requires existing host {name} dependency")
        args.extend(["--extern", f"{name}={matches[0]}"])
    subprocess.run(args, check=True)
    subprocess.run([str(binary)], check=True, timeout=20)


def test_actual_rtc_worker_releases_audio_on_usb_takeover(tmp_path):
    """Run the actual worker and cleanup with fake SDK/socket endpoints, no I/O."""
    source = (ROOT / "main/pet_p4_lan_realtime.inc").read_text()
    enums = source[source.index("enum {"):source.index("typedef struct {")]
    trust = source[source.index("static bool rtc_wire_trusted("):source.index("static atomic_bool rtc_wire_busy;")]
    task = source[source.index("static void rtc_wire_task("):source.index("/* Return true only when the worker owns fd")]
    harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/select.h>
#include "pet_p4_lan_hud.h"
#include "pet_p4_lan_realtime_audio.h"
#define LAN_TAG_SIZE 16
#define LAN_IO_US 1000
#define LAN_VOICE_OFFER_BYTES 20
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define MBEDTLS_CIPHER_ID_AES 1
#define PET_P4_AUDIO_WIFI_PLAY_BEGIN 1
#define PET_P4_AUDIO_WIFI_PLAY_FRAME 2
#define PET_P4_AUDIO_WIFI_PLAY_FLUSH 3
#define PET_P4_AUDIO_WIFI_PLAY_END 4
#define pdMS_TO_TICKS(x) (x)
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
typedef int esp_err_t;
typedef int mbedtls_gcm_context;
typedef unsigned pet_p4_audio_wifi_session_t;
static atomic_bool usb_client_active,rtc_wire_busy,rtc_wire_last_suspended_power_save;
static atomic_uint rtc_wire_last_exit;
static pet_lan_hud_t rtc_hud;
static int rtc_hud_mux;
static uint32_t rtc_hud_generation;
static struct {int fd;uint32_t generation,ptt;uint8_t handshake_key[16],media_key[16];
  int capture_lock;unsigned head,count;uint8_t queue[8][640];} rtc_wire;
static unsigned mode, starts,stops,closed,deleted,offers,selects;
static int64_t clock_us;
static bool ota_wire_trusted(uint32_t g){return g==1;}
static int64_t esp_timer_get_time(void){return clock_us+=1000;}
static void mbedtls_gcm_init(mbedtls_gcm_context *g){*g=0;}
static void mbedtls_gcm_free(mbedtls_gcm_context *g){(void)g;}
static int mbedtls_gcm_setkey(mbedtls_gcm_context *g,int c,const uint8_t*k,int n){(void)g;(void)c;(void)k;(void)n;return 0;}
static void mbedtls_platform_zeroize(void*p,size_t n){memset(p,0,n);}
static bool send_sealed_response(int f,mbedtls_gcm_context*g,const uint8_t*h,uint8_t*w,uint8_t*b,size_t n,int64_t d){(void)f;(void)g;(void)h;(void)w;(void)b;(void)n;(void)d;return true;}
static unsigned rtc_wire_receive_start(int f,mbedtls_gcm_context*g,uint8_t*w,uint8_t*b,uint32_t*s,int64_t d){
  (void)f;(void)g;(void)w;(void)b;(void)s;(void)d;
  if(mode==0){atomic_store(&usb_client_active,true);return 7;}return PET_LAN_RTC_KEEPALIVE;
}
static bool rtc_hud_offer(const uint8_t*b){offers++;return pet_lan_hud_offer(&rtc_hud,b);}
static int pet_device_wifi_suspend_power_save(void){return 0;}
static void pet_device_wifi_restore_power_save(int p){(void)p;}
static void rtc_wire_tap(void){}
static void rtc_wire_ptt(int fd, mbedtls_gcm_context *g, uint8_t *wire, uint8_t *body,
                         uint32_t *rx, uint32_t *tx, uint32_t generation) {
  (void)fd; (void)g; (void)wire; (void)body; (void)rx; (void)tx; (void)generation;
  assert(!"RTC compatibility test must not enter PTT");
}
static int pet_p4_audio_wifi_start(void(*tap)(void),void*c,pet_p4_audio_wifi_session_t*s){(void)tap;(void)c;starts++;*s=1;if(mode==1)atomic_store(&usb_client_active,true);return 0;}
static bool pet_p4_audio_wifi_live(pet_p4_audio_wifi_session_t s){return s==1;}
static int pet_p4_audio_wifi_stop(pet_p4_audio_wifi_session_t s){assert(s==1);stops++;return 0;}
static uint32_t pet_p4_audio_wifi_take_drained_turn(pet_p4_audio_wifi_session_t s){(void)s;return 0;}
static int pet_p4_audio_wifi_play(pet_p4_audio_wifi_session_t s,uint32_t t,int k,const uint8_t*b,size_t n){(void)s;(void)t;(void)k;(void)b;(void)n;return 0;}
static bool rtc_wire_send(int f,mbedtls_gcm_context*g,uint8_t*w,uint32_t*s,unsigned k,unsigned flags,uint8_t*b,size_t n){(void)f;(void)g;(void)w;(void)s;(void)k;(void)flags;(void)b;(void)n;return true;}
static size_t rtc_wire_seal(mbedtls_gcm_context*g,uint8_t*w,uint32_t*s,unsigned k,unsigned flags,uint8_t*b,size_t n){(void)g;(void)w;(void)s;(void)k;(void)flags;(void)b;(void)n;return 0;}
static unsigned rtc_wire_take(uint8_t*b,bool wait){(void)b;(void)wait;return 0;}
static void ota_wire_put_u32(uint8_t*b,uint32_t n){(void)b;(void)n;}
static uint32_t ota_wire_u32(const uint8_t*b){(void)b;return 0;}
static bool rtc_wire_retry_io(void){return false;}
static size_t rtc_wire_check_header(const uint8_t*w,uint32_t s){(void)w;(void)s;return 0;}
static unsigned rtc_wire_open(mbedtls_gcm_context*g,uint8_t*w,uint8_t*b,uint32_t*s,size_t n){(void)g;(void)w;(void)b;(void)s;(void)n;return 0;}
static bool socket_io(int f,uint8_t*b,size_t n,bool w,int64_t d){(void)f;(void)b;(void)n;(void)w;(void)d;return true;}
static void vTaskDelay(int t){(void)t;assert(!"HUD must not sleep");}
static void vTaskDelete(void*p){(void)p;deleted++;}
static void vSemaphoreDelete(int s){(void)s;}
static int fake_close(int f){(void)f;closed++;return 0;}
static int fake_select(int n,fd_set*r,fd_set*w,fd_set*e,struct timeval*t){(void)n;(void)r;(void)w;(void)e;(void)t;selects++;atomic_store(&usb_client_active,true);return 0;}
static int fake_send(int f,const uint8_t*b,size_t n,int flags){(void)f;(void)b;(void)n;(void)flags;assert(false);return 0;}
static int fake_recv(int f,uint8_t*b,size_t n,int flags){(void)f;(void)b;(void)n;(void)flags;assert(false);return 0;}
#define close fake_close
#define select fake_select
#define send fake_send
#define recv fake_recv
''' + enums + trust + task + r'''
static pet_p4_runtime_state_t state;
int main(void){
 for(mode=0;mode<3;mode++){
  starts=stops=closed=deleted=offers=selects=0;clock_us=0;
  atomic_store(&usb_client_active,false);atomic_store(&rtc_wire_busy,true);
  rtc_wire.fd=3;rtc_wire.generation=1;
  rtc_wire_task(NULL);
  assert(starts==(mode>0)&&stops==starts&&closed==1&&deleted==1&&!offers);
  assert(selects==(mode==2));assert(!atomic_load(&rtc_wire_busy));assert(!rtc_hud.live);
  assert(pet_p4_conversation_update(&state,"usb-new","preparing","USB","",false,false,1));
  pet_lan_hud_apply(&rtc_hud,&state,2,true);assert(!strcmp(state.conversation_id,"usb-new"));
  uint8_t late[644]={0};memcpy(late+4,"old-lan",8);assert(!pet_lan_hud_offer(&rtc_hud,late));
  pet_p4_conversation_clear(&state);
 }
 puts("RTC passive, activation and active-loop USB takeover release passed");
}
'''
    c = tmp_path / "worker.c"
    c.write_text(harness)
    binary = tmp_path / "worker"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I", str(ROOT / "main"), str(c), str(ROOT / "main/pet_p4_conversation.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=20)
