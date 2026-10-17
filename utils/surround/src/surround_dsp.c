#include "surround_dsp.h"

#include <math.h>
#include <string.h>

/*
 * Allpass coefficients (a^2 per section) of Olli Niemitalo's 90 degree phase
 * difference network. With the one sample delay after chain A, the phase
 * difference between both paths stays within 90 +-0.7 degrees from ~25 Hz up
 * to the Nyquist frequency at 48 kHz (see tests/test_dsp.c).
 */
static const float coef_a[4] = {
    0.6923878f * 0.6923878f,
    0.9360654322959f * 0.9360654322959f,
    0.9882295226860f * 0.9882295226860f,
    0.9987488452737f * 0.9987488452737f,
};

static const float coef_b[4] = {
    0.4021921162426f * 0.4021921162426f,
    0.8561710882420f * 0.8561710882420f,
    0.9722909545651f * 0.9722909545651f,
    0.9952884791278f * 0.9952884791278f,
};

static inline float chain_process(surr_allpass_chain *c, const float *k, float in) {
  for (int i = 0; i < 4; i++) {
    /* H(z) = (k - z^-2) / (1 - k z^-2) */
    float out = k[i] * (in + c->y2[i]) - c->x2[i];
    c->x2[i] = c->x1[i];
    c->x1[i] = in;
    c->y2[i] = c->y1[i];
    c->y1[i] = out;
    in = out;
  }
  return in;
}

void surr_hilbert_reset(surr_hilbert *h) {
  memset(h, 0, sizeof(*h));
}

void surr_hilbert_process(surr_hilbert *h, float in, float *direct, float *shifted) {
  float a = chain_process(&h->a, coef_a, in);
  *direct = h->a_delay;
  h->a_delay = a;
  *shifted = chain_process(&h->b, coef_b, in);
}

/* denormals in the allpass feedback would slow everything down during silence */
static inline float flush_denormal(float v) {
  return fabsf(v) < 1e-20f ? 0.0f : v;
}

static void hilbert_flush(surr_hilbert *h) {
  for (int i = 0; i < 4; i++) {
    h->a.y1[i] = flush_denormal(h->a.y1[i]);
    h->a.y2[i] = flush_denormal(h->a.y2[i]);
    h->b.y1[i] = flush_denormal(h->b.y1[i]);
    h->b.y2[i] = flush_denormal(h->b.y2[i]);
  }
}

void surr_limiter_init(surr_limiter *l, float threshold, float release_ms, int rate) {
  l->threshold = threshold;
  l->release = 1.0f - expf(-1.0f / (release_ms * 0.001f * (float)rate));
  l->gain = 1.0f;
}

void surr_limiter_process(surr_limiter *l, float *stereo, size_t frames) {
  for (size_t i = 0; i < frames; i++) {
    float *f = stereo + i * 2;
    float peak = fmaxf(fabsf(f[0]), fabsf(f[1]));

    /* release back towards unity gain */
    l->gain += (1.0f - l->gain) * l->release;

    /* instant attack */
    if (peak * l->gain > l->threshold)
      l->gain = l->threshold / peak;

    f[0] *= l->gain;
    f[1] *= l->gain;
  }
}

#define SURR_M3DB 0.70710678f

void surr_plii_init(surr_plii *e, int rate, float lfe_level, float gain) {
  surr_plii_reset(e);
  e->lfe_level = lfe_level;
  e->gain = gain;
  surr_limiter_init(&e->limiter, 0.98f, 150.0f, rate);
}

void surr_plii_reset(surr_plii *e) {
  surr_hilbert_reset(&e->front_l);
  surr_hilbert_reset(&e->front_r);
  surr_hilbert_reset(&e->surr_l);
  surr_hilbert_reset(&e->surr_r);
}

void surr_plii_process(surr_plii *e, const float *in6, float *out2, size_t frames) {
  for (size_t i = 0; i < frames; i++) {
    const float *f = in6 + i * SURR_CHANNELS;
    float common = SURR_M3DB * f[SURR_FC] + e->lfe_level * f[SURR_LFE];
    float front_l = f[SURR_FL] + common;
    float front_r = f[SURR_FR] + common;
    float surr_l = 0.8718f * f[SURR_SL] + 0.4899f * f[SURR_SR];
    float surr_r = 0.4899f * f[SURR_SL] + 0.8718f * f[SURR_SR];

    /* every path goes through the network so all share the same group delay;
     * front channels use the reference output, surrounds the +90 degree output */
    float d, s, fl, fr, sl, sr;
    surr_hilbert_process(&e->front_l, front_l, &fl, &s);
    surr_hilbert_process(&e->front_r, front_r, &fr, &s);
    surr_hilbert_process(&e->surr_l, surr_l, &d, &sl);
    surr_hilbert_process(&e->surr_r, surr_r, &d, &sr);

    out2[i * 2 + 0] = e->gain * (fl - sl);
    out2[i * 2 + 1] = e->gain * (fr + sr);
  }

  hilbert_flush(&e->front_l);
  hilbert_flush(&e->front_r);
  hilbert_flush(&e->surr_l);
  hilbert_flush(&e->surr_r);

  surr_limiter_process(&e->limiter, out2, frames);
}

void surr_stereo_init(surr_stereo *e, int rate, float lfe_level, float gain) {
  e->lfe_level = lfe_level;
  e->gain = gain;
  surr_limiter_init(&e->limiter, 0.98f, 150.0f, rate);
}

void surr_stereo_process(surr_stereo *e, const float *in6, float *out2, size_t frames) {
  for (size_t i = 0; i < frames; i++) {
    const float *f = in6 + i * SURR_CHANNELS;
    float common = SURR_M3DB * f[SURR_FC] + e->lfe_level * f[SURR_LFE];
    out2[i * 2 + 0] = e->gain * (f[SURR_FL] + common + SURR_M3DB * f[SURR_SL]);
    out2[i * 2 + 1] = e->gain * (f[SURR_FR] + common + SURR_M3DB * f[SURR_SR]);
  }

  surr_limiter_process(&e->limiter, out2, frames);
}
