/* ATRAC9 (.at9) checks for replacements of the console's own ATRAC9 sounds,
   such as the home music /system_ex/vsh_asset/bgm_home.at9.

   A replacement must use the system file's exact ATRAC9 format (the same fmt
   chunk: channel layout, sample rate, bitrate and codec config) and the same
   set of chunks, and every superframe must unpack cleanly. Unpacking follows
   the reference decoders (LibAtrac9 and FFmpeg) and takes the stricter rule
   wherever they differ. On top of that it may only use coding features the
   system file itself uses (its profile), because the console's decoder is
   Sony's own. Nothing is decoded to sound here; the browser checks that
   before upload. */

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>

#include "theme.h"
#include "atrac9_tables.h"


#define AT9_MAX_BYTES     (64ull << 20)
#define AT9_GROWTH        2            /* a replacement may be up to twice the system file's length */

enum { MONO, STEREO, LFE };

static const uint8_t channel_configs[6][5] = {
  {MONO}, {MONO, MONO}, {STEREO}, {STEREO, MONO, LFE, STEREO},
  {STEREO, MONO, LFE, STEREO, STEREO}, {STEREO, STEREO},
};
static const uint8_t block_counts[6] = {1, 2, 1, 4, 5, 2};
static const uint8_t max_bands[16] = {8, 8, 12, 12, 12, 18, 18, 18, 8, 8, 12, 12, 12, 16, 16, 16};
static const uint8_t frame_power[16] = {6, 6, 7, 7, 7, 8, 8, 8, 6, 6, 7, 7, 7, 8, 8, 8};
static const uint8_t band_units[19] = {0, 4, 8, 10, 12, 13, 14, 15, 16, 18, 20, 21, 22, 23, 24, 25, 26, 28, 30};
static const uint16_t unit_start[31] = {
  0, 2, 4, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 40, 48, 56,
  64, 72, 80, 88, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256};
static const uint8_t unit_book[30] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3};
static const uint8_t sf_weights[8][32] = {
  {0, 0, 0, 1, 1, 2, 2, 2, 2, 2, 2, 3, 2, 3, 3, 4, 4, 4, 4, 4, 4, 5, 5, 6, 6, 7, 7, 8, 10, 12, 12, 12},
  {3, 2, 2, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 2, 3, 3, 4, 5, 7, 10, 10, 10},
  {0, 2, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 8, 9, 12, 12, 12},
  {0, 1, 1, 2, 2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6, 6, 7, 8, 8, 10, 11, 11, 12, 13, 13, 13, 13},
  {0, 2, 2, 3, 3, 4, 4, 5, 4, 5, 5, 5, 5, 6, 7, 8, 8, 8, 8, 9, 9, 9, 10, 10, 11, 12, 12, 13, 13, 14, 14, 14},
  {1, 1, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 5, 6, 7, 7, 9, 11, 11, 11},
  {0, 5, 8, 10, 11, 11, 12, 12, 12, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 12, 12, 12, 12, 13, 15, 15, 15},
  {0, 2, 3, 4, 5, 6, 6, 7, 7, 8, 8, 8, 9, 9, 10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 15, 15, 15},
};
static const uint8_t base_curve[48] = {
  1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 11, 12, 13,
  15, 16, 18, 19, 20, 21, 22, 23, 24, 25, 26, 26, 27, 27, 28, 28, 28, 29, 29, 29, 29, 30, 30, 30, 30};
static const uint8_t bex_groups[8][3] = {
  {16, 21, 0}, {18, 22, 1}, {20, 22, 2}, {21, 22, 3}, {21, 22, 3}, {23, 24, 4}, {23, 24, 4}, {24, 24, 5}};
static const uint8_t bex_counts[5][6] = {
  {0, 0, 0, 4, 4, 2}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 2, 2, 1}, {0, 0, 0, 2, 2, 2}, {1, 1, 1, 0, 0, 0}};
static const uint8_t bex_lengths[5][6][4] = {
  {{0}, {0}, {0}, {5, 4, 3, 3}, {4, 4, 3, 4}, {4, 5}},
  {{0}},
  {{0}, {0}, {0}, {6, 6}, {6, 6}, {6}},
  {{0}, {0}, {0}, {4, 4}, {4, 4}, {4, 4}},
  {{3}, {3}, {3}}};


