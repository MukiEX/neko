/*
 * lakehp placement + 7.1 rear pair checks, against the LakeHP technical breakdown:
 *   test_lakehp71 <DolbyHph.dll> [original lakehp.c build: compared via lhp_ref_*]
 *
 * Built twice: once against the extended engine (lakehp/lakehp.c) and, for the
 * bit-identity check, linked with the original 5.1 engine renamed to lhpref_*.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lakehp.h"
#include "lhp_dll.h"

#ifdef HAVE_REF
/* original engine, symbols prefixed via -D renames at build time */
typedef struct lhpref_engine lhpref_engine;
lhpref_engine *lhpref_new(void);
int lhpref_load(lhpref_engine *e, const uint8_t *blob, size_t len, const int8_t map[6], int nch);
int lhpref_push(lhpref_engine *e, const float *in, int n, float *out);
#endif

#define FS 48000
static int failures = 0;
#define CHECK(c, ...)                   \
  do {                                  \
    if (!(c)) {                         \
      printf("FAIL: " __VA_ARGS__);     \
      printf("\n");                     \
      failures++;                       \
    }                                   \
  } while (0)

static uint8_t *dll;
static size_t dll_len;

/* engine order of a 7.1 interleave: FL FR FC LFE SL SR BL BR (our element's order) */
static const int8_t MAP71[LHP_NPOS] = {0, 2, 1, 4, 5, 3};
static const int8_t MAP51[LHP_NPOS] = {0, 2, 1, 4, 5, 3};

static lhp_engine *engine(int room, int nch, int back, const lhp_surround_t *sur, const lhp_surround_t *rear) {
  static lhp_engine pool[8];
  static int next = 0;
  lhp_engine *e = &pool[next++ % 8];
  const uint8_t *blob;
  size_t blen;
  if (lhp_dll_find(dll, dll_len, FS, room, &blob, &blen)) {
    printf("no DH%d filter\n", room);
    exit(1);
  }
  lhp_init(e);
  lhp_set_map(e, nch == 8 ? MAP71 : MAP51, nch);
  if (back) lhp_set_back(e, 6, 7);
  lhp_set_placement(e, sur, rear);
  if (lhp_load_filter(e, blob, blen, 1.0f)) {
    printf("load failed\n");
    exit(1);
  }
  return e;
}

static unsigned seed = 12345;
static float noise(void) {
  seed = seed * 1664525u + 1013904223u;
  return ((float)(seed >> 8) / (float)(1u << 24) * 2.0f - 1.0f) * 8000.0f;
}

#define BLOCKS 24

/* render: in is nch interleaved, BLOCKS blocks; returns BLOCKS*1024 stereo frames */
static void render(lhp_engine *e, const float *in, int nch, float *out) {
  for (int b = 0; b < BLOCKS; b++) lhp_push_f32(e, in + (size_t)b * LHP_BLOCK * nch, LHP_BLOCK, out + (size_t)b * LHP_BLOCK * 2);
}

static double maxdiff(const float *a, const float *b, size_t n) {
  double d = 0;
  for (size_t i = 0; i < n; i++) d = fmax(d, fabs(a[i] - b[i]));
  return d;
}

/* interaural level difference (dB) and time difference (samples, + = left leads) */
static void ild_itd(const float *o, size_t frames, double *ild, double *itd) {
  double el = 0, er = 0;
  for (size_t i = 0; i < frames; i++) {
    el += (double)o[2 * i] * o[2 * i];
    er += (double)o[2 * i + 1] * o[2 * i + 1];
  }
  *ild = 10 * log10(el / er);
  double best = -1e300;
  int lag = 0;
  for (int l = -40; l <= 40; l++) {
    double c = 0;
    for (size_t i = 64; i + 64 < frames; i++) c += (double)o[2 * i] * o[2 * (i + l) + 1];
    if (c > best) { best = c; lag = l; }
  }
  *itd = lag;
}

static void test_mapping(void) {
  static const struct { float az, d, hf, eq; } table[] = {
      {90, -7.5f, -6.0f, -1.00f}, {100, -5.2f, -4.0f, -0.64f}, {110, -2.5f, -1.8f, -0.29f},
      {118, 0.0f, 0.0f, 0.00f},   {136, 6.0f, 4.0f, 1.00f},    {150, 11.5f, 7.1f, 1.78f},
      {165, 17.6f, 10.4f, 2.00f},
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    lhp_surround_t s;
    lhp_surround_for_azimuth(table[i].az, &s);
    printf("azimuth %3.0f: delay %+5.1f  far HF %+5.1f dB  rear cues %+5.2f   (breakdown: %+5.1f %+5.1f %+5.2f)\n",
           table[i].az, s.near_delay, s.far_hf_db, s.back_eq, table[i].d, table[i].hf, table[i].eq);
    CHECK(fabsf(s.near_delay - table[i].d) <= 0.15f && fabsf(s.far_hf_db - table[i].hf) <= 0.15f &&
              fabsf(s.back_eq - table[i].eq) <= 0.015f,
          "mapping at %.0f degrees", table[i].az);
  }
  lhp_surround_t f;
  lhp_surround_for_azimuth(118, &f);
  CHECK(lhp_surround_is_factory(&f), "118 degrees is not exactly factory");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s DolbyHph.dll\n", argv[0]);
    return 2;
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f) { perror(argv[1]); return 1; }
  dll = malloc(1 << 20);
  dll_len = fread(dll, 1, 1 << 20, f);
  fclose(f);

  test_mapping();

  const size_t N = (size_t)BLOCKS * LHP_BLOCK;
  float *in6 = calloc(N * 6, sizeof(float)), *in8 = calloc(N * 8, sizeof(float));
  float *a = calloc(N * 2, sizeof(float)), *b = calloc(N * 2, sizeof(float)), *c = calloc(N * 2, sizeof(float));
  lhp_surround_t factory = {0, 0, 0}, s136, s150;
  lhp_surround_for_azimuth(136, &s136);
  lhp_surround_for_azimuth(150, &s150);

