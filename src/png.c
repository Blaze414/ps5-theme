/* PNG pictures that must match the console's own in every respect but the
   pixels: ShellUI crashed on start with interface pictures whose format had
   changed, so a replacement keeps

     - every chunk other than the image data byte for byte and in the same
       order (header, palette, transparency, colour space, text, ...);
     - the image data as one run of IDAT chunks where the console's is, with
       the same zlib header;
   and its image data must be sound: it decompresses to exactly the size the
   header implies, the Adler-32 checksum is right, every row has a valid
   filter, and in a palette picture every pixel is inside the palette. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "theme.h"


#define PNG_RAW_MAX  (64u << 20)
#define PNG_CHUNKS   4096

typedef struct { size_t at; uint32_t len; char type[5]; } chunk_t;

static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }


/* The chunks of a PNG, each with a valid CRC, ending with IEND exactly at
   the end of the data. The number of chunks, or -1. */
static int
png_chunks(const uint8_t *b, size_t n, chunk_t *out) {
  if(n < 8 || memcmp(b, png_sig, 8)) return -1;
  size_t pos = 8;
  for(int k = 0; k < PNG_CHUNKS; k++) {
    if(n - pos < 12) return -1;
    uint32_t len = be32(b + pos);
    if(len > n - pos - 12) return -1;
    if(crc32_update(0, b + pos + 4, 4 + (size_t)len) != be32(b + pos + 8 + len)) return -1;
    out[k].at = pos;
    out[k].len = len;
    memcpy(out[k].type, b + pos + 4, 4);
    out[k].type[4] = 0;
    pos += 12 + (size_t)len;
    if(!strcmp(out[k].type, "IEND")) return pos == n ? k + 1 : -1;
  }
  return -1;
}


/* The IDAT data from chunk `from` to `to` (exclusive), concatenated. */
static uint8_t *
png_idat(const uint8_t *b, const chunk_t *c, int from, int to, size_t *len) {
  size_t total = 0;
  for(int i = from; i < to; i++) total += c[i].len;
  uint8_t *out = malloc(total ? total : 1);
  if(!out) return NULL;
  *len = 0;
  for(int i = from; i < to; i++) {
    memcpy(out + *len, b + c[i].at + 8, c[i].len);
    *len += c[i].len;
  }
  return out;
}


static uint32_t
adler32(const uint8_t *p, size_t n) {
  uint32_t a = 1, b = 0;
  while(n) {
    size_t step = n < 5552 ? n : 5552;
    n -= step;
    while(step--) { a += *p++; b += a; }
    a %= 65521;
    b %= 65521;
  }
  return b << 16 | a;
}


static int
paeth(int a, int b, int c) {
  int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}


