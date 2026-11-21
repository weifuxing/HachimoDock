import test from 'node:test';
import assert from 'node:assert/strict';
import { createRealtimePressHandler } from './realtime-press.js';
const token = { boardDeviceId: 'p4-0123456789ab', bootId: '0123456789abcdef', seq: 1 };
const event = { payload: token };
test('no frontend acceptance until a listener handles the offer; duplicate offers do not toggle twice', async () => {
  let calls = 0, configure = 0;
  const handler = createRealtimePressHandler({accept: async () => { calls++; return {}; }, configure: async () => configure++});
  assert.equal(calls, 0);
  await handler.handle(event); await handler.handle(event);
  assert.equal(calls, 1); assert.equal(configure, 0);
});
test('idle press resolves persona only after native requests configuration; stop needs no persona', async () => {
  const order = [];
  const handler = createRealtimePressHandler({accept: async () => { order.push('offer'); return null; }, configure: async (t, live) => {assert.equal(t, token); assert.equal(live(),true); order.push('configure');}});
  await handler.handle(event);assert.deepEqual(order,['offer','configure']);
});
test('pending offer is serialized; failed acceptance can retry original token', async () => {
  let release, calls=0, errors=0;
  const handler=createRealtimePressHandler({accept: async()=>{calls++; if(calls===1) await new Promise(resolve=>release=resolve); if(calls===1) throw Error('synthetic'); return {};}, configure:async()=>{},onError:()=>errors++});
  const first=handler.handle(event);await handler.handle(event);assert.equal(calls,1);release();await first;
  await handler.handle(event);assert.equal(calls,2);assert.equal(errors,1);
});
test('dispose between acceptance and configuration never starts a conversation', async()=>{
  let release, starts=0;
  const handler=createRealtimePressHandler({accept:()=>new Promise(resolve=>release=resolve),configure:async()=>starts++});
  const pending=handler.handle(event);handler.dispose();release(null);await pending;await handler.handle(event);assert.equal(starts,0);
});
test('new boot may reuse sequence; malformed events never reach native',async()=>{
  let calls=0;const handler=createRealtimePressHandler({accept:async()=>{calls++;return {};},configure:async()=>{}});
  await handler.handle(event);await handler.handle({payload:{...token,bootId:'1123456789abcdef'}});
  for(const seq of [0,-1,1.2,0x100000000])await handler.handle({payload:{...token,seq}});
  await handler.handle({payload:{...token,boardDeviceId:'other'}});assert.equal(calls,2);
});
