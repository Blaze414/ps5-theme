/* Asset policy: what a theme may replace, and how a candidate file is checked
   against the stock file it replaces.

   Only media is replaceable. Code and anything unrecognised (JavaScript
   bundles, executables, libraries, unknown extensions) never is. Each kind is
   validated against the stock file so ShellUI receives exactly the kind of
   data it already handles:
     image  PNG/JPEG/WebP: same format and exact pixel size
     svg    well-formed-looking SVG without scripts or external references
     audio  WAV: same codec, channels, sample rate and bit depth
            AT9: the same ATRAC9 format, and every block unpacks cleanly
            (atrac9.c)
     font   same font container (sfnt/OTF/collection/WOFF/WOFF2)
     json   valid JSON, same top-level type; animations keep their w/h
     video  same container (MP4/WebM)
     texture DDS / zstd-compressed DDS: byte-identical header, same length
     rco    the console's own resource file with only pictures changed
            (rco.c) */

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <unistd.h>

#include <sys/stat.h>

#include "theme.h"


static const struct { const char *ext; int kind; } kinds[] = {
  {".png", KIND_IMAGE}, {".jpg", KIND_IMAGE}, {".jpeg", KIND_IMAGE}, {".webp", KIND_IMAGE},
  {".svg", KIND_SVG},
  {".wav", KIND_AUDIO}, {".at9", KIND_AUDIO},
  {".ttf", KIND_FONT}, {".otf", KIND_FONT}, {".ttc", KIND_FONT}, {".woff", KIND_FONT}, {".woff2", KIND_FONT},
  {".json", KIND_JSON},
  {".mp4", KIND_VIDEO}, {".m4v", KIND_VIDEO}, {".webm", KIND_VIDEO},
  {".dds.zst", KIND_TEXTURE}, {".dds", KIND_TEXTURE},
  {".rco", KIND_RCO},
};

static const uint64_t kind_limit[] = {
  [KIND_BLOCKED] = 0,
  [KIND_IMAGE]   = 32ull << 20,
  [KIND_SVG]     = 2ull << 20,
  [KIND_AUDIO]   = 64ull << 20,
  [KIND_FONT]    = 16ull << 20,
  [KIND_JSON]    = 4ull << 20,
  [KIND_VIDEO]   = 128ull << 20,
  [KIND_TEXTURE] = 128ull << 20,
  [KIND_RCO]     = 96ull << 20,
};


/* `app` points at "/user/app/" in a path. If the path goes on into a
   title's sce_sys folder (not the plugin's own tile, which it writes
   itself), what follows "sce_sys"; otherwise NULL. */
static const char *
app_sce_sys(const char *app) {
  const char *id = app + sizeof(APP_DIR), *s = strchr(id, '/');
  if(!s || s == id || ((size_t)(s - id) == strlen(TITLE_ID) && !strncmp(id, TITLE_ID, s - id))) return NULL;
  if(strncmp(s, "/sce_sys", 8) || (s[8] && s[8] != '/')) return NULL;
  return s + 8;
}


/* 1 if `path` (a file, or a folder with a trailing slash) is outside
   APP_DIR or inside a title's sce_sys folder there: everything else under
   APP_DIR is the game itself. */
int
app_path_ok(const char *path) {
  if(strncmp(path, APP_DIR "/", sizeof(APP_DIR))) return 1;
  const char *rest = app_sce_sys(path);
  return rest && *rest == '/';
}


/* `cat` points at CATALOG_DIR "/" in a path. What follows the title's or
   concept's folder in it (appmeta/<title> or conceptmeta/<concept>), or
   NULL if it isn't in one. */
static const char *
catalog_rest(const char *cat) {
  const char *sub = cat + sizeof(CATALOG_DIR);
  if(!strncmp(sub, "appmeta/", 8)) sub += 8;
  else if(!strncmp(sub, "conceptmeta/", 12)) sub += 12;
  else return NULL;
  const char *s = strchr(sub, '/');
  if(s == sub || !*sub) return NULL;
  return s ? s : sub + strlen(sub);
}


/* 1 if `dir` is an installed title's metadata folder: /user/appmeta/<title>,
   /user/app/<title>/sce_sys, or the pictures downloaded for it,
   CATALOG_DIR/appmeta/<title> and CATALOG_DIR/conceptmeta/<concept>. */
