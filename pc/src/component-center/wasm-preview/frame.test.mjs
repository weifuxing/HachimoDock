import { createHash } from "node:crypto";
import assert from "node:assert/strict";
import { test } from "node:test";
import { loadPreviewFonts } from "../../../scripts/wasm-preview-font.mjs";
import { createWasmFrameRenderer } from "./frame.mjs";

test("native preview composites firmware pixels and glyphs on the same grid", async () => {
  const fonts = await loadPreviewFonts();
  assert.deepEqual(fonts.map(font => font.glyphs.length), [21208, 200]);
  const draw = createWasmFrameRenderer(fonts);
  let rendered;
  const canvas = { getContext: () => ({
    createImageData: (width, height) => ({ width, height, data: new Uint8ClampedArray(width * height * 4) }),
    putImageData: image => { rendered = image.data; },
  }) };
  const frame = { width: 640, height: 480, pixels: [], texts: [], frame: true };
  draw(canvas, { ...frame, frame: false, init: true });
  assert.equal(rendered, undefined);
  draw(canvas, { ...frame, pixels: [
    { kind: "rect", x: 0, y: 0, w: 640, h: 480, rgb: 0x123456 },
    { kind: "blit", x: 0, y: 0, w: 2, h: 1, data: Uint16Array.of(0x0, 0xffe0), mask: Uint8Array.of(0, 1) },
  ], texts: [
    { value: "AV 设备 2048", x: 10, y: 12, px: 27, rgb: 0xf6dcfd },
    { value: "文字层", x: 602, y: 449, px: 18, rgb: 0xd9ff69 },
  ] });
  assert.deepEqual([...rendered.slice(0, 8)], [16, 52, 82, 255, 255, 255, 0, 255]);
  assert.equal(createHash("sha256").update(rendered).digest("hex"), "9c1fe6e669f5b2fd5f4de20eaca03844572c465e32fd138feeec7700402e7036");
});

test("native interpreter RLE reproduces exact pixels and rejects truncated frames",async()=>{
 const draw=createWasmFrameRenderer(await loadPreviewFonts());let rendered;
 const canvas={getContext:()=>({createImageData:(width,height)=>({width,height,data:new Uint8ClampedArray(width*height*4)}),putImageData:image=>{rendered=image.data;}})};
 const frame={width:640,height:480,pixels:[],texts:[],frame:true};
 draw(canvas,{...frame,runs:[1,0xf800,1,0x07e0,307198,0x001f]});
 assert.deepEqual([...rendered.slice(0,12)],[255,0,0,255,0,255,0,255,0,0,255,255]);
 for(const runs of [[307199,0],[307201,0],[0,0],[307200,65536],[1]])assert.throws(()=>draw(canvas,{...frame,runs}));
});
