/*
 * Tests the nekosurround element through GStreamer:
 *   test_element <dir containing libgstnekosurround.so> [DolbyHph.dll]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "lakehp.h"
#include "lhp_dll.h"
#include "surround_dsp.h"

#define RATE 48000
#define FRAMES 480 /* 10 ms buffers like pulsesrc */

static int failures = 0;

#define CHECK(cond, ...)            \
  do {                              \
    if (!(cond)) {                  \
      printf("FAIL: " __VA_ARGS__); \
      printf("\n");                 \
      failures++;                   \
    }                               \
  } while (0)

typedef struct {
  GstElement *pipeline, *src, *sink, *surround;
  guint64 offset;
  gboolean warned;
  int channels;
} Harness;

static void harness_init_ch(Harness *h, int channels, guint64 mask, const char *props) {
  gchar *desc = g_strdup_printf(
      "appsrc name=src format=time is-live=false "
      "caps=audio/x-raw,format=F32LE,layout=interleaved,rate=%d,channels=%d,channel-mask=(bitmask)0x%" G_GINT64_MODIFIER
      "x "
      "! nekosurround name=surround %s ! appsink name=sink sync=false",
      RATE, channels, mask, props);
  GError *err = NULL;
  h->pipeline = gst_parse_launch(desc, &err);
  g_free(desc);
  if (!h->pipeline) {
    printf("pipeline: %s\n", err->message);
    exit(1);
  }
  h->src = gst_bin_get_by_name(GST_BIN(h->pipeline), "src");
  h->sink = gst_bin_get_by_name(GST_BIN(h->pipeline), "sink");
  h->surround = gst_bin_get_by_name(GST_BIN(h->pipeline), "surround");
  h->offset = 0;
  h->warned = FALSE;
  h->channels = channels;
  gst_element_set_state(h->pipeline, GST_STATE_PLAYING);
}

static void harness_init(Harness *h, guint64 mask, const char *props) {
  harness_init_ch(h, 6, mask, props);
}

static void harness_poll_bus(Harness *h) {
  GstBus *bus = gst_element_get_bus(h->pipeline);
  GstMessage *msg;
  while ((msg = gst_bus_pop(bus))) {
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
      GError *err;
      gchar *dbg;
      gst_message_parse_warning(msg, &err, &dbg);
      printf("  bus warning: %s (%s)\n", err->message, dbg);
      h->warned = TRUE;
      g_error_free(err);
      g_free(dbg);
    } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
      GError *err;
      gst_message_parse_error(msg, &err, NULL);
      printf("  bus error: %s\n", err->message);
      failures++;
      g_error_free(err);
    }
    gst_message_unref(msg);
  }
  gst_object_unref(bus);
}

/* pushes one buffer of 6 channel input and returns the produced stereo frames */
static void harness_run(Harness *h, const float *in6, float *out2) {
  GstBuffer *buf = gst_buffer_new_memdup(in6, (gsize)FRAMES * h->channels * sizeof(float));
  GST_BUFFER_PTS(buf) = gst_util_uint64_scale(h->offset, GST_SECOND, RATE);
  GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(FRAMES, GST_SECOND, RATE);
  h->offset += FRAMES;
  gst_app_src_push_buffer(GST_APP_SRC(h->src), buf);

  GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(h->sink), 5 * GST_SECOND);
  if (!sample) {
    printf("FAIL: no output sample\n");
    exit(1);
  }
  GstMapInfo map;
  GstBuffer *out = gst_sample_get_buffer(sample);
  gst_buffer_map(out, &map, GST_MAP_READ);
  if (map.size != FRAMES * 2 * sizeof(float)) {
    printf("FAIL: output size %zu\n", map.size);
    exit(1);
  }
  memcpy(out2, map.data, map.size);
  gst_buffer_unmap(out, &map);
  gst_sample_unref(sample);
  harness_poll_bus(h);
}

static void harness_free(Harness *h) {
  gst_element_set_state(h->pipeline, GST_STATE_NULL);
  gst_object_unref(h->src);
  gst_object_unref(h->sink);
  gst_object_unref(h->surround);
  gst_object_unref(h->pipeline);
}

