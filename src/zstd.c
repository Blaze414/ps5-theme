/* Zstandard decompressor (RFC 8878), used to read and verify the system's
   .dds.zst textures. Decoding only, no dictionaries. Every read is bounds-
   checked; malformed input returns -1 instead of crashing. The content
   checksum (XXH64) is verified when present.

   Structure follows the format specification and the reference "educational
   decoder" in the zstd repository. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "theme.h"


#define ZSTD_MAGIC        0xFD2FB528u
#define SKIP_MAGIC_MASK   0xFFFFFFF0u
#define SKIP_MAGIC        0x184D2A50u
#define BLOCK_MAX         (128 * 1024)
#define HUF_MAX_BITS      11
#define HUF_MAX_SYMBS     256
#define FSE_MAX_SYMBS     256
#define FSE_MAX_LOG       9
#define LL_MAX_LOG        9
#define ML_MAX_LOG        9
#define OF_MAX_LOG        8

#define CHECK(c) do { if(!(c)) return -1; } while(0)


static int
highbit(uint64_t x) {
  int n = -1;
  while(x) { x >>= 1; n++; }
  return n;
}


/* ---------------- bit readers ---------------- */

/* Forward reader over [p, end), little-endian bit order. */
typedef struct fwd {
  const uint8_t *p, *end;
  uint64_t bitpos;
} fwd_t;

static int
fwd_bits(fwd_t *r, int n, uint32_t *out) {
  uint32_t v = 0;
  for(int i = 0; i < n; i++) {
    uint64_t byte = r->bitpos >> 3;
    if(r->p + byte >= r->end) return -1;
    v |= (uint32_t)((r->p[byte] >> (r->bitpos & 7)) & 1) << i;
    r->bitpos++;
  }
  *out = v;
  return 0;
}

/* Backward reader: zstd's FSE/Huffman streams, read from the end. Reading
   past the start yields zero bits and makes `pos` negative, as the format
   expects at the very end of a stream. */
typedef struct bwd {
  const uint8_t *p;
  int64_t pos;            /* bits remaining */
} bwd_t;

static int
bwd_init(bwd_t *r, const uint8_t *p, size_t len) {
  CHECK(len > 0 && p[len - 1] != 0);
  r->p = p;
  r->pos = (int64_t)len * 8 - (8 - highbit(p[len - 1]));
  return 0;
}

static uint64_t
bwd_bits(bwd_t *r, int n) {
  if(n == 0) return 0;
  r->pos -= n;
  int64_t start = r->pos;
  int take = n;
  if(start < 0) {
    take += (int)start;
    start = 0;
  }
  uint64_t v = 0;
  for(int i = 0; i < take; i++) {
    int64_t b = start + i;
    v |= (uint64_t)((r->p[b >> 3] >> (b & 7)) & 1) << i;
  }
  if(r->pos < 0) v = (-r->pos >= 64) ? 0 : v << -r->pos;
  return v;
}


/* ---------------- FSE ---------------- */

typedef struct fse {
  uint8_t  symbol[1 << FSE_MAX_LOG];
  uint8_t  nbits[1 << FSE_MAX_LOG];
  uint16_t base[1 << FSE_MAX_LOG];
  int      log;
} fse_t;

static int
fse_build(fse_t *t, const int16_t *freq, int nsym, int log) {
  CHECK(log <= FSE_MAX_LOG && nsym > 0 && nsym <= FSE_MAX_SYMBS);
  uint32_t size = 1u << log, high = size, step = (size >> 1) + (size >> 3) + 3, mask = size - 1, pos = 0;
  uint16_t next[FSE_MAX_SYMBS];

  for(int s = 0; s < nsym; s++) {
    if(freq[s] == -1) {
      CHECK(high > 0);
      t->symbol[--high] = (uint8_t)s;
      next[s] = 1;
    }
  }
  for(int s = 0; s < nsym; s++) {
    if(freq[s] <= 0) continue;
    next[s] = (uint16_t)freq[s];
    for(int i = 0; i < freq[s]; i++) {
      t->symbol[pos] = (uint8_t)s;
      do { pos = (pos + step) & mask; } while(pos >= high);
    }
  }
  CHECK(pos == 0);
  for(uint32_t i = 0; i < size; i++) {
    uint16_t d = next[t->symbol[i]]++;
    CHECK(d > 0);
    t->nbits[i] = (uint8_t)(log - highbit(d));
    t->base[i] = (uint16_t)(((uint32_t)d << t->nbits[i]) - size);
  }
  t->log = log;
  return 0;
}

