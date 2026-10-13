/* Inflate (RFC 1951), to check the image data of PNG pictures before they
   reach ShellUI. Stored, fixed and dynamic Huffman blocks; canonical codes
   are decoded a bit at a time, which is plenty for icons. Every read is
   bounds-checked, back-references may not reach before the start of the
   output or beyond the stream's window, and the output may not exceed
   `cap`. */

#include <stdint.h>
#include <string.h>

#include "theme.h"


#define MAXBITS   15
#define MAXLCODES 286
#define MAXDCODES 30

typedef struct {
  const uint8_t *in;
  size_t len, pos;
  uint32_t bitbuf;
  int bitcnt;
  uint8_t *out;
  size_t cap, n, window;
} inflate_t;

#define FIXLCODES 288   /* the fixed code has two symbols more than a dynamic one may */

typedef struct {
  int16_t count[MAXBITS + 1];
  int16_t symbol[FIXLCODES];
} huffman_t;


/* -1 when the input runs out. */
static int
bits(inflate_t *s, int need, uint32_t *v) {
  uint32_t b = s->bitbuf;
  while(s->bitcnt < need) {
    if(s->pos >= s->len) return -1;
    b |= (uint32_t)s->in[s->pos++] << s->bitcnt;
    s->bitcnt += 8;
  }
  s->bitbuf = b >> need;
  s->bitcnt -= need;
  *v = b & ((1u << need) - 1);
  return 0;
}


static int
stored(inflate_t *s) {
  s->bitbuf = 0;
  s->bitcnt = 0;
  if(s->len - s->pos < 4) return -1;
  unsigned len = s->in[s->pos] | s->in[s->pos + 1] << 8;
  unsigned nlen = s->in[s->pos + 2] | s->in[s->pos + 3] << 8;
  s->pos += 4;
  if(len != (~nlen & 0xffff) || s->len - s->pos < len || s->cap - s->n < len) return -1;
  memcpy(s->out + s->n, s->in + s->pos, len);
  s->pos += len;
  s->n += len;
  return 0;
}


/* A canonical code from code lengths. 0 if complete or incomplete (a code
   that is never sent is fine), -1 if over-subscribed. */
static int
build(huffman_t *h, const uint8_t *length, int n) {
  int16_t offs[MAXBITS + 1];
  memset(h->count, 0, sizeof(h->count));
  for(int i = 0; i < n; i++) h->count[length[i]]++;
  if(h->count[0] == n) return 0;
  int left = 1;
  for(int len = 1; len <= MAXBITS; len++) {
    left <<= 1;
    left -= h->count[len];
    if(left < 0) return -1;
  }
  offs[1] = 0;
  for(int len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
  for(int i = 0; i < n; i++) if(length[i]) h->symbol[offs[length[i]]++] = (int16_t)i;
  return 0;
}


static int
decode(inflate_t *s, const huffman_t *h) {
  int code = 0, first = 0, index = 0;
  for(int len = 1; len <= MAXBITS; len++) {
    uint32_t b;
    if(bits(s, 1, &b)) return -1;
    code |= (int)b;
    int count = h->count[len];
    if(code - count < first) return h->symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  return -1;
}


static int
codes(inflate_t *s, const huffman_t *lencode, const huffman_t *distcode) {
  static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                     35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                   3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                     257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
  static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                   7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
  for(;;) {
    int symbol = decode(s, lencode);
    if(symbol < 0) return -1;
    if(symbol < 256) {
      if(s->n >= s->cap) return -1;
      s->out[s->n++] = (uint8_t)symbol;
    } else if(symbol == 256) {
      return 0;
    } else {
      symbol -= 257;
      if(symbol >= 29) return -1;
      uint32_t extra, dextra;
      if(bits(s, lext[symbol], &extra)) return -1;
      size_t len = lbase[symbol] + extra;
      int d = decode(s, distcode);
      if(d < 0 || d >= 30 || bits(s, dext[d], &dextra)) return -1;
      size_t dist = dbase[d] + dextra;
      if(dist > s->n || dist > s->window || s->cap - s->n < len) return -1;
      for(size_t i = 0; i < len; i++, s->n++) s->out[s->n] = s->out[s->n - dist];
    }
  }
}


static int
fixed(inflate_t *s) {
  huffman_t lencode, distcode;   /* cheap to build; no shared state between threads */
  uint8_t lengths[288];
  int i = 0;
  for(; i < 144; i++) lengths[i] = 8;
  for(; i < 256; i++) lengths[i] = 9;
  for(; i < 280; i++) lengths[i] = 7;
  for(; i < 288; i++) lengths[i] = 8;
  build(&lencode, lengths, 288);
  for(i = 0; i < 30; i++) lengths[i] = 5;
  build(&distcode, lengths, 30);
  return codes(s, &lencode, &distcode);
}


static int
dynamic(inflate_t *s) {
  static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  uint8_t lengths[MAXLCODES + MAXDCODES];
  huffman_t lencode, distcode;
  uint32_t nlen, ndist, ncode, v;
  if(bits(s, 5, &nlen) || bits(s, 5, &ndist) || bits(s, 4, &ncode)) return -1;
  nlen += 257;
  ndist += 1;
  ncode += 4;
  if(nlen > MAXLCODES || ndist > MAXDCODES) return -1;
  uint32_t index;
  for(index = 0; index < ncode; index++) {
    if(bits(s, 3, &v)) return -1;
    lengths[order[index]] = (uint8_t)v;
  }
  for(; index < 19; index++) lengths[order[index]] = 0;
  if(build(&lencode, lengths, 19)) return -1;
  for(index = 0; index < nlen + ndist;) {
    int symbol = decode(s, &lencode);
    if(symbol < 0) return -1;
    if(symbol < 16) {
      lengths[index++] = (uint8_t)symbol;
      continue;
    }
    uint8_t len = 0;
    uint32_t repeat;
    if(symbol == 16) {
      if(index == 0) return -1;
      len = lengths[index - 1];
      if(bits(s, 2, &repeat)) return -1;
      repeat += 3;
    } else if(symbol == 17) {
      if(bits(s, 3, &repeat)) return -1;
      repeat += 3;
    } else {
      if(bits(s, 7, &repeat)) return -1;
      repeat += 11;
    }
    if(index + repeat > nlen + ndist) return -1;
    while(repeat--) lengths[index++] = len;
  }
  if(lengths[256] == 0) return -1;   /* no end-of-block code */
  if(build(&lencode, lengths, (int)nlen) || build(&distcode, lengths + nlen, (int)ndist)) return -1;
  return codes(s, &lencode, &distcode);
}


long
inflate_raw(const uint8_t *src, size_t len, size_t *used, uint8_t *dst, size_t cap, size_t window) {
  inflate_t s = { .in = src, .len = len, .out = dst, .cap = cap, .window = window };
  uint32_t last, type;
  do {
    if(bits(&s, 1, &last) || bits(&s, 2, &type)) return -1;
    int rc = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
    if(rc) return -1;
  } while(!last);
  *used = s.pos;   /* whole bytes: the rest of the last one is padding */
  return (long)s.n;
}
