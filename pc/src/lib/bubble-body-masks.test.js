import test from 'node:test';
import assert from 'node:assert/strict';
import { renderBubblePixels, bubblePixelKey, bubbleChecksum, startBubblePixelService, collectBubbleTexts } from './bubble-pixels.js';

const instance = 'a'.repeat(32), boot = '1122334455667788';
const pause = () => new Promise(resolve => setTimeout(resolve, 8));
async function until(condition) { for (let i=0;i<100&&!condition();i++) await pause(); assert(condition()); }
function event(text, version=1, extra={}) {
  return {payload:{rendererId:instance,boardDeviceId:'board',connectionId:'1',bootId:boot,
    owner:'1'.repeat(16),version:version.toString(16).padStart(16,'0'),bodyMasks:true,prefetchBody:true,
    topic:'speech/text',payload:{displayTitle:'Agent',displayContent:text},...extra}};
}
function image(style,text,_canvas,page=0) {
  const bytes=new Uint8Array(1674).fill(15);
  return {id:bubblePixelKey(style,text),style,pageIndex:page,pageCount:style===2?4:1,
    pageSizes:[bytes.length],checksum:bubbleChecksum(bytes),data:btoa(String.fromCharCode(...bytes))};
}
async function fixture(render=image) {
  let receive;const sent=[];
  const stop=await startBubblePixelService({instance,interval:1,render,
    listen:async(_,handler)=>{receive=handler;return()=>{};},
    invoke:async(command,args)=>{if(command==='usb_send_bubble_pixels')sent.push(args.frame);}});
  return {emit:e=>receive(e),sent,stop};
}

test('native body uses 500 24px MiSans and preserves sixteen-level alpha in the original 496x54 geometry',()=>{
  const fonts=[],rgba=new Uint8ClampedArray(496*54*4);
  for(let i=0;i<496*54;i++)rgba[i*4+3]=(i%16)*17;
  const ctx={font:'',clearRect(){},fillText(){fonts.push(this.font);},
    measureText:text=>({width:Array.from(text).length*12,actualBoundingBoxAscent:20,actualBoundingBoxDescent:3}),
    getImageData:()=>({data:rgba})};
  const canvas={getContext:()=>ctx};
  const frame=renderBubblePixels(4,'MiSans 中文 㐀',()=>canvas);
  assert.deepEqual([canvas.width,canvas.height],[496,54]);
  assert(fonts.length&&fonts.every(font=>font==='500 24px "Hachimi MiSans Medium"'));
  const data=Uint8Array.from(atob(frame.data),c=>c.charCodeAt(0));
  assert.equal(new Set([...data].map(byte=>byte>>4)).size,16);
  assert.equal([...data].reduce((sum,b)=>sum+(b&15)+1,0),496*54);
  assert.equal(frame.id,bubblePixelKey(4,'MiSans 中文 㐀'));
});

test('USB and confirmed LAN prefetch send only the first body page, with the same grayscale format',async()=>{
  for(const wifi of [false,true]) {
    const f=await fixture();try {
      if(wifi) {f.emit(event('合成正文',1,{wifi,demandOnly:true}));await pause();assert.equal(f.sent.length,0);}
      f.emit(event('合成正文',1,{wifi,demandOnly:false}));await until(()=>f.sent.length===1);
      assert.equal(f.sent[0].style,4);assert.equal(f.sent[0].bodyMasks,true);
      assert(!f.sent[0].glyphCells);assert.equal(f.sent[0].pageIndex,0);assert.equal(Boolean(f.sent[0].wifi),wifi);
    } finally {f.stop();}
  }
});

test('raster cache never substitutes for a new device ACK and keeps new transport tokens',async()=>{
  let renders=0;const f=await fixture((...args)=>{renders++;return image(...args);});
  const text='缓存合成';const need={style:4,id:bubblePixelKey(4,text),page:0,bodyMasks:true,glyphCells:false};
  try {
    f.emit(event(text));await until(()=>f.sent.length===1);
    f.emit(event(text,1,{need}));await until(()=>f.sent.length===2);
    f.emit(event(text,2,{wifi:true,connectionId:'2',need}));await until(()=>f.sent.length===3);
    assert.equal(renders,1);assert.equal(f.sent[2].connectionId,'2');assert.equal(f.sent[2].wifi,true);
  } finally {f.stop();}
});