static void
fse_rle(fse_t *t, uint8_t symbol) {
  t->symbol[0] = symbol;
  t->nbits[0] = 0;
  t->base[0] = 0;
  t->log = 0;
}

/* Table description (RFC 8878 4.1.1); returns bytes consumed. */
static int
fse_read(fse_t *t, const uint8_t *p, size_t len, int max_log, int max_sym, size_t *used) {
  fwd_t r = { p, p + len, 0 };
  uint32_t v;
  int16_t freq[FSE_MAX_SYMBS];
  int sym = 0;

  CHECK(!fwd_bits(&r, 4, &v));
  int log = (int)v + 5;
  CHECK(log <= max_log);
  int32_t remaining = 1 << log;
  while(remaining > 0 && sym <= max_sym) {
    int bits = highbit((uint64_t)remaining + 1) + 1;
    CHECK(!fwd_bits(&r, bits, &v));
    uint32_t lower = (1u << (bits - 1)) - 1;
    uint32_t threshold = (1u << bits) - 1 - (uint32_t)(remaining + 1);
    if((v & lower) < threshold) {
      r.bitpos--;
      v &= lower;
    } else if(v > lower) {
      v -= threshold;
    }
    int16_t prob = (int16_t)((int)v - 1);
    remaining -= prob < 0 ? -prob : prob;
    freq[sym++] = prob;
    if(prob == 0) {
      CHECK(!fwd_bits(&r, 2, &v));
      for(;;) {
        for(uint32_t i = 0; i < v; i++) {
          CHECK(sym <= max_sym);
          freq[sym++] = 0;
        }
        if(v != 3) break;
        CHECK(!fwd_bits(&r, 2, &v));
      }
    }
  }
  CHECK(remaining == 0 && sym <= max_sym + 1);
  *used = (r.bitpos + 7) >> 3;
  return fse_build(t, freq, sym, log);
}

static uint8_t
fse_peek(const fse_t *t, uint16_t state) {
  return t->symbol[state];
}

static void
fse_update(const fse_t *t, uint16_t *state, bwd_t *r) {
  uint8_t n = t->nbits[*state];
  *state = (uint16_t)(t->base[*state] + bwd_bits(r, n));
}


/* ---------------- Huffman ---------------- */

typedef struct huf {
  uint8_t symbol[1 << HUF_MAX_BITS];
  uint8_t nbits[1 << HUF_MAX_BITS];
  int     max_bits;
  int     ready;
} huf_t;

static int
huf_build(huf_t *h, const uint8_t *weights, int n) {
  uint64_t sum = 0;
  uint8_t bits[HUF_MAX_SYMBS];
  uint32_t rank_count[HUF_MAX_BITS + 2] = {0}, rank_idx[HUF_MAX_BITS + 2];

  CHECK(n > 0 && n < HUF_MAX_SYMBS);
  for(int i = 0; i < n; i++) {
    CHECK(weights[i] <= HUF_MAX_BITS);
    if(weights[i]) sum += 1ull << (weights[i] - 1);
  }
  CHECK(sum > 0);
  int max_bits = highbit(sum) + 1;
  CHECK(max_bits <= HUF_MAX_BITS);
  uint64_t left = (1ull << max_bits) - sum;
  CHECK(left && !(left & (left - 1)));
  int last_weight = highbit(left) + 1;
  for(int i = 0; i < n; i++) bits[i] = weights[i] ? (uint8_t)(max_bits + 1 - weights[i]) : 0;
  bits[n] = (uint8_t)(max_bits + 1 - last_weight);
  n++;

  for(int i = 0; i < n; i++) {
    CHECK(bits[i] <= max_bits);
    rank_count[bits[i]]++;
  }
  uint32_t size = 1u << max_bits;
  rank_idx[max_bits] = 0;
  for(int i = max_bits; i >= 1; i--) {
    rank_idx[i - 1] = rank_idx[i] + rank_count[i] * (1u << (max_bits - i));
    CHECK(rank_idx[i - 1] <= size);
    memset(&h->nbits[rank_idx[i]], i, rank_idx[i - 1] - rank_idx[i]);
  }
  CHECK(rank_idx[0] == size);
  for(int i = 0; i < n; i++) {
    if(!bits[i]) continue;
    uint32_t code = rank_idx[bits[i]], len = 1u << (max_bits - bits[i]);
    CHECK(code + len <= size);
    memset(&h->symbol[code], i, len);
    rank_idx[bits[i]] += len;
  }
  h->max_bits = max_bits;
  h->ready = 1;
  return 0;
}