int
title_folder(const char *dir) {
  if(!strncmp(dir, APPMETA_DIR "/", sizeof(APPMETA_DIR))) {
    return dir[sizeof(APPMETA_DIR)] && !strchr(dir + sizeof(APPMETA_DIR), '/');
  }
  if(!strncmp(dir, CATALOG_DIR "/", sizeof(CATALOG_DIR))) {
    const char *rest = catalog_rest(dir);
    return rest && !*rest;
  }
  if(strncmp(dir, APP_DIR "/", sizeof(APP_DIR))) return 0;
  const char *rest = app_sce_sys(dir);
  return rest && !*rest;
}


int
asset_kind(const char *path) {
  const char *slash = strrchr(path, '/');
  const char *name = slash ? slash + 1 : path;
  size_t n = strlen(name);
  /* In a title's metadata only pictures: its param.json and the rest are
     what the system launches it with. (The path may be the folder's stock
     copy or a theme's file, so it is matched anywhere in the path.) Under
     /user/app only the pictures directly in a title's sce_sys folder, and
     under CATALOG_DIR only those directly in a title's or concept's. */
  int appmeta = strstr(path, APPMETA_DIR "/") != NULL;
  const char *app = strstr(path, APP_DIR "/"), *cat = strstr(path, CATALOG_DIR "/");
  if(app) {
    const char *rest = app_sce_sys(app);
    if(!rest || *rest != '/' || strchr(rest + 1, '/')) return KIND_BLOCKED;
    appmeta = 1;
  }
  if(cat) {
    const char *rest = catalog_rest(cat);
    if(!rest || *rest != '/' || strchr(rest + 1, '/')) return KIND_BLOCKED;
    appmeta = 1;
  }

  for(size_t i = 0; i < sizeof(kinds) / sizeof(*kinds); i++) {
    size_t e = strlen(kinds[i].ext);
    if(n > e && !strcasecmp(name + n - e, kinds[i].ext)) {
      if(appmeta && kinds[i].kind != KIND_IMAGE && kinds[i].kind != KIND_TEXTURE) return KIND_BLOCKED;
      return kinds[i].kind;
    }
  }
  return KIND_BLOCKED;
}


const char *
asset_kind_name(int kind) {
  switch(kind) {
  case KIND_IMAGE: return "image";
  case KIND_SVG:   return "svg";
  case KIND_AUDIO: return "audio";
  case KIND_FONT:  return "font";
  case KIND_JSON:  return "json";
  case KIND_VIDEO: return "video";
  case KIND_TEXTURE: return "texture";
  case KIND_RCO:   return "rco";
  default:         return "protected";
  }
}


uint64_t
asset_size_limit(int kind) {
  return kind > KIND_BLOCKED && kind <= KIND_RCO ? kind_limit[kind] : 0;
}


/* Read a whole (bounded) file. Caller frees. */
static uint8_t *
slurp(const char *path, size_t max, size_t *len) {
  struct stat st;
  int fd = open(path, O_RDONLY);
  if(fd < 0) return NULL;
  if(fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size > max) {
    close(fd);
    return NULL;
  }
  uint8_t *buf = malloc((size_t)st.st_size + 1);
  size_t got = 0;
  while(buf && got < (size_t)st.st_size) {
    ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);
    if(r <= 0) break;
    got += r;
  }
  close(fd);
  if(!buf || got != (size_t)st.st_size) {
    free(buf);
    return NULL;
  }
  buf[got] = 0;
  *len = got;
  return buf;
}


static size_t
read_head(const char *path, uint8_t *buf, size_t size) {
  int fd = open(path, O_RDONLY);
  size_t n = 0;
  if(fd < 0) return 0;
  while(n < size) {
    ssize_t r = read(fd, buf + n, size - n);
    if(r <= 0) break;
    n += r;
  }
  close(fd);
  return n;
}


/* ---------------- audio (RIFF/WAVE, incl. ATRAC9 in WAVE_FORMAT_EXTENSIBLE) ---------------- */

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }


int
wav_probe(const char *path, wav_info_t *out) {
  uint8_t head[4096];
  struct stat st;
  size_t n = read_head(path, head, sizeof(head));

  memset(out, 0, sizeof(*out));
  if(stat(path, &st) || n < 12 || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4)) return -1;

  int have_fmt = 0;
  size_t off = 12;
  while(off + 8 <= n) {
    uint32_t size = le32(head + off + 4);
    if(!memcmp(head + off, "fmt ", 4)) {
      if(size < 16 || off + 8 + 16 > n) return -1;
      const uint8_t *f = head + off + 8;
      out->format = le16(f);
      out->channels = le16(f + 2);
      out->rate = le32(f + 4);
      out->bits = le16(f + 14);
      if(out->format == 0xfffe && size >= 40 && off + 8 + 40 <= n) {
        memcpy(out->subformat, f + 24, 16);
      }
      have_fmt = 1;
    } else if(!memcmp(head + off, "data", 4)) {
      out->data_offset = off + 8;
      out->data_size = size;
      break;
    }
    off += 8 + (size_t)size + (size & 1);   /* in 32 bits, a size near 4 GB wraps to 0 */
  }
  if(!have_fmt || !out->data_offset || !out->channels || !out->rate) return -1;
  if(out->data_offset + out->data_size > (uint64_t)st.st_size) return -1;
  return 0;
}


static const uint8_t atrac9_guid[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
                                         0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};

static int
check_audio(const char *cand, const char *stock, char *why, size_t len) {
  wav_info_t a, b;
  if(wav_probe(stock, &b)) { snprintf(why, len, "cannot read the system sound to compare against"); return -1; }
  if(b.format == 0xfffe && !memcmp(b.subformat, atrac9_guid, 16)) return at9_validate(cand, stock, why, len);
  if(wav_probe(cand, &a)) { snprintf(why, len, "not a valid WAV file"); return -1; }
  if(a.format != b.format || memcmp(a.subformat, b.subformat, 16)) {
    snprintf(why, len, "sound must use the same codec as the system file (format 0x%x)", b.format);
    return -1;
  }
  if(a.channels != b.channels || a.rate != b.rate || a.bits != b.bits) {
    snprintf(why, len, "sound must be %u Hz, %u channel(s), %u-bit like the system file (got %u Hz, %u, %u-bit)",
             b.rate, b.channels, b.bits, a.rate, a.channels, a.bits);
    return -1;
  }
  return 0;
}


/* A theme's music source (music.wav): 48 kHz 16-bit stereo PCM. */
int
music_wav_ok(const char *path, wav_info_t *info, char *why, size_t len) {
  if(wav_probe(path, info)) {
    snprintf(why, len, "not a valid WAV file");
    return 0;
  }
  if(info->format != 1 || info->channels != 2 || info->rate != MUSIC_RATE || info->bits != 16) {
    snprintf(why, len, "music must be 48 kHz 16-bit stereo PCM (got %u Hz, %u ch, %u-bit, format %u)",
             info->rate, info->channels, info->bits, info->format);
    return 0;
  }
  if(info->data_size < MUSIC_RATE * 4 / 10) {
    snprintf(why, len, "music is too short");
    return 0;
  }
  return 1;
}


/* ---------------- fonts and video: container magic ---------------- */

static int
font_container(const uint8_t *p, size_t n) {
  if(n < 4) return 0;
  if(!memcmp(p, "\x00\x01\x00\x00", 4) || !memcmp(p, "true", 4)) return 1;  /* TrueType sfnt */
  if(!memcmp(p, "OTTO", 4)) return 2;
  if(!memcmp(p, "ttcf", 4)) return 3;
  if(!memcmp(p, "wOFF", 4)) return 4;
  if(!memcmp(p, "wOF2", 4)) return 5;
  return 0;
}


static int
video_container(const uint8_t *p, size_t n) {
  if(n >= 12 && !memcmp(p + 4, "ftyp", 4)) return 1;
  if(n >= 4 && !memcmp(p, "\x1a\x45\xdf\xa3", 4)) return 2;
  return 0;
}


/* A font: the system's own is TrueType or OpenType, and a replacement may be
   either (the console reads both), but must be a whole one with the
   characters text needs (font_check_mem). Other stock formats keep the
   rule that the container stays the same. */
static int check_magic(const char *cand, const char *stock, int (*container)(const uint8_t *, size_t),
                       const char *what, char *why, size_t len);

