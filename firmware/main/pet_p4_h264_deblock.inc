/* Normal (bS 1–3) horizontal luma edges, H.264 §8.7.2.3.1.
 * The ESP-H264 1.5.0 P4 vector loads faulted on the first frame of a built-in
 * animation. Use the same in-loop filter with byte loads for both contiguous
 * rows and the split rows passed by tinyh264 at macroblock boundaries. */
static inline int h264_luma_abs(int value) {
  return value < 0 ? -value : value;
}

static inline int h264_luma_clip(int value, int low, int high) {
  return value < low ? low : value > high ? high : value;
}

static void IRAM_ATTR h264_filter_horizontal_luma(uint8_t *p, uint8_t *q,
    int alpha, int beta, int tc0, ptrdiff_t stride, unsigned columns) {
  for (unsigned column = 0; column < columns; ++column, ++p, ++q) {
    const int p0 = p[0], q0 = q[0];
    const int p1 = p[-stride], q1 = q[stride];
    if (h264_luma_abs(p0 - q0) >= alpha || h264_luma_abs(p1 - p0) >= beta ||
        h264_luma_abs(q1 - q0) >= beta) continue;

    const int p2 = p[-2 * stride], q2 = q[2 * stride];
    const int adjust_p1 = h264_luma_abs(p2 - p0) < beta;
    const int adjust_q1 = h264_luma_abs(q2 - q0) < beta;
    const int mean = (p0 + q0 + 1) >> 1;
    if (adjust_p1) p[-stride] = (uint8_t)(p1 + h264_luma_clip((p2 + mean - 2 * p1) >> 1, -tc0, tc0));
    if (adjust_q1) q[stride] = (uint8_t)(q1 + h264_luma_clip((q2 + mean - 2 * q1) >> 1, -tc0, tc0));
    const int tc = tc0 + adjust_p1 + adjust_q1;
    const int delta = h264_luma_clip((4 * (q0 - p0) + p1 - q1 + 4) >> 3, -tc, tc);
    p[0] = (uint8_t)h264_luma_clip(p0 + delta, 0, 255);
    q[0] = (uint8_t)h264_luma_clip(q0 - delta, 0, 255);
  }
}

/* tinyh264 passes addresses of 32-bit alpha and beta values and a byte tc0. */
void IRAM_ATTR __wrap_espFilterHorLuma_bS123(uint8_t *q, uint32_t *alpha, uint32_t *beta,
                                           uint32_t tc0, uint32_t stride) {
  h264_filter_horizontal_luma(q - stride, q, *alpha, *beta, tc0, stride, 16);
}

void IRAM_ATTR __wrap_espFilterHorLumaEdge(uint8_t *q, uint32_t *alpha, uint32_t *beta,
                                        uint32_t tc0, uint32_t stride) {
  h264_filter_horizontal_luma(q - stride, q, *alpha, *beta, tc0, stride, 4);
}

void IRAM_ATTR __wrap_espFilterHorLumaCross_bS123(uint8_t *q, uint8_t *p,
    uint32_t *alpha, uint32_t *beta, uint32_t tc0, uint32_t stride) {
  h264_filter_horizontal_luma(p, q, *alpha, *beta, tc0, stride, 16);
}

void IRAM_ATTR __wrap_espFilterHorLumaEdgeCross(uint8_t *q, uint8_t *p,
    uint32_t *alpha, uint32_t *beta, uint32_t tc0, uint32_t stride) {
  h264_filter_horizontal_luma(p, q, *alpha, *beta, tc0, stride, 4);
}