/* Huffman tree description; returns bytes consumed via *used. */
static int
huf_read(huf_t *h, const uint8_t *p, size_t len, size_t *used) {
  uint8_t weights[HUF_MAX_SYMBS];
  int n = 0;

  CHECK(len >= 1);
  uint8_t header = p[0];
  if(header < 128) {
    CHECK(len >= 1u + header && header > 0);
    fse_t local;
    size_t tused;
    CHECK(!fse_read(&local, p + 1, header, 6, 255, &tused));
    CHECK(tused <= header);
    bwd_t r;
    CHECK(!bwd_init(&r, p + 1 + tused, header - tused));
    uint16_t s1 = (uint16_t)bwd_bits(&r, local.log), s2 = (uint16_t)bwd_bits(&r, local.log);
    for(;;) {
      CHECK(n < HUF_MAX_SYMBS - 1);
      weights[n++] = fse_peek(&local, s1);
      fse_update(&local, &s1, &r);
      if(r.pos < 0) { weights[n++] = fse_peek(&local, s2); break; }
      CHECK(n < HUF_MAX_SYMBS - 1);
      weights[n++] = fse_peek(&local, s2);
      fse_update(&local, &s2, &r);
      if(r.pos < 0) { weights[n++] = fse_peek(&local, s1); break; }
    }
    *used = 1 + header;
  } else {
    n = header - 127;
    size_t bytes = ((size_t)n + 1) / 2;
    CHECK(len >= 1 + bytes);
    for(int i = 0; i < n; i += 2) {
      weights[i] = p[1 + i / 2] >> 4;
      if(i + 1 < n) weights[i + 1] = p[1 + i / 2] & 15;
    }
    *used = 1 + bytes;
  }
  return huf_build(h, weights, n);
}

static int
huf_stream(const huf_t *h, const uint8_t *p, size_t len, uint8_t *out, size_t count) {
  bwd_t r;
  CHECK(!bwd_init(&r, p, len));
  uint32_t mask = (1u << h->max_bits) - 1;
  uint32_t state = (uint32_t)bwd_bits(&r, h->max_bits);
  for(size_t i = 0; i < count; i++) {
    out[i] = h->symbol[state];
    uint8_t n = h->nbits[state];
    state = ((state << n) + (uint32_t)bwd_bits(&r, n)) & mask;
  }
  CHECK(r.pos == -h->max_bits);
  return 0;
}


/* ---------------- sequences ---------------- */

static const uint32_t ll_base[36] = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
  16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096,
  8192, 16384, 32768, 65536 };
static const uint8_t ll_bits[36] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const uint32_t ml_base[53] = {
  3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
  27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515,
  1027, 2051, 4099, 8195, 16387, 32771, 65539 };
static const uint8_t ml_bits[53] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const int16_t ll_default[36] = {
  4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1,
  -1, -1, -1, -1 };
static const int16_t ml_default[53] = {
  1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1 };
static const int16_t of_default[29] = {
  1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1 };


typedef struct frame_ctx {
  uint8_t *out;
  size_t   cap, len;
  size_t   window;
  uint32_t rep[3];
  huf_t    huf;
  fse_t    ll, of, ml;
  int      ll_ok, of_ok, ml_ok;
  uint8_t  lit[BLOCK_MAX];
} frame_ctx_t;


static int
read_table(fse_t *t, int *ok, int mode, const uint8_t **p, const uint8_t *end,
           const int16_t *def, int def_n, int def_log, int max_log, int max_sym) {
  switch(mode) {
  case 0:
    CHECK(!fse_build(t, def, def_n, def_log));
    break;
  case 1:
    CHECK(*p < end && **p <= max_sym);
    fse_rle(t, **p);
    (*p)++;
    break;
  case 2: {
    size_t used;
    CHECK(!fse_read(t, *p, end - *p, max_log, max_sym, &used));
    *p += used;
    break;
  }
  default:
    CHECK(*ok);
    return 0;
  }
  *ok = 1;
  return 0;
}


