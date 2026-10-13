/* font: the check a replacement font must pass before it is accepted.

   The console keeps its text fonts as OpenType/TrueType files in
   /preinst/common/font (SST for Latin text, others for Arabic, Thai,
   Japanese, Korean and Chinese). A theme may replace one with a font of its
   own; the home screen reads them as it starts. A file that is not really a
   font, is cut short or has a broken table would reach the system's font
   loader unchecked, so this reads the file's structure first, in memory, and
   refuses anything the loader could trip over:

   - the file is a plain TrueType or OpenType (CFF) font. Collections (.ttc)
     and web fonts (WOFF, WOFF2) are not accepted;
   - its table directory fits inside the file, every table lies inside it,
     and none is listed twice;
   - it has the tables every font needs (cmap, head, hhea, hmtx, maxp, name,
     OS/2, post) and its outlines (glyf and loca, or CFF);
   - head, maxp, hhea, hmtx and loca agree with each other about the glyph
     count and sizes;
   - its character map covers the printable ASCII characters, so text on the
     console doesn't come out with gaps.

   Nothing here touches the console: it works on bytes it is given and never
   reads outside them, whatever they hold (it is fuzzed). */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "theme.h"

#define FONT_MAX_TABLES 64
#define FONT_MAX_SEGMENTS 32768     /* a format 4 map is at most 65535 bytes of segments */
#define FONT_MAX_GROUPS 100000      /* a format 12 map of the whole of Unicode has far fewer */
#define FONT_MAX_MAPS 8             /* character maps looked at, so a hostile file can't make this slow */
#define FONT_LOW  0x20              /* the characters text needs: space to ~ */
#define FONT_HIGH 0x7e

typedef struct {
  const uint8_t *p;
  size_t n;
} bytes_t;

typedef struct {
  char tag[5];
  uint32_t off, len;
} table_t;

static uint32_t
be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

static uint32_t
be32(const uint8_t *p) { return be16(p) << 16 | be16(p + 2); }

