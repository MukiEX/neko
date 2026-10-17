/*
 * nekosurround: GStreamer element turning 5.1 audio into stereo for neko audio tracks.
 *
 *   mode=plii             Dolby Pro Logic II matrix encode (decodable back to 5.1 by any
 *                         PLII receiver, and plays fine as plain stereo)
 *   mode=dolby-headphone  Dolby Headphone virtualization: the lakehp engine (a C port of
 *                         the Lake/Dolby Headphone engine) with the room filters read from
 *                         the server owner's DolbyHph.dll
 *   mode=stereo           plain ITU downmix (also the fallback when Dolby Headphone is
 *                         unavailable, e.g. no DLL or an unsupported sample rate)
 *
 * Example:
 *   pulsesrc device=audio_output.monitor ! audio/x-raw,channels=6 ! audioconvert !
 *   nekosurround mode=plii ! audioconvert ! opusenc ! ...
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <string.h>

#include <gst/audio/audio.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>

#include "lakehp.h"
#include "lhp_dll.h"
#include "surround_dsp.h"

#ifndef PACKAGE
#define PACKAGE "neko-surround"
#endif
#ifndef VERSION
#define VERSION "1.0.0"
#endif

GST_DEBUG_CATEGORY_STATIC(neko_surround_debug);
#define GST_CAT_DEFAULT neko_surround_debug

typedef enum {
  NEKO_SURROUND_MODE_PLII,
  NEKO_SURROUND_MODE_DOLBY_HEADPHONE,
  NEKO_SURROUND_MODE_STEREO,
} NekoSurroundMode;

#define DEFAULT_MODE NEKO_SURROUND_MODE_PLII
#define DEFAULT_LFE_LEVEL 0.5f
#define DEFAULT_GAIN 0.70710678f
#define DEFAULT_DH_DLL "/etc/neko/dolby/DolbyHph.dll"
#define DEFAULT_DH_ROOM 1
#define DEFAULT_DH_SURROUND_ANGLE LHP_AZIMUTH_FACTORY
#define DEFAULT_DH_REAR_ANGLE 0.0f /* 0 = same as the surround angle */
#define DEFAULT_DH_REAR_AUTO TRUE
#define SIDE_SILENCE 1e-6f        /* peak below this counts as silent */
#define SIDE_SILENCE_SECONDS 2    /* sides silent this long: 5.1 content */

/* internal channel order: the 5.1 order of surround_dsp.h plus the 7.1 back pair */
enum { CH_BL = SURR_CHANNELS, CH_BR, CH_COUNT };

/* lakehp works at int16 scale */
#define LHP_SCALE 32768.0f

enum {
  PROP_0,
  PROP_MODE,
  PROP_LFE_LEVEL,
  PROP_GAIN,
  PROP_DH_DLL,
  PROP_DH_ROOM,
  PROP_DH_SURROUND_ANGLE,
  PROP_DH_REAR_ANGLE,
  PROP_DH_REAR_AUTO,
};

#define NEKO_TYPE_SURROUND (neko_surround_get_type())
G_DECLARE_FINAL_TYPE(NekoSurround, neko_surround, NEKO, SURROUND, GstBaseTransform)

struct _NekoSurround {
  GstBaseTransform parent;

  /* properties, protected by the object lock */
  NekoSurroundMode mode;
  gfloat lfe_level;
  gfloat gain;
  gchar *dh_dll;
  gint dh_room;
  gfloat dh_surround_angle;
  gfloat dh_rear_angle;
  gboolean dh_rear_auto;

  /* negotiated */
  gint rate;
  gint channels;           /* input channels, 6 or 8 */
  gint map[CH_COUNT];      /* our channel n is input channel map[n], -1 = silent */
  gfloat *canon;           /* input in our order, CH_COUNT channels */
  gfloat *six;             /* 5.1 for Pro Logic II / stereo: back pair folded into the surrounds */
  gsize scratch_frames;
  gboolean sides_active;   /* 7.1 content: the side pair carries sound */
  guint64 sides_silent;    /* frames the side pair has been silent */

  surr_plii plii;
  surr_stereo stereo;