test('new prefetch version cancels an unfinished raster rather than dropping the update',async()=>{
  let release;const hold=new Promise(resolve=>{release=resolve;});let first=true;
  const f=await fixture(async(...args)=>{if(first){first=false;await hold;}return image(...args);});
  try {
    f.emit(event('旧合成'));await pause();f.emit(event('新合成',2));release();
    await until(()=>f.sent.length===1);assert.equal(f.sent[0].id,bubblePixelKey(4,'新合成'));
  } finally {release();f.stop();}
});

test('body page cache is bounded and format-separated from the legacy full-page path',async()=>{
  let renders=0;const f=await fixture((...args)=>{renders++;return image(...args);});
  try {
    for(let n=1;n<=13;n++){f.emit(event(`合成${n}`,n));await until(()=>f.sent.length===n);}
    f.emit(event('合成1',14));await until(()=>f.sent.length===14);assert.equal(renders,14);
    const text='合成1';f.emit(event(text,15,{bodyMasks:false,prefetchBody:false,demandOnly:true}));
    f.emit(event(text,15,{bodyMasks:false,prefetchBody:false,need:{style:4,id:bubblePixelKey(4,text),page:0}}));
    await until(()=>f.sent.length===15);assert.equal(renders,15);assert(!f.sent[14].bodyMasks);
  } finally {f.stop();}
});

test('prefetch body hash uses the same 383-byte clipping as the device and later caption pages remain demand-driven',async()=>{
  const text='中'.repeat(160);
  const body=collectBubbleTexts('speech/text',{text}).find(r=>r.style===4);
  assert.equal(new TextEncoder().encode(body.text).length,381);
  const f=await fixture();try {
    f.emit(event('字幕',1,{topic:'ui/conversation',payload:{name:'宠物',text:'字幕',role:'assistant'}}));
    await until(()=>f.sent.length===1);assert.equal(f.sent[0].style,2);assert.equal(f.sent[0].pageIndex,0);
    f.emit(event('字幕',1,{topic:'device/need',payload:{text:'字幕'},need:{style:2,id:bubblePixelKey(2,'字幕'),page:2,bodyMasks:true}}));
    await until(()=>f.sent.length===2);assert.equal(f.sent[1].pageIndex,2);
  } finally {f.stop();}
});

test('a 32px title uses only bundled Medium and retains all 16 alpha levels', () => {
  const fonts = [], rgba = new Uint8ClampedArray(474 * 38 * 4);
  for (let i = 0; i < rgba.length / 4; i++) rgba[i * 4 + 3] = (i % 16) * 17;
  const context = { clearRect() {}, fillText() { fonts.push(this.font); },
    measureText: text => ({ width: Array.from(text).length * 18 }), getImageData: () => ({ data: rgba }) };
  const canvas = { getContext: () => context };
  const title = '任务 **完成** Agent';
  const frame = renderBubblePixels(1, title, () => canvas);
  assert.deepEqual([canvas.width, canvas.height, frame.pageCount], [474, 38, 1]);
  assert.deepEqual([...new Set(fonts)], ['500 32px "Hachimi MiSans Medium"']);
  const bytes = Uint8Array.from(atob(frame.data), char => char.charCodeAt(0));
  assert.equal(new Set(bytes.map(byte => byte >> 4)).size, 16);
  assert.equal([...bytes].reduce((sum, byte) => sum + (byte & 15) + 1, 0), 474 * 38);
  assert.equal(frame.id, bubblePixelKey(1, title));
});

test('USB and confirmed Wi-Fi send a changed title once and can restore it after eviction', async () => {
  for (const wifi of [false, true]) {
    const f = await fixture();
    try {
      if (wifi) {
        f.emit(event('A', 1, { wifi, prefetchTitle: true, demandOnly: true }));
        await pause();
        assert.equal(f.sent.length, 0);
      }
      f.emit(event('A', 1, { wifi, prefetchTitle: true }));
      await until(() => f.sent.length === 2);
      f.emit(event('B', 2, { wifi, prefetchTitle: false }));
      await until(() => f.sent.length === 3);
      f.emit(event('B', 2, { wifi, topic: 'device/need', payload: { text: 'Agent' },
        need: { id: bubblePixelKey(1, 'Agent'), style: 1, page: 0 } }));
      await until(() => f.sent.length === 4);
      assert.deepEqual(f.sent.map(frame => frame.style), [1, 4, 4, 1]);
      for (const title of [f.sent[0], f.sent[3]]) {
        assert.equal(Boolean(title.wifi), wifi);
        assert.ok(!title.glyphCells && !title.bodyMasks);
      }
    } finally { f.stop(); }
  }
});