/* ---- Huffman lookup (built once, read-only afterwards) ---- */

static int16_t lookup_sf_unsigned[7][256];
static int16_t lookup_sf_signed[6][256];
static int16_t lookup_spectrum[2][8][4][1024];
static pthread_once_t lookup_once = PTHREAD_ONCE_INIT;

static void
build_lookup(const at9_huff_t *h, int16_t *table) {
  if(!h->bits) return;
  for(int i = 0; i < 1 << h->max_bits; i++) table[i] = -1;
  for(int s = 0; s < h->length; s++) {
    if(!h->bits[s]) continue;
    int shift = h->max_bits - h->bits[s];
    for(int j = h->codes[s] << shift; j < (h->codes[s] + 1) << shift; j++) table[j] = (int16_t)s;
  }
}

static void
build_lookups(void) {
  for(int i = 0; i < 7; i++) build_lookup(&at9_sf_unsigned[i], lookup_sf_unsigned[i]);
  for(int i = 0; i < 6; i++) build_lookup(&at9_sf_signed[i], lookup_sf_signed[i]);
  for(int s = 0; s < 2; s++)
    for(int p = 0; p < 8; p++)
      for(int b = 0; b < 4; b++) build_lookup(&at9_spectrum[s][p][b], lookup_spectrum[s][p][b]);
}


/* ---- bit reader, bounded to one superframe ---- */

typedef struct {
  const uint8_t *b;
  size_t pos, end;              /* in bits */
} br_t;

static uint32_t
peek(const br_t *r, int n) {
  uint32_t w = 0;
  size_t byte = r->pos >> 3, last = r->end >> 3;
  for(int i = 0; i < 4; i++) w = w << 8 | (byte + i < last ? r->b[byte + i] : 0);
  return (uint32_t)(w << (r->pos & 7)) >> (32 - n);
}

/* Reads past the superframe leave pos > end; callers check once per block. */
static uint32_t
rd(br_t *r, int n) {
  if(!n) return 0;
  uint32_t v = peek(r, n);
  r->pos += n;
  return v;
}

static int
sign_extend(uint32_t v, int bits) {
  return (int)(v << (32 - bits)) >> (32 - bits);
}

static int
huff(br_t *r, const at9_huff_t *h, const int16_t *table) {
  int s = table[peek(r, h->max_bits)];
  if(s < 0) return -1;
  r->pos += h->bits[s];
  return s;
}


/* ---- unpacking ---- */

typedef struct {
  int index, coded;
  int sf[49], sf_prev[49], prec[48], fine[48];
  uint8_t set[48];
} chan_t;

typedef struct {
  int type, nch;
  chan_t ch[2];
  int have_bands, band_count, units, stereo_units, bex, ext_unit, units_prev;
  int grad_mode, boundary, primary, gradient[49];
} block_t;

typedef struct {
  int sri, cci, high, frame_bytes, frames, superframe_bytes;
} config_t;

static const char *
make_gradient(block_t *b, int start, int end, int start_value, int end_value) {
  if(start >= end || end > 31) return "invalid bit allocation range";
  if(b->boundary > b->units) return "invalid bit allocation boundary";
  int values = end_value - start_value;
  for(int i = 0; i < end; i++) b->gradient[i] = start_value;
  for(int i = end; i <= b->units; i++) b->gradient[i] = end_value;
  if(!values) return NULL;
  int len = end - start;
  double scale = (abs(values) - 1) / 31.0;
  for(int i = start; i < end; i++) {
    int step = (int)(base_curve[(i - start) * 48 / len] * scale);
    b->gradient[i] = values > 0 ? start_value + 1 + step : start_value - 1 - step;
  }
  return NULL;
}

static const char *
precisions(block_t *b, chan_t *c) {
  int mask[48] = {0};
  if(b->grad_mode) {
    for(int i = 1; i < b->units; i++) {
      int d = c->sf[i] - c->sf[i - 1];
      if(d > 1) mask[i] += d - 1 < 5 ? d - 1 : 5;
      else if(d < -1) mask[i - 1] += -d - 1 < 5 ? -d - 1 : 5;
    }
  }
  for(int i = 0; i < b->units; i++) {
    int p;
    if(b->grad_mode) {
      p = c->sf[i] + mask[i] - b->gradient[i];
      if(p > 0) p = b->grad_mode == 1 ? p >> 1 : b->grad_mode == 2 ? (3 * p) >> 3 : p >> 2;
    } else {
      p = c->sf[i] - b->gradient[i];
    }
    if(p < 1) p = 1;
    if(i < b->boundary) p++;
    if(p > 30) return "precision out of range";
    c->fine[i] = p > 15 ? p - 15 : 0;
    c->prec[i] = p > 15 ? 15 : p;
  }
  return NULL;
}

