/*
 * Finds the Dolby Headphone room filters inside the server owner's DolbyHph.dll
 * (Lake Technology, 1.20.0276). C version of lakehp's tools/extract_filters.py:
 * the image is scanned for filter blobs and every header is validated.
 */
#ifndef NEKO_LHP_DLL_H
#define NEKO_LHP_DLL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  size_t offset;  /* blob offset in the DLL image */
  size_t length;  /* blob length */
  int room;       /* 0 = stereo mixdown, 1..3 = DH1..DH3 */
  uint32_t rate;  /* 0xFFFFFFFF = any */
  char name[32];
} lhp_dll_filter;

/* Scans the DLL image. Writes up to max entries, returns how many were found
 * (may be more than max), or -1 if the data is not a PE file. */
int lhp_dll_scan(const uint8_t *image, size_t len, lhp_dll_filter *out, int max);

/* Finds the room filter for a sample rate. Returns 0 and sets blob/blob_len on success. */
int lhp_dll_find(const uint8_t *image, size_t len, uint32_t rate, int room, const uint8_t **blob,
                 size_t *blob_len);

#endif
