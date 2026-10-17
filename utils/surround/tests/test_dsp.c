/* Unit tests for the neko-surround DSP: cc -O2 -I../src test_dsp.c ../src/surround_dsp.c -lm */
#include "surround_dsp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define RATE 48000
#define FRAMES (RATE / 2)
#define SETTLE (RATE / 4)

static int failures = 0;

#define CHECK(cond, ...)                 \
  do {                                   \
    if (!(cond)) {                       \
      printf("FAIL: " __VA_ARGS__);      \
      printf("\n");                      \
      failures++;                        \
    }                                    \
  } while (0)

/* amplitude and phase of a signal at frequency f (single bin DFT over settled part) */
static void measure(const float *x, size_t stride, size_t n, double f, double *amp, double *phase) {
  double re = 0, im = 0;
  size_t count = 0;
  for (size_t i = SETTLE; i < n; i++, count++) {
    double w = 2 * M_PI * f * (double)i / RATE;
    re += x[i * stride] * cos(w);
    im -= x[i * stride] * sin(w);
  }
  *amp = 2 * sqrt(re * re + im * im) / (double)count;
  *phase = atan2(im, re) * 180 / M_PI;
}

static double wrap(double deg) {
  while (deg > 180) deg -= 360;
  while (deg <= -180) deg += 360;
  return deg;
}

static void test_hilbert(void) {
  const double freqs[] = {30, 50, 100, 300, 1000, 3000, 8000, 12000, 16000, 20000};
  float *direct = malloc(sizeof(float) * FRAMES);
  float *shifted = malloc(sizeof(float) * FRAMES);

  for (size_t k = 0; k < sizeof(freqs) / sizeof(freqs[0]); k++) {
    surr_hilbert h;
    surr_hilbert_reset(&h);
    for (size_t i = 0; i < FRAMES; i++) {
      float in = (float)sin(2 * M_PI * freqs[k] * (double)i / RATE);
      surr_hilbert_process(&h, in, &direct[i], &shifted[i]);
    }

    double ad, pd, as, ps;
    measure(direct, 1, FRAMES, freqs[k], &ad, &pd);
    measure(shifted, 1, FRAMES, freqs[k], &as, &ps);
    double diff = wrap(ps - pd);

    printf("hilbert %6.0f Hz: |direct| %.3f |shifted| %.3f phase diff %+.2f\n", freqs[k], ad, as, diff);
    CHECK(fabs(ad - 1) < 0.01 && fabs(as - 1) < 0.01, "allpass gain at %.0f Hz", freqs[k]);
    CHECK(fabs(diff - 90) < 1.5, "phase difference at %.0f Hz is %.2f", freqs[k], diff);
  }

  free(direct);
  free(shifted);
}

/* runs the PLII encoder on a sine in the given input channel, returns Lt/Rt amplitude and phase */
static void plii_single(int channel, double f, double *al, double *pl, double *ar, double *pr) {
  float *in = calloc(FRAMES * SURR_CHANNELS, sizeof(float));
  float *out = calloc(FRAMES * 2, sizeof(float));
  for (size_t i = 0; i < FRAMES; i++)
    in[i * SURR_CHANNELS + channel] = 0.25f * (float)sin(2 * M_PI * f * (double)i / RATE);

  surr_plii e;
  surr_plii_init(&e, RATE, 0.0f, 1.0f);
  surr_plii_process(&e, in, out, FRAMES);

  measure(out, 2, FRAMES, f, al, pl);
  measure(out + 1, 2, FRAMES, f, ar, pr);
  free(in);
  free(out);
}