static void
codebook_sets(const config_t *cfg, chan_t *c, int units) {
  int *sf = c->sf;
  memset(c->set, 0, sizeof(c->set));
  if(units <= 1 || cfg->high) return;
  int keep = sf[units];
  sf[units] = sf[units - 1];
  int avg = 0;
  if(units > 12) {
    for(int i = 0; i < 12; i++) avg += sf[i];
    avg = (avg + 6) / 12;
  }
  for(int i = 8; i < units; i++) {
    int lo = sf[i - 1] < sf[i + 1] ? sf[i - 1] : sf[i + 1];
    if(sf[i] - lo >= 3 || sf[i] - sf[i - 1] + sf[i] - sf[i + 1] >= 3) c->set[i] = 1;
  }
  for(int i = 12; i < units; i++) {
    if(c->set[i]) continue;
    int lo = sf[i - 1] < sf[i + 1] ? sf[i - 1] : sf[i + 1];
    if(sf[i] - lo >= 2 && sf[i] >= avg - (unit_start[i + 1] - unit_start[i] == 16 ? 1 : 0)) c->set[i] = 1;
  }
  sf[units] = keep;
}

static const char *
delta_offset(br_t *r, int *sf, const uint8_t *weights, int base, int len, int count) {
  const at9_huff_t *h = &at9_sf_unsigned[len];
  sf[0] = (int)rd(r, len);
  for(int i = 1; i < count; i++) {
    int d = huff(r, h, lookup_sf_unsigned[len]);
    if(d < 0) return "invalid scale factor code";
    sf[i] = (sf[i - 1] + d) & ((1 << len) - 1);
  }
  for(int i = 0; i < count; i++) sf[i] += base - (weights ? weights[i] : 0);
  return NULL;
}

static const char *
distance(br_t *r, int *sf, const int *baseline, int base_len, int n) {
  int len = (int)rd(r, 2) + 2, count = n < base_len ? n : base_len;
  const at9_huff_t *h = &at9_sf_signed[len];
  for(int i = 0; i < count; i++) {
    int s = huff(r, h, lookup_sf_signed[len]);
    if(s < 0) return "invalid scale factor code";
    sf[i] = baseline[i] + sign_extend((uint32_t)s, len);
  }
  for(int i = count; i < n; i++) sf[i] = (int)rd(r, 5);
  return NULL;
}

static const char *
offset_baseline(br_t *r, int *sf, const int *baseline, int base_len, int n) {
  int base = (int)rd(r, 5) - 16, len = (int)rd(r, 2) + 1, count = n < base_len ? n : base_len;
  const char *err = delta_offset(r, sf, NULL, 0, len, count);
  if(err) return err;
  for(int i = 0; i < count; i++) sf[i] += base + baseline[i];
  for(int i = count; i < n; i++) sf[i] = (int)rd(r, 5);
  return NULL;
}

static const char *
read_scale_factors(br_t *r, block_t *b, chan_t *c, int first) {
  int n = b->ext_unit, mode = (int)rd(r, 2);
  const char *err = NULL;
  memset(c->sf, 0, sizeof(c->sf));
  if(mode == 0) {
    const uint8_t *w = sf_weights[rd(r, 3)];
    int base = (int)rd(r, 5), len = (int)rd(r, 2) + 3;
    err = delta_offset(r, c->sf, w, base, len, n);
  } else if(c->index == 0) {
    if(mode == 1) {
      int len = (int)rd(r, 2) + 2, base = len < 5 ? (int)rd(r, 5) : 0;
      for(int i = 0; i < n; i++) c->sf[i] = (int)rd(r, len) + base;
    } else if(first) {
      return "scale factors refer to a previous frame in the first frame";
    } else if(mode == 2) {
      err = distance(r, c->sf, c->sf_prev, b->units_prev, n);
    } else {
      err = offset_baseline(r, c->sf, c->sf_prev, b->units_prev, n);
    }
  } else if(mode == 1) {
    err = distance(r, c->sf, b->ch[0].sf, n, n);
  } else if(mode == 2) {
    err = offset_baseline(r, c->sf, b->ch[0].sf, n, n);
  } else if(first) {
    return "scale factors refer to a previous frame in the first frame";
  } else {
    err = distance(r, c->sf, c->sf_prev, b->units_prev, n);
  }
  if(err) return err;
  for(int i = 0; i < n; i++) if(c->sf[i] < 0 || c->sf[i] > 31) return "scale factor out of range";
  memcpy(c->sf_prev, c->sf, sizeof(c->sf));
  return NULL;
}