static int
check_font(const char *cand, const char *stock, char *why, size_t len) {
  uint8_t head[16];
  int kind = font_container(head, read_head(stock, head, sizeof(head)));
  if(kind != 1 && kind != 2) return check_magic(cand, stock, font_container, "font", why, len);
  size_t n;
  uint8_t *data = slurp(cand, asset_size_limit(KIND_FONT), &n);
  if(!data) { snprintf(why, len, "cannot read the font file"); return -1; }
  int rc = font_check_mem(data, n, why, len);
  free(data);
  return rc;
}


static int
check_magic(const char *cand, const char *stock, int (*container)(const uint8_t *, size_t),
            const char *what, char *why, size_t len) {
  uint8_t a[16], b[16];
  size_t na = read_head(cand, a, sizeof(a)), nb = read_head(stock, b, sizeof(b));
  int cb = container(b, nb), ca = container(a, na);
  if(!cb) { snprintf(why, len, "the system %s is in a format this tool does not handle", what); return -1; }
  if(ca != cb) { snprintf(why, len, "%s must use the same format as the system file", what); return -1; }
  return 0;
}


/* ---------------- SVG ---------------- */

/* SVG text in memory (a file, or a picture inside a resource file). */
int
svg_check_mem(const uint8_t *data, size_t n, char *why, size_t len) {
  static const char *const banned[] = {
    "<script", "javascript:", "<foreignobject", "href=\"http", "href='http", "href=\"//",
    "xlink:href=\"http", "@import", "url(http", "<iframe", "<!entity",
  };
  char *buf = malloc(n + 1);
  if(!buf) { snprintf(why, len, "cannot read the SVG"); return -1; }
  for(size_t i = 0; i < n; i++) {
    if(!data[i]) { free(buf); snprintf(why, len, "SVG contains binary data"); return -1; }
    buf[i] = (char)((data[i] >= 'A' && data[i] <= 'Z') ? data[i] + 32 : data[i]);
  }
  buf[n] = 0;
  int ok = strstr(buf, "<svg") != NULL;
  for(size_t i = 0; ok && i < sizeof(banned) / sizeof(*banned); i++) {
    if(strstr(buf, banned[i])) {
      snprintf(why, len, "SVG contains \"%s\", which is not allowed", banned[i]);
      free(buf);
      return -1;
    }
  }
  free(buf);
  if(!ok) { snprintf(why, len, "not an SVG file"); return -1; }
  return 0;
}


static int
check_svg(const char *cand, char *why, size_t len) {
  size_t n;
  uint8_t *buf = slurp(cand, kind_limit[KIND_SVG], &n);
  if(!buf) { snprintf(why, len, "cannot read the SVG"); return -1; }
  int rc = svg_check_mem(buf, n, why, len);
  free(buf);
  return rc;
}


/* ---------------- JSON (validating parser, depth-limited) ---------------- */

typedef struct jp {
  const char *p, *end;
  int depth;
  double w, h;       /* top-level numeric "w" and "h" (Lottie canvas), NaN-free flags below */
  int has_w, has_h;
} jp_t;

