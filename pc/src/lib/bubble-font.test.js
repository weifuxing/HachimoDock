import test from 'node:test';
import assert from 'node:assert/strict';
import { createBubbleFontLoader, createBubbleCanvas, BUBBLE_FONT_FAMILY } from './bubble-font.js';
import { startBubblePixelService, bubblePixelKey } from './bubble-pixels.js';
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
test('official WOFF2 is unchanged and the family has no system fallback',()=>{
 const bytes=readFileSync(new URL('../assets/fonts/MiSans-Medium.woff2',import.meta.url));
 assert.equal(bytes.length,4942368);
 assert.equal(createHash('sha256').update(bytes).digest('hex'),'44e28ca6c2f0ca79829f192831ef87b5eec7c464f5cfb7a83467f57bb6e58114');
 assert.equal(BUBBLE_FONT_FAMILY,'"Hachimi MiSans Medium"');
});
test('concurrent font preparation shares one load and waits for registration',async()=>{
 let finish,loads=0,adds=0;
 class Font {constructor(family,url,options){assert.equal(family,'Hachimi MiSans Medium');assert.match(url,/MiSans-Medium/);assert.equal(options.weight,'500');}load(){loads++;return new Promise(resolve=>{finish=()=>resolve({status:'loaded'});});}}
 const loader=createBubbleFontLoader({FontFaceClass:Font,fontSet:{add(){adds++;},check(){return adds===1;}}});
 const a=loader.load(),b=loader.load();assert.equal(a,b);assert.equal(loader.ready(),false);assert.equal(loads,1);assert.equal(adds,0);
 finish();await a;assert(loader.ready());assert.equal(adds,1);
});
test('font failure cannot enable silent fallback',async()=>{
 for(const mode of ['download','registration']) {
  let deleted=0;
  class Font {load(){return mode==='download'?Promise.reject(Error('broken font')):Promise.resolve({status:'loaded'});}}
  const loader=createBubbleFontLoader({FontFaceClass:Font,fontSet:{add(){},check(){return false;},delete(){deleted++;}}});
  await assert.rejects(loader.load());assert.equal(loader.ready(),false);assert.equal(deleted,mode==='registration'?1:0);
 }
 assert.throws(()=>createBubbleCanvas(),/MiSans must load/);
});
test('missing font API prevents listener and native ready',async()=>{
 let calls=0;
 await assert.rejects(startBubblePixelService({instance:'0'.repeat(32),listen:async()=>{calls++;return()=>{};},invoke:async()=>{calls++;}}),/MiSans/);
 assert.equal(calls,0);
});
test('wire hash is unchanged',()=>{
 let h=14695981039346656037n;
 for(const b of [...new TextEncoder().encode('p4-text-alpha-v2:rle4:1:'),2,...new TextEncoder().encode('中文 MiSans')])h=BigInt.asUintN(64,(h^BigInt(b))*1099511628211n);
 assert.equal(bubblePixelKey(2,'中文 MiSans'),h.toString(16).padStart(16,'0'));
});

test('production service advertises ready only after FontFace load resolves',async()=>{
 const {spawnSync}=await import('node:child_process');
 const moduleUrl=new URL('./bubble-pixels.js',import.meta.url).href;
 const result=spawnSync(process.execPath,['--input-type=module','-e',`
  import assert from 'node:assert/strict';
  let release,registered=false,calls=0;
  globalThis.FontFace=class {load(){return new Promise(r=>{release=()=>r({status:'loaded'});});}};
  globalThis.document={fonts:{add(){registered=true;},check(){return registered;}}};
  const {startBubblePixelService}=await import(${JSON.stringify(moduleUrl)});
  const pending=startBubblePixelService({instance:'a'.repeat(32),listen:async()=>{assert(registered);return()=>{};},invoke:async(_,p)=>{if(p.ready){assert(registered);calls++;}}});
  await Promise.resolve();assert.equal(calls,0);release();const stop=await pending;assert.equal(calls,1);stop();
 `],{encoding:'utf8',timeout:10000});
 assert.equal(result.status,0,result.stderr);
});
