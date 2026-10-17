/*
 * lakehp.c - portable C reimplementation of the Lake/Dolby Headphone engine.
 * See lakehp.h for the signal-flow overview. Function-by-function notes
 * reference the original DolbyHph.dll (1.20.0276, x87 code path) addresses.
 *
 * neko copy: adds speaker placement and the 7.1 rear pair as described in the
 * LakeHP technical breakdown (sections 4 and 5). With the factory placement and
 * no rear pair the output is bit-identical to the original port.
 */
#include "lakehp.h"
#include <string.h>
#include <math.h>

#ifndef LHP_PI
#define LHP_PI 3.14159265358979323846
#endif

#ifdef LHP_COEF_HALF
#define CF(x) ((float)(x))
#define TOCF(x) ((lhp_coef_t)(x))
#else
#define CF(x) (x)
#define TOCF(x) (x)
#endif

/* Time-critical functions can be placed in fast RAM on MCUs. */
#ifndef LHP_HOT
#if defined(LHP_PICO_RAMFUNC)                 /* pico-sdk: .time_critical -> SRAM */
#define LHP_HOT __attribute__((section(".time_critical.lhp"), noinline))
#else
#define LHP_HOT
#endif
#endif

/* ------------------------------------------------------------------------ */
/* Tables  (DLL: 0x1000336c rot/win, 0x10004dcc/0x10004ea4 FFT twiddles)     */
/* ------------------------------------------------------------------------ */
static void build_tables(lhp_engine *e)
{
    for (int i = 0; i < LHP_BLOCK; i++) {
        double th = (double)i * LHP_PI * 0.5 / 1024.0;
        float c = (float)(32768.0 * cos(th));
        float s = (float)(32768.0 * sin(th));
        e->rot[i].re = c;
        e->rot[i].im = s;
        e->win[i] = (uint16_t)(int32_t)s;          /* truncation (fistp, RC=chop) */
    }
    e->win[LHP_BLOCK] = 0;
    for (int i = 0; i < LHP_BLOCK / 2; i++) {
        double th = (double)(i + i) * LHP_PI / (double)LHP_BLOCK;
        e->fft_fwd[i].re = (float)cos(th);
        e->fft_fwd[i].im = (float)-sin(th);
        e->fft_inv[i].re = (float)cos(th);
        e->fft_inv[i].im = (float)sin(th);
    }
}