  /* Dolby Headphone, only touched from the streaming thread */
  lhp_engine *lhp;
  guint8 *dll_data;        /* contents of dh_dll */
  gsize dll_len;
  gchar *dll_loaded_path;  /* path dll_data was read from */
  gint lhp_room;           /* room of the loaded filter, 0 = none */
  gint lhp_rate;           /* rate of the loaded filter */
  gint lhp_channels;       /* input channels the engine was set up for */
  gfloat lhp_sur_angle;    /* placement of the loaded filter */
  gfloat lhp_rear_angle;
  gboolean lhp_failed;     /* last load failed for (dll, room, rate), don't retry per buffer */
  gboolean lhp_active;     /* the previous buffer went through lakehp */
  gfloat lhp_in[LHP_BLOCK * CH_COUNT];
  gfloat lhp_block[LHP_BLOCK * 2];
  gfloat *lhp_out;         /* output FIFO, stereo frames at +-1 scale */
  gsize lhp_out_fill;      /* frames in lhp_out */
  gsize lhp_out_size;      /* capacity in frames */
};

G_DEFINE_TYPE(NekoSurround, neko_surround, GST_TYPE_BASE_TRANSFORM)

#define SINK_CAPS                                                    \
  "audio/x-raw, format = (string) " GST_AUDIO_NE(F32) ", "          \
  "layout = (string) interleaved, rate = (int) [ 8000, 192000 ], " \
  "channels = (int) { 6, 8 }"

#define SRC_CAPS                                                     \
  "audio/x-raw, format = (string) " GST_AUDIO_NE(F32) ", "          \
  "layout = (string) interleaved, rate = (int) [ 8000, 192000 ], " \
  "channels = (int) 2, channel-mask = (bitmask) 0x3"

static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS(SINK_CAPS));
static GstStaticPadTemplate src_template =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS(SRC_CAPS));

static GType neko_surround_mode_get_type(void) {
  static GType type = 0;
  static const GEnumValue values[] = {
      {NEKO_SURROUND_MODE_PLII, "Dolby Pro Logic II matrix encode", "plii"},
      {NEKO_SURROUND_MODE_DOLBY_HEADPHONE, "Dolby Headphone (needs DolbyHph.dll)", "dolby-headphone"},
      {NEKO_SURROUND_MODE_STEREO, "Plain stereo downmix", "stereo"},
      {0, NULL, NULL},
  };
  if (g_once_init_enter(&type)) {
    GType t = g_enum_register_static("NekoSurroundMode", values);
    g_once_init_leave(&type, t);
  }
  return type;
}

/* ------------------------------------------------------------------------------------ */
/* Dolby Headphone (lakehp)                                                             */
/* ------------------------------------------------------------------------------------ */

/* restarts the stream through lakehp: clears its history and primes the output FIFO
 * with one block of silence, which is the engine's latency */
static void lhp_restart(NekoSurround *self) {
  lhp_reset(self->lhp);
  memset(self->lhp_out, 0, LHP_BLOCK * 2 * sizeof(gfloat));
  self->lhp_out_fill = LHP_BLOCK;
}

/* makes sure the filter for the current dll, room and rate is loaded; on failure the
 * caller falls back to the stereo downmix. Warns once per failing configuration. */