int
png_check_against(const uint8_t *stock, size_t sl, const uint8_t *cand, size_t cl, char *why, size_t len) {
  chunk_t *sc = malloc(sizeof(chunk_t) * PNG_CHUNKS), *cc = malloc(sizeof(chunk_t) * PNG_CHUNKS);
  uint8_t *sz = NULL, *cz = NULL, *raw = NULL;
  int ret = -1, ns, nc;
  const char *bad = NULL;

  if(!sc || !cc) { bad = "out of memory"; goto done; }
  if((ns = png_chunks(stock, sl, sc)) < 2 || strcmp(sc[0].type, "IHDR") || sc[0].len != 13) {
    snprintf(why, len, "cannot read the system picture to compare against");
    goto done_why;
  }
  if((nc = png_chunks(cand, cl, cc)) < 0) { bad = "it is not a complete PNG (a chunk or its checksum is damaged)"; goto done; }

  /* The same chunks apart from the image data: one run of IDAT chunks, at
     the same place; everything else byte for byte. */
  int i = 0, j = 0, s_from = -1, s_to = -1, c_from = -1, c_to = -1;
  while(i < ns || j < nc) {
    int s_idat = i < ns && !strcmp(sc[i].type, "IDAT"), c_idat = j < nc && !strcmp(cc[j].type, "IDAT");
    if(s_idat || c_idat) {
      if(!s_idat || !c_idat || s_from >= 0) { bad = "its chunks differ from the console's picture"; goto done; }
      for(s_from = i; i < ns && !strcmp(sc[i].type, "IDAT"); i++) {}
      for(c_from = j; j < nc && !strcmp(cc[j].type, "IDAT"); j++) {}
      s_to = i;
      c_to = j;
      continue;
    }
    if(i >= ns || j >= nc || sc[i].len != cc[j].len || memcmp(stock + sc[i].at, cand + cc[j].at, 12 + (size_t)sc[i].len)) {
      bad = "its chunks differ from the console's picture";
      if(i < ns && j < nc && !strcmp(sc[i].type, "IHDR") && !strcmp(cc[j].type, "IHDR")) bad = "its size or pixel format differs";
      goto done;
    }
    i++;
    j++;
  }
  if(s_from < 0) { snprintf(why, len, "cannot read the system picture to compare against"); goto done_why; }

  /* The same zlib header, then a complete stream with the right checksum. */
  size_t szl, czl, used;
  if(!(sz = png_idat(stock, sc, s_from, s_to, &szl)) || !(cz = png_idat(cand, cc, c_from, c_to, &czl))) {
    bad = "out of memory";
    goto done;
  }
  if(szl < 2 || czl < 6 || memcmp(sz, cz, 2)) { bad = "its image data is compressed differently from the console's"; goto done; }
  if((cz[0] & 15) != 8 || (cz[0] >> 4) > 7 || (cz[1] & 0x20) || ((cz[0] << 8) | cz[1]) % 31) {
    bad = "its image data has a damaged zlib header";
    goto done;
  }
  const uint8_t *ihdr = stock + sc[0].at + 8;
  uint32_t w = be32(ihdr), h = be32(ihdr + 4);
  unsigned depth = ihdr[8], type = ihdr[9];
  static const int channels_of[7] = {1, 0, 3, 1, 2, 0, 4};
  int channels = type < 7 ? channels_of[type] : 0;
  if(ihdr[12]) { bad = "interlaced pictures can't be checked"; goto done; }
  if(!channels || !w || !h || !depth) { snprintf(why, len, "cannot read the system picture to compare against"); goto done_why; }
  uint64_t bits = (uint64_t)w * channels * depth, rowbytes = (bits + 7) / 8, rawlen = (uint64_t)h * (1 + rowbytes);
  if(rawlen > PNG_RAW_MAX) { bad = "it is too large to check"; goto done; }
  if(!(raw = malloc((size_t)rawlen))) { bad = "out of memory"; goto done; }
  long got = inflate_raw(cz + 2, czl - 6, &used, raw, (size_t)rawlen, (size_t)1 << ((cz[0] >> 4) + 8));
  if(got != (long)rawlen || used != czl - 6 || adler32(raw, (size_t)rawlen) != be32(cz + czl - 4)) {
    bad = "its image data is damaged or the wrong size";
    goto done;
  }

  /* Valid row filters; in a palette picture, every pixel in the palette. */
  size_t bpp = bits / w >= 8 ? (size_t)(bits / w / 8) : 1, palette = 0;
  for(int k = 0; type == 3 && k < ns; k++) if(!strcmp(sc[k].type, "PLTE")) palette = sc[k].len / 3;
  uint8_t *prev = NULL;
  for(uint32_t y = 0; y < h; y++) {
    uint8_t *row = raw + (size_t)y * (1 + rowbytes), f = row[0], *cur = row + 1;
    if(f > 4) { bad = "its image data has an invalid row filter"; goto done; }
    for(size_t x = 0; type == 3 && x < rowbytes; x++) {
      int a = x >= bpp ? cur[x - bpp] : 0, b = prev ? prev[x] : 0, c = prev && x >= bpp ? prev[x - bpp] : 0;
      cur[x] = (uint8_t)(cur[x] + (f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : f == 4 ? paeth(a, b, c) : 0));
    }
    for(uint32_t x = 0; type == 3 && x < w; x++) {
      size_t bit = (size_t)x * depth;
      unsigned index = (cur[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
      if(index >= palette) { bad = "it uses colours outside its palette"; goto done; }
    }
    prev = cur;
  }
  ret = 0;

done:
  if(bad) snprintf(why, len, "not the same kind of PNG as the console's: %s", bad);
done_why:
  free(sc);
  free(cc);
  free(sz);
  free(cz);
  free(raw);
  return ret;
}
