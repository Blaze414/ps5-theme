/* RCO resource containers (RCOF version 0x110): ShellUI keeps its interface
   pictures, sounds, strings and colours in them, in the .rco files of
   /system_ex/vsh_asset.

   Layout: a 0x50-byte header with the offset and size of nine tables (the
   element tree, ID strings, ID integers, strings, wide strings, style IDs,
   integer arrays, float arrays, and last the file table). The tree is a run
   of elements, each 28 bytes (name, attribute count, then parent, previous,
   next, first and last child) plus 16 bytes per attribute (name, type, two
   values). An attribute of type 8 points into the file table: (offset, size).

   A theme may replace one only with the console's own file in which pictures
   were swapped. Everything but the file table's contents and the pointers
   into it must be byte-identical; each PNG must match the console's own in
   every respect but its pixels (png.c), since ShellUI crashed on start with
   pictures whose format had changed; each SVG must keep the console's own
   <svg> opening byte for byte and pass the SVG check; every other embedded
   file (sounds, strings, colours) must be unchanged. */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include "theme.h"


#define RCO_MAGIC        "RCOF"
#define RCO_VERSION      0x110
#define RCO_HEADER       0x50
#define RCO_MAX_BYTES    (96ull << 20)
#define RCO_PICTURE_MAX  (8u << 20)
#define ATTR_FILE        8

enum { T_TREE, T_IDSTR, T_IDINT, T_STR, T_CHAR, T_STYLE, T_INTARR, T_FLOATARR, T_FILE, T_COUNT };

typedef struct rco {
  uint8_t *b;
  size_t   n;
  uint32_t off[T_COUNT], size[T_COUNT];
} rco_t;


static uint32_t
le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}


static int
rco_load(const char *path, rco_t *r) {
  struct stat st;
  memset(r, 0, sizeof(*r));
  int fd = open(path, O_RDONLY);
  if(fd < 0) return -1;
  if(fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < RCO_HEADER || (uint64_t)st.st_size > RCO_MAX_BYTES) {
    close(fd);
    return -1;
  }
  r->b = malloc((size_t)st.st_size);
  while(r->b && r->n < (size_t)st.st_size) {
    ssize_t got = read(fd, r->b + r->n, (size_t)st.st_size - r->n);
    if(got <= 0) break;
    r->n += (size_t)got;
  }
  close(fd);
  if(!r->b || r->n != (size_t)st.st_size || memcmp(r->b, RCO_MAGIC, 4) || le32(r->b + 4) != RCO_VERSION) return -1;
  for(int i = 0; i < T_COUNT; i++) {
    r->off[i] = le32(r->b + 8 + 8 * i);
    r->size[i] = le32(r->b + 12 + 8 * i);
    if(r->off[i] < RCO_HEADER || (uint64_t)r->off[i] + r->size[i] > r->n) return -1;
  }
  /* The file table comes last; the tree comes first. */
  if((uint64_t)r->off[T_FILE] + r->size[T_FILE] != r->n || r->off[T_TREE] != RCO_HEADER) return -1;
  return 0;
}


/* A NUL-terminated string at `off` in table `t`, or "" if there is none. */
static const char *
table_str(const rco_t *r, int t, uint32_t off) {
  if(off >= r->size[t]) return "";
  const char *s = (const char *)r->b + r->off[t] + off;
  return memchr(s, 0, r->size[t] - off) ? s : "";
}


/* An element's id: ID strings are stored after a 4-byte element offset. */
static const char *
element_id(const rco_t *r, const uint8_t *el, uint32_t nattr) {
  for(uint32_t i = 0; i < nattr; i++) {
    const uint8_t *a = el + 28 + 16 * i;
    uint32_t type = le32(a + 4);
    if((type == 9 || type == 10) && !strcmp(table_str(r, T_STR, le32(a)), "id")) {
      uint32_t off = le32(a + 8);
      return off <= UINT32_MAX - 4 ? table_str(r, T_IDSTR, off + 4) : "";
    }
  }
  return "";
}


static const char *
element_type(const rco_t *r, const uint8_t *el, uint32_t nattr) {
  for(uint32_t i = 0; i < nattr; i++) {
    const uint8_t *a = el + 28 + 16 * i;
    if(le32(a + 4) == 3 && !strcmp(table_str(r, T_STR, le32(a)), "type")) return table_str(r, T_STR, le32(a + 8));
  }
  return "";
}


/* "8-bit palette" and so on, from a PNG's IHDR (already checked to exist). */
static void
png_format(const uint8_t *png, char *out, size_t size) {
  static const char *const types[] = { "grey", "?", "RGB", "palette", "grey and alpha", "?", "RGBA" };
  unsigned depth = png[24], type = png[25];
  snprintf(out, size, "%u-bit %s%s", depth, type < 7 ? types[type] : "?", png[28] ? ", interlaced" : "");
}


/* The length of an SVG's beginning up to the end of its <svg ...> start tag
   (the XML declaration, comments, and the root element's attributes: its
   size and view box), or 0. */
static size_t
svg_root_end(const uint8_t *d, size_t n) {
  for(size_t i = 0; i + 4 < n; i++) {
    if(memcmp(d + i, "<svg", 4) || (d[i + 4] != ' ' && d[i + 4] != '>' && d[i + 4] != '\n' && d[i + 4] != '\r' && d[i + 4] != '\t')) continue;
    for(size_t k = i + 4; k < n; k++) if(d[k] == '>') return d[k - 1] == '/' ? 0 : k + 1;
    return 0;
  }
  return 0;
}


