#include "lhp_dll.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p) {
  return (uint16_t)(p[0] | p[1] << 8);
}

static int s8(uint8_t b) {
  return (int8_t)b;
}

static int rate_from_code(int code, uint32_t *rate) {
  switch (code) {
    case -1: *rate = 0xFFFFFFFFu; return 0;
    case 0: *rate = 48000; return 0;
    case 1: *rate = 44100; return 0;
    case 2: *rate = 32000; return 0;
    case 3: *rate = 22050; return 0;
    case 4: *rate = 16000; return 0;
    case 5: *rate = 11025; return 0;
    case 6: *rate = 8000; return 0;
    case 16: *rate = 192000; return 0;
    case 17: *rate = 96000; return 0;
    default: return -1;
  }
}

/* same checks as parse_header() in tools/extract_filters.py */
static int parse_header(const uint8_t *d, size_t len, size_t off, lhp_dll_filter *f) {
  if (off + 0x60 > len) return -1;

  int version = d[off];
  if (version != 1 && version != 2) return -1;

  uint32_t total = rd32(d + off + 1);
  if (total < 0x40 || total > 0x40000 || off + total > len) return -1;

  uint16_t hlen = rd16(d + off + 5);
  if (hlen < 0x30 || hlen > 0x200 || hlen >= total) return -1;

  size_t p = off + 7;
  int room = s8(d[p + 2]);
  int rate_code = s8(d[p + 3]);
  int max_bits = s8(d[p + 5]);
  p += 6;

  uint32_t rate;
  if (rate_from_code(rate_code, &rate) || room < 0 || room > 3) return -1;

  int scale = max_bits;
  if (version >= 2) scale = s8(d[p++]);
  if (max_bits < 1 || max_bits > 32 || scale < 1 || scale > 31) return -1;

  p += 10; /* 5 x q14 position gains */
  p += 24; /* 12 x u16 tail cut-offs */
  p += 20; /* 10 x q14 misc */
  p += 1;  /* hdr37 */
  int name_len = s8(d[p]) + 1;
  p += 1;
  if (name_len < 0 || p + (size_t)name_len > off + hlen) return -1;

  if (f) {
    memset(f, 0, sizeof(*f));
    size_t n = (size_t)name_len < sizeof(f->name) - 1 ? (size_t)name_len : sizeof(f->name) - 1;
    memcpy(f->name, d + p, n);
    f->name[n] = 0;
    f->offset = off;
    f->length = total;
    f->room = room;
    f->rate = rate;
  }

  p += (size_t)name_len;
  return p - off == hlen ? 0 : -1;
}

int lhp_dll_scan(const uint8_t *image, size_t len, lhp_dll_filter *out, int max) {
  if (len < 2 || image[0] != 'M' || image[1] != 'Z') return -1;

  int found = 0;
  size_t off = 0;
  while (off + 0x60 < len) {
    lhp_dll_filter f;
    if (parse_header(image, len, off, &f) == 0) {
      if (found < max) out[found] = f;
      found++;
      off += f.length;
    } else {
      off++;
    }
  }
  return found;
}

int lhp_dll_find(const uint8_t *image, size_t len, uint32_t rate, int room, const uint8_t **blob,
                 size_t *blob_len) {
  lhp_dll_filter filters[64];
  int n = lhp_dll_scan(image, len, filters, 64);
  if (n > 64) n = 64;

  for (int i = 0; i < n; i++) {
    if (filters[i].room == room && filters[i].rate == rate) {
      *blob = image + filters[i].offset;
      *blob_len = filters[i].length;
      return 0;
    }
  }
  return -1;
}
