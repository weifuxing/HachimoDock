export const WIDTH = 320;
export const HEIGHT = 240;
export const NATIVE_CANVAS = "640x480-v1";
export const HOST_PROFILE = "hachi-host-v1";
export const AUDIO_PROFILE = "hachi-audio-v2";
export const MAX_WASM_BYTES = 192 * 1024;
const hostImports = ["clock_read", "uv_read", "random_u32"];
const allowedImports = new Set(["canvas640", "text", "rect", "blit", "sprite", "tone", "sample", "load", "save", "media_cmd", ...hostImports]);
const utf8 = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });

export function checkProfile(bytes) {
  if (bytes.length < 8 || bytes.length > MAX_WASM_BYTES ||
      ![0, 97, 115, 109, 1, 0, 0, 0].every((value, i) => bytes[i] === value)) {
    throw new Error("WASM 文件头或大小无效");
  }
  let offset = 8;
  function u32(limit) {
    let value = 0;
    for (let shift = 0; shift <= 28 && offset < limit; shift += 7) {
      const byte = bytes[offset++];
      if (shift === 28 && byte > 15) break;
      value += (byte & 127) * 2 ** shift;
      if (!(byte & 128)) return value;
    }
    throw new Error("WASM 区段编码无效");
  }
  let memory = false;
  while (offset < bytes.length) {
    const id = bytes[offset++];
    const size = u32(bytes.length);
    const end = offset + size;
    if (end > bytes.length || id === 8) throw new Error("WASM 区段无效或包含 start");
    if (id === 5) {
      if (memory || u32(end) !== 1 || u32(end) !== 1) throw new Error("模块必须声明固定内存");
      const pages = u32(end);
      if ((pages !== 2 && pages !== 4) || u32(end) !== pages || offset !== end) {
        throw new Error("模块必须声明两页或四页固定内存");
      }
      memory = true;
    }
    offset = end;
  }
  if (!memory) throw new Error("缺少组件内存");
}