/* One embedded file of `c` against the same one in `s`. 1 if it is a
   changed picture, 0 if unchanged, -1 (with `why`) if not allowed. */
static int
check_embedded(const rco_t *s, const rco_t *c, const char *ename, const char *etype, const char *id,
               uint32_t soff, uint32_t ssize, uint32_t coff, uint32_t csize, char *why, size_t len) {
  if((uint64_t)soff + ssize > s->size[T_FILE]) {
    snprintf(why, len, "cannot read the system file to compare against");
    return -1;
  }
  if(coff % 16 || (uint64_t)coff + csize > c->size[T_FILE]) {
    snprintf(why, len, "damaged resource file: %s points outside its data", id);
    return -1;
  }
  const uint8_t *sd = s->b + s->off[T_FILE] + soff, *cd = c->b + c->off[T_FILE] + coff;
  if(ssize == csize && !memcmp(sd, cd, ssize)) return 0;

  /* What a picture is comes from its bytes, not its declared type: some
     "texture/svg" entries hold PNG data (13.42's app tile placeholders). */
  static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  int is_texture = !strcmp(ename, "texture") && (!strcmp(etype, "texture/png") || !strcmp(etype, "texture/svg"));
  int stock_png = ssize >= 8 && !memcmp(sd, png_sig, 8);
  if(is_texture && stock_png) {
    char detail[160];
    if(csize > RCO_PICTURE_MAX || png_check_against(sd, ssize, cd, csize, detail, sizeof(detail))) {
      if(csize > RCO_PICTURE_MAX) snprintf(detail, sizeof(detail), "too large");
      image_info_t want;
      char format[48] = "";
      if(!image_probe_mem(sd, ssize, &want) && ssize >= 29) {
        png_format(sd, format, sizeof(format));
        snprintf(why, len, "picture %s (a %ux%u PNG, %s) is %s", id, want.width, want.height, format, detail);
      } else {
        snprintf(why, len, "picture %s is %s", id, detail);
      }
      return -1;
    }
    return 1;
  }
  size_t root = is_texture && !stock_png ? svg_root_end(sd, ssize) : 0;
  if(root) {
    char svg_why[160];
    if(csize > RCO_PICTURE_MAX || svg_check_mem(cd, csize, svg_why, sizeof(svg_why))) {
      snprintf(why, len, "picture %s: %s", id, csize > RCO_PICTURE_MAX ? "too large" : svg_why);
      return -1;
    }
    if(csize < root || memcmp(cd, sd, root)) {
      snprintf(why, len, "picture %s must keep the console's own <svg> opening, byte for byte", id);
      return -1;
    }
    return 1;
  }
  snprintf(why, len, "only pictures can be changed in a resource file (%s %s differs)", ename, id);
  return -1;
}


int
rco_validate(const char *cand, const char *stock, char *why, size_t len) {
  rco_t s, c;
  int ret = -1;

  if(rco_load(stock, &s)) {
    snprintf(why, len, "cannot read the system file to compare against");
    goto done_s;
  }
  if(rco_load(cand, &c)) {
    snprintf(why, len, "not a resource file (.rco) of this kind");
    goto done;
  }

  /* The same header, apart from the file table's size. */
  uint8_t head[RCO_HEADER];
  memcpy(head, c.b, RCO_HEADER);
  memcpy(head + 12 + 8 * T_FILE, s.b + 12 + 8 * T_FILE, 4);
  const char *mismatch = "must be this console's own resource file with only pictures changed";
  if(memcmp(head, s.b, RCO_HEADER)) {
    snprintf(why, len, "%s", mismatch);
    goto done;
  }

  /* Every byte before the file table must match, apart from the pointers
     into it; each embedded file is then checked. Elements are stored one
     after another, as the tree's size accounts for. */
  const uint8_t *tree = s.b + s.off[T_TREE];
  size_t tsize = s.size[T_TREE], pos = RCO_HEADER;
  for(size_t at = 0; at < tsize;) {
    if(tsize - at < 28) {
      snprintf(why, len, "cannot read the system file to compare against");
      goto done;
    }
    const uint8_t *el = tree + at;
    uint32_t nattr = le32(el + 4);
    if(nattr > (tsize - at - 28) / 16) {
      snprintf(why, len, "cannot read the system file to compare against");
      goto done;
    }
    const char *ename = table_str(&s, T_STR, le32(el)), *etype = element_type(&s, el, nattr);
    const char *id = element_id(&s, el, nattr);
    for(uint32_t i = 0; i < nattr; i++) {
      const uint8_t *a = el + 28 + 16 * i;
      if(le32(a + 4) != ATTR_FILE) continue;
      size_t slot = s.off[T_TREE] + at + 28 + 16 * i + 8;
      if(memcmp(c.b + pos, s.b + pos, slot - pos)) {
        snprintf(why, len, "%s", mismatch);
        goto done;
      }
      pos = slot + 8;
      if(check_embedded(&s, &c, ename, etype, id[0] ? id : ename, le32(a + 8), le32(a + 12),
                        le32(c.b + slot), le32(c.b + slot + 4), why, len) < 0) {
        goto done;
      }
    }
    at += 28 + 16 * (size_t)nattr;
  }
  if(memcmp(c.b + pos, s.b + pos, s.off[T_FILE] - pos)) {
    snprintf(why, len, "%s", mismatch);
    goto done;
  }
  ret = 0;

done:
  free(c.b);
done_s:
  free(s.b);
  return ret;
}