static gboolean lhp_ensure(NekoSurround *self) {
  GST_OBJECT_LOCK(self);
  gchar *path = g_strdup(self->dh_dll);
  gint room = self->dh_room;
  gfloat sur_angle = self->dh_surround_angle;
  gfloat rear_angle = self->dh_rear_angle > 0 ? self->dh_rear_angle : sur_angle;
  GST_OBJECT_UNLOCK(self);

  gboolean dll_changed = g_strcmp0(path, self->dll_loaded_path) != 0;
  gboolean layout_same = self->rate == self->lhp_rate && self->channels == self->lhp_channels;
  if (!dll_changed && layout_same && room == self->lhp_room && sur_angle == self->lhp_sur_angle &&
      rear_angle == self->lhp_rear_angle) {
    g_free(path);
    return !self->lhp_failed;
  }

  /* only the room or the placement changed while running: swap the filter in place */
  gboolean swap_room = !dll_changed && layout_same && self->lhp && !self->lhp_failed;

  gchar *reason = NULL;

  if (dll_changed) {
    g_free(self->dll_data);
    self->dll_data = NULL;
    self->dll_len = 0;
    g_free(self->dll_loaded_path);
    self->dll_loaded_path = g_strdup(path);

    GError *error = NULL;
    gchar *data = NULL;
    gsize len = 0;
    if (!g_file_get_contents(path, &data, &len, &error)) {
      reason = g_strdup(error->message);
      g_error_free(error);
    } else {
      self->dll_data = (guint8 *)data;
      self->dll_len = len;
    }
  }

  if (!reason && !self->dll_data) reason = g_strdup_printf("unable to read %s", path);

  const uint8_t *blob = NULL;
  size_t blob_len = 0;
  if (!reason && lhp_dll_find(self->dll_data, self->dll_len, (uint32_t)self->rate, room, &blob, &blob_len)) {
    lhp_dll_filter filters[64];
    int n = lhp_dll_scan(self->dll_data, self->dll_len, filters, 64);
    if (n < 0)
      reason = g_strdup_printf("%s is not a DLL", path);
    else if (n == 0)
      reason = g_strdup_printf("no Dolby Headphone filters in %s, is it DolbyHph.dll?", path);
    else
      reason = g_strdup_printf("%s has no DH%d filter for %d Hz (rooms exist at 48000, 44100, 32000, 22050, "
                               "16000, 11025 and 8000 Hz)",
                               path, room, self->rate);
  }

  if (!reason) {
    if (!self->lhp) self->lhp = g_malloc0(sizeof(lhp_engine));

    /* our channel order is FL FR FC LFE SL SR BL BR, lakehp positions are L C R Ls Rs LFE
     * plus the 7.1 rear pair */
    static const int8_t map[LHP_NPOS] = {SURR_FL, SURR_FC, SURR_FR, SURR_SL, SURR_SR, SURR_LFE};
    if (!swap_room) {
      lhp_init(self->lhp);
      lhp_set_map(self->lhp, map, CH_COUNT);
      if (self->channels == 8) lhp_set_back(self->lhp, CH_BL, CH_BR);
    }

    lhp_surround_t sur, rear;
    lhp_surround_for_azimuth(sur_angle, &sur);
    lhp_surround_for_azimuth(rear_angle, &rear);
    lhp_set_placement(self->lhp, &sur, &rear);

    int r = lhp_load_filter(self->lhp, blob, blob_len, 1.0f);
    if (r) reason = g_strdup_printf("unable to load the DH%d filter from %s (error %d)", room, path, r);
  }

  self->lhp_room = room;
  self->lhp_rate = self->rate;
  self->lhp_channels = self->channels;
  self->lhp_sur_angle = sur_angle;
  self->lhp_rear_angle = rear_angle;
  if (!swap_room) self->lhp_active = FALSE;

  if (reason) {
    self->lhp_failed = TRUE;
    GST_ELEMENT_WARNING(self, RESOURCE, FAILED, ("Dolby Headphone unavailable, using stereo downmix"),
                        ("%s", reason));
    g_free(reason);
    g_free(path);
    return FALSE;
  }

  self->lhp_failed = FALSE;
  GST_INFO_OBJECT(self, "Dolby Headphone: %s filter at %d Hz from %s, %d channels, surrounds at %.0f, rear at %.0f degrees%s",
                  self->lhp->info.name, self->rate, path, self->channels, sur_angle, rear_angle,
                  self->lhp->back_own ? " (own filters)" : "");
  g_free(path);
  return TRUE;
}

static void lhp_process(NekoSurround *self, const gfloat *in8, gfloat *out2, gsize frames) {
  /* the FIFO holds at most the primed block plus whatever this buffer completes */
  gsize need = 2 * LHP_BLOCK + frames;
  if (need > self->lhp_out_size) {
    self->lhp_out = g_renew(gfloat, self->lhp_out, need * 2);
    self->lhp_out_size = need;
  }

  if (!self->lhp_active) {
    lhp_restart(self);
    self->lhp_active = TRUE;
  }

  for (gsize done = 0; done < frames;) {
    int space = LHP_BLOCK - self->lhp->fill;
    int n = (int)MIN((gsize)space, frames - done);

    for (gsize i = 0; i < (gsize)n * CH_COUNT; i++) self->lhp_in[i] = in8[done * CH_COUNT + i] * LHP_SCALE;

    if (lhp_push_f32(self->lhp, self->lhp_in, n, self->lhp_block) == LHP_BLOCK) {
      gfloat *dst = self->lhp_out + self->lhp_out_fill * 2;
      for (gsize i = 0; i < LHP_BLOCK * 2; i++) dst[i] = self->lhp_block[i] / LHP_SCALE;
      self->lhp_out_fill += LHP_BLOCK;
    }
    done += (gsize)n;
  }

  /* the primed block guarantees enough output */
  memcpy(out2, self->lhp_out, frames * 2 * sizeof(gfloat));
  self->lhp_out_fill -= frames;
  memmove(self->lhp_out, self->lhp_out + frames * 2, self->lhp_out_fill * 2 * sizeof(gfloat));
}

