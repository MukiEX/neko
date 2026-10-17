/*
 * neko-surround: 5.1 -> stereo downmix DSP used by the nekosurround GStreamer element.
 *
 * Input frames are 6 interleaved float channels in this fixed order:
 *   FL, FR, FC, LFE, SL, SR   (SL/SR = surround left/right, rear or side)
 * Output frames are 2 interleaved float channels: L, R.
 */
#ifndef NEKO_SURROUND_DSP_H
#define NEKO_SURROUND_DSP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  SURR_FL = 0,
  SURR_FR,
  SURR_FC,
  SURR_LFE,
  SURR_SL,
  SURR_SR,
  SURR_CHANNELS
};

/* 90 degree phase difference network (two chains of 4 second-order allpass sections).
 * "direct" is the reference path, "shifted" leads it by 90 degrees (+j). */
typedef struct {
  float x1[4], x2[4], y1[4], y2[4];
} surr_allpass_chain;

typedef struct {
  surr_allpass_chain a;  /* reference path */
  float a_delay;         /* one sample delay after the reference chain */
  surr_allpass_chain b;  /* +90 degree path */
} surr_hilbert;

void surr_hilbert_reset(surr_hilbert *h);
/* processes one sample, returns reference (direct) and +90 degree (shifted) outputs */
void surr_hilbert_process(surr_hilbert *h, float in, float *direct, float *shifted);

/* simple peak limiter (instant attack, exponential release) used as a clipping safety net */
typedef struct {
  float threshold; /* linear peak level the limiter keeps the output under */
  float release;   /* per-sample release coefficient */
  float gain;      /* current gain */
} surr_limiter;

void surr_limiter_init(surr_limiter *l, float threshold, float release_ms, int rate);
void surr_limiter_process(surr_limiter *l, float *stereo, size_t frames);

/* Dolby Pro Logic II matrix encoder:
 *   Lt = L + 0.7071 C + lfe*LFE - j(0.8718 SL + 0.4899 SR)
 *   Rt = R + 0.7071 C + lfe*LFE + j(0.4899 SL + 0.8718 SR)
 * followed by a gain and the limiter. */
typedef struct {
  surr_hilbert front_l, front_r, surr_l, surr_r;
  float lfe_level;
  float gain;
  surr_limiter limiter;
} surr_plii;

void surr_plii_init(surr_plii *e, int rate, float lfe_level, float gain);
void surr_plii_reset(surr_plii *e);
void surr_plii_process(surr_plii *e, const float *in6, float *out2, size_t frames);

/* Plain ITU-R BS.775 style downmix without matrix encoding (used as fallback):
 *   L = FL + 0.7071 FC + 0.7071 SL + lfe*LFE
 *   R = FR + 0.7071 FC + 0.7071 SR + lfe*LFE */
typedef struct {
  float lfe_level;
  float gain;
  surr_limiter limiter;
} surr_stereo;

void surr_stereo_init(surr_stereo *e, int rate, float lfe_level, float gain);
void surr_stereo_process(surr_stereo *e, const float *in6, float *out2, size_t frames);

#ifdef __cplusplus
}
#endif

#endif