static int
decode_literals(frame_ctx_t *c, const uint8_t **pp, const uint8_t *end, size_t *nlit) {
  const uint8_t *p = *pp;
  CHECK(p < end);
  int type = p[0] & 3, fmt = (p[0] >> 2) & 3;
  size_t regen, comp = 0, hdr;
  int streams = 1;

  if(type < 2) {
    if(fmt == 0 || fmt == 2) { regen = p[0] >> 3; hdr = 1; }
    else if(fmt == 1) { CHECK(end - p >= 2); regen = (p[0] >> 4) + ((size_t)p[1] << 4); hdr = 2; }
    else { CHECK(end - p >= 3); regen = (p[0] >> 4) + ((size_t)p[1] << 4) + ((size_t)p[2] << 12); hdr = 3; }
    CHECK(regen <= BLOCK_MAX);
    p += hdr;
    if(type == 0) {
      CHECK((size_t)(end - p) >= regen);
      memcpy(c->lit, p, regen);
      p += regen;
    } else {
      CHECK(p < end);
      memset(c->lit, *p, regen);
      p++;
    }
  } else {
    uint64_t v = 0;
    if(fmt <= 1) {
      CHECK(end - p >= 3);
      v = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
      regen = (v >> 4) & 0x3ff; comp = (v >> 14) & 0x3ff; hdr = 3;
      streams = fmt == 0 ? 1 : 4;
    } else if(fmt == 2) {
      CHECK(end - p >= 4);
      v = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint64_t)p[3] << 24;
      regen = (v >> 4) & 0x3fff; comp = (v >> 18) & 0x3fff; hdr = 4; streams = 4;
    } else {
      CHECK(end - p >= 5);
      v = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32;
      regen = (v >> 4) & 0x3ffff; comp = (v >> 22) & 0x3ffff; hdr = 5; streams = 4;
    }
    CHECK(regen <= BLOCK_MAX);
    p += hdr;
    CHECK((size_t)(end - p) >= comp);
    const uint8_t *q = p, *qend = p + comp;
    if(type == 2) {
      size_t used;
      CHECK(!huf_read(&c->huf, q, comp, &used));
      q += used;
    } else {
      CHECK(c->huf.ready);
    }
    if(streams == 1) {
      CHECK(!huf_stream(&c->huf, q, qend - q, c->lit, regen));
    } else {
      CHECK(qend - q >= 6);
      size_t s1 = q[0] | q[1] << 8, s2 = q[2] | q[3] << 8, s3 = q[4] | q[5] << 8;
      q += 6;
      CHECK((size_t)(qend - q) >= s1 + s2 + s3);
      size_t s4 = (qend - q) - s1 - s2 - s3;
      size_t part = (regen + 3) / 4;
      CHECK(regen >= 3 * part);
      CHECK(!huf_stream(&c->huf, q, s1, c->lit, part));
      CHECK(!huf_stream(&c->huf, q + s1, s2, c->lit + part, part));
      CHECK(!huf_stream(&c->huf, q + s1 + s2, s3, c->lit + 2 * part, part));
      CHECK(!huf_stream(&c->huf, q + s1 + s2 + s3, s4, c->lit + 3 * part, regen - 3 * part));
    }
    p = qend;
  }
  *nlit = regen;
  *pp = p;
  return 0;
}


static int
emit(frame_ctx_t *c, const uint8_t *src, size_t n) {
  CHECK(c->len + n <= c->cap);
  memcpy(c->out + c->len, src, n);
  c->len += n;
  return 0;
}


