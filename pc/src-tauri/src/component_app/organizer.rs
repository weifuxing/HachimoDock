//! Optional notes + checklist workspace, selected by declared service operations.
//! Reuses the original note directory and the unified todo store, without copying data.
use super::*;
pub(super) fn enabled(d:&Declaration)->bool { d.service=="notes"&&d.actions.values().any(|op|op=="tasks") }
fn chunks(text:&str)->Vec<String>{
    let chars:Vec<_>=text.chars().collect();
    if chars.is_empty(){return vec![String::new()];}
    chars.chunks(36).map(|s|s.iter().collect()).collect()
}
fn page(values:&mut Value,index:&mut usize){
    let text=values["note"].as_str().unwrap_or("").to_owned();
    let blocks=chunks(&text);let count=blocks.len().div_ceil(2);
    *index=(*index).min(count-1);
    values["headline"]=blocks[*index*2].clone().into();
    values["note"]=blocks.get(*index*2+1).cloned().unwrap_or_default().into();
    values["badge"]=format!("{}/{}",*index+1,count).into();
}
pub(super) fn controls(id:&str,dir:&Path,d:&Declaration,s:&State)->Result<Value,String>{
    let (mode,mut ops,context)=if s.organizer_tasks{
        let ops=s.todos.ui["operations"].as_array().map(|v|v.iter().filter_map(Value::as_str).collect::<Vec<_>>()).unwrap_or_default();
        (if matches!(s.todos.ui["mode"].as_u64(),Some(1|5)){3}else{2},ops,s.todos.ui["context"].as_str().unwrap_or("").to_owned())
    }else{crate::component_notes::controls(id,dir)?};
    if s.organizer_tasks { ops.push("tasks");ops.push("record");if ops.contains(&"open"){ops.push("save");} }
    else {
        // Switching away cannot hide an unsaved draft or a recording in progress.
        if !matches!(mode,1|4|6){ops.push("tasks");}
        if mode==5 {ops.extend(["up","down","previous","next"]);}
        if mode==4 {ops.extend(["previous","next"]);}
    }
    let actions:Vec<_>=d.actions.iter().filter(|(_,op)|ops.contains(&op.as_str())).map(|(key,_)|key.as_str()).collect();
    Ok(json!({"mode":mode,"actions":actions,"context":context}))
}
pub(super) fn apply(id:&str,dir:&Path,s:&mut State,op:&str,input:&Value,usb:&crate::usb_serial::UsbSerialManager,values:&mut Value)->Result<(),String>{
    let mut operation=op;
    if op=="tasks" {
        let (phase,_,_)=crate::component_notes::controls(id,dir)?;
        if matches!(phase,1|4|6){return Err("请先结束录音并保存当前想法，再切换待办".into());}
        s.organizer_tasks=!s.organizer_tasks;operation="status";
    }
    if s.organizer_tasks&&op=="record"{s.organizer_tasks=false;s.organizer_page=0;}
    if s.organizer_tasks {
        operation=if operation=="save"{"open"}else{operation};
        if !matches!(operation,"status"|"enable"|"open"|"toggle"|"filter"|"previous"|"next"|"up"|"down"|"row_a"|"row_b"|"row_c") { return Err("请先切回想法，再操作笔记".into()); }
        merge(values,crate::component_todos::execute_rows(&mut s.todos,operation,input,3)?);
        if s.todos.ui["mode"]==2 {
            values["eyebrow"]="还没有事项".into();values["headline"]="先录下想做的事".into();values["metricLabel"]="保存后，点按转为待办".into();
        }
        values["footer"]=if matches!(s.todos.ui["mode"].as_u64(),Some(1|5)){"2 返回清单 · 点按完成或撤销"}else{"2 查看事项 · 上下选择 · 左右翻页 · 中按切换"}.into();
        return Ok(());
    }
    if matches!(op,"toggle"|"filter"|"row_a"|"row_b"|"row_c"|"open"){return Err("请先切到待办清单".into());}
    let text_op=match operation {"up"=>"previous","down"=>"next","previous"|"next"=>"status",other=>other};
    if !matches!(operation,"status"|"enable"|"previous"|"next"){s.organizer_page=0;}
    let mut note=crate::component_notes::execute(id,text_op,input,dir,usb)?;
    let (phase,_,_)=crate::component_notes::controls(id,dir)?;
    if matches!(phase,4|5){
        let count=chunks(note["note"].as_str().unwrap_or("")).len().div_ceil(2);
        if operation=="previous"{s.organizer_page=s.organizer_page.saturating_sub(1);}
        if operation=="next"{s.organizer_page=(s.organizer_page+1).min(count-1);}
        note["metricValue"]=if phase==4{"转写完成 · 待保存"}else{"已保存的想法"}.into();
        page(&mut note,&mut s.organizer_page);
    } else {s.organizer_page=0;}
    if phase==0 {note["metricValue"]="想法收件箱".into();note["headline"]="先记下来，慢慢完成".into();note["note"]="按录制说出想法，转写后保存；想去做的事，一键加入待办。".into();}
    note["footer"]=match phase {
        0=>"1 录下想法 · 中按查看待办",1=>"说完后按 1 结束录音",4=>"2 保存想法 · 点按转为待办 · 左右翻页",
        5=>"上下选择想法 · 左右翻页 · 中按查看待办",6=>"正在处理，请稍候",_=>"1 重新录制 · 中按查看待办"
    }.into();
    merge(values,note);Ok(())
}
#[cfg(test)]mod tests{
    use super::*;
    #[test]fn long_note_pages_are_lossless_and_bounded(){
        let text="想法🙂".repeat(100);let blocks=chunks(&text);assert_eq!(blocks.concat(),text);
        assert!(blocks.iter().all(|b|b.len()<=144));
        let mut collected=String::new();
        for mut i in 0..blocks.len().div_ceil(2){let mut v=json!({"note":text});page(&mut v,&mut i);collected+=v["headline"].as_str().unwrap();collected+=v["note"].as_str().unwrap();}
        assert_eq!(collected,text);
    }
    #[test]fn final_workspace_package_passes_native_import_and_preserves_full_note_reading(){
        let package=PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../builtin-clawpkgs/hachimi-notes");
        let entry=crate::component_library::inspect(&package).unwrap();assert!(entry.valid,"{:?}",entry.validation_errors);
        let runtime=entry.app_runtime.unwrap();let d=app_services::parse(&std::fs::read(package.join("pc/application.json")).unwrap(),&runtime).unwrap();assert!(enabled(&d));
        let dir=tempfile::tempdir().unwrap();let text="把今天的好想法记录下来🙂".repeat(10);
        std::fs::write(dir.path().join("notes.json"),serde_json::to_vec(&json!({"items":[{"id":"example-note","text":text,"created":"2026-11-13T00:00:00Z","todo":null}],"selected":0})).unwrap()).unwrap();
        let mut s=State::default();let usb=crate::usb_serial::UsbSerialManager::new();let mut joined=String::new();
        for i in 0..chunks(&text).len().div_ceil(2){let mut v=json!({});apply("workspace-test",dir.path(),&mut s,if i==0{"status"}else{"next"},&json!({}),&usb,&mut v).unwrap();joined+=v["headline"].as_str().unwrap();joined+=v["note"].as_str().unwrap();}
        assert_eq!(joined,text);let ui=controls("workspace-test",dir.path(),&d,&s).unwrap();assert_eq!(ui["mode"],5);assert!(ui["actions"].as_array().unwrap().contains(&json!("host.todo")));
    }
    #[test]fn old_state_defaults_preserve_unified_todo_and_note_identity(){
        let s:State=serde_json::from_value(json!({"selected":2})).unwrap();assert!(!s.organizer_tasks);assert_eq!(s.organizer_page,0);assert_eq!(s.selected,2);
    }
    #[test]fn unconfigured_workspace_never_starts_capture_on_status(){
        let dir=tempfile::tempdir().unwrap();let usb=crate::usb_serial::UsbSerialManager::new();let mut s=State::default();let mut v=json!({});
        apply("test-organizer",dir.path(),&mut s,"status",&json!({}),&usb,&mut v).unwrap();
        assert_eq!(v["headline"],"先记下来，慢慢完成");assert!(!dir.path().join("notes.json").exists());
        assert_eq!(crate::component_notes::diagnostics("test-organizer"),Value::Null);
    }
}