static const char *
read_spectra(br_t *r, const config_t *cfg, chan_t *c) {
  int max_huff = cfg->high ? 1 : 7;
  for(int i = 0; i < c->coded; i++) {
    int bits = c->prec[i] + 1, count = unit_start[i + 1] - unit_start[i];
    if(bits <= max_huff) {
      int set = c->set[i], book = unit_book[i];
      const at9_huff_t *h = &at9_spectrum[set][bits][book];
      if(!h->bits) return "invalid spectrum codebook";
      for(int k = 0; k < count; k += h->value_count) {
        if(huff(r, h, lookup_spectrum[set][bits][book]) < 0) return "invalid spectrum code";
      }
    } else {
      r->pos += (size_t)count * bits;
    }
  }
  for(int i = 0; i < c->coded; i++) {
    if(c->fine[i]) r->pos += (size_t)(unit_start[i + 1] - unit_start[i]) * (c->fine[i] + 1);
  }
  return NULL;
}

static const char *
read_bex(br_t *r, block_t *b) {
  int band = 0, mode[2] = {0, 0};
  if(b->bex) {
    if(b->units < 13 || b->units > 20) return "band extension with an invalid band count";
    band = bex_groups[b->units - 13][2];
    if(b->type == STEREO) {
      int m = (int)rd(r, 2);
      mode[1] = band > 2 ? m : 4;
    } else {
      rd(r, 1);
    }
  }
  if(!rd(r, 1)) return NULL;
  if(!b->bex) {
    rd(r, 2);
    r->pos += rd(r, 5);
    return NULL;
  }
  int m = (int)rd(r, 2);
  mode[0] = band > 2 ? m : 4;
  int len = (int)rd(r, 5);
  if(!len) return NULL;
  size_t end = r->pos + len;
  for(int c = 0; c < b->nch; c++) {
    for(int i = 0; i < bex_counts[mode[c]][band]; i++) r->pos += bex_lengths[mode[c]][band][i];
  }
  if(r->pos > end) return "band extension data is longer than declared";
  return NULL;
}

/* The coding features a file uses. A replacement must stay within the
   system file's profile. The console decodes with Sony's decoder, which minds
   things LibAtrac9 and FFmpeg ignore: zeros in the unused end of superframes,
   where Sony writes 0x01, made it play ticks or silence (confirmed on 13.42).
   The other limits keep replacements to what that decoder demonstrably
   plays. */
typedef struct {
  unsigned modes, lfe_reuse;       /* bit masks */
  int max_bands, max_precision, fine, bex, max_frame;
  uint8_t padding[32];             /* byte values filling the unused end of superframes */
} profile_t;