static int
compressed_block(frame_ctx_t *c, const uint8_t *p, size_t size) {
  const uint8_t *end = p + size;
  size_t nlit;
  CHECK(!decode_literals(c, &p, end, &nlit));

  CHECK(p < end);
  uint32_t nseq = p[0];
  if(nseq == 0) {
    p++;
  } else if(nseq < 128) {
    p++;
  } else if(nseq < 255) {
    CHECK(end - p >= 2);
    nseq = ((nseq - 128) << 8) + p[1];
    p += 2;
  } else {
    CHECK(end - p >= 3);
    nseq = p[1] + ((uint32_t)p[2] << 8) + 0x7F00;
    p += 3;
  }
  if(nseq == 0) return emit(c, c->lit, nlit);

  CHECK(p < end);
  uint8_t modes = *p++;
  CHECK((modes & 3) == 0);
  CHECK(!read_table(&c->ll, &c->ll_ok, modes >> 6, &p, end, ll_default, 36, 6, LL_MAX_LOG, 35));
  CHECK(!read_table(&c->of, &c->of_ok, (modes >> 4) & 3, &p, end, of_default, 29, 5, OF_MAX_LOG, 31));
  CHECK(!read_table(&c->ml, &c->ml_ok, (modes >> 2) & 3, &p, end, ml_default, 53, 6, ML_MAX_LOG, 52));

  bwd_t r;
  CHECK(!bwd_init(&r, p, end - p));
  uint16_t sll = (uint16_t)bwd_bits(&r, c->ll.log);
  uint16_t sof = (uint16_t)bwd_bits(&r, c->of.log);
  uint16_t sml = (uint16_t)bwd_bits(&r, c->ml.log);
  size_t lit_pos = 0;

  for(uint32_t i = 0; i < nseq; i++) {
    uint8_t of_code = fse_peek(&c->of, sof), ll_code = fse_peek(&c->ll, sll), ml_code = fse_peek(&c->ml, sml);
    CHECK(of_code <= 31 && ll_code <= 35 && ml_code <= 52);
    uint32_t ofv = (1u << of_code) + (uint32_t)bwd_bits(&r, of_code);
    uint32_t ml = ml_base[ml_code] + (uint32_t)bwd_bits(&r, ml_bits[ml_code]);
    uint32_t ll = ll_base[ll_code] + (uint32_t)bwd_bits(&r, ll_bits[ll_code]);

    uint32_t offset;
    if(ofv > 3) {
      offset = ofv - 3;
      c->rep[2] = c->rep[1];
      c->rep[1] = c->rep[0];
      c->rep[0] = offset;
    } else {
      uint32_t idx = ofv - 1 + (ll == 0);
      offset = idx == 3 ? c->rep[0] - 1 : c->rep[idx];
      if(idx > 0) {
        if(idx > 1) c->rep[2] = c->rep[1];
        c->rep[1] = c->rep[0];
        c->rep[0] = offset;
      }
    }

    CHECK(lit_pos + ll <= nlit);
    CHECK(!emit(c, c->lit + lit_pos, ll));
    lit_pos += ll;
    CHECK(offset > 0 && offset <= c->len && c->len + ml <= c->cap);
    uint8_t *dst = c->out + c->len;
    const uint8_t *src = dst - offset;
    for(uint32_t k = 0; k < ml; k++) dst[k] = src[k];
    c->len += ml;

    if(i + 1 < nseq) {
      fse_update(&c->ll, &sll, &r);
      fse_update(&c->ml, &sml, &r);
      fse_update(&c->of, &sof, &r);
    }
  }
  CHECK(r.pos == 0);
  return emit(c, c->lit + lit_pos, nlit - lit_pos);
}


/* ---------------- XXH64 (content checksum) ---------------- */

#define P1 0x9E3779B185EBCA87ull
#define P2 0xC2B2AE3D27D4EB4Full
#define P3 0x165667B19E3779F9ull
#define P4 0x85EBCA77C2B2AE63ull
#define P5 0x27D4EB2F165667C5ull