#ifdef HAVE_REF
  /* 1. factory 5.1 is bit-identical to the original engine, all rooms */
  for (size_t i = 0; i < N * 6; i++) in6[i] = noise();
  for (int room = 1; room <= 3; room++) {
    const uint8_t *blob;
    size_t blen;
    lhp_dll_find(dll, dll_len, FS, room, &blob, &blen);
    lhpref_engine *r = lhpref_new();
    lhpref_load(r, blob, blen, MAP51, 6);
    for (int k = 0; k < BLOCKS; k++) lhpref_push(r, in6 + (size_t)k * LHP_BLOCK * 6, LHP_BLOCK, b + (size_t)k * LHP_BLOCK * 2);
    render(engine(room, 6, 0, &factory, &factory), in6, 6, a);
    double d = maxdiff(a, b, N * 2);
    printf("DH%d 5.1 factory vs original engine: max difference %g\n", room, d);
    CHECK(d == 0, "5.1 output changed for DH%d", room);
  }
#endif

  /* 2. factory 7.1: back channel == same signal on the 5.1 surround */
  memset(in6, 0, N * 6 * sizeof(float));
  memset(in8, 0, N * 8 * sizeof(float));
  for (size_t i = 0; i < N; i++) {
    float v = noise(), w = noise();
    in8[i * 8 + 6] = v; in8[i * 8 + 7] = w;   /* BL BR */
    in6[i * 6 + 4] = v; in6[i * 6 + 5] = w;   /* SL SR */
  }
  render(engine(2, 8, 1, &factory, &factory), in8, 8, a);
  render(engine(2, 6, 0, &factory, &factory), in6, 6, b);
  printf("7.1 factory: back channels vs 5.1 surrounds: max difference %g\n", maxdiff(a, b, N * 2));
  CHECK(maxdiff(a, b, N * 2) == 0, "factory back != surround");

  /* 3. rear pair at 150 == 5.1 surround placed at 150 */
  render(engine(2, 8, 1, &factory, &s150), in8, 8, a);
  render(engine(2, 6, 0, &s150, &s150), in6, 6, b);
  printf("7.1 rear pair at 150 vs 5.1 surrounds at 150: max difference %g\n", maxdiff(a, b, N * 2));
  CHECK(maxdiff(a, b, N * 2) == 0, "rear pair at 150 != surround at 150");

  /* 4. side + back = sum of parts */
  memset(in8, 0, N * 8 * sizeof(float));
  float *side = calloc(N * 8, sizeof(float)), *back = calloc(N * 8, sizeof(float));
  for (size_t i = 0; i < N; i++) {
    side[i * 8 + 4] = noise(); side[i * 8 + 5] = noise();
    back[i * 8 + 6] = noise(); back[i * 8 + 7] = noise();
    for (int ch = 0; ch < 8; ch++) in8[i * 8 + ch] = side[i * 8 + ch] + back[i * 8 + ch];
  }
  render(engine(3, 8, 1, &s136, &s150), in8, 8, a);
  render(engine(3, 8, 1, &s136, &s150), side, 8, b);
  render(engine(3, 8, 1, &s136, &s150), back, 8, c);
  for (size_t i = 0; i < N * 2; i++) b[i] += c[i];
  printf("7.1 side + back vs sum of parts: max difference %.3g LSB\n", maxdiff(a, b, N * 2));
  CHECK(maxdiff(a, b, N * 2) <= 1.0, "side + back is not the sum of its parts");

  /* 5. placement moves the speaker back: smaller ITD and ILD as the angle grows */
  memset(in6, 0, N * 6 * sizeof(float));
  for (size_t i = 0; i < N; i++) in6[i * 6 + 4] = noise();   /* left surround only */
  double prev_ild = 1e9, prev_itd = 1e9;
  const float angles[] = {100, 118, 136, 150};
  for (size_t k = 0; k < 4; k++) {
    lhp_surround_t p;
    lhp_surround_for_azimuth(angles[k], &p);
    render(engine(1, 6, 0, &p, &p), in6, 6, a);
    double ild, itd;
    ild_itd(a + LHP_BLOCK * 4, N - LHP_BLOCK * 4, &ild, &itd);
    printf("left surround at %3.0f degrees: ILD %+5.1f dB, ITD %+3.0f samples\n", angles[k], ild, itd);
    CHECK(ild > 0, "left surround not on the left at %.0f", angles[k]);
    CHECK(ild < prev_ild + 0.05 && itd <= prev_itd, "placement does not move back at %.0f", angles[k]);
    prev_ild = ild;
    prev_itd = itd;
  }

  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all lakehp 7.1 tests passed\n");
  return 0;
}