static const char *
unpack_block(br_t *r, const config_t *cfg, block_t *b, int frame, profile_t *prof) {
  int first = !rd(r, 1), reuse = (int)rd(r, 1);
  const char *err;
  if(first != (frame == 0)) return "superframe flag does not match the frame position";
  if(b->type == LFE) {
    r->pos += 10 + 4 * ((reuse ? 8 : 4) + 1);
    prof->lfe_reuse |= 1u << reuse;
    return NULL;
  }
  if(first && reuse) return "first frame of a superframe reuses band parameters";
  int min_bands = cfg->high ? 1 : 3;
  if(!reuse) {
    b->band_count = (int)rd(r, 4) + min_bands;
    if(b->band_count > max_bands[cfg->sri]) return "band count out of range";
    b->units = band_units[b->band_count];
    int stereo_band = b->type == STEREO ? (int)rd(r, 4) + min_bands : b->band_count;
    if(stereo_band > b->band_count) return "stereo band out of range";
    b->stereo_units = band_units[stereo_band];
    b->bex = (int)rd(r, 1);
    if(b->bex) {
      int ext = (int)rd(r, 4) + min_bands;
      if(ext < b->band_count || ext > (cfg->high ? 16 : 18)) return "extension band out of range";
      b->ext_unit = band_units[ext];
    } else {
      b->ext_unit = b->units;
    }
    b->have_bands = 1;
  } else if(!b->have_bands) {
    return "block reuses band parameters it never had";
  }

  int start, end, start_value, end_value;
  b->grad_mode = (int)rd(r, 2);
  if(b->grad_mode) {
    start = (int)rd(r, 5); end = 31; start_value = (int)rd(r, 5); end_value = 31;
  } else {
    start = (int)rd(r, 6); end = (int)rd(r, 6) + 1; start_value = (int)rd(r, 5); end_value = (int)rd(r, 5);
  }
  b->boundary = (int)rd(r, 4);
  if((err = make_gradient(b, start, end, start_value, end_value))) return err;

  b->primary = 0;
  if(b->type == STEREO) {
    b->primary = (int)rd(r, 1);
    if(rd(r, 1)) r->pos += b->units - b->stereo_units;
  }
  if((err = read_bex(r, b))) return err;

  for(int i = 0; i < b->nch; i++) {
    chan_t *c = &b->ch[i];
    c->coded = c->index == b->primary ? b->units : b->stereo_units;
    if((err = read_scale_factors(r, b, c, first))) return err;
    if((err = precisions(b, c))) return err;
    codebook_sets(cfg, c, c->coded);
    if((err = read_spectra(r, cfg, c))) return err;
    for(int u = 0; u < c->coded; u++) {
      if(c->prec[u] > prof->max_precision) prof->max_precision = c->prec[u];
      if(c->fine[u]) prof->fine = 1;
    }
  }
  b->units_prev = b->bex ? b->ext_unit : b->units;
  prof->modes |= 1u << b->grad_mode;
  if(b->band_count > prof->max_bands) prof->max_bands = b->band_count;
  if(b->bex) prof->bex = 1;
  return NULL;
}


/* ---- container ---- */

typedef struct {
  const uint8_t *fmt;
  uint32_t fmt_size;
  const uint8_t *fact, *smpl;
  uint32_t fact_size, smpl_size;
  uint64_t data_offset, data_size;
} riff_t;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Walk the chunks of a RIFF WAVE held in `b` (n bytes, possibly just the
   head of the file when total > n: then the data chunk need not fit).
   `strict` refuses chunks other than fmt, fact, smpl and data. */