static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t rd64(const uint8_t *p) { uint64_t v = 0; for(int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t xround(uint64_t acc, uint64_t in) { return rotl(acc + in * P2, 31) * P1; }
static uint64_t xmerge(uint64_t acc, uint64_t v) { return (acc ^ xround(0, v)) * P1 + P4; }

uint64_t
xxh64(const void *data, size_t len) {
  const uint8_t *p = data, *end = p + len;
  uint64_t h;
  if(len >= 32) {
    uint64_t v1 = P1 + P2, v2 = P2, v3 = 0, v4 = -P1;
    while(end - p >= 32) {
      v1 = xround(v1, rd64(p)); v2 = xround(v2, rd64(p + 8));
      v3 = xround(v3, rd64(p + 16)); v4 = xround(v4, rd64(p + 24));
      p += 32;
    }
    h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
    h = xmerge(h, v1); h = xmerge(h, v2); h = xmerge(h, v3); h = xmerge(h, v4);
  } else {
    h = P5;
  }
  h += len;
  while(end - p >= 8) { h ^= xround(0, rd64(p)); h = rotl(h, 27) * P1 + P4; p += 8; }
  if(end - p >= 4) { h ^= (uint64_t)rd32(p) * P1; h = rotl(h, 23) * P2 + P3; p += 4; }
  while(p < end) { h ^= (*p++) * P5; h = rotl(h, 11) * P1; }
  h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
  return h;
}


/* ---------------- frames ---------------- */

/* Content size declared by the first frame, or -1 if unknown/invalid. */
int64_t
zstd_content_size(const uint8_t *src, size_t len) {
  if(len < 6 || rd32(src) != ZSTD_MAGIC) return -1;
  uint8_t fhd = src[4];
  int fcs_flag = fhd >> 6, single = (fhd >> 5) & 1, dict_flag = fhd & 3;
  size_t pos = 5 + (single ? 0 : 1) + (dict_flag == 3 ? 4 : dict_flag);
  int fcs_bytes = fcs_flag == 0 ? (single ? 1 : 0) : 1 << fcs_flag;
  if(!fcs_bytes || pos + fcs_bytes > len) return -1;
  uint64_t v = 0;
  for(int i = fcs_bytes - 1; i >= 0; i--) v = v << 8 | src[pos + i];
  if(fcs_bytes == 2) v += 256;
  return (int64_t)v;
}


/* Decompress all frames of `src` into `dst` (capacity cap). Returns the
   decompressed length, or -1 on any error. */
int64_t
zstd_decompress(const uint8_t *src, size_t len, uint8_t *dst, size_t cap) {
  frame_ctx_t *c = calloc(1, sizeof(*c));
  if(!c) return -1;
  c->out = dst;
  c->cap = cap;
  const uint8_t *p = src, *end = src + len;
  int ok = 1;

  while(ok && p < end) {
    if(end - p < 4) { ok = 0; break; }
    uint32_t magic = rd32(p);
    if((magic & SKIP_MAGIC_MASK) == SKIP_MAGIC) {
      if(end - p < 8) { ok = 0; break; }
      uint32_t n = rd32(p + 4);
      if((size_t)(end - p - 8) < n) { ok = 0; break; }
      p += 8 + n;
      continue;
    }
    if(magic != ZSTD_MAGIC || end - p < 6) { ok = 0; break; }
    uint8_t fhd = p[4];
    int fcs_flag = fhd >> 6, single = (fhd >> 5) & 1, checksum = (fhd >> 2) & 1, dict_flag = fhd & 3;
    if(fhd & 8) { ok = 0; break; }                 /* reserved bit */
    const uint8_t *q = p + 5;
    if(!single) {
      if(q >= end) { ok = 0; break; }
      int exp = *q >> 3, mant = *q & 7;
      uint64_t base = 1ull << (10 + exp);
      c->window = (size_t)(base + (base / 8) * mant);
      q++;
    }
    int dict_bytes = dict_flag == 3 ? 4 : dict_flag;
    if(end - q < dict_bytes) { ok = 0; break; }
    uint32_t dict = 0;
    for(int i = dict_bytes - 1; i >= 0; i--) dict = dict << 8 | q[i];
    if(dict) { ok = 0; break; }                    /* dictionaries not supported */
    q += dict_bytes;
    int fcs_bytes = fcs_flag == 0 ? (single ? 1 : 0) : 1 << fcs_flag;
    if(end - q < fcs_bytes) { ok = 0; break; }
    q += fcs_bytes;

    size_t frame_start = c->len;
    c->rep[0] = 1; c->rep[1] = 4; c->rep[2] = 8;
    c->huf.ready = 0;
    c->ll_ok = c->of_ok = c->ml_ok = 0;
    for(;;) {
      if(end - q < 3) { ok = 0; break; }
      uint32_t bh = q[0] | q[1] << 8 | q[2] << 16;
      int last = bh & 1, type = (bh >> 1) & 3;
      size_t size = bh >> 3;
      q += 3;
      if(type == 0) {
        if((size_t)(end - q) < size || emit(c, q, size)) { ok = 0; break; }
        q += size;
      } else if(type == 1) {
        if(q >= end || c->len + size > c->cap) { ok = 0; break; }
        memset(c->out + c->len, *q, size);
        c->len += size;
        q++;
      } else if(type == 2) {
        if(size > BLOCK_MAX || (size_t)(end - q) < size || compressed_block(c, q, size)) { ok = 0; break; }
        q += size;
      } else {
        ok = 0;
        break;
      }
      if(last) break;
    }
    if(!ok) break;
    if(checksum) {
      if(end - q < 4) { ok = 0; break; }
      uint64_t h = xxh64(c->out + frame_start, c->len - frame_start);
      if((uint32_t)h != rd32(q)) { ok = 0; break; }
      q += 4;
    }
    p = q;
  }
  int64_t out = ok ? (int64_t)c->len : -1;
  free(c);
  return out;
}
