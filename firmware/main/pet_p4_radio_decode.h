#pragma once

#include <stdint.h>
#include <stddef.h>

#define PET_RADIO_PCM_RATE 48000
#define PET_RADIO_MP3_FRAME_MAX 1152
#define PET_RADIO_PCM_MAX (PET_RADIO_MP3_FRAME_MAX * 6)

typedef struct {
  int previous;
  int rate;
  int phase;
} pet_radio_resampler_t;

/* Linear interpolation with the fractional source position carried between frames.
 * Supported MP3 rates are 8–48 kHz, so each input produces at most six outputs. */
static inline size_t pet_radio_resample(pet_radio_resampler_t *s, const int16_t *in,
                                       int frames, int channels, int rate, int16_t *out) {
  if (rate < 8000 || rate > PET_RADIO_PCM_RATE || frames <= 0 ||
      frames > PET_RADIO_MP3_FRAME_MAX || (channels != 1 && channels != 2)) return 0;
  if (s->rate != rate) {
    s->rate = rate;
    s->previous = channels == 1 ? in[0] : ((int)in[0] + in[1]) / 2;
    s->phase = PET_RADIO_PCM_RATE;
  }
  size_t n = 0;
  for (int i = 0; i < frames; ++i, in += channels) {
    int next = channels == 1 ? in[0] : ((int)in[0] + in[1]) / 2;
    while (s->phase <= PET_RADIO_PCM_RATE) {
      out[n++] = (int16_t)((s->previous * (PET_RADIO_PCM_RATE - s->phase) +
                          next * s->phase) / PET_RADIO_PCM_RATE);
      s->phase += rate;
    }
    s->phase -= PET_RADIO_PCM_RATE;
    s->previous = next;
  }
  return n;
}
