// RGB565 and glyph placement match hachi_runtime.c and pet_p4_wasm_text.h.
export function rgb565(rgb) {
  return ((rgb >>> 8) & 0xf800) | ((rgb >>> 5) & 0x7e0) | ((rgb >>> 3) & 31);
}

export function createWasmFrameRenderer(fonts) {
  const surfaces = new WeakMap();
  const [font16, font32] = fonts.map(font => {
    const lookup = new Uint16Array(65536);
    for (const [start, length, first, sparse] of font.mappings) {
      if (sparse) sparse.forEach((offset, i) => { lookup[start + offset] = first + i; });
      else for (let i = 0; i < length; i++) lookup[start + i] = first + i;
    }
    return { ...font, lookup, bitmap: Uint8Array.from(atob(font.bitmap), c => c.charCodeAt(0)) };
  });

  function blend(background, foreground, alpha) {
    const inverse = 15 - alpha;
    const r = ((((foreground >>> 11) * alpha + (background >>> 11) * inverse + 7) / 15) | 0) << 11;
    const g = (((((foreground >>> 5) & 63) * alpha + ((background >>> 5) & 63) * inverse + 7) / 15) | 0) << 5;
    const b = ((((foreground & 31) * alpha + (background & 31) * inverse + 7) / 15) | 0);
    return r | g | b;
  }

  function drawText(buffer, width, height, text) {
    const characters = Array.from(text.value, character => character.codePointAt(0));
    const target = rgb565(text.rgb);
    const f = Math.fround;
    const advanceScale = text.px / 16;
    let pen = text.x;
    for (let i = 0; i < characters.length && pen < width; i++) {
      const cp = characters[i];
      if (cp === 0xfe0f || cp === 0x200d) continue;
      const id = font16.lookup[cp];
      const next = font16.lookup[characters[i + 1]];
      const { kerning } = font16;
      const l = kerning.left[id], r = kerning.right[next];
      const kern = l && r ? kerning.values[(l - 1) * kerning.columns + r - 1] * kerning.scale >> 4 : 0;
      const base = id ? font16.glyphs[id] : null;
      const advance = base ? ((base[1] + kern + 8) >> 4) || base[2] : cp >= 0x2300 ? 16 : 10;
      if (!advance) continue;
      const nativeId = text.px >= 24 ? font32.lookup[cp] : 0;
      const font = nativeId ? font32 : font16;
      const glyph = font.glyphs[nativeId || id || font16.lookup[63]];
      const scale = text.px / (nativeId ? 32 : 16);
      const left = f(pen + glyph[4] * scale);
      const [at, , w, h, , dy] = glyph;
      const top = f(text.y + (font.baseline - h - dy) * scale);
      pen = f(pen + advance * advanceScale);
      if (!w || !h) continue;
      const inverse = f(1 / scale);
      const sample = (x, y) => {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0;
        const bit = (y * w + x) * font.bpp;
        const mask = (1 << font.bpp) - 1;
        return ((font.bitmap[at + (bit >>> 3)] >>> (8 - font.bpp - (bit & 7))) & mask) * (255 / mask);
      };
      const samplePosition = (pixel, edge) => f(f(f(pixel + 0.5 - edge) * inverse) - 0.5);
      const mix = (a, b, weight) => f(f(a * f(1 - weight)) + f(b * weight));
      const x0 = Math.max(0, Math.floor(left) - 1), x1 = Math.min(width, Math.ceil(left + w * scale) + 1);
      const y0 = Math.max(0, Math.floor(top) - 1), y1 = Math.min(height, Math.ceil(top + h * scale) + 1);
      for (let y = y0; y < y1; y++) {
        const sy = samplePosition(y, top), iy = Math.floor(sy), fy = f(sy - iy);
        for (let x = x0; x < x1; x++) {
          const sx = samplePosition(x, left), ix = Math.floor(sx), fx = f(sx - ix);
          const a = mix(sample(ix, iy), sample(ix + 1, iy), fx);
          const b = mix(sample(ix, iy + 1), sample(ix + 1, iy + 1), fx);
          const alpha = Math.trunc(f(f(mix(a, b, fy) / 17) + 0.5));
          if (alpha) {
            const pixel = y * width + x;
            buffer[pixel] = blend(buffer[pixel], target, alpha);
          }
        }
      }
    }
  }

  return (canvas, { width, height, pixels, texts, frame, init, runs }) => {
    if (canvas.width !== width) canvas.width = width;
    if (canvas.height !== height) canvas.height = height;
    const ctx = canvas.getContext("2d", { alpha: false });
    let surface = surfaces.get(canvas);
    if (init || surface?.image.width !== width || surface.image.height !== height) {
      surface = { image: ctx.createImageData(width, height), buffer: new Uint16Array(width * height),
        alternate: width === 320 ? new Uint16Array(width * height) : null };
      surfaces.set(canvas, surface);
    }
    const { buffer, image } = surface;
    if (frame && width === 640) buffer.fill(0);
    if (runs) {
      let at = 0;
      if (runs.length % 2) throw new Error("无效原生画面");
      for (let i=0; i<runs.length; i+=2) {
        const count=runs[i], color=runs[i+1];
        if (!Number.isInteger(count) || count<1 || at+count>buffer.length || !Number.isInteger(color) || color<0 || color>65535) throw new Error("无效原生画面");
        buffer.fill(color, at, at+count); at+=count;
      }
      if (frame && at !== buffer.length) throw new Error("原生画面不完整");
    }
    for (const op of pixels) {
      for (let row = 0; row < op.h; row++) {
        const start = (op.y + row) * width + op.x;
        if (op.kind === "rect") buffer.fill(rgb565(op.rgb), start, start + op.w);
        else if (!op.mask) buffer.set(op.data.subarray(row * op.w, (row + 1) * op.w), start);
        else for (let col = 0; col < op.w; col++) {
          const index = row * op.w + col;
          if (op.mask[index]) buffer[start + col] = op.data[index];
        }
      }
    }
    if (!frame) return;
    for (const text of texts) drawText(buffer, width, height, text);
    for (let i = 0; i < buffer.length; i++) {
      const word = buffer[i], r = word >>> 11, g = (word >>> 5) & 63, b = word & 31;
      const at = i * 4;
      image.data[at] = (r << 3) | (r >>> 2);
      image.data[at + 1] = (g << 2) | (g >>> 4);
      image.data[at + 2] = (b << 3) | (b >>> 2);
      image.data[at + 3] = 255;
    }
    ctx.putImageData(image, 0, 0);
    if (surface.alternate) {
      surface.buffer = surface.alternate;
      surface.alternate = buffer;
    }
  };
}