static void fill_input(float *in6, guint64 offset) {
  for (int i = 0; i < FRAMES; i++) {
    double t = (double)(offset + (guint64)i) / RATE;
    for (int c = 0; c < 6; c++) in6[i * 6 + c] = 0.1f * (float)sin(2 * M_PI * (200.0 + 150.0 * c) * t);
  }
}

static double max_diff(const float *a, const float *b, int n) {
  double d = 0;
  for (int i = 0; i < n; i++) d = fmax(d, fabs(a[i] - b[i]));
  return d;
}

/* PLII output matches the DSP library bit for bit, for rear and side 5.1 layouts */
static void test_plii(guint64 mask, const char *name) {
  Harness h;
  harness_init(&h, mask, "mode=plii");
  surr_plii ref;
  surr_plii_init(&ref, RATE, 0.5f, 0.70710678f);

  float in[FRAMES * 6], out[FRAMES * 2], want[FRAMES * 2];
  double worst = 0;
  for (int b = 0; b < 50; b++) {
    fill_input(in, h.offset);
    surr_plii_process(&ref, in, want, FRAMES);
    harness_run(&h, in, out);
    worst = fmax(worst, max_diff(out, want, FRAMES * 2));
  }
  printf("plii (%s): max difference to reference %.2g\n", name, worst);
  CHECK(worst < 1e-6, "plii %s output differs", name);
  harness_free(&h);
}

/* reads a whole file */
static uint8_t *read_file(const char *path, size_t *len) {
  gchar *data = NULL;
  gsize n = 0;
  if (!g_file_get_contents(path, &data, &n, NULL)) return NULL;
  *len = n;
  return (uint8_t *)data;
}

/* lakehp fed exactly like the element does: our FL FR FC LFE SL SR order, int16 scale,
 * output delayed by one primed block, then gain */
typedef struct {
  lhp_engine e;
  float out[(LHP_BLOCK * 2 + FRAMES) * 2];
  size_t fill;
} Reference;

static int reference_init(Reference *r, const uint8_t *dll, size_t dll_len, int room) {
  static const int8_t map[LHP_NPOS] = {SURR_FL, SURR_FC, SURR_FR, SURR_SL, SURR_SR, SURR_LFE};
  const uint8_t *blob;
  size_t blob_len;
  if (lhp_dll_find(dll, dll_len, RATE, room, &blob, &blob_len)) return -1;
  lhp_init(&r->e);
  lhp_set_map(&r->e, map, 6);
  if (lhp_load_filter(&r->e, blob, blob_len, 1.0f)) return -1;
  memset(r->out, 0, sizeof(r->out));
  r->fill = LHP_BLOCK;
  return 0;
}

static int reference_room(Reference *r, const uint8_t *dll, size_t dll_len, int room) {
  const uint8_t *blob;
  size_t blob_len;
  if (lhp_dll_find(dll, dll_len, RATE, room, &blob, &blob_len)) return -1;
  return lhp_load_filter(&r->e, blob, blob_len, 1.0f);
}

static void reference_run(Reference *r, const float *in6, float *want) {
  static float scaled[FRAMES * 6], block[LHP_BLOCK * 2];
  for (int i = 0; i < FRAMES * 6; i++) scaled[i] = in6[i] * 32768.0f;
  for (int done = 0; done < FRAMES;) {
    int n = LHP_BLOCK - r->e.fill;
    if (n > FRAMES - done) n = FRAMES - done;
    if (lhp_push_f32(&r->e, scaled + done * 6, n, block) == LHP_BLOCK) {
      for (int i = 0; i < LHP_BLOCK * 2; i++) r->out[r->fill * 2 + i] = block[i] / 32768.0f;
      r->fill += LHP_BLOCK;
    }
    done += n;
  }
  for (int i = 0; i < FRAMES * 2; i++) want[i] = 0.70710678f * r->out[i];
  r->fill -= FRAMES;
  memmove(r->out, r->out + FRAMES * 2, r->fill * 2 * sizeof(float));
}

static double energy(const float *s, int frames, int ch) {
  double e = 0;
  for (int i = 0; i < frames; i++) e += (double)s[i * 2 + ch] * s[i * 2 + ch];
  return e;
}