/* ------------------------------------------------------------------------------------ */
/* GstBaseTransform                                                                     */
/* ------------------------------------------------------------------------------------ */

static GstCaps *neko_surround_transform_caps(GstBaseTransform *trans, GstPadDirection direction, GstCaps *caps,
                                             GstCaps *filter) {
  GstCaps *result = gst_caps_new_empty();

  for (guint i = 0; i < gst_caps_get_size(caps); i++) {
    GstStructure *s = gst_structure_copy(gst_caps_get_structure(caps, i));

    if (direction == GST_PAD_SINK) {
      gst_structure_set(s, "channels", G_TYPE_INT, 2, "channel-mask", GST_TYPE_BITMASK, (guint64)0x3, NULL);
    } else {
      GValue list = G_VALUE_INIT, v = G_VALUE_INIT;
      g_value_init(&list, GST_TYPE_LIST);
      g_value_init(&v, G_TYPE_INT);
      g_value_set_int(&v, 6);
      gst_value_list_append_value(&list, &v);
      g_value_set_int(&v, 8);
      gst_value_list_append_value(&list, &v);
      gst_structure_take_value(s, "channels", &list);
      g_value_unset(&v);
      gst_structure_remove_field(s, "channel-mask");
    }

    result = gst_caps_merge_structure(result, s);
  }

  if (filter) {
    GstCaps *tmp = gst_caps_intersect_full(filter, result, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(result);
    result = tmp;
  }

  GST_DEBUG_OBJECT(trans, "transformed %" GST_PTR_FORMAT " into %" GST_PTR_FORMAT, caps, result);
  return result;
}

static gboolean neko_surround_transform_size(GstBaseTransform *trans, GstPadDirection direction, GstCaps *caps,
                                             gsize size, GstCaps *othercaps, gsize *othersize) {
  (void)trans;

  /* 6 or 8 float channels in, 2 float channels out */
  gint channels = 0;
  GstCaps *sink_caps = direction == GST_PAD_SINK ? caps : othercaps;
  if (!sink_caps || gst_caps_is_empty(sink_caps) ||
      !gst_structure_get_int(gst_caps_get_structure(sink_caps, 0), "channels", &channels) ||
      (channels != 6 && channels != 8))
    return FALSE;
  const gsize in_bpf = (gsize)channels * sizeof(gfloat);
  const gsize out_bpf = 2 * sizeof(gfloat);

  if (direction == GST_PAD_SINK)
    *othersize = size / in_bpf * out_bpf;
  else
    *othersize = size / out_bpf * in_bpf;
  return TRUE;
}

/* our channel for an input position; in 5.1 rear and side surrounds are both "the
 * surrounds", in 7.1 the sides are the surrounds and the rears the back pair */
static gint position_to_channel(GstAudioChannelPosition pos, gint channels) {
  switch (pos) {
    case GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT:
      return SURR_FL;
    case GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT:
      return SURR_FR;
    case GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER:
      return SURR_FC;
    case GST_AUDIO_CHANNEL_POSITION_LFE1:
      return SURR_LFE;
    case GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT:
      return SURR_SL;
    case GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT:
      return SURR_SR;
    case GST_AUDIO_CHANNEL_POSITION_REAR_LEFT:
      return channels == 8 ? CH_BL : SURR_SL;
    case GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT:
      return channels == 8 ? CH_BR : SURR_SR;
    default:
      return -1;
  }
}

static gboolean neko_surround_set_caps(GstBaseTransform *trans, GstCaps *incaps, GstCaps *outcaps) {
  NekoSurround *self = NEKO_SURROUND(trans);
  GstAudioInfo info;
  (void)outcaps;

  if (!gst_audio_info_from_caps(&info, incaps)) return FALSE;

  gint channels = GST_AUDIO_INFO_CHANNELS(&info);
  if (channels != 6 && channels != 8) return FALSE;

  /* default (unpositioned) orders: 5.1 FL FR FC LFE SL SR, 7.1 FL FR FC LFE BL BR SL SR */
  static const gint def51[CH_COUNT] = {0, 1, 2, 3, 4, 5, -1, -1};
  static const gint def71[CH_COUNT] = {0, 1, 2, 3, 6, 7, 4, 5};
  const gint *def = channels == 8 ? def71 : def51;

  gint map[CH_COUNT];
  for (gint c = 0; c < CH_COUNT; c++) map[c] = def[c];

  if (!GST_AUDIO_INFO_IS_UNPOSITIONED(&info)) {
    gint found[CH_COUNT];
    for (gint c = 0; c < CH_COUNT; c++) found[c] = -1;
    gboolean ok = TRUE;
    for (gint i = 0; i < channels && ok; i++) {
      gint ch = position_to_channel(info.position[i], channels);
      if (ch < 0 || found[ch] >= 0)
        ok = FALSE;
      else
        found[ch] = i;
    }
    if (ok) {
      for (gint c = 0; c < CH_COUNT; c++) map[c] = found[c];
    } else {
      GST_WARNING_OBJECT(self, "unexpected channel layout %" GST_PTR_FORMAT ", assuming %s", incaps,
                         channels == 8 ? "FL FR FC LFE BL BR SL SR" : "FL FR FC LFE SL SR");
    }
  }

  for (gint c = 0; c < CH_COUNT; c++) self->map[c] = map[c];
  self->channels = channels;

  gint rate = GST_AUDIO_INFO_RATE(&info);
  if (rate != self->rate) {
    self->rate = rate;
    surr_plii_init(&self->plii, rate, self->lfe_level, self->gain);
    surr_stereo_init(&self->stereo, rate, self->lfe_level, self->gain);
    /* lhp_ensure() picks the filter for the new rate on the next buffer */
  }

  GST_INFO_OBJECT(self, "configured for %d Hz, %d channels, map %d %d %d %d %d %d %d %d", rate, channels, map[0],
                  map[1], map[2], map[3], map[4], map[5], map[6], map[7]);
  return TRUE;
}

static GstFlowReturn neko_surround_transform(GstBaseTransform *trans, GstBuffer *inbuf, GstBuffer *outbuf) {
  NekoSurround *self = NEKO_SURROUND(trans);
  GstMapInfo in, out;

  if (!gst_buffer_map(inbuf, &in, GST_MAP_READ)) return GST_FLOW_ERROR;
  if (!gst_buffer_map(outbuf, &out, GST_MAP_WRITE)) {
    gst_buffer_unmap(inbuf, &in);
    return GST_FLOW_ERROR;
  }

  const gint channels = self->channels;
  gsize frames = in.size / ((gsize)channels * sizeof(gfloat));
  const gfloat *src = (const gfloat *)in.data;

  if (self->scratch_frames < frames) {
    self->canon = g_renew(gfloat, self->canon, frames * CH_COUNT);
    self->six = g_renew(gfloat, self->six, frames * SURR_CHANNELS);
    self->scratch_frames = frames;
  }

  GST_OBJECT_LOCK(self);
  NekoSurroundMode mode = self->mode;
  GST_OBJECT_UNLOCK(self);

  /* our order: FL FR FC LFE SL SR BL BR, missing channels silent */
  gfloat *canon = self->canon;
  for (gsize f = 0; f < frames; f++)
    for (gint c = 0; c < CH_COUNT; c++)
      canon[f * CH_COUNT + c] = self->map[c] >= 0 ? src[f * channels + self->map[c]] : 0.0f;

  /* 5.1 content in a 7.1 stream (players often label 5.1 surrounds "rear"): while
   * the sides are silent, the back pair plays through the surround speakers */
  if (channels == 8 && mode == NEKO_SURROUND_MODE_DOLBY_HEADPHONE) {
    gfloat peak = 0.0f;
    for (gsize f = 0; f < frames; f++) {
      peak = MAX(peak, fabsf(canon[f * CH_COUNT + SURR_SL]));
      peak = MAX(peak, fabsf(canon[f * CH_COUNT + SURR_SR]));
    }
    gboolean was = self->sides_active;
    if (peak > SIDE_SILENCE) {
      self->sides_active = TRUE;
      self->sides_silent = 0;
    } else {
      self->sides_silent += frames;
      if (self->sides_silent >= (guint64)self->rate * SIDE_SILENCE_SECONDS) self->sides_active = FALSE;
    }
    if (was != self->sides_active)
      GST_INFO_OBJECT(self, "%s", self->sides_active ? "side channels active: 7.1 placement"
                                                     : "side channels silent: back channels play as surrounds");

    GST_OBJECT_LOCK(self);
    gboolean rear_auto = self->dh_rear_auto;
    GST_OBJECT_UNLOCK(self);

    if (rear_auto && !self->sides_active)
      for (gsize f = 0; f < frames; f++) {
        gfloat *c8 = canon + f * CH_COUNT;
        c8[SURR_SL] += c8[CH_BL];
        c8[SURR_SR] += c8[CH_BR];
        c8[CH_BL] = c8[CH_BR] = 0.0f;
      }
  }

  /* Pro Logic II and stereo have no back pair: fold it into the surrounds */
  if (mode != NEKO_SURROUND_MODE_DOLBY_HEADPHONE) {
    for (gsize f = 0; f < frames; f++) {
      const gfloat *c8 = canon + f * CH_COUNT;
      gfloat *c6 = self->six + f * SURR_CHANNELS;
      for (gint c = 0; c < SURR_CHANNELS; c++) c6[c] = c8[c];
      c6[SURR_SL] += c8[CH_BL];
      c6[SURR_SR] += c8[CH_BR];
    }
  }
  const gfloat *six = self->six;

  gfloat *dst = (gfloat *)out.data;

  gboolean used_lhp = FALSE;

  switch (mode) {
    case NEKO_SURROUND_MODE_PLII:
      surr_plii_process(&self->plii, six, dst, frames);
      break;

    case NEKO_SURROUND_MODE_DOLBY_HEADPHONE:
      if (lhp_ensure(self)) {
        lhp_process(self, canon, dst, frames);
        for (gsize i = 0; i < frames * 2; i++) dst[i] *= self->gain;
        surr_limiter_process(&self->stereo.limiter, dst, frames);
        used_lhp = TRUE;
        break;
      }
      /* no DLL, unsupported rate...: plain stereo downmix */
      for (gsize f = 0; f < frames; f++) {
        const gfloat *c8 = canon + f * CH_COUNT;
        gfloat *c6 = self->six + f * SURR_CHANNELS;
        for (gint c = 0; c < SURR_CHANNELS; c++) c6[c] = c8[c];
        c6[SURR_SL] += c8[CH_BL];
        c6[SURR_SR] += c8[CH_BR];
      }
      /* fall through */
    case NEKO_SURROUND_MODE_STEREO:
      surr_stereo_process(&self->stereo, six, dst, frames);
      break;
  }

  /* leaving Dolby Headphone: start over (with a primed FIFO) when it is used again */
  if (!used_lhp) self->lhp_active = FALSE;

  gst_buffer_unmap(outbuf, &out);
  gst_buffer_unmap(inbuf, &in);
  return GST_FLOW_OK;
}

static gboolean neko_surround_start(GstBaseTransform *trans) {
  NekoSurround *self = NEKO_SURROUND(trans);
  self->rate = 0;
  self->lhp_active = FALSE;
  /* re-read the DLL on every start, it may have been replaced */
  g_free(self->dll_loaded_path);
  self->dll_loaded_path = NULL;
  self->lhp_room = 0;
  self->lhp_failed = FALSE;
  return TRUE;
}

static gboolean neko_surround_stop(GstBaseTransform *trans) {
  NekoSurround *self = NEKO_SURROUND(trans);

  g_free(self->lhp);
  self->lhp = NULL;
  g_free(self->dll_data);
  self->dll_data = NULL;
  self->dll_len = 0;
  g_free(self->dll_loaded_path);
  self->dll_loaded_path = NULL;
  g_free(self->lhp_out);
  self->lhp_out = NULL;
  self->lhp_out_size = 0;
  self->lhp_out_fill = 0;
  self->lhp_room = 0;

  g_free(self->canon);
  self->canon = NULL;
  g_free(self->six);
  self->six = NULL;
  self->scratch_frames = 0;
  return TRUE;
}

/* the Dolby Headphone engine delays by one block */
static gboolean neko_surround_query(GstBaseTransform *trans, GstPadDirection direction, GstQuery *query) {
  NekoSurround *self = NEKO_SURROUND(trans);

  gboolean res = GST_BASE_TRANSFORM_CLASS(neko_surround_parent_class)->query(trans, direction, query);

  if (res && GST_QUERY_TYPE(query) == GST_QUERY_LATENCY && direction == GST_PAD_SRC) {
    GST_OBJECT_LOCK(self);
    gboolean dh = self->mode == NEKO_SURROUND_MODE_DOLBY_HEADPHONE;
    GST_OBJECT_UNLOCK(self);

    if (dh && self->rate > 0) {
      gboolean live;
      GstClockTime min, max;
      gst_query_parse_latency(query, &live, &min, &max);
      GstClockTime ours = gst_util_uint64_scale_int(LHP_BLOCK, GST_SECOND, self->rate);
      min += ours;
      if (GST_CLOCK_TIME_IS_VALID(max)) max += ours;
      gst_query_set_latency(query, live, min, max);
    }
  }

  return res;
}

/* ------------------------------------------------------------------------------------ */
/* GObject                                                                              */
/* ------------------------------------------------------------------------------------ */

static void neko_surround_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
  NekoSurround *self = NEKO_SURROUND(object);

  GST_OBJECT_LOCK(self);
  switch (prop_id) {
    case PROP_MODE:
      self->mode = g_value_get_enum(value);
      break;
    case PROP_LFE_LEVEL:
      self->lfe_level = g_value_get_float(value);
      self->plii.lfe_level = self->lfe_level;
      self->stereo.lfe_level = self->lfe_level;
      break;
    case PROP_GAIN:
      self->gain = g_value_get_float(value);
      self->plii.gain = self->gain;
      self->stereo.gain = self->gain;
      break;
    case PROP_DH_DLL:
      g_free(self->dh_dll);
      self->dh_dll = g_value_dup_string(value);
      break;
    case PROP_DH_ROOM:
      self->dh_room = g_value_get_int(value);
      break;
    case PROP_DH_SURROUND_ANGLE:
      self->dh_surround_angle = g_value_get_float(value);
      break;
    case PROP_DH_REAR_ANGLE:
      self->dh_rear_angle = g_value_get_float(value);
      break;
    case PROP_DH_REAR_AUTO:
      self->dh_rear_auto = g_value_get_boolean(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK(self);
}

static void neko_surround_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
  NekoSurround *self = NEKO_SURROUND(object);

  GST_OBJECT_LOCK(self);
  switch (prop_id) {
    case PROP_MODE:
      g_value_set_enum(value, self->mode);
      break;
    case PROP_LFE_LEVEL:
      g_value_set_float(value, self->lfe_level);
      break;
    case PROP_GAIN:
      g_value_set_float(value, self->gain);
      break;
    case PROP_DH_DLL:
      g_value_set_string(value, self->dh_dll);
      break;
    case PROP_DH_ROOM:
      g_value_set_int(value, self->dh_room);
      break;
    case PROP_DH_SURROUND_ANGLE:
      g_value_set_float(value, self->dh_surround_angle);
      break;
    case PROP_DH_REAR_ANGLE:
      g_value_set_float(value, self->dh_rear_angle);
      break;
    case PROP_DH_REAR_AUTO:
      g_value_set_boolean(value, self->dh_rear_auto);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK(self);
}

static void neko_surround_finalize(GObject *object) {
  NekoSurround *self = NEKO_SURROUND(object);
  g_free(self->dh_dll);
  g_free(self->canon);
  g_free(self->six);
  g_free(self->lhp);
  g_free(self->dll_data);
  g_free(self->dll_loaded_path);
  g_free(self->lhp_out);
  G_OBJECT_CLASS(neko_surround_parent_class)->finalize(object);
}

static void neko_surround_init(NekoSurround *self) {
  self->mode = DEFAULT_MODE;
  self->lfe_level = DEFAULT_LFE_LEVEL;
  self->gain = DEFAULT_GAIN;
  self->dh_dll = g_strdup(DEFAULT_DH_DLL);
  self->dh_room = DEFAULT_DH_ROOM;
  self->dh_surround_angle = DEFAULT_DH_SURROUND_ANGLE;
  self->dh_rear_angle = DEFAULT_DH_REAR_ANGLE;
  self->dh_rear_auto = DEFAULT_DH_REAR_AUTO;
  self->sides_silent = G_MAXUINT64 / 2; /* until the sides carry sound, assume 5.1 content */
  self->channels = 6;
  for (gint c = 0; c < CH_COUNT; c++) self->map[c] = c < SURR_CHANNELS ? c : -1;

  surr_plii_init(&self->plii, 48000, self->lfe_level, self->gain);
  surr_stereo_init(&self->stereo, 48000, self->lfe_level, self->gain);
}

static void neko_surround_class_init(NekoSurroundClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS(klass);

  gobject_class->set_property = neko_surround_set_property;
  gobject_class->get_property = neko_surround_get_property;
  gobject_class->finalize = neko_surround_finalize;

  g_object_class_install_property(
      gobject_class, PROP_MODE,
      g_param_spec_enum("mode", "Mode", "How 5.1 is turned into stereo", neko_surround_mode_get_type(),
                        DEFAULT_MODE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING));
  g_object_class_install_property(
      gobject_class, PROP_LFE_LEVEL,
      g_param_spec_float("lfe-level", "LFE level",
                         "Linear gain of the LFE channel mixed into both outputs (Pro Logic II and stereo; Dolby "
                         "Headphone mixes LFE itself)",
                         0.0f, 2.0f, DEFAULT_LFE_LEVEL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(
      gobject_class, PROP_GAIN,
      g_param_spec_float("gain", "Gain", "Linear output gain (a peak limiter keeps it from clipping)", 0.0f, 4.0f,
                         DEFAULT_GAIN, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(
      gobject_class, PROP_DH_DLL,
      g_param_spec_string("dh-dll", "DolbyHph.dll",
                          "DolbyHph.dll provided by the server owner, the room filters are read from it",
                          DEFAULT_DH_DLL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(
      gobject_class, PROP_DH_ROOM,
      g_param_spec_int("dh-room", "Room", "Dolby Headphone room: 1 small (DH1), 2 medium (DH2), 3 large (DH3)", 1, 3,
                       DEFAULT_DH_ROOM, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING));

  g_object_class_install_property(
      gobject_class, PROP_DH_SURROUND_ANGLE,
      g_param_spec_float("dh-surround-angle", "Surround angle",
                         "Dolby Headphone: azimuth of the surround speakers (the sides in 7.1), degrees from "
                         "straight ahead; 118 = the filters as made",
                         LHP_AZIMUTH_MIN, LHP_AZIMUTH_MAX, DEFAULT_DH_SURROUND_ANGLE,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING));
  g_object_class_install_property(
      gobject_class, PROP_DH_REAR_ANGLE,
      g_param_spec_float("dh-rear-angle", "Rear angle",
                         "Dolby Headphone: azimuth of the 7.1 rear speakers, degrees from straight ahead; "
                         "0 = same as the surrounds (mixed into them at no extra cost)",
                         0.0f, LHP_AZIMUTH_MAX, DEFAULT_DH_REAR_ANGLE,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING));

  g_object_class_install_property(
      gobject_class, PROP_DH_REAR_AUTO,
      g_param_spec_boolean("dh-rear-auto", "Rear auto",
                           "Dolby Headphone, 7.1 input: while the side channels are silent (5.1 content whose "
                           "surrounds are labelled rear), play the back channels through the surround speakers",
                           DEFAULT_DH_REAR_AUTO,
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING));

  gst_element_class_add_static_pad_template(element_class, &sink_template);
  gst_element_class_add_static_pad_template(element_class, &src_template);
  gst_element_class_set_static_metadata(element_class, "Neko surround downmix", "Filter/Effect/Audio",
                                        "Downmixes 5.1 to stereo with Pro Logic II or Dolby Headphone", "neko");

  trans_class->transform_caps = GST_DEBUG_FUNCPTR(neko_surround_transform_caps);
  trans_class->transform_size = GST_DEBUG_FUNCPTR(neko_surround_transform_size);
  trans_class->set_caps = GST_DEBUG_FUNCPTR(neko_surround_set_caps);
  trans_class->transform = GST_DEBUG_FUNCPTR(neko_surround_transform);
  trans_class->start = GST_DEBUG_FUNCPTR(neko_surround_start);
  trans_class->stop = GST_DEBUG_FUNCPTR(neko_surround_stop);
  trans_class->query = GST_DEBUG_FUNCPTR(neko_surround_query);
}

static gboolean plugin_init(GstPlugin *plugin) {
  GST_DEBUG_CATEGORY_INIT(neko_surround_debug, "nekosurround", 0, "neko surround downmix");
  return gst_element_register(plugin, "nekosurround", GST_RANK_NONE, NEKO_TYPE_SURROUND);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, nekosurround, "5.1 to stereo downmix for neko", plugin_init,
                  VERSION, "LGPL", PACKAGE, "https://github.com/m1k1o/neko")