static void test_plii(void) {
  const double f = 1000;
  double al, pl, ar, pr;

  /* front left only goes to Lt */
  plii_single(SURR_FL, f, &al, &pl, &ar, &pr);
  printf("plii FL: Lt %.3f Rt %.3f\n", al, ar);
  CHECK(fabs(al - 0.25) < 0.005 && ar < 0.001, "FL isolation");

  /* center: equal and in phase on both channels at -3 dB */
  plii_single(SURR_FC, f, &al, &pl, &ar, &pr);
  printf("plii FC: Lt %.3f Rt %.3f phase %+.1f\n", al, ar, wrap(pr - pl));
  CHECK(fabs(al - 0.25 * 0.7071) < 0.005 && fabs(ar - al) < 0.002, "FC level");
  CHECK(fabs(wrap(pr - pl)) < 1, "FC in phase");

  /* surround left: Lt 0.8718, Rt 0.4899, opposite polarity, 90 degrees from the fronts */
  double fl_phase = pl;
  plii_single(SURR_FL, f, &al, &fl_phase, &ar, &pr);
  plii_single(SURR_SL, f, &al, &pl, &ar, &pr);
  printf("plii SL: Lt %.3f Rt %.3f L/R phase %+.1f, vs front %+.1f\n", al, ar, wrap(pr - pl), wrap(pl - fl_phase));
  CHECK(fabs(al - 0.25 * 0.8718) < 0.005 && fabs(ar - 0.25 * 0.4899) < 0.005, "SL levels");
  CHECK(fabs(fabs(wrap(pr - pl)) - 180) < 1.5, "SL anti-phase");
  CHECK(fabs(fabs(wrap(pl - fl_phase)) - 90) < 1.5, "SL 90 degrees from front");

  /* surround right mirrors surround left */
  plii_single(SURR_SR, f, &al, &pl, &ar, &pr);
  printf("plii SR: Lt %.3f Rt %.3f L/R phase %+.1f\n", al, ar, wrap(pr - pl));
  CHECK(fabs(al - 0.25 * 0.4899) < 0.005 && fabs(ar - 0.25 * 0.8718) < 0.005, "SR levels");
  CHECK(fabs(fabs(wrap(pr - pl)) - 180) < 1.5, "SR anti-phase");

  /* LFE is dropped with lfe_level 0 */
  plii_single(SURR_LFE, 60, &al, &pl, &ar, &pr);
  CHECK(al < 0.001 && ar < 0.001, "LFE dropped");
}

/* a passive matrix decode of a hard-panned surround source must land in the surround (L-R) sum */
static void test_plii_decode(void) {
  float *in = calloc(FRAMES * SURR_CHANNELS, sizeof(float));
  float *out = calloc(FRAMES * 2, sizeof(float));
  for (size_t i = 0; i < FRAMES; i++) {
    float s = 0.25f * (float)sin(2 * M_PI * 440 * (double)i / RATE);
    in[i * SURR_CHANNELS + SURR_SL] = s;
    in[i * SURR_CHANNELS + SURR_SR] = s;
  }

  surr_plii e;
  surr_plii_init(&e, RATE, 0.0f, 1.0f);
  surr_plii_process(&e, in, out, FRAMES);

  double c = 0, s = 0;
  for (size_t i = SETTLE; i < FRAMES; i++) {
    double sum = out[i * 2] + out[i * 2 + 1];
    double diff = out[i * 2] - out[i * 2 + 1];
    c += sum * sum;
    s += diff * diff;
  }
  double separation = 10 * log10(s / (c + 1e-12));
  printf("plii passive decode, rear source: surround vs center %.1f dB\n", separation);
  CHECK(separation > 40, "rear source leaks into center");

  free(in);
  free(out);
}

static void test_limiter(void) {
  float buf[2 * 4800];
  surr_limiter l;
  surr_limiter_init(&l, 0.98f, 150.0f, RATE);
  for (int i = 0; i < 4800; i++) {
    buf[i * 2] = 4.0f * (float)sin(2 * M_PI * 100 * i / (double)RATE);
    buf[i * 2 + 1] = -buf[i * 2];
  }
  surr_limiter_process(&l, buf, 4800);
  float peak = 0;
  for (int i = 0; i < 2 * 4800; i++) peak = fmaxf(peak, fabsf(buf[i]));
  printf("limiter peak %.3f\n", peak);
  CHECK(peak <= 0.9801f, "limiter peak");
}

static void test_silence_denormals(void) {
  /* long silence after a burst must decay to exact zeros (no denormal tail) */
  float in[SURR_CHANNELS * 1024] = {0};
  float out[2 * 1024];
  surr_plii e;
  surr_plii_init(&e, RATE, 0.5f, 1.0f);
  in[SURR_SL] = 1.0f;
  surr_plii_process(&e, in, out, 1024);
  in[SURR_SL] = 0.0f;
  for (int k = 0; k < 2000; k++) surr_plii_process(&e, in, out, 1024);
  CHECK(out[2046] == 0.0f && out[2047] == 0.0f, "silence decays to zero");
}

int main(void) {
  test_hilbert();
  test_plii();
  test_plii_decode();
  test_limiter();
  test_silence_denormals();

  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all DSP tests passed\n");
  return 0;
}