/* the DLL's filters are found like tools/extract_filters.py finds them */
static void test_dll_scan(const uint8_t *dll, size_t len) {
  lhp_dll_filter f[64];
  int n = lhp_dll_scan(dll, len, f, 64);
  int rooms = 0, at48 = 0;
  for (int i = 0; i < n && i < 64; i++) {
    if (f[i].room >= 1 && f[i].room <= 3) rooms++;
    if (f[i].rate == 48000) at48++;
  }
  printf("dll scan: %d filter blobs, %d room filters, %d at 48 kHz, first '%s'\n", n, rooms, at48, n > 0 ? f[0].name : "");
  CHECK(n == 22 && rooms == 21 && at48 == 3, "unexpected filter set (DolbyHph.dll 1.20.0276 has 22 blobs)");
}

/* Dolby Headphone matches lakehp sample for sample, including a room change while playing */
static void test_dolby_headphone(const char *dll_path) {
  size_t dll_len;
  uint8_t *dll = read_file(dll_path, &dll_len);
  if (!dll) {
    printf("FAIL: unable to read %s\n", dll_path);
    failures++;
    return;
  }
  test_dll_scan(dll, dll_len);

  gchar *props = g_strdup_printf("mode=dolby-headphone dh-room=1 dh-dll=\"%s\"", dll_path);
  Harness h;
  harness_init(&h, 0x3f, props);
  g_free(props);

  static Reference ref;
  if (reference_init(&ref, dll, dll_len, 1)) {
    printf("FAIL: reference engine did not load DH1\n");
    failures++;
    harness_free(&h);
    g_free(dll);
    return;
  }

  float in[FRAMES * 6], out[FRAMES * 2], want[FRAMES * 2];
  double worst = 0, peak = 0;
  for (int b = 0; b < 60; b++) {
    if (b == 30) {
      g_object_set(h.surround, "dh-room", 3, NULL);
      CHECK(reference_room(&ref, dll, dll_len, 3) == 0, "reference DH3");
    }
    fill_input(in, h.offset);
    reference_run(&ref, in, want);
    harness_run(&h, in, out);
    worst = fmax(worst, max_diff(out, want, FRAMES * 2));
    for (int i = 0; i < FRAMES * 2; i++) peak = fmax(peak, fabs(out[i]));
  }
  printf("dolby headphone: max difference to lakehp %.2g (output peak %.3f), DH1 then DH3\n", worst, peak);
  CHECK(worst < 1e-6, "dolby headphone output differs from lakehp");
  CHECK(peak > 0.01, "dolby headphone output is silent");
  CHECK(!h.warned, "unexpected warning");
  harness_free(&h);

  /* a source hard left / hard right lands mostly in that ear (broadband noise: with a
   * single tone the head-related level difference can go either way) */
  for (int ch = SURR_FL; ch <= SURR_FR; ch++) {
    guint32 seed = 1;
    props = g_strdup_printf("mode=dolby-headphone dh-dll=\"%s\"", dll_path);
    harness_init(&h, 0x3f, props);
    g_free(props);
    double el = 0, er = 0;
    for (int b = 0; b < 100; b++) {
      memset(in, 0, sizeof(in));
      for (int i = 0; i < FRAMES; i++)
      {
        seed = seed * 1664525u + 1013904223u;
        in[i * 6 + ch] = 0.1f * ((float)(seed >> 8) / (float)(1u << 24) * 2.0f - 1.0f);
      }
      harness_run(&h, in, out);
      el += energy(out, FRAMES, 0);
      er += energy(out, FRAMES, 1);
    }
    double ratio = 10 * log10(ch == SURR_FL ? el / er : er / el);
    printf("dolby headphone: %s source, near ear %+.1f dB over far ear\n", ch == SURR_FL ? "left" : "right", ratio);
    CHECK(ratio > 3, "no left/right separation for channel %d", ch);
    harness_free(&h);
  }

  g_free(dll);
}

