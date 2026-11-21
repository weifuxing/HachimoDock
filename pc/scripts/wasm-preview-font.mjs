import { readFile } from "node:fs/promises";
import { inflateSync } from "node:zlib";

const firmware = new URL("../../firmware/", import.meta.url);
const names = ["pet_p4_font_cn", "pet_p4_font_cn_32"];

function array(source, name) {
  const body = source.match(new RegExp(`\\b${name}\\[\\]\\s*=\\s*\\{([\\s\\S]*?)\\n\\};`))?.[1];
  if (!body) throw new Error(`Missing firmware font table ${name}`);
  return body;
}

const integers = text => [...text.matchAll(/-?(?:0x[0-9a-f]+|\d+)/gi)].map(([number]) => Number(number));
function field(source, key) {
  const value = source.match(new RegExp(`\\.${key}\\s*=\\s*(-?\\d+)`))?.[1];
  if (value === undefined) throw new Error(`Missing firmware font field ${key}`);
  return Number(value);
}

export async function loadPreviewFonts() {
  return Promise.all(names.map(async (name, index) => {
    const size = index ? 32 : 16;
    const [source, packed] = await Promise.all([
      readFile(new URL(`main/${name}.c`, firmware), "utf8"),
      readFile(new URL(`assets/fonts/pet_cn_${size}.deflate`, firmware)),
    ]);
    const glyphs = [...array(source, "glyph_dsc").matchAll(/\{([^{}]+)\}/g)].map(([, glyph]) =>
      ["bitmap_index", "adv_w", "box_w", "box_h", "ofs_x", "ofs_y"].map(key => field(glyph, key)));
    const mappings = [...array(source, "cmaps").matchAll(/\{([^{}]+)\}/g)].map(([, map]) => {
      const type = map.match(/LV_FONT_FMT_TXT_CMAP_(\w+)/)?.[1];
      if (type !== "SPARSE_TINY" && type !== "FORMAT0_TINY") throw new Error("Unsupported firmware font cmap");
      const sparse = type === "SPARSE_TINY" ? integers(array(source, map.match(/\.unicode_list\s*=\s*(\w+)/)[1])) : null;
      return [field(map, "range_start"), field(map, "range_length"), field(map, "glyph_id_start"), sparse];
    });
    const bitmap = inflateSync(packed);
    const bpp = field(source, "bpp");
    if (glyphs.some(([at, , w, h]) => at + Math.ceil(w * h * bpp / 8) > bitmap.length)) {
      throw new Error("Firmware font metrics do not fit the bitmap");
    }
    const kerning = index ? null : {
      left: integers(array(source, "kern_left_class_mapping")),
      right: integers(array(source, "kern_right_class_mapping")),
      values: integers(array(source, "kern_class_values")),
      columns: field(source, "right_class_cnt"),
      scale: field(source, "kern_scale"),
    };
    return { glyphs, mappings, bitmap: bitmap.toString("base64"), bpp, kerning,
      baseline: field(source, "line_height") - field(source, "base_line") };
  }));
}

export function wasmPreviewFontPlugin() {
  const id = "\0virtual:p4-wasm-font";
  return {
    name: "p4-wasm-preview-font",
    resolveId(source) { return source === "virtual:p4-wasm-font" ? id : null; },
    async load(source) {
      if (source !== id) return null;
      for (const name of names) this.addWatchFile(new URL(`main/${name}.c`, firmware).pathname);
      for (const size of [16, 32]) this.addWatchFile(new URL(`assets/fonts/pet_cn_${size}.deflate`, firmware).pathname);
      return `export default ${JSON.stringify(await loadPreviewFonts())};`;
    },
  };
}
