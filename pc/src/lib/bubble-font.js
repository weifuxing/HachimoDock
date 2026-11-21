// Desktop-only official, unmodified font. Never change the wire text hash:
// JS/Rust/firmware share it, including older firmware versions.
export const BUBBLE_FONT_FAMILY = '"Hachimi MiSans Medium"';
export const BUBBLE_FONT_REVISION = "misans-medium-official-20250124";
export const BUBBLE_FONT_URL = new URL("../assets/fonts/MiSans-Medium.woff2", import.meta.url).href;

export function createBubbleFontLoader({ FontFaceClass = globalThis.FontFace,
  fontSet = globalThis.document?.fonts, url = BUBBLE_FONT_URL } = {}) {
  let promise, ready = false;
  return {
    ready: () => ready,
    load() {
      if (!promise) promise = (async () => {
        if (!FontFaceClass || !fontSet) throw Error("MiSans font loading is unavailable");
        const face = new FontFaceClass("Hachimi MiSans Medium", `url("${url}")`, { weight: "500", style: "normal" });
        const loaded = await face.load();
        if (loaded.status !== "loaded") throw Error("MiSans font failed to load");
        fontSet.add(loaded);
        if (!fontSet.check(`500 24px ${BUBBLE_FONT_FAMILY}`, "中文MiSans")) {
          fontSet.delete(loaded);
          throw Error("MiSans font registration failed");
        }
        ready = true;
      })();
      return promise;
    },
  };
}
const bundledFont = createBubbleFontLoader();
export const ensureBubbleFont = () => bundledFont.load();
export function createBubbleCanvas() {
  if (!bundledFont.ready()) throw Error("MiSans must load before bitmap rendering");
  return document.createElement("canvas");
}