/* Dolby Headphone without a usable DLL, or at a rate without filters: stereo downmix and one warning */
static void test_dolby_headphone_fallback(const char *props_extra, int rate_ok, const char *what) {
  gchar *props = g_strdup_printf("mode=dolby-headphone %s", props_extra);
  Harness h;
  harness_init(&h, 0x3f, props);
  g_free(props);
  surr_stereo ref;
  surr_stereo_init(&ref, RATE, 0.5f, 0.70710678f);

  float in[FRAMES * 6], out[FRAMES * 2], want[FRAMES * 2];
  double worst = 0;
  for (int b = 0; b < 30; b++) {
    fill_input(in, h.offset);
    surr_stereo_process(&ref, in, want, FRAMES);
    harness_run(&h, in, out);
    worst = fmax(worst, max_diff(out, want, FRAMES * 2));
  }
  printf("fallback (%s): max difference to stereo downmix %.2g, warned %d\n", what, worst, h.warned);
  (void)rate_ok;
  CHECK(worst < 1e-6, "%s: fallback output differs", what);
  CHECK(h.warned, "%s: no warning", what);
  harness_free(&h);
}

/* 7.1 input in GStreamer order: FL FR FC LFE BL BR SL SR */
#define MASK71 0xc3f /* GStreamer bits: FL FR FC LFE1 RL RR SL SR */
enum { I_FL, I_FR, I_FC, I_LFE, I_BL, I_BR, I_SL, I_SR };

static void fill_input71(float *in8, guint64 offset, int only) {
  for (int i = 0; i < FRAMES; i++) {
    double t = (double)(offset + (guint64)i) / RATE;
    for (int c = 0; c < 8; c++)
      in8[i * 8 + c] = (only < 0 || only == c) ? 0.08f * (float)sin(2 * M_PI * (170.0 + 130.0 * c) * t) : 0.0f;
  }
}

/* Pro Logic II with 7.1 input: the back pair is folded into the surrounds */
static void test_plii71(void) {
  Harness h;
  harness_init_ch(&h, 8, MASK71, "mode=plii");
  surr_plii ref;
  surr_plii_init(&ref, RATE, 0.5f, 0.70710678f);
  float in[FRAMES * 8], six[FRAMES * 6], out[FRAMES * 2], want[FRAMES * 2];
  double worst = 0;
  for (int b = 0; b < 30; b++) {
    fill_input71(in, h.offset, -1);
    for (int i = 0; i < FRAMES; i++) {
      const float *c = in + i * 8;
      float *d = six + i * 6;
      d[SURR_FL] = c[I_FL]; d[SURR_FR] = c[I_FR]; d[SURR_FC] = c[I_FC]; d[SURR_LFE] = c[I_LFE];
      d[SURR_SL] = c[I_SL] + c[I_BL]; d[SURR_SR] = c[I_SR] + c[I_BR];
    }
    surr_plii_process(&ref, six, want, FRAMES);
    harness_run(&h, in, out);
    worst = fmax(worst, max_diff(out, want, FRAMES * 2));
  }
  printf("plii 7.1: max difference to folded 5.1 reference %.2g\n", worst);
  CHECK(worst < 1e-6, "plii 7.1 output differs");
  harness_free(&h);
}

/* lakehp fed like the element feeds it in 7.1 (our order FL FR FC LFE SL SR BL BR) */
typedef struct {
  lhp_engine e;
  float out[(LHP_BLOCK * 2 + FRAMES) * 2];
  size_t fill;
} Reference71;

static int ref71_load(Reference71 *r, const uint8_t *dll, size_t dll_len, int room, float sur, float rear, int init) {
  static const int8_t map[LHP_NPOS] = {SURR_FL, SURR_FC, SURR_FR, SURR_SL, SURR_SR, SURR_LFE};
  const uint8_t *blob;
  size_t blen;
  if (lhp_dll_find(dll, dll_len, RATE, room, &blob, &blen)) return -1;
  if (init) {
    lhp_init(&r->e);
    lhp_set_map(&r->e, map, 8);
    lhp_set_back(&r->e, 6, 7);
    memset(r->out, 0, sizeof(r->out));
    r->fill = LHP_BLOCK;
  }
  lhp_surround_t ps, pr;
  lhp_surround_for_azimuth(sur, &ps);
  lhp_surround_for_azimuth(rear > 0 ? rear : sur, &pr);
  lhp_set_placement(&r->e, &ps, &pr);
  return lhp_load_filter(&r->e, blob, blen, 1.0f);
}