/* ------------------------------------------------------------------------ */
/* FFT: radix-2 DIT, bit-reversed input permutation, natural-order output,  */
/* no scaling.  Same loop structure as DLL 0x10004f7c / 0x1000511c.          */
/* ------------------------------------------------------------------------ */
LHP_HOT void lhp_fft(lhp_cpx *d, const lhp_cpx *tw)
{
    const unsigned n = LHP_BLOCK;
    /* bit reversal */
    for (unsigned i = 0, j = 0; i < n; i++) {
        if (j > i) { lhp_cpx t = d[j]; d[j] = d[i]; d[i] = t; }
        unsigned m = n >> 1;
        while (m & j) m >>= 1;
        j ^= (m - 1) ^ (n - 1);
        /* equivalent to: j = bitrev(i+1) */
    }
    for (unsigned h = 1, s = n >> 1; s; h <<= 1, s >>= 1) {
        for (unsigned j = 0; j < h; j++) {
            const float wr = tw[j * s].re, wi = tw[j * s].im;
            lhp_cpx *p0 = d + j, *p1 = d + j + h;
            for (unsigned g = 0; g < s; g++) {
                float tr = wr * p1->re - wi * p1->im;
                float ti = wi * p1->re + wr * p1->im;
                float ar = p0->re, ai = p0->im;
                p1->re = ar - tr; p1->im = ai - ti;
                p0->re = ar + tr; p0->im = ai + ti;
                p0 += 2 * h; p1 += 2 * h;
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Channel maps (DLL table @0x100a1f80): AC-3 acmod + 8*lfeon               */
/* entries: nch, then input-channel index for L,C,R,Ls,Rs,LFE (-1 none)     */
/* ------------------------------------------------------------------------ */
static const int8_t k_maps[16][7] = {
    {2,  0,-1, 1,-1,-1,-1}, {1, -1, 0,-1,-1,-1,-1}, {2,  0,-1, 1,-1,-1,-1},
    {3,  0, 1, 2,-1,-1,-1}, {3,  0,-1, 1, 2, 2,-1}, {4,  0, 1, 2, 3, 3,-1},
    {4,  0,-1, 1, 2, 3,-1}, {5,  0, 1, 2, 3, 4,-1}, {3,  0,-1, 1,-1,-1, 2},
    {2, -1, 0,-1,-1,-1, 1}, {3,  0,-1, 1,-1,-1, 2}, {4,  0, 1, 2,-1,-1, 3},
    {4,  0,-1, 1, 2, 2, 3}, {5,  0, 1, 2, 3, 3, 4}, {5,  0,-1, 1, 2, 3, 4},
    {6,  0, 1, 2, 3, 4, 5},
};

int lhp_set_map(lhp_engine *e, const int8_t map[LHP_NPOS], int nch)
{
    if (nch < 1 || nch > LHP_MAXCH) return -1;
    for (int p = 0; p < LHP_NPOS; p++) {
        if (map[p] >= nch) return -1;
        e->map[p] = map[p];
    }
    e->nch = nch;
    memset(e->inbuf, 0, sizeof e->inbuf);       /* DLL clears unused channels */
    return 0;
}

int lhp_set_acmod(lhp_engine *e, int cfg)
{
    if (cfg < 0 || cfg > 15) return -1;
    return lhp_set_map(e, &k_maps[cfg][1], k_maps[cfg][0]);
}

void lhp_reset(lhp_engine *e)   /* DLL LakeFlush */
{
    memset(e->hist, 0, sizeof e->hist);
    memset(e->mix, 0, sizeof e->mix);
    memset(e->inbuf, 0, sizeof e->inbuf);
    e->fill = 0;
}

void lhp_init(lhp_engine *e)
{
    memset(e, 0, sizeof *e);
    build_tables(e);
    e->gain = 1.0f;
    e->back[0] = e->back[1] = -1;
    lhp_set_acmod(e, 15);
}

int lhp_set_back(lhp_engine *e, int lb, int rb)
{
    if (lb >= e->nch || rb >= e->nch) return -1;
    e->back[0] = (int8_t)(lb < 0 ? -1 : lb);
    e->back[1] = (int8_t)(rb < 0 ? -1 : rb);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Speaker placement (LakeHP breakdown section 4, additions to the engine)   */
/* ------------------------------------------------------------------------ */
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float round_to(float v, float step) { return (float)(floor(v / step + 0.5) * step); }

int lhp_surround_is_factory(const lhp_surround_t *s)
{
    return s->near_delay == 0.0f && s->far_hf_db == 0.0f && s->back_eq == 0.0f;
}

static int surround_equal(const lhp_surround_t *a, const lhp_surround_t *b)
{
    return a->near_delay == b->near_delay && a->far_hf_db == b->far_hf_db && a->back_eq == b->back_eq;
}

void lhp_surround_for_azimuth(float az, lhp_surround_t *out)
{
    az = clampf(az, LHP_AZIMUTH_MIN, LHP_AZIMUTH_MAX);
    if (az == LHP_AZIMUTH_FACTORY) {              /* the filters as made */
        out->near_delay = out->far_hf_db = out->back_eq = 0.0f;
        return;
    }
    /* Woodworth: ITD = r/c * (theta + sin theta), theta = lateral angle */
    const double r = 0.0875, c = 343.0;
    const double theta = (180.0 - az) * LHP_PI / 180.0;
    const double itd = r / c * (theta + sin(theta));
    float delay = (float)((0.000500 - itd) * 48000.0);
    float hf = (az - LHP_AZIMUTH_FACTORY) * 4.0f / 18.0f;
    float eq = az >= LHP_AZIMUTH_FACTORY ? (az - LHP_AZIMUTH_FACTORY) / 18.0f : (az - LHP_AZIMUTH_FACTORY) / 28.0f;
    out->near_delay = round_to(clampf(delay, -12.0f, 24.0f), 0.1f);
    out->far_hf_db  = round_to(clampf(hf, -6.0f, 12.0f), 0.1f);
    out->back_eq    = round_to(clampf(eq, -1.0f, 2.0f), 0.01f);
}

void lhp_set_placement(lhp_engine *e, const lhp_surround_t *sur, const lhp_surround_t *back)
{
    static const lhp_surround_t factory = {0, 0, 0};
    e->place_sur = sur ? *sur : factory;
    e->place_back = back ? *back : e->place_sur;
}

/* y[n] = (1-f) x[n-D] + f x[n-D-1], D + f = d (linear interpolation) */
static void frac_delay(float *t, double d)
{
    if (d <= 0) return;
    int D = (int)d;
    float f = (float)(d - D);
    for (int n = LHP_BLOCK - 1; n >= 0; n--) {
        float a = n - D >= 0 ? t[n - D] : 0.0f;
        float b = n - D - 1 >= 0 ? t[n - D - 1] : 0.0f;
        t[n] = (1.0f - f) * a + f * b;
    }
}

/* RBJ cookbook biquads, run over the impulse response with zero state */
static void biquad(float *t, double b0, double b1, double b2, double a0, double a1, double a2)
{
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
    for (int n = 0; n < LHP_BLOCK; n++) {
        double x = t[n];
        double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        t[n] = (float)y;
    }
}

static void high_shelf(float *t, double fs, double f0, double gain_db)
{
    if (gain_db == 0 || f0 >= 0.45 * fs) return;
    const double A = pow(10.0, gain_db / 40.0), w = 2 * LHP_PI * f0 / fs;
    const double cw = cos(w), alpha = sin(w) / 2 * sqrt(2.0), sa = 2 * sqrt(A) * alpha;   /* S = 1 */
    biquad(t, A * ((A + 1) + (A - 1) * cw + sa), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sa),
           (A + 1) - (A - 1) * cw + sa, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sa);
}

static void peaking(float *t, double fs, double f0, double q, double gain_db)
{
    if (gain_db == 0 || f0 >= 0.45 * fs) return;
    const double A = pow(10.0, gain_db / 40.0), w = 2 * LHP_PI * f0 / fs;
    const double cw = cos(w), alpha = sin(w) / (2 * q);
    biquad(t, 1 + alpha * A, -2 * cw, 1 - alpha * A, 1 + alpha / A, -2 * cw, 1 - alpha / A);
}

/* placement on one ear of one surround HRTF */
static void place_hrtf(float *t, int is_near, const lhp_surround_t *pl, double fs)
{
    const double d = pl->near_delay * fs / 48000.0;
    if (is_near && d > 0) frac_delay(t, d);
    if (!is_near && d < 0) frac_delay(t, -d);
    if (!is_near) high_shelf(t, fs, 2000.0, pl->far_hf_db);
    if (pl->back_eq != 0) {
        peaking(t, fs, 1000.0, 1.2, 2.5 * pl->back_eq);
        peaking(t, fs, 4500.0, 0.9, -3.0 * pl->back_eq);
        peaking(t, fs, 11000.0, 1.5, 2.0 * pl->back_eq);
    }
}

/* ------------------------------------------------------------------------ */
/* Filter blob decoding (DLL 0x10001354 header, 0x10001814 samples)          */
/* ------------------------------------------------------------------------ */
typedef struct {
    const uint8_t *p;
    uint32_t rem;      /* bytes allowed to read                           */
    int bits;          /* unread bits left in cur                         */
    int cur;           /* current byte (sign-extended, as in the DLL)     */
    int maxbits, scale_exp, hb, mask, off;
} lhp_br;

static int br_s8(lhp_br *b) { if (!b->rem) return 0; b->rem--; return (int8_t)*b->p++; }
static int br_u8(lhp_br *b) { if (!b->rem) return 0; b->rem--; return *b->p++; }
static int br_u16(lhp_br *b) { int lo = br_u8(b); int hi = br_s8(b); return (hi << 8) | lo; }
static uint32_t br_u32(lhp_br *b)
{
    uint32_t v = (uint32_t)br_u8(b);
    v |= (uint32_t)br_u8(b) << 8;
    v |= (uint32_t)br_u8(b) << 16;
    v |= (uint32_t)br_s8(b) << 24;
    return v;
}
static float br_q14(lhp_br *b) { return (float)(int16_t)br_u16(b) / 16384.0f; }

/* LSB-first bit reader returning a sign-extended n-bit value (0x10001b18). */
static int32_t br_bits(lhp_br *b, int n)
{
    static const uint8_t top[9] = {0x00,0x80,0xc0,0xe0,0xf0,0xf8,0xfc,0xfe,0xff};
    const int total = n;
    uint32_t r = 0;
    while (n >= b->bits) {
        uint32_t t = ((uint32_t)b->cur & top[b->bits]) << 24;
        r = (b->bits >= 32 ? 0 : (r >> b->bits)) | t;
        n -= b->bits;
        b->bits = 8;
        b->cur = br_s8(b);
    }
    if (n > 0) {
        uint32_t t = (((uint32_t)b->cur << (b->bits - n)) & top[n]) << 24;
        r = (r >> n) | t;
        b->bits -= n;
    }
    if (total <= 0) return 0;
    return (int32_t)r >> (32 - total);
}

static uint32_t rate_from_code(int c)
{
    switch (c) {
    case -1: return 0xffffffffu;
    case 0: return 48000; case 1: return 44100; case 2: return 32000;
    case 3: return 22050; case 4: return 16000; case 5: return 11025;
    case 6: return 8000; case 16: return 192000; case 17: return 96000;
    default: return 0;
    }
}

static int parse_header(lhp_br *b, const uint8_t *blob, size_t len, lhp_filter_info *h)
{
    memset(h, 0, sizeof *h);
    memset(b, 0, sizeof *b);
    b->p = blob; b->rem = len < 5 ? (uint32_t)len : 5;
    h->version = br_s8(b);
    if (h->version == 1) { b->off = 1; b->hb = 4; }
    else if (h->version == 2) { b->hb = 5; }
    else return -1;
    b->mask = (1 << b->hb) - 1;
    h->total_len = br_u32(b);
    if (h->total_len > len || h->total_len < 5) return -2;
    b->rem = h->total_len - 5;
    h->header_len = (uint16_t)br_u16(b);
    h->hdr3 = br_s8(b);
    h->hdr4 = br_s8(b);
    h->room = br_s8(b);
    h->sample_rate = rate_from_code(br_s8(b));
    if (!h->sample_rate) return -3;
    h->flags = br_s8(b) & 0xff;
    h->max_bits = br_s8(b);
    b->maxbits = h->max_bits;
    h->scale_exp = (h->version >= 2) ? br_s8(b) : h->max_bits;
    b->scale_exp = h->scale_exp;
    for (int i = 0; i < 5; i++) h->pos_gain[i] = br_q14(b);
    for (int i = 0; i < 12; i++) h->tail_cut[i] = (uint16_t)br_u16(b);
    for (int i = 0; i < 10; i++) h->misc[i] = br_q14(b);
    h->hdr37 = br_s8(b);
    int nlen = br_s8(b) + 1;
    for (int i = 0; i < nlen; i++) {
        int c = br_s8(b);
        if (i < (int)sizeof h->name - 1) h->name[i] = (char)c;
    }
    if (b->rem != h->total_len - h->header_len) return -4;
    return 0;
}

int lhp_parse_header(const uint8_t *blob, size_t len, lhp_filter_info *out)
{
    lhp_br b;
    return parse_header(&b, blob, len, out);
}

/* one 1024-sample impulse-response partition (64 groups of 16) */
static void decode_part(lhp_br *b, float *dst)
{
    const float scale = 1.0f / (float)(1 << (b->scale_exp - 1));
    for (int g = 0; g < LHP_BLOCK; g += 16) {
        int e = (br_bits(b, b->hb) & b->mask) + b->off;
        int nb = e, sh = 0;
        if (nb > b->maxbits) { nb = b->maxbits; sh = e - b->maxbits; }
        for (int i = 0; i < 16; i++) {
            int32_t v = br_bits(b, nb) << sh;
            dst[g + i] = (float)v * scale;
        }
    }
}

/* time-domain partition -> lapped spectrum (0x10003618 + FFT) */
static void part_to_spectrum(lhp_engine *e, const float *t, lhp_cpx *X)
{
    /* DLL scales by 2^-55. We store coefficients 2^40 larger (exact power-of-two
     * shift, keeps them inside FP16 range) and undo it in the output stage. */
    const float k55 = 3.0517578125e-05f;         /* 2^-55 * 2^40 = 2^-15 */
    for (int k = 0; k < LHP_BLOCK; k++) {
        float f = t[k] * k55 * e->gain;
        X[k].re = f * e->rot[k].re;
        X[k].im = f * e->rot[k].im;
    }
    lhp_fft(X, e->fft_fwd);
}

/* per-bin partition counts, including the DLL's one-step-per-bin RLE (0x100031e0) */
static void build_counts(lhp_engine *e)
{
    int cut[6];
    uint8_t raw[LHP_BLOCK];
    for (int i = 0; i < 6; i++) {
        int32_t x = (int32_t)((uint32_t)e->info.tail_cut[2 * i] << 10);
        uint32_t t = ((uint32_t)(x >> 8)) >> 23;
        cut[i] = (int32_t)(x + t) >> 9;
    }
    for (int k = 0; k < 512; k++) {
        int c = 0;
        for (int i = 0; i < 6; i++) if (cut[i] > k) c++;
        if (!c) c = 1;
        raw[k] = (uint8_t)c;
        raw[1023 - k] = (uint8_t)c;
    }
    uint16_t thr[12];
    for (int i = 0; i < 12; i++) thr[i] = 0x400;
    int cur = raw[0], n = 0, k;
    for (k = 0; k < 512; k++)
        if (raw[k] < cur && n < 12) { thr[n++] = (uint16_t)k; cur--; }
    for (; k < 1024; k++)
        if (raw[k] > cur && n < 12) { thr[n++] = (uint16_t)(k | 0x4000); cur++; }
    cur = raw[0];
    int ti = 0, hs = 0, cs = 0;
    for (k = 0; k < LHP_BLOCK; k++) {
        uint16_t t = ti < 12 ? thr[ti] : 0x400;
        if (k >= (t & 0xbfff)) { cur += (t & 0x4000) ? 1 : -1; ti++; }
        if (cur < 0) cur = 0;
        if (cur > LHP_MAXPART) cur = LHP_MAXPART;
        e->cnt[k] = (uint8_t)cur;
        cs += cur;
        hs += cur > 1 ? cur - 1 : 0;
    }
    e->coef_total = (uint16_t)cs;
    e->hist_total = (uint16_t)hs;
}

int lhp_load_filter(lhp_engine *e, const uint8_t *blob, size_t len, float gain)
{
    lhp_br b;
    lhp_filter_info h;
    if (parse_header(&b, blob, len, &h)) return -1;
    if (h.room == 0) return -2;          /* stereo-mixdown filter: other code path, not ported */

    e->loaded = 0;
    e->info = h;
    e->gain = gain;
    build_counts(e);
    if (e->coef_total > LHP_TAIL_MAX || e->hist_total > LHP_HIST_MAX) return -3;
    memset(e->dir, 0, sizeof e->dir);
    memset(e->tail, 0, sizeof e->tail);
    memset(e->hist, 0, sizeof e->hist);
    memset(e->mix, 0, sizeof e->mix);

    /* Scratch: outL is rebuilt every block, so it is free during a load.
     * (Keeps this off the stack - MCU task stacks are small.) */
    float *t = (float *)e->outL;
    lhp_cpx *X = e->X;

    /* 1) room tail: flags bit0, ear-major, 6 partitions of 1024 samples.
     *    Partition i goes to slot (cnt-1-i) of each bin with cnt > i (0x10003d54). */
    if (h.flags & 1) {
        for (int ear = 0; ear < 2; ear++)
            for (int i = 0; i < LHP_MAXPART; i++) {
                decode_part(&b, t);
                part_to_spectrum(e, t, X);
                int base = 0;
                for (int k = 0; k < LHP_BLOCK; k++) {
                    int c = e->cnt[k];
                    if (c > i) {
                        lhp_cpx2 *d = &e->tail[base + c - 1 - i];
                        if (ear == 0) { d->lr = TOCF(X[k].re); d->li = TOCF(X[k].im); }
                        else          { d->rr = TOCF(X[k].re); d->ri = TOCF(X[k].im); }
                    }
                    base += c;
                }
            }
    }
    /* 2) direct HRTFs: position-major (L,C,R,Ls,Rs), each gated by a flag bit
     *    (L and R share bit1). Missing ones are zero filters. (0x10003ca4) */
    static const int pmask[5] = {2, 4, 2, 8, 16};
    const double fs = h.sample_rate == 0xffffffffu ? 48000.0 : (double)h.sample_rate;
    for (int p = 0; p < 5; p++)
        for (int ear = 0; ear < 2; ear++) {
            if (h.flags & pmask[p]) decode_part(&b, t);
            else memset(t, 0, LHP_BLOCK * sizeof(float));
            if (p == LHP_POS_LS || p == LHP_POS_RS) {
                /* keep the surround HRTFs for the 7.1 rear pair; ear 0 is near for Ls */
                const int sp = p - LHP_POS_LS;
                memcpy(e->tsur[sp][ear], t, LHP_BLOCK * sizeof(float));
                if (!lhp_surround_is_factory(&e->place_sur)) place_hrtf(t, ear == sp, &e->place_sur, fs);
            }
            part_to_spectrum(e, t, X);
            for (int k = 0; k < LHP_BLOCK; k++) {
                if (ear == 0) { e->dir[p][k].lr = TOCF(X[k].re); e->dir[p][k].li = TOCF(X[k].im); }
                else          { e->dir[p][k].rr = TOCF(X[k].re); e->dir[p][k].ri = TOCF(X[k].im); }
            }
        }

    /* 7.1 rear pair: same surround HRTFs, own placement (else folded into Ls/Rs) */
    e->back_own = !surround_equal(&e->place_sur, &e->place_back);
    if (e->back_own)
        for (int sp = 0; sp < 2; sp++)
            for (int ear = 0; ear < 2; ear++) {
                memcpy(t, e->tsur[sp][ear], LHP_BLOCK * sizeof(float));
                place_hrtf(t, ear == sp, &e->place_back, fs);
                part_to_spectrum(e, t, X);
                for (int k = 0; k < LHP_BLOCK; k++) {
                    if (ear == 0) { e->dir_back[sp][k].lr = TOCF(X[k].re); e->dir_back[sp][k].li = TOCF(X[k].im); }
                    else          { e->dir_back[sp][k].rr = TOCF(X[k].re); e->dir_back[sp][k].ri = TOCF(X[k].im); }
                }
            }

    e->loaded = 1;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Processing                                                               */
/* ------------------------------------------------------------------------ */
void lhp_stage_input_s16(lhp_engine *e, const int16_t *in, int n)
{
    float (*buf)[LHP_BLOCK] = e->inbuf[e->cur];
    const int nch = e->nch;
    for (int f = 0; f < n; f++)
        for (int c = 0; c < nch; c++)
            buf[c][e->fill + f] = (float)in[f * nch + c];
    e->fill += n;
}

static void stage_input_f32(lhp_engine *e, const float *in, int n)
{
    float (*buf)[LHP_BLOCK] = e->inbuf[e->cur];
    const int nch = e->nch;
    for (int f = 0; f < n; f++)
        for (int c = 0; c < nch; c++)
            buf[c][e->fill + f] = in[f * nch + c];
    e->fill += n;
}

/* Room tail: frequency-dependent partitioned convolution of the previous
 * block's room-send spectrum (DLL 0x10002144). Initialises outL/outR. */
LHP_HOT void lhp_stage_tail(lhp_engine *e)
{
    lhp_cpx *h = e->hist;
    const lhp_cpx2 *c = e->tail;
    for (int k = 0; k < LHP_BLOCK; k++) {
        const int cnt = e->cnt[k];
        float lr = 0, li = 0, rr = 0, ri = 0;
        const lhp_cpx x = e->mix[k];
        int n = cnt;
        if (n > 1) {                                   /* oldest, no shift */
            float hr = h->re, hi = h->im;
            lr += hr * CF(c->lr) - hi * CF(c->li); li += hr * CF(c->li) + hi * CF(c->lr);
            rr += hr * CF(c->rr) - hi * CF(c->ri); ri += hr * CF(c->ri) + hi * CF(c->rr);
            h++; c++; n--;
            while (n > 1) {                            /* shift history down */
                hr = h->re; hi = h->im;
                lr += hr * CF(c->lr) - hi * CF(c->li); li += hr * CF(c->li) + hi * CF(c->lr);
                rr += hr * CF(c->rr) - hi * CF(c->ri); ri += hr * CF(c->ri) + hi * CF(c->rr);
                h[-1] = *h;
                h++; c++; n--;
            }
        }
        if (n) {                                       /* newest = last block's send */
            lr += x.re * CF(c->lr) - x.im * CF(c->li); li += x.re * CF(c->li) + x.im * CF(c->lr);
            rr += x.re * CF(c->rr) - x.im * CF(c->ri); ri += x.re * CF(c->ri) + x.im * CF(c->rr);
            c++;
        }
        if (cnt > 1) h[-1] = x;
        e->mix[k].re = 0; e->mix[k].im = 0;
        e->outL[k].re = lr; e->outL[k].im = li;
        e->outR[k].re = rr; e->outR[k].im = ri;
    }
}

/* (prev + j*new) * e^{j*pi*k/2048} using the integer window table (0x10001d38) */
static LHP_HOT void lapped(const lhp_engine *e, const float *nw, const float *pv, lhp_cpx *X)
{
    X[0].re = pv[0] * 32768.0f;
    X[0].im = nw[0] * 32768.0f;
    for (int k = 1; k < LHP_BLOCK; k++) {
        const float a = (float)e->win[k];
        const float b = (float)e->win[LHP_BLOCK - k];
        X[k].re = pv[k] * b - nw[k] * a;
        X[k].im = pv[k] * a + nw[k] * b;
    }
}

static LHP_HOT void accumulate_dir(lhp_engine *e, const lhp_cpx2 *d, float g)
{
    for (int k = 0; k < LHP_BLOCK; k++) {
        const float xr = e->X[k].re, xi = e->X[k].im;
        e->outL[k].re += xr * CF(d[k].lr) - xi * CF(d[k].li);
        e->outL[k].im += xr * CF(d[k].li) + xi * CF(d[k].lr);
        e->outR[k].re += xr * CF(d[k].rr) - xi * CF(d[k].ri);
        e->outR[k].im += xr * CF(d[k].ri) + xi * CF(d[k].rr);
        e->mix[k].re  += xr * g;
        e->mix[k].im  += xi * g;
    }
}

static LHP_HOT void accumulate(lhp_engine *e, int p)   /* 0x100024dc */
{
    accumulate_dir(e, e->dir[p], e->info.pos_gain[p]);
}

LHP_HOT void lhp_stage_direct(lhp_engine *e)   /* 0x10001c54 */
{
    float (*nw)[LHP_BLOCK] = e->inbuf[e->cur];
    float (*pv)[LHP_BLOCK] = e->inbuf[e->cur ^ 1];

    /* fold LFE into L+R, else into C (0x10001f00) */
    const int lfe = e->map[LHP_POS_LFE];
    if (lfe >= 0) {
        const int L = e->map[LHP_POS_L], R = e->map[LHP_POS_R], C = e->map[LHP_POS_C];
        if (L >= 0 && R >= 0) {
            for (int i = 0; i < LHP_BLOCK; i++) { nw[L][i] += nw[lfe][i]; nw[R][i] += nw[lfe][i]; }
        } else if (C >= 0) {
            for (int i = 0; i < LHP_BLOCK; i++) nw[C][i] += nw[lfe][i];
        }
    }
    /* 7.1 rear pair at the surround placement: fold into Ls/Rs like LFE */
    if (!e->back_own)
        for (int b = 0; b < 2; b++) {
            const int src = e->back[b], dst = e->map[LHP_POS_LS + b];
            if (src >= 0 && dst >= 0 && src != dst)
                for (int i = 0; i < LHP_BLOCK; i++) nw[dst][i] += nw[src][i];
        }
    int last = -1;
    for (int p = 0; p < 5; p++) {
        const int ch = e->map[p];
        if (ch < 0) continue;
        if (ch != last) {
            last = ch;
            lapped(e, nw[ch], pv[ch], e->X);
            lhp_fft(e->X, e->fft_fwd);
        }
        accumulate(e, p);
    }
    /* 7.1 rear pair with its own placement: two extra forward transforms */
    if (e->back_own)
        for (int b = 0; b < 2; b++) {
            const int ch = e->back[b];
            if (ch < 0) continue;
            lapped(e, nw[ch], pv[ch], e->X);
            lhp_fft(e->X, e->fft_fwd);
            accumulate_dir(e, e->dir_back[b], e->info.pos_gain[LHP_POS_LS + b]);
        }
    e->cur ^= 1;
}

LHP_HOT void lhp_stage_inverse(lhp_engine *e)
{
    lhp_fft(e->outL, e->fft_inv);
    lhp_fft(e->outR, e->fft_inv);
}

static inline int16_t sat16(float v)
{
    long i = lrintf(v);
    if (i > 32767) i = 32767;
    if (i < -32768) i = -32768;
    return (int16_t)i;
}

#define LHP_UNSCALE 9.094947017729282e-13f   /* 2^-40, see part_to_spectrum */

LHP_HOT void lhp_stage_output_s16(lhp_engine *e, int16_t *out)   /* 0x1000266c */
{
    float b = 32768.0f, a = (float)e->win[0];
    for (int k = 0; k < LHP_BLOCK; k++) {
        float l = (e->outL[k].im * b - e->outL[k].re * a) * LHP_UNSCALE;
        float r = (e->outR[k].im * b - e->outR[k].re * a) * LHP_UNSCALE;
        out[2 * k] = sat16(l);
        out[2 * k + 1] = sat16(r);
        b = (float)e->win[LHP_BLOCK - (k + 1)];
        a = (float)e->win[k + 1];
    }
}

LHP_HOT void lhp_stage_output_f32(lhp_engine *e, float *out)
{
    float b = 32768.0f, a = (float)e->win[0];
    for (int k = 0; k < LHP_BLOCK; k++) {
        out[2 * k]     = (e->outL[k].im * b - e->outL[k].re * a) * LHP_UNSCALE;
        out[2 * k + 1] = (e->outR[k].im * b - e->outR[k].re * a) * LHP_UNSCALE;
        b = (float)e->win[LHP_BLOCK - (k + 1)];
        a = (float)e->win[k + 1];
    }
}

static void run_block(lhp_engine *e)
{
    lhp_stage_tail(e);
    lhp_stage_direct(e);
    lhp_stage_inverse(e);
    e->fill = 0;
}

int lhp_push_s16(lhp_engine *e, const int16_t *in, int n, int16_t *out)
{
    if (n > LHP_BLOCK - e->fill) n = LHP_BLOCK - e->fill;
    lhp_stage_input_s16(e, in, n);
    if (e->fill < LHP_BLOCK) return 0;
    if (!e->loaded) { memset(out, 0, LHP_BLOCK * 4); e->fill = 0; return LHP_BLOCK; }
    run_block(e);
    lhp_stage_output_s16(e, out);
    return LHP_BLOCK;
}

int lhp_push_f32(lhp_engine *e, const float *in, int n, float *out)
{
    if (n > LHP_BLOCK - e->fill) n = LHP_BLOCK - e->fill;
    stage_input_f32(e, in, n);
    if (e->fill < LHP_BLOCK) return 0;
    if (!e->loaded) { memset(out, 0, LHP_BLOCK * 8); e->fill = 0; return LHP_BLOCK; }
    run_block(e);
    lhp_stage_output_f32(e, out);
    return LHP_BLOCK;
}