static void ws(jp_t *j) { while(j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++; }
static int value(jp_t *j, const char *key, int top);

static int
string(jp_t *j, char *copy, size_t size) {
  size_t o = 0;
  if(j->p >= j->end || *j->p != '"') return -1;
  j->p++;
  while(j->p < j->end && *j->p != '"') {
    unsigned char c = (unsigned char)*j->p;
    if(c < 0x20) return -1;
    if(c == '\\') {
      j->p++;
      if(j->p >= j->end) return -1;
      if(*j->p == 'u') {
        for(int i = 1; i <= 4; i++) {
          if(j->p + i >= j->end) return -1;
          char h = j->p[i];
          if(!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) return -1;
        }
        j->p += 4;
      } else if(!strchr("\"\\/bfnrt", *j->p)) {
        return -1;
      }
    } else if(copy && o + 1 < size) {
      copy[o++] = (char)c;
    }
    j->p++;
  }
  if(j->p >= j->end) return -1;
  j->p++;
  if(copy) copy[o] = 0;
  return 0;
}

static int
number(jp_t *j, double *out) {
  const char *s = j->p;
  if(j->p < j->end && *j->p == '-') j->p++;
  if(j->p >= j->end || !(*j->p >= '0' && *j->p <= '9')) return -1;
  while(j->p < j->end && (strchr("0123456789.eE+-", *j->p))) j->p++;
  char tmp[64];
  size_t n = j->p - s;
  if(n >= sizeof(tmp)) n = sizeof(tmp) - 1;
  memcpy(tmp, s, n);
  tmp[n] = 0;
  char *e;
  *out = strtod(tmp, &e);
  return *e ? -1 : 0;
}

static int
literal(jp_t *j, const char *word) {
  size_t n = strlen(word);
  if((size_t)(j->end - j->p) < n || memcmp(j->p, word, n)) return -1;
  j->p += n;
  return 0;
}

static int
value(jp_t *j, const char *key, int top) {
  if(++j->depth > 64) return -1;
  ws(j);
  if(j->p >= j->end) return -1;
  int ret = 0;
  if(*j->p == '{') {
    j->p++;
    ws(j);
    if(j->p < j->end && *j->p == '}') { j->p++; goto done; }
    for(;;) {
      char k[8];
      ws(j);
      if(string(j, k, sizeof(k))) return -1;
      ws(j);
      if(j->p >= j->end || *j->p != ':') return -1;
      j->p++;
      if(value(j, k, top == 1 ? 2 : 0)) return -1;
      ws(j);
      if(j->p < j->end && *j->p == ',') { j->p++; continue; }
      if(j->p < j->end && *j->p == '}') { j->p++; break; }
      return -1;
    }
  } else if(*j->p == '[') {
    j->p++;
    ws(j);
    if(j->p < j->end && *j->p == ']') { j->p++; goto done; }
    for(;;) {
      if(value(j, NULL, 0)) return -1;
      ws(j);
      if(j->p < j->end && *j->p == ',') { j->p++; continue; }
      if(j->p < j->end && *j->p == ']') { j->p++; break; }
      return -1;
    }
  } else if(*j->p == '"') {
    ret = string(j, NULL, 0);
  } else if(*j->p == 't') {
    ret = literal(j, "true");
  } else if(*j->p == 'f') {
    ret = literal(j, "false");
  } else if(*j->p == 'n') {
    ret = literal(j, "null");
  } else {
    double d;
    ret = number(j, &d);
    if(!ret && top == 2 && key) {
      if(!strcmp(key, "w")) { j->w = d; j->has_w = 1; }
      if(!strcmp(key, "h")) { j->h = d; j->has_h = 1; }
    }
  }
done:
  j->depth--;
  return ret;
}

/* Returns the top-level type ('{' or '['), or 0 if invalid. */
static int
json_parse(const char *path, jp_t *j) {
  size_t n;
  uint8_t *buf = slurp(path, kind_limit[KIND_JSON], &n);
  memset(j, 0, sizeof(*j));
  if(!buf) return 0;
  const char *start = (const char *)buf;
  if(n >= 3 && !memcmp(start, "\xef\xbb\xbf", 3)) start += 3;
  j->p = start;
  j->end = (const char *)buf + n;
  ws(j);
  int type = j->p < j->end ? *j->p : 0;
  int ok = !value(j, NULL, 1);
  ws(j);
  ok = ok && j->p == j->end;
  free(buf);
  return ok && (type == '{' || type == '[') ? type : 0;
}

static int
check_json(const char *cand, const char *stock, char *why, size_t len) {
  jp_t a, b;
  int tb = json_parse(stock, &b);
  if(!tb) { snprintf(why, len, "cannot read the system JSON to compare against"); return -1; }
  int ta = json_parse(cand, &a);
  if(!ta) { snprintf(why, len, "not valid JSON"); return -1; }
  if(ta != tb) { snprintf(why, len, "JSON must have the same structure (top-level %s)", tb == '{' ? "object" : "array"); return -1; }
  if((b.has_w && (!a.has_w || a.w != b.w)) || (b.has_h && (!a.has_h || a.h != b.h))) {
    snprintf(why, len, "animation canvas must stay %gx%g", b.w, b.h);
    return -1;
  }
  return 0;
}


/* ---------------- entry point ---------------- */

static int validate_now(const char *cand, const char *stock, char *why, size_t len);


/* ---- remembered checks ----
   Checking a 4K texture means decompressing it and the system's, so a theme
   with many was slow to apply at every start. A result is remembered for the
   exact pair of files it was made for: device, inode, size and modification
   time of both, and the plugin version (whose rules made it). Theme and stock
   files are only ever replaced, never changed in place, so a new or edited
   file is a different file and is checked again. Kept in CHECKED_FILE so it
   survives restarts; a check made at upload counts at apply too, because a
   rename keeps the file's identity. */

#define CHECKED_FILE BASE_DIR "/checked"

/* What a remembered result was made by: the plugin's version and the revision of the rules its checks apply. A
   result made by other rules is not trusted. Bump CHECK_RULES whenever what the checks accept or refuse
   changes: the same version number can ship twice while a feature is built, and a refusal remembered by the
   first build would otherwise be handed back by the second without its new code ever running. */
#define CHECK_RULES "3"
#define CHECKED_TAG PLUGIN_VERSION "r" CHECK_RULES
#define CHECKED_MAX  8192

typedef struct file_id {
  unsigned long long dev, ino, size, mtime_s, mtime_ns;
} file_id_t;

typedef struct checked {
  file_id_t cand, stock;
  int ok;
  char why[160];
} checked_t;

static pthread_mutex_t checked_lock = PTHREAD_MUTEX_INITIALIZER;
static checked_t *checked;
static int nchecked, checked_loaded, checked_lines;

static int
file_id(const char *path, file_id_t *id) {
  struct stat st;
  if(stat(path, &st) || !S_ISREG(st.st_mode)) return -1;
  id->dev = st.st_dev;
  id->ino = st.st_ino;
  id->size = st.st_size;
  id->mtime_s = st.st_mtim.tv_sec;
  id->mtime_ns = st.st_mtim.tv_nsec;
  return 0;
}

static int
same_id(const file_id_t *a, const file_id_t *b) {
  return a->dev == b->dev && a->ino == b->ino && a->size == b->size &&
         a->mtime_s == b->mtime_s && a->mtime_ns == b->mtime_ns;
}

static const char *
id_line(char *out, size_t size, const checked_t *c) {
  snprintf(out, size, "%s\t%llu %llu %llu %llu %llu\t%llu %llu %llu %llu %llu\t%d\t%s\n", CHECKED_TAG,
           c->cand.dev, c->cand.ino, c->cand.size, c->cand.mtime_s, c->cand.mtime_ns,
           c->stock.dev, c->stock.ino, c->stock.size, c->stock.mtime_s, c->stock.mtime_ns, c->ok, c->why);
  return out;
}

static void
checked_add(const checked_t *c) {
  if(!checked && !(checked = calloc(CHECKED_MAX, sizeof(*checked)))) return;
  for(int i = 0; i < nchecked; i++) {
    if(same_id(&checked[i].cand, &c->cand) && same_id(&checked[i].stock, &c->stock)) {
      checked[i] = *c;
      return;
    }
  }
  if(nchecked == CHECKED_MAX) {   /* keep the newer half */
    memmove(checked, checked + CHECKED_MAX / 2, (CHECKED_MAX / 2) * sizeof(*checked));
    nchecked = CHECKED_MAX / 2;
  }
  checked[nchecked++] = *c;
}

/* Results from earlier runs, made by this version. Called with checked_lock. */
static void
checked_load(void) {
  char line[512], version[16];
  checked_loaded = 1;
  FILE *f = fopen(CHECKED_FILE, "r");
  if(!f) return;
  while(fgets(line, sizeof(line), f)) {
    checked_t c;
    int used = 0;
    memset(&c, 0, sizeof(c));
    checked_lines++;
    if(sscanf(line, "%15[^\t]\t%llu %llu %llu %llu %llu\t%llu %llu %llu %llu %llu\t%d\t%n", version,
              &c.cand.dev, &c.cand.ino, &c.cand.size, &c.cand.mtime_s, &c.cand.mtime_ns,
              &c.stock.dev, &c.stock.ino, &c.stock.size, &c.stock.mtime_s, &c.stock.mtime_ns,
              &c.ok, &used) != 12 || strcmp(version, CHECKED_TAG)) {
      continue;
    }
    snprintf(c.why, sizeof(c.why), "%s", line + used);
    c.why[strcspn(c.why, "\r\n")] = 0;
    checked_add(&c);
  }
  fclose(f);
}

/* Remember a result: append it, or write the file afresh when it holds many
   stale lines. Called with checked_lock. */
static void
checked_save(const checked_t *c) {
  char line[512];
  checked_add(c);
  if(checked_lines > 2 * nchecked + 64) {
    char tmp[] = CHECKED_FILE ".tmp";
    int fd = fs_open_write(tmp);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
    if(!f) {
      if(fd >= 0) close(fd);
      return;
    }
    for(int i = 0; i < nchecked; i++) fputs(id_line(line, sizeof(line), &checked[i]), f);
    int ok = !ferror(f);
    if(fclose(f) || !ok || fs_rename(tmp, CHECKED_FILE)) fs_unlink(tmp);
    else checked_lines = nchecked;
    return;
  }
  if(!fs_write_allowed(CHECKED_FILE)) return;
  int fd = open(CHECKED_FILE, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0644);
  if(fd < 0) return;
  id_line(line, sizeof(line), c);
  if(write(fd, line, strlen(line)) > 0) checked_lines++;
  close(fd);
}


static atomic_int remembered_count;

int
asset_checks_remembered(void) {
  return atomic_exchange(&remembered_count, 0);
}


int
asset_validate(const char *cand, const char *stock, char *why, size_t len) {
  checked_t c;
  memset(&c, 0, sizeof(c));
  int ids = !file_id(cand, &c.cand) && !file_id(stock, &c.stock);
  if(ids) {
    pthread_mutex_lock(&checked_lock);
    if(!checked_loaded) checked_load();
    for(int i = 0; i < nchecked; i++) {
      if(same_id(&checked[i].cand, &c.cand) && same_id(&checked[i].stock, &c.stock)) {
        int ok = checked[i].ok;
        snprintf(why, len, "%s", checked[i].why);
        pthread_mutex_unlock(&checked_lock);
        atomic_fetch_add(&remembered_count, 1);
        return ok ? 0 : -1;
      }
    }
    pthread_mutex_unlock(&checked_lock);
  }

  int rc = validate_now(cand, stock, why, len);

  /* Only a file that is still the one checked is remembered. */
  file_id_t after;
  if(ids && !file_id(cand, &after) && same_id(&after, &c.cand)) {
    c.ok = !rc;
    snprintf(c.why, sizeof(c.why), "%s", rc ? why : "");
    for(char *q = c.why; *q; q++) if(*q == '\t' || *q == '\n' || *q == '\r') *q = ' ';
    pthread_mutex_lock(&checked_lock);
    checked_save(&c);
    pthread_mutex_unlock(&checked_lock);
  }
  return rc;
}


static int
validate_now(const char *cand, const char *stock, char *why, size_t len) {
  struct stat st;
  int kind = asset_kind(stock);

  if(kind == KIND_BLOCKED) {
    snprintf(why, len, "this file type is protected and cannot be themed");
    return -1;
  }
  if(stat(cand, &st) || !S_ISREG(st.st_mode) || !st.st_size) {
    snprintf(why, len, "empty or unreadable file");
    return -1;
  }
  if((uint64_t)st.st_size > asset_size_limit(kind)) {
    snprintf(why, len, "file is larger than the %llu MB limit for %s files",
             (unsigned long long)(asset_size_limit(kind) >> 20), asset_kind_name(kind));
    return -1;
  }

  switch(kind) {
  case KIND_IMAGE: {
    image_info_t want, got;
    if(image_probe(stock, &want)) { snprintf(why, len, "cannot read the system image to compare against"); return -1; }
    if(image_probe(cand, &got) || got.format != want.format) {
      snprintf(why, len, "file is not a valid %s image", image_format_name(want.format));
      return -1;
    }
    if(got.width != want.width || got.height != want.height) {
      snprintf(why, len, "image is %ux%u but the system asset is %ux%u",
               got.width, got.height, want.width, want.height);
      return -1;
    }
    return 0;
  }
  case KIND_SVG:   return check_svg(cand, why, len);
  case KIND_AUDIO: return check_audio(cand, stock, why, len);
  case KIND_FONT:  return check_font(cand, stock, why, len);
  case KIND_JSON:  return check_json(cand, stock, why, len);
  case KIND_VIDEO: return check_magic(cand, stock, video_container, "video", why, len);
  case KIND_TEXTURE: return texture_validate(cand, stock, why, len);
  case KIND_RCO:   return rco_validate(cand, stock, why, len);
  }
  snprintf(why, len, "unsupported file type");
  return -1;
}