static void ref71_run(Reference71 *r, const float *in8, float *want) {
  static float ours[FRAMES * 8], block[LHP_BLOCK * 2];
  for (int i = 0; i < FRAMES; i++) {
    const float *c = in8 + i * 8;
    float *d = ours + i * 8;
    d[SURR_FL] = c[I_FL]; d[SURR_FR] = c[I_FR]; d[SURR_FC] = c[I_FC]; d[SURR_LFE] = c[I_LFE];
    d[SURR_SL] = c[I_SL]; d[SURR_SR] = c[I_SR]; d[6] = c[I_BL]; d[7] = c[I_BR];
    for (int k = 0; k < 8; k++) d[k] *= 32768.0f;
  }
  for (int done = 0; done < FRAMES;) {
    int n = LHP_BLOCK - r->e.fill;
    if (n > FRAMES - done) n = FRAMES - done;
    if (lhp_push_f32(&r->e, ours + done * 8, n, block) == LHP_BLOCK) {
      for (int i = 0; i < LHP_BLOCK * 2; i++) r->out[r->fill * 2 + i] = block[i] / 32768.0f;
      r->fill += LHP_BLOCK;
    }
    done += n;
  }
  for (int i = 0; i < FRAMES * 2; i++) want[i] = 0.70710678f * r->out[i];
  r->fill -= FRAMES;
  memmove(r->out, r->out + FRAMES * 2, r->fill * 2 * sizeof(float));
}

static void test_dolby_headphone71(const char *dll_path) {
  size_t dll_len;
  uint8_t *dll = read_file(dll_path, &dll_len);
  if (!dll) {
    printf("FAIL: unable to read %s\n", dll_path);
    failures++;
    return;
  }

  /* own placements: surrounds at 136, rear pair at 150, then the rear moved to 165 while playing */
  gchar *props = g_strdup_printf("mode=dolby-headphone dh-room=2 dh-surround-angle=136 dh-rear-angle=150 dh-dll=\"%s\"",
                                 dll_path);
  Harness h;
  harness_init_ch(&h, 8, MASK71, props);
  g_free(props);
  static Reference71 ref;
  CHECK(ref71_load(&ref, dll, dll_len, 2, 136, 150, 1) == 0, "reference load");
  CHECK(ref.e.back_own, "rear pair should have its own filters");

  float in[FRAMES * 8], out[FRAMES * 2], want[FRAMES * 2];
  double worst = 0;
  for (int b = 0; b < 60; b++) {
    if (b == 30) {
      g_object_set(h.surround, "dh-rear-angle", 165.0f, NULL);
      CHECK(ref71_load(&ref, dll, dll_len, 2, 136, 165, 0) == 0, "reference reload");
    }
    fill_input71(in, h.offset, -1);
    ref71_run(&ref, in, want);
    harness_run(&h, in, out);
    worst = fmax(worst, max_diff(out, want, FRAMES * 2));
  }
  printf("dolby headphone 7.1: surrounds 136, rear 150 then 165: max difference to lakehp %.2g\n", worst);
  CHECK(worst < 1e-6, "7.1 output differs from lakehp");
  CHECK(!h.warned, "unexpected warning");
  harness_free(&h);

  /* default angles: the back pair sounds exactly like the same signal on the 5.1 surrounds */
  props = g_strdup_printf("mode=dolby-headphone dh-dll=\"%s\"", dll_path);
  Harness h8, h6;
  harness_init_ch(&h8, 8, MASK71, props);
  harness_init(&h6, 0x3f, props);
  g_free(props);
  float in6[FRAMES * 6], out6[FRAMES * 2];
  worst = 0;
  for (int b = 0; b < 30; b++) {
    memset(in, 0, sizeof(in));
    memset(in6, 0, sizeof(in6));
    for (int i = 0; i < FRAMES; i++) {
      double t = (double)(h8.offset + (guint64)i) / RATE;
      in[i * 8 + I_BL] = in6[i * 6 + SURR_SL] = 0.1f * (float)sin(2 * M_PI * 330 * t);
      in[i * 8 + I_BR] = in6[i * 6 + SURR_SR] = 0.1f * (float)sin(2 * M_PI * 520 * t);
    }
    harness_run(&h8, in, out);
    harness_run(&h6, in6, out6);
    worst = fmax(worst, max_diff(out, out6, FRAMES * 2));
  }
  printf("dolby headphone 7.1 default angles: back pair vs 5.1 surrounds %.2g\n", worst);
  CHECK(worst == 0, "default rear pair differs from the surrounds");
  harness_free(&h8);
  harness_free(&h6);

  /* 5.1 content in a 7.1 stream (sides silent, sound on the backs) with a separate rear
   * angle: plays at the surround angle, exactly like 5.1 input */
  props = g_strdup_printf("mode=dolby-headphone dh-surround-angle=136 dh-rear-angle=160 dh-dll=\"%s\"", dll_path);
  harness_init_ch(&h8, 8, MASK71, props);
  harness_init(&h6, 0x3f, props);
  g_free(props);
  worst = 0;
  for (int b = 0; b < 30; b++) {
    memset(in, 0, sizeof(in));
    memset(in6, 0, sizeof(in6));
    for (int i = 0; i < FRAMES; i++) {
      double t = (double)(h8.offset + (guint64)i) / RATE;
      in[i * 8 + I_FL] = in6[i * 6 + SURR_FL] = 0.1f * (float)sin(2 * M_PI * 250 * t);
      in[i * 8 + I_BL] = in6[i * 6 + SURR_SL] = 0.1f * (float)sin(2 * M_PI * 330 * t);
      in[i * 8 + I_BR] = in6[i * 6 + SURR_SR] = 0.1f * (float)sin(2 * M_PI * 520 * t);
    }
    harness_run(&h8, in, out);
    harness_run(&h6, in6, out6);
    worst = fmax(worst, max_diff(out, out6, FRAMES * 2));
  }
  printf("dolby headphone 7.1 carrying 5.1 (sides silent): vs 5.1 input %.2g\n", worst);
  CHECK(worst == 0, "5.1 content in 7.1 is not played at the surround angle");
  harness_free(&h8);
  harness_free(&h6);

  /* the same with dh-rear-auto=false: the backs keep the rear placement */
  props = g_strdup_printf("mode=dolby-headphone dh-rear-auto=false dh-surround-angle=136 dh-rear-angle=160 dh-dll=\"%s\"",
                          dll_path);
  harness_init_ch(&h8, 8, MASK71, props);
  harness_init(&h6, 0x3f, props);
  g_free(props);
  worst = 0;
  for (int b = 0; b < 30; b++) {
    memset(in, 0, sizeof(in));
    memset(in6, 0, sizeof(in6));
    for (int i = 0; i < FRAMES; i++) {
      double t = (double)(h8.offset + (guint64)i) / RATE;
      in[i * 8 + I_BL] = in6[i * 6 + SURR_SL] = 0.1f * (float)sin(2 * M_PI * 330 * t);
    }
    harness_run(&h8, in, out);
    harness_run(&h6, in6, out6);
    worst = fmax(worst, max_diff(out, out6, FRAMES * 2));
  }
  printf("dolby headphone 7.1, rear auto off: backs differ from the surrounds by %.3f\n", worst);
  CHECK(worst > 1e-3, "dh-rear-auto=false did not keep the rear placement");
  harness_free(&h8);
  harness_free(&h6);
  g_free(dll);
}

