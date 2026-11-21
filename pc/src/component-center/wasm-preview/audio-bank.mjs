const BLOCK_BYTES = 166;
const BLOCK_SAMPLES = 320;
const STEP = [
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
  73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
  494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
  2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
];
const INDEX = [-1, -1, -1, -1, 2, 4, 6, 8];
export const SAMPLE_RATE = 16000;

export function loadAudioBank(buffer) {
  const bytes = new DataView(buffer);
  const badBank = () => new Error("WASM 语音资源格式无效");
  if (bytes.byteLength < 16 || bytes.byteLength > 512 * 1024 ||
      bytes.getUint32(0) !== 0x48414231) throw badBank();
  const count = bytes.getUint32(4, true);
  let cursor = 8 + count * 8;
  if (count < 1 || count > 64 || cursor > bytes.byteLength) throw badBank();
  const clips = [];
  for (let i = 0; i < count; i++) {
    const offset = bytes.getUint32(8 + i * 8, true);
    const length = bytes.getUint32(12 + i * 8, true);
    if (offset !== cursor || length < BLOCK_BYTES || length > BLOCK_BYTES * 400 ||
        length % BLOCK_BYTES || length > bytes.byteLength - cursor) throw badBank();
    clips.push([offset, length]);
    cursor += length;
  }
  if (cursor !== bytes.byteLength) throw badBank();
  return (cue) => {
    const clip = clips[cue];
    if (!clip) return null;
    const [offset, length] = clip;
    const pcm = new Float32Array(length / BLOCK_BYTES * BLOCK_SAMPLES);
    let output = 0;
    for (let at = offset; at < offset + length; at += BLOCK_BYTES) {
      let predictor = bytes.getInt16(at, true), index = bytes.getUint8(at + 2);
      if (index > 88 || bytes.getUint8(at + 3) || bytes.getUint16(at + 4, true) !== BLOCK_SAMPLES ||
          bytes.getUint8(at + BLOCK_BYTES - 1) & 0xf0) throw badBank();
      pcm[output++] = predictor / 32768;
      for (let i = 0; i < BLOCK_SAMPLES - 1; i++) {
        const code = (bytes.getUint8(at + 6 + (i >> 1)) >> ((i & 1) * 4)) & 15;
        const step = STEP[index];
        const delta = (step >> 3) + ((code & 4) ? step : 0) +
          ((code & 2) ? step >> 1 : 0) + ((code & 1) ? step >> 2 : 0);
        predictor = Math.min(32767, Math.max(-32768, predictor + ((code & 8) ? -delta : delta)));
        index = Math.min(88, Math.max(0, index + INDEX[code & 7]));
        pcm[output++] = predictor / 32768;
      }
    }
    return pcm;
  };
}
