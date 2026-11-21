import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {DEVICE_BUTTON_CONFIG_MODEL_VERSION, DEVICE_BUTTON_CONFIG_STORAGE_KEY, migrateP4V7ShippedBoardDefaults} from '../component-center/button-config.js';
import {decodeBluetoothShortcut} from '../lib/bluetooth-shortcut.js';
const dashboard=readFileSync(new URL('../DeviceDashboard.jsx',import.meta.url),'utf8');
const panel=readFileSync(new URL('./BoardButtonPanel.jsx',import.meta.url),'utf8');
// Execute production schema/normalization, without mounting React or storage.
const code=dashboard.slice(dashboard.indexOf('export const VOICE_CONFIG_STORAGE_KEY'),dashboard.indexOf('export function mergeBoardButtonConfig')).replaceAll('export ','');
function schema(platform){return new Function('DEVICE_BUTTON_CONFIG_MODEL_VERSION','DEVICE_BUTTON_CONFIG_STORAGE_KEY','decodeBluetoothShortcut','navigator',code+'\nreturn {normalizeVoiceConfig,buildBoardButtonConfigBindings,P4_BUTTON_CONTROL_ROWS,DEFAULT_BUTTON_ACTIONS,DEFAULT_VOICE_CONFIG,VOICE_KEY_PRESETS};')(DEVICE_BUTTON_CONFIG_MODEL_VERSION,DEVICE_BUTTON_CONFIG_STORAGE_KEY,decodeBluetoothShortcut,{platform});}
for(const platform of ['Win32','MacIntel']){
 test(`${platform}: absent SW3 uses system settings in UI and outbound binding`,()=>{
  const s=schema(platform);
  for(const input of [{},{buttonModelVersion:9,buttonActions:{p4_sw1_short:'page_back'}}]){
   const c=s.normalizeVoiceConfig(input);assert.equal(c.buttonActions.p4_sw3_long,'system_settings');
   const bindings=s.buildBoardButtonConfigBindings(c.buttonActions,c.buttonValues,'esp-p4',c.enabled);
   assert.equal(bindings.find(b=>b.event==='button.sw3.long_press').action,'system_settings');
   assert.equal(bindings.find(b=>b.event==='button.sw3.hold').action,'disabled');
  }
 });
 test(`${platform}: ambiguous v9 disabled and saved custom actions are preserved`,()=>{
  const s=schema(platform);
  for(const action of ['disabled','agent_prompt','voice_ptt','realtime_chat']){
   const c=s.normalizeVoiceConfig({buttonModelVersion:9,buttonActions:{p4_sw3_long:action},buttonValues:{p4_sw3_long:'custom prompt'}});
   assert.equal(c.buttonActions.p4_sw3_long,action);assert.equal(c.buttonValues.p4_sw3_long,'custom prompt');
  }
  const old=s.normalizeVoiceConfig({buttonModelVersion:8,buttonActions:{p4_sw3_long:'agent_prompt'},buttonValues:{p4_sw3_long:'keep'}});
  assert.equal(old.buttonActions.p4_sw3_long,'agent_prompt');assert.equal(old.buttonValues.p4_sw3_long,'keep');
 });
}
for (const previousAction of ['disabled','agent_prompt','voice_ptt','realtime_chat','voice_key_forward']) test(`explicit restore from ${previousAction} updates only SW3 through the edit callback`,()=>{
 const s=schema('Win32'),current=s.normalizeVoiceConfig({buttonModelVersion:9,buttonActions:{p4_sw3_long:previousAction,p4_sw2_long:'agent_prompt'},buttonValues:{p4_sw2_long:'keep prompt'}});
 const marker='  const onButtonActionChange = (row, actionId) => {';
 const begin=panel.indexOf(marker),end=panel.indexOf('\n  };',begin);
 assert(begin>=0&&end>begin);
 const body=panel.slice(begin+marker.length,end);let changes=[];
 const context={buttonActions:current.buttonActions,buttonValues:current.buttonValues,buttonLabels:current.buttonLabels,
  DEFAULT_BUTTON_ACTIONS:s.DEFAULT_BUTTON_ACTIONS,DEFAULT_VOICE_CONFIG:s.DEFAULT_VOICE_CONFIG,VOICE_KEY_PRESETS:s.VOICE_KEY_PRESETS,
  decodeBluetoothShortcut,triggerId:'sw1.hold',controlRows:s.P4_BUTTON_CONTROL_ROWS,onVoiceConfigChange:p=>changes.push(p)};
 const edit=new Function(...Object.keys(context),'return (row,actionId)=>{'+body+'};')(...Object.values(context));
 assert.equal(changes.length,0);const row=s.P4_BUTTON_CONTROL_ROWS.find(r=>r.id==='p4_sw3_long');edit(row,row.defaultAction);
 assert.equal(changes.length,1);const expected={...current.buttonActions,p4_sw3_long:'system_settings'};
 assert.deepEqual(changes[0].buttonActions,expected);assert.deepEqual(changes[0].buttonValues,current.buttonValues);
 assert.equal(current.buttonActions.p4_sw3_long,previousAction);
 assert.doesNotMatch(body,/invoke\(|dispatchBoardButtonConfig|reset_input|usb_/);
 const condition=panel.match(/\{(row.id === "p4_sw3_long"[^\n]+) && \(/)[1];
 const visible=new Function('row','currentActionId',`return ${condition};`);
 assert.equal(visible(row,previousAction),true);assert.equal(visible(row,row.defaultAction),false);
 assert.equal(visible({...row,id:'p4_sw2_long'},previousAction),false);
 assert.match(panel,/默认动作：打开系统设置/);
 assert.match(panel,/disabled=\{!systemSettingsSupported \|\| syncPending\}/);
 assert.match(panel,/onClick=\{\(\) => onButtonActionChange\(row, row.defaultAction\)\}/);
});


test('version gate migrates old v8 SW3 default but does not reinterpret identical v9 disabled',()=>{
 const s=schema('Win32');
 const actions={...s.DEFAULT_BUTTON_ACTIONS,p4_sw2_long:'disabled',p4_sw3_long:'disabled',p4_joystick_up:'disabled',p4_joystick_down:'disabled'};
 assert.equal(s.normalizeVoiceConfig({buttonModelVersion:8,buttonActions:actions}).buttonActions.p4_sw3_long,'system_settings');
 assert.equal(s.normalizeVoiceConfig({buttonModelVersion:9,buttonActions:actions}).buttonActions.p4_sw3_long,'disabled');
 assert.equal(actions.p4_sw3_long,'disabled');
});

test('whole-board v7 migration already requires SW3 long settings and changes only short navigation',()=>{
 const src=readFileSync(new URL('../component-center/button-config.js',import.meta.url),'utf8');
 const start=src.indexOf('const P4_V7_SHIPPED_DEFAULT_BINDINGS =');
 const end=src.indexOf('export function migrateP4V7ShippedBoardDefaults',start);
 const defaults=new Function(src.slice(start,end)+'return P4_V7_SHIPPED_DEFAULT_BINDINGS;')();
 assert.equal(defaults['button.sw3.long_press'],'system_settings');
 const response={version:7,bindings:Object.entries(defaults).map(([event,action])=>({event,action,value:''}))};
 const result=migrateP4V7ShippedBoardDefaults(response,'esp-p4');assert(result.migrated);
 const changed=result.response.bindings.filter((b,i)=>b.action!==response.bindings[i].action).map(b=>b.event);
 assert.deepEqual(changed,['button.sw1.short_press','button.sw3.short_press']);
 const disabled=structuredClone(response);disabled.bindings.find(b=>b.event==='button.sw3.long_press').action='disabled';
 const kept=migrateP4V7ShippedBoardDefaults(disabled,'esp-p4');assert.equal(kept.migrated,false);assert.equal(kept.response,disabled);
});