/* Fail with a plain message. */
static int
no(char *why, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static int
no(char *why, size_t len, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(why, len, fmt, ap);
  va_end(ap);
  return -1;
}

static const table_t *
find(const table_t *t, int n, const char *tag) {
  for(int i = 0; i < n; i++) if(!memcmp(t[i].tag, tag, 4)) return &t[i];
  return NULL;
}

/* The glyph a character map gives for `cp` in the table at [c, c+clen), or 0.
   Reads only inside the table. */
static uint32_t
cmap_glyph(const uint8_t *c, size_t clen, uint32_t sub, uint32_t cp) {
  if(sub + 4 > clen) return 0;
  const uint8_t *s = c + sub;
  size_t room = clen - sub;
  uint32_t fmt = be16(s);
  if(fmt == 4) {
    if(room < 16) return 0;
    uint32_t segx2 = be16(s + 6), seg = segx2 / 2;
    /* endCode[seg], pad, startCode[seg], idDelta[seg], idRangeOffset[seg] */
    if(!seg || seg > FONT_MAX_SEGMENTS || 16 + (size_t)seg * 8 > room) return 0;
    const uint8_t *end = s + 14, *start = end + segx2 + 2, *delta = start + segx2, *range = delta + segx2;
    for(uint32_t i = 0; i < seg; i++) {
      if(cp > be16(end + 2 * i)) continue;
      uint32_t first = be16(start + 2 * i);
      if(cp < first) return 0;
      uint32_t d = be16(delta + 2 * i), r = be16(range + 2 * i);
      if(!r) return (cp + d) & 0xffff;
      size_t at = (size_t)(range + 2 * i - s) + r + 2 * (cp - first);   /* from this entry on */
      if(at + 2 > room) return 0;
      uint32_t g = be16(s + at);
      return g ? (g + d) & 0xffff : 0;
    }
    return 0;
  }
  if(fmt == 12) {
    if(room < 16) return 0;
    uint32_t groups = be32(s + 12);
    if(groups > FONT_MAX_GROUPS || groups > (room - 16) / 12) return 0;
    for(uint32_t i = 0; i < groups; i++) {
      const uint8_t *g = s + 16 + (size_t)i * 12;
      uint32_t lo = be32(g), hi = be32(g + 4);
      if(cp >= lo && cp <= hi) {
        uint64_t glyph = (uint64_t)be32(g + 8) + (cp - lo);
        return glyph > 0xffffffffu ? 0 : (uint32_t)glyph;
      }
    }
    return 0;
  }
  return 0;
}

/* Does the font's character map cover every printable ASCII character? Any
   one Unicode map that does is enough. When none does, *missing is the first
   character the first Unicode map lacks, or 0 if the font has no Unicode map
   this can read. */
static int
cmap_covers_ascii(const uint8_t *c, size_t clen, uint32_t nglyphs, int *missing) {
  *missing = 0;
  if(clen < 4) return 0;
  uint32_t n = be16(c + 2);
  if(n > 64 || 4 + (size_t)n * 8 > clen) return 0;
  int looked = 0;
  for(uint32_t i = 0; i < n && looked < FONT_MAX_MAPS; i++) {
    uint32_t plat = be16(c + 4 + 8 * i), enc = be16(c + 6 + 8 * i), sub = be32(c + 8 + 8 * i);
    if(!(plat == 0 || (plat == 3 && (enc == 1 || enc == 10)))) continue;   /* Unicode maps only */
    if((size_t)sub + 2 > clen || (be16(c + sub) != 4 && be16(c + sub) != 12)) continue;   /* formats this reads */
    looked++;
    int first_missing = 0;
    for(uint32_t cp = FONT_LOW; cp <= FONT_HIGH; cp++) {
      uint32_t g = cmap_glyph(c, clen, sub, cp);
      if(!g || g >= nglyphs) { first_missing = (int)cp; break; }
    }
    if(!first_missing) return 1;
    if(!*missing) *missing = first_missing;
  }
  return 0;
}

/* 0 if the bytes are a font the console can load; otherwise -1 with a plain
   reason in `why`. */
int
font_check_mem(const uint8_t *buf, size_t n, char *why, size_t len) {
  if(n < 12) return no(why, len, "this file is too short to be a font");
  uint32_t ver = be32(buf);
  int cff;
  if(ver == 0x00010000 || ver == 0x74727565) cff = 0;          /* TrueType */
  else if(ver == 0x4f54544f) cff = 1;                          /* "OTTO": OpenType with CFF outlines */
  else if(ver == 0x74746366) return no(why, len, "font collections (.ttc) are not supported: use a single TTF or OTF font");
  else if(ver == 0x774f4646 || ver == 0x774f4632) return no(why, len, "web fonts (WOFF) are not supported: use a TTF or OTF font");
  else return no(why, len, "this is not a TrueType or OpenType font file");

  uint32_t ntab = be16(buf + 4);
  if(!ntab || ntab > FONT_MAX_TABLES || 12 + (size_t)ntab * 16 > n) {
    return no(why, len, "the font's table list is damaged or incomplete");
  }
  table_t t[FONT_MAX_TABLES];
  for(uint32_t i = 0; i < ntab; i++) {
    const uint8_t *r = buf + 12 + 16 * i;
    memcpy(t[i].tag, r, 4);
    t[i].tag[4] = 0;
    t[i].off = be32(r + 8);
    t[i].len = be32(r + 12);
    if((uint64_t)t[i].off + t[i].len > n || t[i].off < 12 + (size_t)ntab * 16) {
      return no(why, len, "the font file is cut short or damaged (its '%.4s' table lies outside the file)", t[i].tag);
    }
    for(uint32_t k = 0; k < i; k++) {
      if(!memcmp(t[k].tag, t[i].tag, 4)) return no(why, len, "the font lists its '%.4s' table twice", t[i].tag);
    }
  }

  static const char *const need[] = {"cmap", "head", "hhea", "hmtx", "maxp", "name", "OS/2", "post"};
  for(size_t i = 0; i < sizeof(need) / sizeof(*need); i++) {
    if(!find(t, (int)ntab, need[i])) return no(why, len, "the font has no '%s' table, which every font needs", need[i]);
  }
  const table_t *head = find(t, (int)ntab, "head"), *hhea = find(t, (int)ntab, "hhea");
  const table_t *hmtx = find(t, (int)ntab, "hmtx"), *maxp = find(t, (int)ntab, "maxp");
  const table_t *cmap = find(t, (int)ntab, "cmap");

  if(head->len < 54 || be32(buf + head->off + 12) != 0x5f0f3cf5) return no(why, len, "the font's header table is damaged");
  uint32_t em = be16(buf + head->off + 18), locfmt = be16(buf + head->off + 50);
  if(em < 16 || em > 16384) return no(why, len, "the font's size grid (units per em) is %u: it must be 16 to 16384", em);

  if(maxp->len < 6) return no(why, len, "the font's glyph count table is damaged");
  uint32_t mver = be32(buf + maxp->off), nglyphs = be16(buf + maxp->off + 4);
  if(mver != 0x00005000 && mver != 0x00010000) return no(why, len, "the font's glyph count table has an unknown version");
  if(mver == 0x00010000 && maxp->len < 32) return no(why, len, "the font's glyph count table is cut short");
  if(!nglyphs) return no(why, len, "the font has no glyphs");

  if(hhea->len < 36) return no(why, len, "the font's horizontal header is damaged");
  uint32_t nhm = be16(buf + hhea->off + 34);
  if(!nhm || nhm > nglyphs) return no(why, len, "the font's horizontal metrics don't match its glyph count");
  if((uint64_t)hmtx->len < (uint64_t)nhm * 4 + (uint64_t)(nglyphs - nhm) * 2) {
    return no(why, len, "the font's horizontal metrics table is cut short");
  }

  if(cff) {
    const table_t *c = find(t, (int)ntab, "CFF ");
    if(!c) return no(why, len, "an OpenType font with CFF outlines has no 'CFF ' table");
    if(c->len < 4 || buf[c->off] != 1) return no(why, len, "the font's CFF outline table is damaged");
  } else {
    const table_t *glyf = find(t, (int)ntab, "glyf"), *loca = find(t, (int)ntab, "loca");
    if(!glyf || !loca) return no(why, len, "a TrueType font needs 'glyf' and 'loca' tables for its outlines");
    if(locfmt > 1) return no(why, len, "the font's glyph index format is unknown");
    if((uint64_t)loca->len < (uint64_t)(nglyphs + 1) * (locfmt ? 4 : 2)) {
      return no(why, len, "the font's glyph index table is cut short");
    }
  }

  int missing;
  if(!cmap_covers_ascii(buf + cmap->off, cmap->len, nglyphs, &missing)) {
    if(missing == ' ') return no(why, len, "the font has no space character: text in it would run together");
    if(missing) {
      return no(why, len, "the font has no '%c': text in it would show gaps. Use a font with the full Latin alphabet, digits and punctuation", missing);
    }
    return no(why, len, "the font has no Unicode character map this tool can read (format 4 or 12)");
  }
  return 0;
}