export async function createRuntime(bytes, initialSave = {}, host = {}) {
  checkProfile(bytes);
  const module = await WebAssembly.compile(bytes);
  const imports = WebAssembly.Module.imports(module);
  if (imports.length > allowedImports.size) throw new Error("宿主接口导入数量超限");
  const names = new Set();
  for (const item of imports) {
    if (item.module !== "hachi" || item.kind !== "function" || !allowedImports.has(item.name) || names.has(item.name)) {
      throw new Error("组件导入了未开放或重复的宿主接口");
    }
    names.add(item.name);
  }
  const native = names.has("canvas640");
  const hostProfile = hostImports.some(name => names.has(name)) ? HOST_PROFILE : undefined;
  const audioProfile = names.has("sample") ? AUDIO_PROFILE : undefined;
  if (!native && names.has("text")) throw new Error("文字层要求原生画布");
  const screenWidth = native ? 640 : WIDTH;
  const screenHeight = native ? 480 : HEIGHT;
  const required = { hachi_init: 0, hachi_event: 3, hachi_frame: 1 };
  const expectedExports = new Map(Object.keys(required).map(name => [name, "function"]));
  expectedExports.set("memory", "memory");
  const exported = WebAssembly.Module.exports(module);
  if (exported.length !== expectedExports.size ||
      exported.some(item => expectedExports.get(item.name) !== item.kind)) {
    throw new Error("组件导出的入口无效");
  }
  let pixels, texts, tones, saves, operations, area, textWork, frame;
  let sampleEvent, cue, music;
  const slots = {};
  for (const [key, value] of Object.entries(initialSave)) {
    if (/^[0-7]$/.test(key) && (value | 0) === value) slots[key] = value;
  }
  function region(x, y, w, h) {
    if (++operations > 128) throw new Error("本次绘图指令过多");
    if (w < 1 || h < 1 || w > screenWidth || h > screenHeight) return;
    if (x < -screenWidth || x >= screenWidth || y < -screenHeight || y >= screenHeight) return;
    const left = Math.max(0, x), top = Math.max(0, y);
    const right = Math.min(screenWidth, x + w), bottom = Math.min(screenHeight, y + h);
    if (right <= left || bottom <= top) return;
    area += (right - left) * (bottom - top);
    if (area > screenWidth * screenHeight * 4) throw new Error("本次绘图面积超限");
    return { left, top, right, bottom };
  }
  function snapshot(address, sample) {
    const offset = address >>> 0;
    const buffer = instance.exports.memory.buffer;
    if (!frame || offset + 16 > buffer.byteLength) throw new Error("宿主快照调用无效");
    const output = new DataView(buffer, offset, 16);
    output.setBigUint64(0, BigInt(sample?.time ?? 0), true);
    output.setInt32(8, sample?.value ?? 0, true);
    output.setUint32(12, sample?.age ?? 0, true);
    return sample?.status ?? 0;
  }
  const instance = await WebAssembly.instantiate(module, { hachi: {
    canvas640() {},
    text(x, y, px, rgb, address, length) {
      const memory = instance.exports.memory.buffer;
      const offset = address >>> 0;
      if (!frame || !native || x < 0 || x >= screenWidth || y < 0 || y >= screenHeight ||
          px < 12 || px > 48 || length < 1 || length > 127 || offset + length > memory.byteLength)
        throw new Error("文字参数无效");
      const value = utf8.decode(new Uint8Array(memory, offset, length));
      if (/[\u0000-\u001f\u007f]/.test(value)) throw new Error("文字必须是单行 UTF-8");
      textWork += Array.from(value).length * px * px;
      if (texts.length >= 40 || textWork > 240000) throw new Error("文字预算超限");
      texts.push({ x, y, px, rgb: rgb & 0xffffff, value });
    },
    clock_read(kind, address) {
      const time = host.now ? host.now() : Date.now();
      if (kind === 0) return snapshot(address, time === null ? null : {
        time, value: -new Date(time).getTimezoneOffset(), status: 1,
      });
      if (kind === 1) return snapshot(address, {
        time: host.monotonic ? host.monotonic() : Math.trunc(performance.now()), status: 1,
      });
      return snapshot(address, null);
    },
    uv_read(address) { return snapshot(address, host.uv?.()); },
    random_u32() { return host.random ? host.random() : crypto.getRandomValues(new Uint32Array(1))[0]; },
    media_cmd() {},
    sample(index) {
      const background = index >= 64;
      if (!sampleEvent || (background ? music : cue) !== undefined || index < -1 || index > 128) {
        throw new Error("WASM 语音调用无效");
      }
      if (background) music = index === 128 ? -1 : index - 64;
      else cue = index;
    },
    rect(x, y, w, h, rgb) {
      const box = region(x, y, w, h);
      if (box) pixels.push({ kind: "rect", x: box.left, y: box.top,
        w: box.right - box.left, h: box.bottom - box.top, rgb: rgb & 0xffffff });
    },
    blit(x, y, w, h, address) {
      const box = region(x, y, w, h);
      if (!box) return;
      const memory = instance.exports.memory.buffer;
      const offset = address >>> 0;
      if (offset + w * h * 2 > memory.byteLength) throw new Error("像素地址越界");
      const source = new DataView(memory, offset, w * h * 2);
      const width = box.right - box.left, height = box.bottom - box.top;
      const data = new Uint16Array(width * height);
      for (let row = 0; row < height; ++row) {
        for (let column = 0; column < width; ++column) {
          const index = ((box.top - y + row) * w + box.left - x + column) * 2;
          data[row * width + column] = source.getUint16(index, true);
        }
      }
      pixels.push({ kind: "blit", x: box.left, y: box.top, w: width, h: height, data });
    },
    sprite(x, y, w, h, address, length) {
      const box = region(x, y, w, h);
      if (!box) return;
      const memory = instance.exports.memory.buffer;
      const offset = address >>> 0;
      if (length < 1 || length > 65536 || offset + length > memory.byteLength)
        throw new Error("素材地址越界");
      const source = new Uint8Array(memory, offset, length);
      const count = source[0];
      let cursor = 1 + count * 2, position = 0;
      if (!count || cursor > length) throw new Error("素材调色板无效");
      const width = box.right - box.left, height = box.bottom - box.top;
      const data = new Uint16Array(width * height), mask = new Uint8Array(width * height);
      const previous = new Uint8Array(w);
      while (position < w * h && cursor < length) {
        const code = source[cursor++], run = (code & 127) + 1, literal = code < 128;
        const consumed = literal ? run : 1;
        if (cursor + consumed > length || position + run > w * h) throw new Error("素材长度无效");
        for (let i = 0; i < run; ++i, ++position) {
          const encoded = source[cursor + (literal ? i : 0)];
          if (encoded > count + 1) throw new Error("素材调色板索引无效");
          const column = position % w;
          const index = encoded ? encoded - 1 : previous[column];
          previous[column] = index;
          const px = x + column, py = y + Math.floor(position / w);
          if (index && px >= box.left && px < box.right && py >= box.top && py < box.bottom) {
            const at = (py - box.top) * width + px - box.left;
            data[at] = source[2 * index - 1] | (source[2 * index] << 8);
            mask[at] = 1;
          }
        }
        cursor += consumed;
      }
      if (position !== w * h || cursor !== length) throw new Error("素材长度无效");
      pixels.push({ kind: "blit", x: box.left, y: box.top, w: width, h: height, data, mask });
    },
    tone(hz, ms, volume) {
      if (hz >= 40 && hz <= 8000 && ms >= 1 && ms <= 2000 && volume >= 0 && volume <= 100) {
        if (tones.length >= 8) throw new Error("本次音频事件过多");
        tones.push({ hz, ms, volume });
      }
    },
    load(slot, fallback) {
      return slot >= 0 && slot < 8 ? slots[slot] ?? fallback : fallback;
    },
    save(slot, value) {
      if (slot < 0 || slot >= 8) return;
      if (saves.length >= 8) throw new Error("本次存档写入过多");
      saves.push([slot, value]);
      slots[slot] = value;
    },
  } });
  const exports = instance.exports;
  for (const [name, argc] of Object.entries(required)) {
    if (typeof exports[name] !== "function" || exports[name].length !== argc) {
      throw new Error("组件入口不完整");
    }
  }
  if (!(exports.memory instanceof WebAssembly.Memory)) throw new Error("组件未导出内存");
  function run(type, args) {
    sampleEvent = type === "frame" || (type === "event" && [1, 3, 4].includes(args[0]));
    cue = music = undefined;
    pixels = []; texts = []; tones = []; saves = [];
    operations = 0; area = 0; textWork = 0; frame = type === "frame";
    const entry = { init: "hachi_init", event: "hachi_event", frame: "hachi_frame" }[type];
    if (!entry) throw new Error("未知组件事件");
    const before = { ...slots };
    try {
      exports[entry](...args);
    } catch (error) {
      for (const key of Object.keys(slots)) delete slots[key];
      Object.assign(slots, before);
      throw error;
    }
    return { hostProfile, audioProfile, cue, music, pixels, texts, tones, saves, width: screenWidth, height: screenHeight, frame, init: type === "init" };
  }
  return run;
}
