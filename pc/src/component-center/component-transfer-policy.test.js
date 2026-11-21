import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {syncAudioPreflight,deviceDeletePolicy,componentTransferError,componentCommitOutcomeUnknown} from './component-transfer-policy.js';

test('sync audio preflight issues only a readonly correlated status request and preserves voice block',async()=>{
 const calls=[];
 const result=await syncAudioPreflight(async(command,input)=>{calls.push([command,input]);return {syncAudio:{blocked:true,reason:'voice'}};},'board-a');
 assert.equal(result.reason,'voice');
 assert.deepEqual(calls,[['usb_get_diagnostics',{expectedBoardDeviceId:'board-a',componentSync:true}]]);
});
test('unknown audio status fails closed before any installation',async()=>{
 await assert.rejects(syncAudioPreflight(async()=>({}),'board-a'),/尚未开始同步/);
 await assert.rejects(syncAudioPreflight(async()=>{throw Error('disconnected');},'board-a'),/disconnected/);
});
test('device saves default to preserve with exact advertised capability',()=>{
 assert.deepEqual(deviceDeletePolicy({transport:'usb',capabilities:{widgetDeletePreserveSave:true}}),{preserveSave:true,clearDeviceSaveConfirmed:false});
 for(const transport of ['usb','ssh'])assert.throws(()=>deviceDeletePolicy({transport,capabilities:{widgetDeletePreserveSave:'true'}}),/未删除/);
 assert.deepEqual(deviceDeletePolicy({clearSave:true,transport:'usb'}),{preserveSave:false,clearDeviceSaveConfirmed:true});
});
test('busy commit messages distinguish exclusive audio, storage, and ambiguous older firmware',()=>{
 const audio = componentTransferError('device audio is busy; catalog not committed; staged package preserved');
 assert.match(audio,/正在播放或使用语音/);
 const storage = componentTransferError(new Error('component storage is busy; commit has not started; staged package preserved'));
 assert.match(storage,/处理其他存储操作.*尚未开始安装/);
 assert.doesNotMatch(storage,/音频|语音/);
 assert.match(componentTransferError('component storage interrupted; catalog not committed; staged package preserved'),/存储操作中断/);
 for (const error of [
   'audio or component storage is busy; staged package preserved',
   'audio started; catalog commit deferred and staged package preserved',
 ]) {
   assert.match(componentTransferError(error),/可能是音频占用或组件存储忙/);
   assert.equal(componentCommitOutcomeUnknown(error),false);
 }
 assert.match(componentTransferError('component storage is busy'),/检查设备清单/);
 assert.match(componentTransferError('component storage is busy or recovery blocked'),/查看是否有存储异常/);
});
test('unknown commits require a fresh device inventory instead of an immediate retry',()=>{
 for (const error of [
   'widget commit acknowledgement timed out',
   '未收到板端组件 OTA 确认: transferId=widget-install-1 phase=commit',
   '组件提交结果尚未确认，请刷新设备组件清单核对。',
   '组件提交回执格式未确认，结果未知；不会自动重发或取消传输。',
   'component catalog outcome unknown; inspect only, do not replay',
   'durable catalog media publication failed; old runtime retained, recovery required',
 ]) {
   assert.equal(componentCommitOutcomeUnknown(error),true);
   assert.match(componentTransferError(error),/先刷新设备组件清单/);
 }
 assert.equal(componentCommitOutcomeUnknown('component storage is busy; commit has not started'),false);
});
test('USB automatically repeats only pre-write deferrals for the same staged package',()=>{
 const usb = readFileSync(new URL('../../src-tauri/src/usb_serial.rs', import.meta.url), 'utf8');
 const install = usb.split('fn install_widget_file_transfer<F>(', 2)[1].split('/// Transfer appearance', 1)[0];
 const commit = install.slice(install.indexOf('let commit ='));
 assert.match(install,/self\.send_inner\(topic, payload, true, Some\(board\), Some\(connection_id\), None\)/);
 assert.match(commit,/"transferId":id,"widgetId":widget,"packageSha256":digest/);
 assert.match(commit,/then_some\(attempt\)[\s\S]*payload\["index"\][\s\S]*send_phase\("widget\/commit", "commit", &payload, None, index/);
 assert.match(commit,/Duration::from_millis\(3500\)/);
 assert.match(commit,/ok == Some\(false\) && error_code == "commit_storage_deferred"[\s\S]*?<= retry_window[\s\S]*?continue;/);
 assert.match(commit,/None => \{\s*return match status\(\) \{\s*Ok\(WidgetTransferStatus::Committed\) => completed\(\),\s*_ => Err/);
 assert.equal(commit.match(/\bcontinue;/g)?.length, 1);
});
