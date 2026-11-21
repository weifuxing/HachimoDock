import { createBubbleCanvas } from "./bubble-font.js";
import { BUBBLE_FONT_FAMILY, encodeBubbleAlpha } from "./bubble-pixels.js";

// A single line using the same bundled MiSans font and RLE4 coverage as speech bubbles.
// Width and height precede the runs. The device supplies the current ink color.
export function renderCenterText(text, { px, width, weight = 500, family = BUBBLE_FONT_FAMILY },
  createCanvas = createBubbleCanvas) {
  const canvas = createCanvas();
  canvas.width = width;
  canvas.height = px + 6;
  const context = canvas.getContext("2d", { willReadFrequently: true });
  if (!context) throw Error("Canvas text rendering is unavailable");
  context.font = `${weight} ${px}px ${family}`;
  context.textBaseline = "alphabetic";
  context.fillStyle = "white";
  const source = text.replace(/\s+/gu, " ").trim();
  let line = source;
  if (context.measureText(line).width > width - 4) {
    line = "";
    for (const char of source) {
      if (context.measureText(line + char + "…").width > width - 4) break;
      line += char;
    }
    line += "…";
  }
  const metric = context.measureText(line);
  const w = Math.min(width, Math.max(4, Math.ceil(Math.max(metric.width, metric.actualBoundingBoxRight || 0)) + 4));
  const h = canvas.height;
  const ascent = metric.actualBoundingBoxAscent || px * 0.85;
  const descent = metric.actualBoundingBoxDescent || 0;
  context.fillText(line, 2, Math.round((h + ascent - descent) / 2));
  const runs = encodeBubbleAlpha(context.getImageData(0, 0, w, h).data);
  return new Uint8Array([w & 255, w >> 8, h, ...runs]);
}

export const CENTER_CARD_TITLE = Object.freeze({ px: 27, width: 252, weight: 700 });

export function centerTitleForSync(title) {
  if (!title || typeof title !== "string") return null;
  // The firmware retains at most 63 UTF-8 bytes in the catalog title.
  const encoder = new TextEncoder();
  let text = "";
  for (const char of title.split("\0", 1)[0]) {
    if (encoder.encode(text + char).length > 63) break;
    text += char;
  }
  if (!text.trim()) return null;
  try {
    return { text, data: btoa(String.fromCharCode(...renderCenterText(text, CENTER_CARD_TITLE))) };
  } catch {
    return null;
  }
}
