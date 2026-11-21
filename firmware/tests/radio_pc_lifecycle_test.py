"""Host execution of production radio admission/visibility/autoplay branches.

USB IO, resolve, discovery and decoder startup are stubs. Actual board/boot
validation, request admission, visibility and maybe_auto are extracted unchanged;
real owner tests run separately in the same crate. Never opens a serial port or
uses the user's component library. Firmware event generation is covered by
p4_media_runtime_test.py, including deviceDirect compatibility for legacy PCs.
"""
from pathlib import Path
import os
import shutil
import subprocess
import pytest


def fixture():
    root=Path(__file__).resolve().parents[2]/'pc/src-tauri/src'
    s=(root/'component_media.rs').read_text()
    def between(a,b): return s[s.index(a):s.index(b,s.index(a))]
    head=r'''
    #![allow(dead_code,unused_imports)]
    use std::{collections::HashMap,path::PathBuf,sync::{Arc,Mutex,OnceLock,atomic::{AtomicBool,AtomicU64,Ordering}},time::{Duration,Instant}};
    use serde::{Serialize,Deserialize}; use serde_json::{Value,json};
    #[derive(Clone,Default)] struct UsbConnectionStatus {connected:bool, connection_id:String,board_device_id:String,boot_id:String,capabilities:Value}
    #[derive(Clone)] struct UsbSerialManager(Arc<Mutex<UsbConnectionStatus>>);
    impl UsbSerialManager {fn status(&self)->UsbConnectionStatus{self.0.lock().unwrap().clone()}}
    mod owner {
     #[derive(Clone,Debug,PartialEq,Eq)] pub struct Scope {pub board:String,pub boot:String}
     pub struct Guard;
     impl Guard {pub fn ensure_known(&self,_:&Scope)->Result<(),String>{Ok(())}}
    }
    #[derive(Clone,Serialize,Deserialize)] struct Station {id:String,hls:bool}
    #[derive(Clone)] struct Discovery {auto_play_on_enter:bool}
    #[derive(Clone)] struct Extension {discovery:Option<Discovery>,permissions:Value,catalog_rows:Vec<Value>}
    impl Extension {fn map_results(&self,v:&Value,_:&str)->Vec<Station>{v.as_array().unwrap().iter().filter_map(|row|serde_json::from_value(row.clone()).ok()).collect()}}
    #[derive(Clone,Serialize,Deserialize)] struct Snapshot {state:String,message:String,current:Option<Station>,catalog:Vec<Station>}
    impl Default for Snapshot {fn default()->Self{Self{state:"idle".into(),message:String::new(),current:None,catalog:vec![]}}}
    impl Snapshot {fn queue(&self)->Vec<Station>{self.catalog.clone()}}
    #[derive(Default)] struct DeviceState {unknown:AtomicBool}
    impl DeviceState {fn ensure_known(&self)->Result<(),String>{if self.unknown.load(Ordering::SeqCst){Err("unknown ACK".into())}else{Ok(())}}}
    struct Service {id:String,version:String,extension:Extension,usb:UsbSerialManager,data:Mutex<Snapshot>,binding:Mutex<Option<RadioConnectionBinding>>,operation:Mutex<()>,visibility:Mutex<(u64,u64,bool,Instant)>,pending_auto:AtomicBool,auto_station:Mutex<Option<String>>,device_state:DeviceState,library_fail:AtomicBool,starts:AtomicU64}
    struct Host {owner:Arc<owner::Guard>,root:PathBuf,usb:UsbSerialManager,services:Mutex<HashMap<String,Arc<Service>>>,operation:Mutex<()>,requests:Mutex<()>}
    static HOST:OnceLock<Host>=OnceLock::new();
    mod realtime_chat_log {
     use super::*;
     pub static LOG:Mutex<Vec<Value>>=Mutex::new(Vec::new());
     pub fn record(_:&str,event:&str,mut v:Value){v["event"]=event.into();LOG.lock().unwrap().push(v)}
     pub fn error_kind(_:&str)->&str{"mock"}
    }
    '''
    # Real board/boot validation, no substituted permissive identity check.
    o=(root/'component_media/owner.rs').read_text();head+=o[o.index('impl Scope {'):o.index('#[derive(Clone, Debug, PartialEq, Eq)]\npub(super) struct Record')].replace('impl Scope','impl owner::Scope')
    head+=between('#[derive(Clone, Debug, PartialEq, Eq)]\nstruct RadioConnectionBinding','fn radio_device_state_for_binding')
    head+=between('fn service(id:','// App-private durable owner evidence')
    head+='impl Service {\n'+between('    fn owner_scope(', '    fn poison_bound_owner(')
    head+=r'''
     fn retire_for_connection_change(&self){self.pending_auto.store(false,Ordering::SeqCst);}
     fn stop(&self,_:&str,_:&str){self.pending_auto.store(false,Ordering::SeqCst);}
     fn ensure_catalog(self:&Arc<Self>){} // Mock asynchronous discovery boundary: tests inject its result.
     fn snapshot(&self)->Value{json!(*self.data.lock().unwrap())}
     fn library(&self)->Result<(),String>{if self.library_fail.load(Ordering::SeqCst){self.device_state.unknown.store(true,Ordering::SeqCst);Err("unknown ACK".into())}else{Ok(())}}
     fn start_with_policy(&self,station:Station,_:bool)->Result<(),String>{self.data.lock().unwrap().current=Some(station);self.starts.fetch_add(1,Ordering::SeqCst);self.data.lock().unwrap().state="buffering".into();Ok(())}
    '''
    head+=between('    fn maybe_auto(', '    fn start(self:')+'}\n'
    # Exact execute preconditions + locking + visibility branch; omit unrelated search/controls.
    e=between('fn execute_for_binding(', '    if matches!(op,"stop"|"toggle"')
    a=e.index('    if op == "search"');b=e.index('    let host = HOST.get()',a)
    e=e[:a]+e[b:]
    head+=e+'    Err("unsupported mock operation".into())\n}\n'
    head+=r'''
    fn resolve(_:&str)->Result<(String,Extension),String>{Ok(("verified-fixture".into(),Extension{discovery:Some(Discovery{auto_play_on_enter:true}),permissions:json!({}),catalog_rows:vec![json!({"id":"preset","hls":false})]}))}
    fn create_service_with_owner(id:String,version:String,extension:Extension,_:PathBuf,usb:UsbSerialManager,_:Arc<owner::Guard>)->Arc<Service>{
     let binding=radio_connection_binding(&usb.status()).ok();
     Arc::new(Service{id,version,extension,usb,data:Mutex::new(Snapshot{current:Some(Station{id:"hls".into(),hls:true}),catalog:vec![Station{id:"hls".into(),hls:true}],..Default::default()}),binding:Mutex::new(binding),operation:Mutex::new(()),visibility:Mutex::new((0,0,false,Instant::now())),pending_auto:AtomicBool::new(false),auto_station:Mutex::new(None),device_state:DeviceState::default(),library_fail:AtomicBool::new(false),starts:AtomicU64::new(0)})
    }
    '''
    head+=f'#[path="{root}/component_service_settings.rs"] mod component_service_settings;\n#[path="{root}/component_media/owner.rs"] mod real_owner;\n'
    head+=between('fn request_for_binding(', '#[tauri::command]\npub async fn component_extension_request')
    # Frozen R280 visibility branch: unknown metadata must not start stale current.
    head+='fn legacy_visibility(s:Arc<Service>,input:Value)->Result<Value,String>{\nlet id="city-radio";let op="visibility";\n    if op=="visibility" {\n        let boot=input["boot"].as_u64().ok_or("无效媒体生命周期")?;\n        let epoch=input["epoch"].as_u64().ok_or("无效媒体生命周期")?;\n        let visible=input["visible"].as_bool().ok_or("无效媒体生命周期")?;\n        let mut prior=s.visibility.lock().unwrap();\n        if boot==prior.0 && epoch<prior.1 {return Ok(s.snapshot());}\n        let entered=visible && (boot!=prior.0 || epoch!=prior.1 || !prior.2);\n        *prior=(boot,epoch,visible,Instant::now());drop(prior);\n        let device_direct = id == "city-radio" && input["deviceDirect"].as_bool() == Some(true);\n        if !visible || device_direct {s.pending_auto.store(false,Ordering::SeqCst);}\n        else if entered && s.extension.discovery.as_ref().is_some_and(|d|d.auto_play_on_enter) {\n            s.pending_auto.store(true,Ordering::SeqCst);\n            crate::realtime_chat_log::record("","radio_auto_play",json!({"stage":"enter","count":s.data.lock().unwrap().queue().len()}));\n        }\n        s.maybe_auto();\n        return Ok(s.snapshot());\n    }\nErr("unsupported legacy operation".into())\n}\n'
    head+=r'''
    #[test] fn fresh_service_initial_visibility_matrix(){
     let dir=tempfile::tempdir().unwrap();
     let usb=UsbSerialManager(Arc::new(Mutex::new(UsbConnectionStatus{connected:true,connection_id:"7".into(),board_device_id:"synthetic-board".into(),boot_id:"0123456789abcdef".into(),capabilities:json!({})})));
     assert!(HOST.set(Host{owner:Arc::new(owner::Guard),root:dir.path().into(),usb:usb.clone(),services:Mutex::new(HashMap::new()),operation:Mutex::new(()),requests:Mutex::new(())}).is_ok());
     let binding=radio_connection_binding(&usb.status()).unwrap();
     let event=|direct|json!({"operation":"visibility","source":"pc.city-radio","boot":43,"epoch":1,"visible":true,"deviceDirect":direct});
     let reset=||{HOST.get().unwrap().services.lock().unwrap().clear();realtime_chat_log::LOG.lock().unwrap().clear();};
     let log_count=||realtime_chat_log::LOG.lock().unwrap().iter().filter(|v|v["event"]=="radio_auto_play").count();
     // Real request admission creates a fresh enabled service; no pre-existing UI status call.
     request_for_binding("city-radio".into(),event(false),Some(&binding)).unwrap();
     let s=service("city-radio").unwrap();assert_eq!(s.starts.load(Ordering::SeqCst),1);assert_eq!(log_count(),2);
     execute_for_binding("city-radio",event(false),Some(&binding)).unwrap();assert_eq!(s.starts.load(Ordering::SeqCst),1);
     println!("PASS fresh visibility: enter + start; heartbeat no repeat");
     reset();request_for_binding("city-radio".into(),event(true),Some(&binding)).unwrap();
     let s=service("city-radio").unwrap();assert_eq!(s.starts.load(Ordering::SeqCst),0);assert_eq!(log_count(),0);assert_eq!(s.data.lock().unwrap().state,"idle");assert!(s.data.lock().unwrap().current.as_ref().unwrap().hls);
     println!("PASS accepted deviceDirect: current HLS + idle + zero auto logs");
     execute_for_binding("city-radio",event(false),Some(&binding)).unwrap();assert_eq!(s.starts.load(Ordering::SeqCst),0);assert_eq!(log_count(),0);
     println!("PASS same-epoch deviceDirect loss: no new entry, no PC autoplay");
     // New epoch still claims deviceDirect for legacy-PC safety. New PCs
     // recognize the explicit format reason and choose the exact verified key.
     let fallback=json!({"operation":"visibility","boot":43,"epoch":2,"visible":true,"deviceDirect":true,"fallback":"format_unsupported","key":"local"});
     legacy_visibility(s.clone(),fallback.clone()).unwrap();
     assert_eq!(s.starts.load(Ordering::SeqCst),0);assert!(!s.pending_auto.load(Ordering::SeqCst));
     // Restore previous epoch so the new PC receives the same first handoff.
     s.visibility.lock().unwrap().1=1;
     s.data.lock().unwrap().catalog.push(Station{id:"local".into(),hls:false});
     execute_for_binding("city-radio",fallback.clone(),Some(&binding)).unwrap();
     assert_eq!(s.starts.load(Ordering::SeqCst),1);assert_eq!(s.data.lock().unwrap().current.as_ref().unwrap().id,"local");
     execute_for_binding("city-radio",fallback.clone(),Some(&binding)).unwrap();assert_eq!(s.starts.load(Ordering::SeqCst),1);
     s.stop("idle","user stop");s.data.lock().unwrap().state="idle".into();
     execute_for_binding("city-radio",fallback.clone(),Some(&binding)).unwrap();assert_eq!(s.starts.load(Ordering::SeqCst),1);
     assert!(realtime_chat_log::LOG.lock().unwrap().iter().any(|v|v["reason"]=="format_fallback"));
     for unknown in [false,true] {
       reset();request_for_binding("city-radio".into(),event(true),Some(&binding)).unwrap();let s=service("city-radio").unwrap();
       s.device_state.unknown.store(unknown,Ordering::SeqCst);
       execute_for_binding("city-radio",fallback.clone(),Some(&binding)).unwrap();assert_eq!(s.starts.load(Ordering::SeqCst),0);
       if !unknown { // No matching catalog key yet: do not start stale HLS current.
         assert!(s.pending_auto.load(Ordering::SeqCst));
         s.data.lock().unwrap().catalog.push(Station{id:"local".into(),hls:false});s.maybe_auto();
         assert_eq!(s.starts.load(Ordering::SeqCst),1);assert_eq!(s.data.lock().unwrap().current.as_ref().unwrap().id,"local");
       } else {
         assert!(!s.pending_auto.load(Ordering::SeqCst));
         s.device_state.unknown.store(false,Ordering::SeqCst);s.data.lock().unwrap().catalog.push(Station{id:"local".into(),hls:false});
         execute_for_binding("city-radio",fallback.clone(),Some(&binding)).unwrap();assert_eq!(s.starts.load(Ordering::SeqCst),0);
       }
     }
     reset();request_for_binding("city-radio".into(),event(true),Some(&binding)).unwrap();let s=service("city-radio").unwrap();
     let mut preset=fallback.clone();preset["key"]="preset".into();
     execute_for_binding("city-radio",preset,Some(&binding)).unwrap();assert_eq!(s.starts.load(Ordering::SeqCst),1);
     assert_eq!(s.data.lock().unwrap().current.as_ref().unwrap().id,"preset");
     println!("PASS exact-key format fallback, delayed catalog, unknown ACK and user stop guards");
     reset();component_service_settings::set_enabled(&dir.path().join("city-radio"),false).unwrap();
     assert!(request_for_binding("city-radio".into(),event(false),Some(&binding)).is_err());assert!(service("city-radio").is_err());assert_eq!(log_count(),0);
     component_service_settings::set_enabled(&dir.path().join("city-radio"),true).unwrap();println!("PASS disabled preference: reject before service/auto log");
     reset();usb.0.lock().unwrap().boot_id.clear();assert!(radio_connection_binding(&usb.status()).is_err());assert!(request_for_binding("city-radio".into(),event(false),Some(&binding)).is_err());assert_eq!(log_count(),0);usb.0.lock().unwrap().boot_id=binding.scope.boot.clone();
     println!("PASS invalid boot binding: admission blocked before auto log");
     reset();request_for_binding("city-radio".into(),json!({"operation":"status"}),Some(&binding)).unwrap();let s=service("city-radio").unwrap();s.data.lock().unwrap().catalog.clear();
     execute_for_binding("city-radio",event(false),Some(&binding)).unwrap();assert!(s.pending_auto.load(Ordering::SeqCst));assert_eq!(log_count(),1);
     s.data.lock().unwrap().catalog=vec![Station{id:"hls".into(),hls:true}];s.maybe_auto();assert_eq!(s.starts.load(Ordering::SeqCst),1);assert_eq!(log_count(),2);println!("PASS delayed catalog: pending survives then starts");
     reset();request_for_binding("city-radio".into(),json!({"operation":"status"}),Some(&binding)).unwrap();let s=service("city-radio").unwrap();s.library_fail.store(true,Ordering::SeqCst);
     execute_for_binding("city-radio",event(false),Some(&binding)).unwrap();assert_eq!(log_count(),3);assert_eq!(s.starts.load(Ordering::SeqCst),0);assert!(!s.pending_auto.load(Ordering::SeqCst));
     execute_for_binding("city-radio",event(false),Some(&binding)).unwrap();assert_eq!(log_count(),3);println!("PASS unknown library ACK: enter/start/error logged; no unsafe retry");
    }
    '''
    return head

def test_radio_pc_fresh_visibility_format_handoff_and_owner(tmp_path):
    cargo = shutil.which("cargo")
    if not cargo:
        candidates = list((Path.home()/".rustup/toolchains").glob("stable-*/bin/cargo"))
        cargo = str(candidates[0]) if candidates else None
    if not cargo:
        pytest.skip("Rust toolchain required for production radio host tests")
    harness = Path(__file__).with_name("radio_pc_lifecycle_harness")
    for name in ("Cargo.toml", "Cargo.lock"):
        shutil.copyfile(harness/name, tmp_path/name)
    (tmp_path/"generated.rs").write_text(fixture())
    env = dict(os.environ, PATH=str(Path(cargo).parent)+os.pathsep+os.environ.get("PATH", ""))
    subprocess.run([cargo,"test","--offline","--locked","--manifest-path",str(tmp_path/"Cargo.toml"),"--","--nocapture"], env=env, check=True, timeout=120)