static const char *
parse_riff(const uint8_t *b, size_t n, uint64_t total, int strict, riff_t *out) {
  memset(out, 0, sizeof(*out));
  if(n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return "not a RIFF WAVE file";
  if((uint64_t)le32(b + 4) + 8 != total) return "RIFF size does not match the file size";
  for(uint64_t off = 12; off < total;) {
    if(off + 8 > n) {
      if(!strict && out->data_offset) break;      /* only the head was read; chunks after data are skipped */
      return "damaged chunk list";
    }
    const uint8_t *h = b + off;
    uint32_t size = le32(h + 4);
    uint64_t next = off + 8 + size + (size & 1);
    if(off + 8 + size > total) return "a chunk runs past the end of the file";
    if(!memcmp(h, "data", 4)) {
      if(out->data_offset) return "more than one data chunk";
      out->data_offset = off + 8;
      out->data_size = size;
    } else {
      int fmt = !memcmp(h, "fmt ", 4), fact = !memcmp(h, "fact", 4), smpl = !memcmp(h, "smpl", 4);
      const uint8_t **slot = fmt ? &out->fmt : fact ? &out->fact : smpl ? &out->smpl : NULL;
      uint32_t *slot_size = fmt ? &out->fmt_size : fact ? &out->fact_size : &out->smpl_size;
      if(!slot) {
        if(strict) return "unexpected chunk in an ATRAC9 file";
      } else {
        if(*slot) return "repeated chunk";
        if(off + 8 + size > n) return "header chunk too large";
        *slot = h + 8;
        *slot_size = size;
      }
    }
    off = next;
  }
  if(!out->fmt || !out->data_offset) return "missing fmt or data chunk";
  return NULL;
}

static int
is_atrac9(const riff_t *r) {
  static const uint8_t guid[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
                                   0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
  return r->fmt_size >= 52 && le16(r->fmt) == 0xfffe && !memcmp(r->fmt + 24, guid, 16);
}

static uint8_t *
read_file(const char *path, size_t max, size_t *len) {
  struct stat st;
  int fd = open(path, O_RDONLY);
  if(fd < 0) return NULL;
  if(fstat(fd, &st) || !S_ISREG(st.st_mode) || !st.st_size || (uint64_t)st.st_size > max) {
    close(fd);
    return NULL;
  }
  uint8_t *b = malloc((size_t)st.st_size);
  size_t got = 0;
  while(b && got < (size_t)st.st_size) {
    ssize_t r = read(fd, b + got, (size_t)st.st_size - got);
    if(r <= 0) break;
    got += (size_t)r;
  }
  close(fd);
  if(!b || got != (size_t)st.st_size) {
    free(b);
    return NULL;
  }
  *len = got;
  return b;
}


static void
config_from_fmt(const uint8_t *fmt, config_t *cfg) {
  const uint8_t *b = fmt + 44;
  cfg->sri = b[1] >> 4;
  cfg->cci = (b[1] >> 1) & 7;
  cfg->high = cfg->sri > 7;
  cfg->frame_bytes = ((b[2] << 3) | (b[3] >> 5)) + 1;
  cfg->frames = 1 << ((b[3] >> 3) & 3);
  cfg->superframe_bytes = cfg->frame_bytes * cfg->frames;
}

/* Unpack every superframe of `data` and record its profile. On damage,
   returns what is wrong and sets *at to the frame number. */
static const char *
scan(const uint8_t *data, uint64_t superframes, const config_t *cfg, profile_t *prof, uint64_t *at) {
  const char *err = NULL;
  block_t *blocks = calloc(5, sizeof(block_t));
  if(!blocks) return "out of memory";
  pthread_once(&lookup_once, build_lookups);
  memset(prof, 0, sizeof(*prof));
  int nblocks = block_counts[cfg->cci];
  for(int i = 0; i < nblocks; i++) {
    blocks[i].type = channel_configs[cfg->cci][i];
    blocks[i].nch = blocks[i].type == STEREO ? 2 : 1;
    blocks[i].ch[0].index = 0;
    blocks[i].ch[1].index = 1;
  }
  for(uint64_t sf = 0; sf < superframes && !err; sf++) {
    br_t r = { data + sf * cfg->superframe_bytes, 0, (size_t)cfg->superframe_bytes * 8 };
    for(int frame = 0; frame < cfg->frames && !err; frame++) {
      size_t start = r.pos;
      *at = sf * cfg->frames + frame;
      for(int i = 0; i < nblocks && !err; i++) {
        err = unpack_block(&r, cfg, &blocks[i], frame, prof);
        if(!err && r.pos > r.end) err = "a frame runs past the end of its block";
        r.pos = (r.pos + 7) & ~(size_t)7;
      }
      if((int)((r.pos - start) >> 3) > prof->max_frame) prof->max_frame = (int)((r.pos - start) >> 3);
    }
    if(!err) {
      const uint8_t *sb = data + sf * cfg->superframe_bytes;
      for(size_t p = r.pos >> 3; p < (size_t)cfg->superframe_bytes; p++) prof->padding[sb[p] >> 3] |= 1u << (sb[p] & 7);
    }
  }
  free(blocks);
  return err;
}

/* Why `got` uses something `stock` never does, or NULL (same rules and
   wording as fitsProfile in web/atrac9.js). */
static const char *
outside_profile(const profile_t *got, const profile_t *stock, int frame_bytes, char *buf, size_t len) {
  int limit = frame_bytes * 5 / 4 > stock->max_frame ? frame_bytes * 5 / 4 : stock->max_frame;
  if(got->modes & ~stock->modes) {
    unsigned extra = got->modes & ~stock->modes;
    int m = 0;
    while(!(extra & (1u << m))) m++;
    snprintf(buf, len, "uses bit allocation mode %d, which the system sound never does", m);
  } else if(got->max_bands > stock->max_bands) {
    snprintf(buf, len, "uses %d bands; the system sound uses at most %d", got->max_bands, stock->max_bands);
  } else if(got->max_precision > stock->max_precision) {
    snprintf(buf, len, "uses precision %d; the system sound uses at most %d", got->max_precision, stock->max_precision);
  } else if(got->fine && !stock->fine) {
    snprintf(buf, len, "uses fine precision, which the system sound never does");
  } else if(got->bex && !stock->bex) {
    snprintf(buf, len, "uses band extension, which the system sound never does");
  } else if(got->lfe_reuse & ~stock->lfe_reuse) {
    snprintf(buf, len, "codes the LFE channel differently from the system sound");
  } else if(got->max_frame > limit) {
    snprintf(buf, len, "has a %d-byte frame; the limit is %d", got->max_frame, limit);
  } else {
    /* Sony fills the unused end of a superframe with 0x01; its decoder minds. */
    int pad = 1, any = 0;
    for(int v = 0; v < 256; v++) {
      if(stock->padding[v >> 3] & (1u << (v & 7))) {
        if(!any || v == 1) pad = v;
        any = 1;
      }
    }
    for(int v = 0; v < 256; v++) {
      int allowed = v == pad || (stock->padding[v >> 3] & (1u << (v & 7)));
      if((got->padding[v >> 3] & (1u << (v & 7))) && !allowed) {
        snprintf(buf, len, "fills unused space with 0x%02x where the system sound uses 0x%02x", v, pad);
        return buf;
      }
    }
    return NULL;
  }
  return buf;
}

/* Profiles of system files, kept for the life of the plugin (they never
   change while it runs; the key includes size and modification time). */
#define PROFILE_CACHE 8
static struct {
  char path[512];
  off_t size;
  time_t mtime;
  profile_t prof;
} profile_cache[PROFILE_CACHE];
static int profile_next;
static pthread_mutex_t profile_lock = PTHREAD_MUTEX_INITIALIZER;

static int
stock_profile(const char *path, const struct stat *st, const riff_t *s, const uint8_t *b, profile_t *prof) {
  pthread_mutex_lock(&profile_lock);
  for(int i = 0; i < PROFILE_CACHE; i++) {
    if(!strcmp(profile_cache[i].path, path) && profile_cache[i].size == st->st_size && profile_cache[i].mtime == st->st_mtime) {
      *prof = profile_cache[i].prof;
      pthread_mutex_unlock(&profile_lock);
      return 0;
    }
  }
  pthread_mutex_unlock(&profile_lock);

  config_t cfg;
  uint64_t at;
  config_from_fmt(s->fmt, &cfg);
  if(cfg.cci > 5 || !cfg.superframe_bytes || s->data_size % cfg.superframe_bytes ||
     scan(b + s->data_offset, s->data_size / cfg.superframe_bytes, &cfg, prof, &at)) return -1;

  pthread_mutex_lock(&profile_lock);
  int i = profile_next++ % PROFILE_CACHE;
  snprintf(profile_cache[i].path, sizeof(profile_cache[i].path), "%s", path);
  profile_cache[i].size = st->st_size;
  profile_cache[i].mtime = st->st_mtime;
  profile_cache[i].prof = *prof;
  pthread_mutex_unlock(&profile_lock);
  return 0;
}


/* Policy check: `cand` must be a well-formed ATRAC9 file in exactly the
   format of the system file `stock`, using only coding features `stock`
   uses. 0 if it is. */
int
at9_validate(const char *cand, const char *stock, char *why, size_t len) {
  struct stat st;
  riff_t s, c;
  profile_t sprof, cprof;
  const char *err;
  size_t slen = 0, clen = 0;
  char buf[160];

  uint8_t *sb = stat(stock, &st) ? NULL : read_file(stock, AT9_MAX_BYTES, &slen);
  if(!sb || parse_riff(sb, slen, slen, 0, &s) || !is_atrac9(&s) || stock_profile(stock, &st, &s, sb, &sprof)) {
    snprintf(why, len, "cannot read the system sound to compare against");
    free(sb);
    return -1;
  }
  uint64_t stock_data = s.data_size;
  uint8_t stock_fmt[64];
  uint32_t stock_fmt_size = s.fmt_size < sizeof(stock_fmt) ? s.fmt_size : sizeof(stock_fmt);
  int stock_fact = !!s.fact, stock_smpl = !!s.smpl;
  memcpy(stock_fmt, s.fmt, stock_fmt_size);
  free(sb);

  uint8_t *b = read_file(cand, AT9_MAX_BYTES, &clen);
  if(!b) {
    snprintf(why, len, "not a readable ATRAC9 file");
    return -1;
  }
  if((err = parse_riff(b, clen, clen, 1, &c)) || !is_atrac9(&c)) {
    snprintf(why, len, "not a valid ATRAC9 (.at9) file%s%s", err ? ": " : "", err ? err : "");
    free(b);
    return -1;
  }

  /* Same format: the fmt chunk must match byte for byte, except the ATRAC9
     version word (1 and 2 are both in use). */
  uint32_t cver = le32(c.fmt + 40);
  if(c.fmt_size != stock_fmt_size || memcmp(c.fmt, stock_fmt, 40) || (cver != 1 && cver != 2) ||
     memcmp(c.fmt + 44, stock_fmt + 44, stock_fmt_size - 44)) {
    snprintf(why, len, "sound must be ATRAC9 in exactly the system file's format (%u Hz, %u channels, %u bytes per block)",
             le32(stock_fmt + 4), le16(stock_fmt + 2), le16(stock_fmt + 12));
    free(b);
    return -1;
  }
  if(!!c.fact != stock_fact || !!c.smpl != stock_smpl) {
    snprintf(why, len, "sound must have the same %s chunk as the system file", !!c.fact != stock_fact ? "fact" : "loop (smpl)");
    free(b);
    return -1;
  }

  config_t cfg;
  config_from_fmt(c.fmt, &cfg);
  const uint8_t *cfgb = c.fmt + 44;
  if(cfgb[0] != 0xfe || (cfgb[1] & 1) || cfg.cci > 5 || cfg.frames == 2 || cfg.frames == 8 ||
     le16(c.fmt + 12) != cfg.superframe_bytes) {
    snprintf(why, len, "invalid ATRAC9 configuration");
    free(b);
    return -1;
  }
  if(!c.data_size || c.data_size % cfg.superframe_bytes) {
    snprintf(why, len, "ATRAC9 data is not a whole number of blocks");
    free(b);
    return -1;
  }
  if(c.data_size > stock_data * AT9_GROWTH) {
    snprintf(why, len, "sound is longer than %d times the system sound", AT9_GROWTH);
    free(b);
    return -1;
  }

  uint64_t superframes = c.data_size / cfg.superframe_bytes;
  uint64_t samples = superframes * ((uint64_t)cfg.frames << frame_power[cfg.sri]);
  if(c.fact && (c.fact_size < 4 || !le32(c.fact) || le32(c.fact) > samples ||
                (c.fact_size >= 12 && le32(c.fact) + (uint64_t)le32(c.fact + 8) > samples))) {
    snprintf(why, len, "sample count (fact chunk) does not match the audio data");
    free(b);
    return -1;
  }
  if(c.smpl) {
    if(c.smpl_size < 36 || c.smpl_size < 36 + 24ull * le32(c.smpl + 28)) {
      snprintf(why, len, "damaged loop (smpl) chunk");
      free(b);
      return -1;
    }
    for(uint32_t i = 0; i < le32(c.smpl + 28); i++) {
      const uint8_t *l = c.smpl + 36 + 24 * i;
      if(le32(l + 8) > le32(l + 12) || le32(l + 12) >= samples) {
        snprintf(why, len, "loop points (smpl chunk) are outside the audio");
        free(b);
        return -1;
      }
    }
  }

  uint64_t at = 0;
  uint32_t rate = le32(c.fmt + 4);
  err = scan(b + c.data_offset, superframes, &cfg, &cprof, &at);
  free(b);
  if(err) {
    unsigned t = (unsigned)(at * (1u << frame_power[cfg.sri]) / rate);
    snprintf(why, len, "ATRAC9 data is damaged at %u:%02u (%s)", t / 60, t % 60, err);
    return -1;
  }
  if((err = outside_profile(&cprof, &sprof, cfg.frame_bytes, buf, sizeof(buf)))) {
    snprintf(why, len, "the console cannot play this sound: it %s. Encode it again in the theme builder", err);
    return -1;
  }
  return 0;
}