int main(int argc, char **argv) {
  if (argc < 2 || argc > 3) {
    fprintf(stderr, "usage: %s <plugin dir> [DolbyHph.dll]\n", argv[0]);
    return 2;
  }

  gst_init(NULL, NULL);
  if (!gst_registry_scan_path(gst_registry_get(), argv[1]) && !gst_element_factory_find("nekosurround")) {
    printf("FAIL: nekosurround not found in %s\n", argv[1]);
    return 1;
  }

  test_plii(0x3f, "5.1 rear");
  test_plii(0xc0f, "5.1 side"); /* GStreamer bits: FL FR FC LFE1 SL SR */

  gchar *not_dll = g_strdup_printf("dh-dll=\"%s\"", argv[0]);
  test_dolby_headphone_fallback("dh-dll=/nonexistent/DolbyHph.dll", 1, "missing DLL");
  test_dolby_headphone_fallback(not_dll, 1, "not a DLL");
  g_free(not_dll);

  test_plii71();

  if (argc == 3 && argv[2][0]) {
    test_dolby_headphone(argv[2]);
    test_dolby_headphone71(argv[2]);
  } else {
    printf("dolby headphone: skipped, pass DolbyHph.dll to test it\n");
  }

  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("all element tests passed\n");
  return 0;
}
