//! A disposable native WAMR process per preview, using the firmware interpreter.
use std::{collections::HashMap,io::{BufRead,BufReader,Write},process::{Child,ChildStdin,Command,Stdio},sync::{mpsc,Mutex,OnceLock},time::{Duration,Instant}};
use serde_json::Value;
struct Session { child:Child, input:ChildStdin, output:mpsc::Receiver<Result<Value,String>>, _module:tempfile::NamedTempFile, touched:Instant }
impl Drop for Session { fn drop(&mut self){let _=self.child.kill();let _=self.child.wait();} }
impl Session {
 fn receive(&mut self)->Result<Value,String>{self.touched=Instant::now();let value=self.output.recv_timeout(Duration::from_secs(3)).map_err(|_|"组件预览执行超时")??;if let Some(e)=value["error"].as_str(){return Err(e.into());}Ok(value)}
 fn send(&mut self,line:&str)->Result<Value,String>{writeln!(self.input,"{line}").and_then(|_|self.input.flush()).map_err(|_|"组件预览进程已退出")?;self.receive()}
}
static SESSIONS:OnceLock<Mutex<HashMap<String,Session>>>=OnceLock::new();
fn sessions()->&'static Mutex<HashMap<String,Session>>{SESSIONS.get_or_init(||{
 std::thread::spawn(||loop{std::thread::sleep(Duration::from_secs(10));if let Some(s)=SESSIONS.get(){if let Ok(mut s)=s.lock(){s.retain(|_,s|s.touched.elapsed()<Duration::from_secs(30));}}});
 Mutex::new(HashMap::new())
})}
pub fn open(bytes:Vec<u8>,epoch:u64,offset:i32)->Result<String,String>{
 if bytes.is_empty()||bytes.len()>crate::clawpkg::HACHI_MAX_MODULE_BYTES||epoch>4102444800000||!(-840..=840).contains(&offset){return Err("预览参数无效".into());}
 let mut module=tempfile::NamedTempFile::new().map_err(|e|e.to_string())?;
 module.write_all(&bytes).map_err(|e|e.to_string())?;
 let checker=crate::petui_authoring::checker()?;
 let mut command=Command::new(checker);
 command.arg(module.path()).arg("--serve").stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::null());
 #[cfg(windows)] {use std::os::windows::process::CommandExt;command.creation_flags(0x08000000);}
 let mut child=command.spawn().map_err(|e|e.to_string())?;
 let input=child.stdin.take().ok_or("预览输入不可用")?;
 let output=child.stdout.take().ok_or("预览输出不可用")?;
 let (tx,rx)=mpsc::sync_channel(1);
 std::thread::spawn(move||{let mut reader=BufReader::new(output);loop{
  let mut data=Vec::new();use std::io::Read;
  match reader.by_ref().take(5*1024*1024).read_until(b'\n',&mut data){
   Ok(0)=>break,Ok(_) if data.last()==Some(&b'\n')=>{if tx.send(serde_json::from_slice(&data).map_err(|_|"预览输出格式无效".into())).is_err(){break;}},_=>{let _=tx.send(Err("预览输出过大或已中断".into()));break;}
  }
 }});
 let mut session=Session{child,input,output:rx,_module:module,touched:Instant::now()};
 writeln!(session.input,"clock {epoch} {offset}").and_then(|_|session.input.flush()).map_err(|e|e.to_string())?;
 session.receive()?;
 let mut map=sessions().lock().map_err(|_|"预览繁忙")?;
 if map.len()>=4{return Err("同时试玩的组件过多".into());}
 let id=uuid::Uuid::new_v4().to_string();map.insert(id.clone(),session);Ok(id)
}
pub fn tick(id:&str,dt:u32,events:Vec<[i32;3]>,epoch:u64,offset:i32)->Result<Vec<Value>,String>{
 if dt>80||events.len()>12||epoch>4102444800000||!(-840..=840).contains(&offset){return Err("预览输入超限".into());}
 let mut map=sessions().lock().map_err(|_|"预览繁忙")?;
 let result=(||{let s=map.get_mut(id).ok_or("试玩已结束，请重新打开")?;s.send(&format!("clock {epoch} {offset}"))?;
  let mut results=Vec::new();for [kind,a,b] in events {if !(1..=6).contains(&kind)||!(0..=640).contains(&a)||!(0..=480).contains(&b){return Err("预览事件无效".into());}results.push(s.send(&format!("event {kind} {a} {b}"))?);}
  results.push(s.send(&format!("frame {dt}"))?);Ok(results)
 })();if result.is_err(){map.remove(id);}result
}
pub fn close(id:&str){if let Ok(mut s)=sessions().lock(){s.remove(id);}}
