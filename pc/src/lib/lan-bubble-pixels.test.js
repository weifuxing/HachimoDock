import test from 'node:test';
import assert from 'node:assert/strict';
import { startBubblePixelService, bubblePixelKey } from './bubble-pixels.js';

const instance = 'a'.repeat(32);
function event(text, version = 1, need = false, wifi = true) {
  return { payload: { rendererId: instance, boardDeviceId: 'board', connectionId: '1',
    bootId: '1122334455667788', owner: '1'.repeat(16), version: version.toString(16).padStart(16, '0'),
    wifi, demandOnly: !need, topic: 'device/need', payload: { text },
    ...(need ? { need: { id: bubblePixelKey(4, text), style: 4, page: 0 } } : {}) } };
}
const wait = () => new Promise(resolve => setTimeout(resolve, 15));
async function setup(render = (style, text) => ({ style, text, id: bubblePixelKey(style, text) })) {
  let receive;
  const sent = [];
  const stop = await startBubblePixelService({ instance, interval: 1, render,
    listen: async (_, handler) => { receive = handler; return () => {}; },
    invoke: async (command, args) => { if (command === 'usb_send_bubble_pixels') sent.push(args.frame); } });
  return { sent, stop, emit: event => receive(event) };
}

test('Wi-Fi metadata alone never renders; authenticated miss preserves native transport', async () => {
  const f = await setup();
  try {
    f.emit(event('中文 English')); await wait(); assert.equal(f.sent.length, 0);
    f.emit(event('中文 English', 1, true)); await wait();
    assert.equal(f.sent.length, 1); assert.equal(f.sent[0].wifi, true);
    assert.equal(f.sent[0].text, '中文 English');
    f.emit(event('新版', 2)); f.emit(event('中文 English', 1, true)); await wait();
    assert.equal(f.sent.length, 1, 'late old-version need must be dropped');
    f.emit(event('新版', 2, true)); await wait();
    assert.equal(f.sent.length, 2); assert.equal(f.sent[1].text, '新版');
  } finally { f.stop(); }
});

test('new Wi-Fi metadata invalidates an asynchronous old render before native submission', async () => {
  let release;
  const held = new Promise(resolve => { release = resolve; });
  const f = await setup(async (style, text) => { await held; return { style, text, id: bubblePixelKey(style, text) }; });
  try {
    f.emit(event('旧字')); f.emit(event('旧字', 1, true)); await wait();
    f.emit(event('新字', 2)); release(); await wait(); assert.equal(f.sent.length, 0);
    f.emit(event('新字', 2, true)); await wait(); assert.equal(f.sent[0].text, '新字');
  } finally { release(); f.stop(); }
});

test('USB takeover invalidates Wi-Fi raster even when numerical peer tokens coincide', async () => {
  let release;
  const held = new Promise(resolve => { release = resolve; });
  const f = await setup(async (style, text) => { await held; return { style, text, id: bubblePixelKey(style, text) }; });
  try {
    f.emit(event('WiFi')); f.emit(event('WiFi', 1, true)); await wait();
    f.emit(event('USB', 1, true, false)); release(); await wait();
    assert(f.sent.length > 0); assert(f.sent.every(frame => frame.text === 'USB' && !frame.wifi));
  } finally { release(); f.stop(); }
});
