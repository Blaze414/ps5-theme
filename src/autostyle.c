/* Icon style for games and apps installed later.

   A theme's icon style (the manager's Games and apps part) is made in the manager's page for the titles installed
   when it is used. For a title installed afterwards, this styles its icon here, by itself, once the install is over:

     - only for the theme that is applied, whose style says so (icon-style/style.ini: auto=1), with the page's
       whole template (icon-style/base.png, the backdrop with the shape's shadow, icon-style/mask.png, the shape,
       and icon-style/known.txt): without the list of titles installed then, every title would look new;
     - only for a title not in the list of titles present when the page last used the style (icon-style/known.txt)
       and not styled here before (icon-style/made.txt): a title whose icon you put back by hand is never styled
       again behind your back, and neither is one you changed yourself (the theme has a file for it);
     - only once nothing in its folders has changed for AUTOSTYLE_SETTLE seconds: never while the console is still
       installing or downloading it, since a theme over a title's pictures keeps the console from writing them;
     - never during an apply. A game being played doesn't hold it up: the PS5 keeps a game left on the home screen
       running, using the processor as much as one being played (measured on 13.42), so the two can't be told
       apart that way, and waiting for none to be open could mean waiting for good. An apply touches the home
       screen's folders and the system's, never a game's own (its sandbox holds nothing of the theme's).
     Each title is tried once per run of the plugin.

   A title the theme already has pictures for, that wasn't installed when the theme was applied (uninstalled, and
   installed again; main.c lists them), gets them by itself too, once its install is over: the theme is applied
   again. If those pictures are the style's (the page's list of the titles it styled, icon-style/styled.txt, or
   made.txt), they are made again first with the style as it is now, from your picture kept for the title
   (icon-style/own/<title>.png) or else its own icon.

   The title's own icon (an 8-bit PNG, png_decode_rgba) is set into the shape over the template, then written as
   each of its icon files is (png_encode_like, texture_encode_like: the console's own form), and checked exactly as
   an upload is (asset_validate) before it goes into the theme. Then the theme is applied again, said once on the
   console when it is done, by the game's name (reapply_quietly): a game may be open, and an apply you start says
   each step. A title whose icons can't all be made this way (a compressed texture, a picture of another kind) is
   left for the manager's page, which handles every form, and the console says so.

   The console is told once when it sees such a title still installing (by the time the style follows), so nothing
   happens unannounced. The manager's page follows the same rule (autostyle_wait), and sees what changed here through
   titles_stamp in its status. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>

#include "platform.h"
#include "theme.h"


#define AUTOSTYLE_POLL     15            /* seconds between looks */
#define AUTOSTYLE_SETTLE   120           /* (600 before 1.73, when downloads weren't watched) */
#define AUTOSTYLE_TRIED    512
#define STYLE_DIR          "icon-style"
#define TEMPLATE_MAX_BYTES (8u << 20)
#define ICON_MAX_BYTES     (32u << 20)

#define AUTOSTYLE_MAX      16            /* titles looked at in one pass */

/* (the styler's thread alone uses these) */
static char tried[AUTOSTYLE_TRIED][16];
static int ntried;
static char said_template[128]; /* the theme whose incomplete template was last logged (said once, not each minute) */

static int
was_tried(const char *title) {
  for(int i = 0; i < ntried; i++) if(!strcmp(tried[i], title)) return 1;
  return 0;
}

static void
mark_tried(const char *title) {
  if(ntried < AUTOSTYLE_TRIED && !was_tried(title)) snprintf(tried[ntried++], sizeof(tried[0]), "%s", title);
}

/* A short list of title IDs kept by the styler's thread. */
typedef struct { char id[64][16]; int n; } idlist_t;

static int
in_list(const idlist_t *l, const char *title) {
  for(int i = 0; i < l->n; i++) if(!strcmp(l->id[i], title)) return 1;
  return 0;
}

/* Put `title` in the list or take it out; whether it is in the list afterwards (a full list takes no more). */
static int
mark_list(idlist_t *l, const char *title, int on) {
  for(int i = 0; i < l->n; i++) {
    if(strcmp(l->id[i], title)) continue;
    if(!on) memmove(l->id[i], l->id[i + 1], sizeof(l->id[0]) * (size_t)(--l->n - i));
    return on;
  }
  if(!on || l->n >= (int)(sizeof(l->id) / sizeof(*l->id))) return 0;
  snprintf(l->id[l->n++], sizeof(l->id[0]), "%s", title);
  return 1;
}

/* Titles whose pictures were put back since they were last seen not installed: once each install. */
static idlist_t backed;
/* Titles the console was told are installing: once each install. */
static idlist_t told;

/* Something the styler did that the manager's page shows (a title styled, put back, or left for the page): the page
   reads the theme again when this changes (titles_stamp). */
static atomic_uint changes;

static int
installed(const char *title) {
  char p[PATH_MAX];
  struct stat st;
  snprintf(p, sizeof(p), "%s/%s", APPMETA_DIR, title);
  return !stat(p, &st) && S_ISDIR(st.st_mode);
}

/* Apply the theme again, said on the console once it is done, unless another theme was chosen meanwhile (it is
   applied without these, and they wait for this one's next apply). */
static void
apply_again(const char *theme, const char *message) {
  if(reapply_quietly(theme, message)) {
    log_msg("icon style: theme %s is no longer the one chosen; its icons wait for its next apply", theme);
  }
}

static int
env_seconds(const char *name, int fallback) {
#ifdef TEST_HOOKS
  const char *v = getenv(name);
  if(v && *v) return atoi(v);
#else
  (void)name;
#endif
  return fallback;
}

/* How long nothing in a title's folders must change before it counts as installed. */
int
autostyle_settle(void) {
  int s = env_seconds("PS5THEME_TEST_AUTOSTYLE_SETTLE", AUTOSTYLE_SETTLE);
  return s >= 0 ? s : AUTOSTYLE_SETTLE;
}

static uint8_t *
read_file(const char *path, size_t max, size_t *len) {
  struct stat st;
  int fd = open(path, O_RDONLY);
  if(fd < 0) return NULL;
  if(fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > max) { close(fd); return NULL; }
  uint8_t *b = malloc((size_t)st.st_size);
  size_t got = 0;
  while(b && got < (size_t)st.st_size) {
    ssize_t n = read(fd, b + got, (size_t)st.st_size - got);
    if(n <= 0) break;
    got += (size_t)n;
  }
  close(fd);
  if(!b || got != (size_t)st.st_size) { free(b); return NULL; }
  *len = got;
  return b;
}

/* Whether `title` is a line of `list` (a file's text, read once a pass). */
static int
listed(const char *list, size_t len, const char *title) {
  size_t n = strlen(title);
  for(const char *p = list, *end = list + len; p && p + n <= end;) {
    if(!memcmp(p, title, n) && (p + n == end || p[n] == '\n' || p[n] == '\r')) return 1;
    p = memchr(p, '\n', (size_t)(end - p));
    if(p) p++;
  }
  return 0;
}

/* A title ID as the console's folders name them: nine letters and digits. */
static int
title_id_valid(const char *s) {
  if(strlen(s) != 9) return 0;
  for(const char *p = s; *p; p++) if(!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) return 0;
  return strcmp(s, TITLE_ID) != 0;
}

/* A look at a title's folders: whether its icon is there, the newest date on anything in them, and a print that
   changes when anything does (any file's date or size, a file coming or going). The game's own package (app.pkg, or a
   patch's) grows while it downloads without its folder changing, and so does the console's download of it
   (/user/download/<ID>, written in place, its size fixed from the start), so they are looked at too. */
static void
fold(uint64_t *h, uint64_t v) {
  *h = (*h ^ v) * 1099511628211ull;
}

static void
fold_path(const char *p, time_t *newest, uint64_t *print) {
  struct stat st;
  if(stat(p, &st)) { fold(print, 0x6e6f6e65u); return; }   /* (not there: a print of its own) */
  if(st.st_mtime > *newest) *newest = st.st_mtime;
  fold(print, (uint64_t)st.st_mtime);
  fold(print, (uint64_t)st.st_size);
}

static int
look_at(const char *title, time_t *newest, uint64_t *print) {
  char p[PATH_MAX];
  struct stat st;
  *newest = 0;
  *print = 1469598103934665603ull;
  snprintf(p, sizeof(p), "%s/%s/icon0.png", APPMETA_DIR, title);
  if(stat(p, &st) || !S_ISREG(st.st_mode)) return -1;
  const char *const parts[] = { "", "/icon0.png", "/icon0.dds", "/param.json", "/param.sfo" };
  for(size_t i = 0; i < sizeof(parts) / sizeof(*parts); i++) {
    snprintf(p, sizeof(p), "%s/%s%s", APPMETA_DIR, title, parts[i]);
    fold_path(p, newest, print);
    snprintf(p, sizeof(p), "%s/%s/sce_sys%s", APP_DIR, title, parts[i]);
    fold_path(p, newest, print);
  }
  const char *const others[] = { APP_DIR "/%s", APP_DIR "/%s/app.pkg", PATCH_DIR "/%s", PATCH_DIR "/%s/patch.pkg", DOWNLOAD_DIR "/%s" };
  for(size_t i = 0; i < sizeof(others) / sizeof(*others); i++) {
    snprintf(p, sizeof(p), others[i], title);
    fold_path(p, newest, print);
  }
  snprintf(p, sizeof(p), "%s/%s", DOWNLOAD_DIR, title);
  DIR *d = opendir(p);
  struct dirent *e;
  for(int n = 0; d && n < 32 && (e = readdir(d)); ) {   /* its download's files, in whatever order they are listed */
    char f[PATH_MAX];
    time_t t = 0;
    uint64_t one = 1469598103934665603ull;
    if(e->d_name[0] == '.' || snprintf(f, sizeof(f), "%s/%s", p, e->d_name) >= (int)sizeof(f)) continue;
    for(const char *c = e->d_name; *c; c++) fold(&one, (uint8_t)*c);
    fold_path(f, &t, &one);
    if(t > *newest) *newest = t;
    *print ^= one;
    n++;
  }
  if(d) closedir(d);
  return 0;
}

static time_t
mono_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec;
}

/* What each title looked like when last looked at, and since when it has looked so, by this plugin's own clock:
   the wait is timed here, never from the files' dates alone. An installer may date them ahead of the console's
   clock (one did by an hour, 2026-10-29) or behind it; a title changing all along waits until it stops. At the
   first look (the plugin just started), its files' newest date says how long it has been as it is, at most until
   now. Shared by the styler and the page's requests. */
#define SEEN_MAX 256
static struct { char id[16]; uint64_t print; time_t since, used; } seen[SEEN_MAX];
static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;

static time_t
quiet_since(const char *title, time_t newest, uint64_t print) {
  time_t now = mono_now(), age = time(NULL) - newest;
  pthread_mutex_lock(&seen_lock);
  int at = -1, spare = 0;
  for(int i = 0; i < SEEN_MAX && at < 0; i++) {
    if(!strcmp(seen[i].id, title)) at = i;
    else if(seen[i].used < seen[spare].used) spare = i;   /* (the one looked at longest ago, when it is full) */
  }
  if(at < 0) {
    at = spare;
    snprintf(seen[at].id, sizeof(seen[at].id), "%s", title);
    seen[at].print = print;
    seen[at].since = now - (age > 0 ? age : 0);
  } else if(seen[at].print != print) {
    seen[at].print = print;
    seen[at].since = now;
  }
  seen[at].used = now;
  time_t since = seen[at].since;
  pthread_mutex_unlock(&seen_lock);
  return since;
}

/* Seconds the title has looked as it does now; -1 while its icon isn't there. */
static long
quiet_for(const char *title) {
  time_t newest;
  uint64_t print;
  if(look_at(title, &newest, &print)) return -1;
  return (long)(mono_now() - quiet_since(title, newest, print));
}

/* Whether nothing in the title's folders has changed for `settle` seconds, and its icon is there. */
static int
settled(const char *title, int settle) {
  return quiet_for(title) >= settle;
}

/* Seconds until the title counts as installed (0: it does); -1 for no title, or one whose icon isn't there yet.
   The manager's page waits as this does before it styles a title or applies the theme for one. */
int
autostyle_wait(const char *title) {
  if(!title_id_valid(title)) return -1;
  long quiet = quiet_for(title), settle = autostyle_settle();
  if(quiet < 0) return -1;
  return quiet >= settle ? 0 : (int)(settle - (quiet > 0 ? quiet : 0));
}

/* The installed titles and what the styler did, as one short text that changes when either does: the manager's
   page reads its titles and the theme again when it does. */
void
titles_stamp(char *out, size_t size) {
  unsigned n = 0;
  uint32_t mix = 0;
  DIR *d = opendir(APPMETA_DIR);
  struct dirent *e;
  while(d && (e = readdir(d))) {
    if(!title_id_valid(e->d_name)) continue;
    uint32_t h = 2166136261u;
    for(const char *c = e->d_name; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    mix ^= h;   /* (the same whatever order the folder lists them in) */
    n++;
  }
  if(d) closedir(d);
  snprintf(out, size, "%u.%08x.%u", n, (unsigned)mix, atomic_load(&changes));
}

/* ---- A title's name, for the console's messages ---- */

static uint32_t le32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static uint32_t le16(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8; }

static int
hex_digit(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Cut a last character that didn't fit whole. */
static void
utf8_whole(char *s) {
  size_t n = strlen(s), i = n;
  if(!n) return;
  while(i > 0 && ((uint8_t)s[i - 1] & 0xc0) == 0x80) i--;   /* back to the last character's first byte */
  if(!i) { s[0] = 0; return; }
  uint8_t lead = (uint8_t)s[i - 1];
  size_t want = lead < 0x80 ? 1 : lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
  if(n - (i - 1) < want) s[i - 1] = 0;
}

static void
put_char(char *out, size_t size, size_t *o, int *full, uint32_t c) {
  char b[4];
  size_t k = 0;
  if(c < 0x20) c = ' ';   /* (a line break or tab in a name: a space) */
  if(c < 0x80) b[k++] = (char)c;
  else if(c < 0x800) { b[k++] = (char)(0xc0 | c >> 6); b[k++] = (char)(0x80 | (c & 0x3f)); }
  else if(c < 0x10000) { b[k++] = (char)(0xe0 | c >> 12); b[k++] = (char)(0x80 | (c >> 6 & 0x3f)); b[k++] = (char)(0x80 | (c & 0x3f)); }
  else { b[k++] = (char)(0xf0 | c >> 18); b[k++] = (char)(0x80 | (c >> 12 & 0x3f)); b[k++] = (char)(0x80 | (c >> 6 & 0x3f)); b[k++] = (char)(0x80 | (c & 0x3f)); }
  if(*full || *o + k >= size) { *full = 1; return; }
  memcpy(out + *o, b, k);
  *o += k;
}

/* The JSON string at `*p` (its opening quote), as UTF-8 into `out`, cut short to fit; `*p` after it. -1 if it isn't one. */
static int
json_string(const char **pp, const char *end, char *out, size_t size) {
  const char *p = *pp;
  size_t o = 0;
  int full = 0;
  if(p >= end || *p != '"') return -1;
  for(p++; p < end && *p != '"'; p++) {
    uint32_t c = (uint8_t)*p;
    if(c == '\\') {
      if(++p >= end) return -1;
      if(*p == 'u') {
        int ok = end - p > 4;
        c = 0;
        for(int i = 1; ok && i <= 4; i++) { int h = hex_digit(p[i]); if(h < 0) ok = 0; else c = c << 4 | (uint32_t)h; }
        if(!ok) return -1;
        p += 4;
        if(c >= 0xd800 && c < 0xdc00 && end - p > 6 && p[1] == '\\' && p[2] == 'u') {   /* a pair: one character */
          uint32_t lo = 0;
          for(int i = 3; ok && i <= 6; i++) { int h = hex_digit(p[i]); if(h < 0) ok = 0; else lo = lo << 4 | (uint32_t)h; }
          if(ok && lo >= 0xdc00 && lo < 0xe000) { c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00); p += 6; }
        }
        if(c >= 0xd800 && c < 0xe000) c = 0xfffd;
        put_char(out, size, &o, &full, c);
        continue;
      }
      c = *p == 'n' || *p == 't' || *p == 'r' || *p == 'b' || *p == 'f' ? ' ' : (uint8_t)*p;
    }
    if(c < 0x20) c = ' ';
    if(!full && o + 1 < size) out[o++] = (char)c;   /* (bytes of the file's own UTF-8 as they are) */
    else full = 1;
  }
  if(p >= end) return -1;
  if(size) { out[o < size ? o : size - 1] = 0; utf8_whole(out); }
  *pp = p + 1;
  return 0;
}

/* What a param.json gives: its default language, and each language's titleName. */
typedef struct {
  const char *p, *end;
  char keys[3][32];
  char lang[32];
  struct { char lang[32], name[96]; } names[24];
  int nnames;
} names_scan_t;

static void
skip_space(names_scan_t *s) {
  while(s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')) s->p++;
}

/* One JSON value at `depth` keys in (keys[0..depth-1] name it, as far as three). -1 if it isn't JSON. */
static int
scan_value(names_scan_t *s, int depth) {
  skip_space(s);
  if(s->p >= s->end || depth > 16) return -1;
  char c = *s->p;
  if(c == '{' || c == '[') {
    char close = c == '{' ? '}' : ']';
    s->p++;
    skip_space(s);
    if(s->p < s->end && *s->p == close) { s->p++; return 0; }
    for(;;) {
      if(c == '{') {
        char key[32];
        skip_space(s);
        if(json_string(&s->p, s->end, key, sizeof(key))) return -1;
        if(depth < 3) snprintf(s->keys[depth], sizeof(s->keys[0]), "%s", key);
        skip_space(s);
        if(s->p >= s->end || *s->p != ':') return -1;
        s->p++;
      } else if(depth < 3) {
        s->keys[depth][0] = 0;
      }
      if(scan_value(s, depth + 1)) return -1;
      skip_space(s);
      if(s->p < s->end && *s->p == ',') { s->p++; continue; }
      if(s->p < s->end && *s->p == close) { s->p++; return 0; }
      return -1;
    }
  }
  if(c == '"') {
    char v[96];
    if(json_string(&s->p, s->end, v, sizeof(v))) return -1;
    if(depth == 2 && !strcmp(s->keys[0], "localizedParameters") && !strcmp(s->keys[1], "defaultLanguage")) {
      snprintf(s->lang, sizeof(s->lang), "%s", v);
    } else if(depth == 3 && !strcmp(s->keys[0], "localizedParameters") && !strcmp(s->keys[2], "titleName") && v[0] &&
              s->nnames < (int)(sizeof(s->names) / sizeof(*s->names))) {
      snprintf(s->names[s->nnames].lang, sizeof(s->names[0].lang), "%s", s->keys[1]);
      snprintf(s->names[s->nnames].name, sizeof(s->names[0].name), "%s", v);
      s->nnames++;
    }
    return 0;
  }
  while(s->p < s->end && *s->p != ',' && *s->p != '}' && *s->p != ']' && *s->p != ' ' && *s->p != '\n' && *s->p != '\r' && *s->p != '\t') s->p++;
  return 0;   /* a number, true, false or null */
}

/* A PS4 title's name: the TITLE of its param.sfo. */
static int
sfo_title(const uint8_t *b, size_t n, char *out, size_t size) {
  if(n < 20 || memcmp(b, "\0PSF", 4) || !size) return -1;
  uint32_t keys = le32(b + 8), data = le32(b + 12), count = le32(b + 16);
  for(uint32_t i = 0; i < count && i < 256; i++) {
    size_t e = 20 + (size_t)i * 16;
    if(e + 16 > n) return -1;
    size_t k = (size_t)keys + le16(b + e), d = (size_t)data + le32(b + e + 12), len = le32(b + e + 4);
    if(k + 6 > n || memcmp(b + k, "TITLE\0", 6)) continue;
    if(d > n || len > n - d) return -1;
    size_t o = 0;
    for(size_t j = 0; j < len && b[d + j] && o + 1 < size; j++) out[o++] = b[d + j] < 0x20 ? ' ' : (char)b[d + j];
    out[o] = 0;
    utf8_whole(out);
    return out[0] ? 0 : -1;
  }
  return -1;
}

/* The title's name as the console shows it: its param.json's in its default language (else English, else any),
   or a PS4 title's param.sfo's; the title ID when neither can be read. */
void
title_name(const char *title, char *out, size_t size) {
  char p[PATH_MAX];
  size_t len = 0;
  uint8_t *b;
  snprintf(out, size, "%s", title);
  if(!title_id_valid(title)) return;
  snprintf(p, sizeof(p), "%s/%s/param.json", APPMETA_DIR, title);
  if((b = read_file(p, 256u << 10, &len))) {
    names_scan_t *s = calloc(1, sizeof(*s));
    if(s) {
      s->p = (const char *)b;
      s->end = (const char *)b + len;
      if(!scan_value(s, 0)) {
        const char *pick = NULL;
        for(int i = 0; i < s->nnames && !pick; i++) if(s->lang[0] && !strcmp(s->names[i].lang, s->lang)) pick = s->names[i].name;
        for(int i = 0; i < s->nnames && !pick; i++) if(!strcmp(s->names[i].lang, "en-US")) pick = s->names[i].name;
        if(!pick && s->nnames) pick = s->names[0].name;
        if(pick) { snprintf(out, size, "%s", pick); utf8_whole(out); }
      }
      int found = s->nnames > 0;
      free(s);
      free(b);
      if(found) return;
    } else {
      free(b);
    }
  }
  snprintf(p, sizeof(p), "%s/%s/param.sfo", APPMETA_DIR, title);
  if((b = read_file(p, 1u << 20, &len))) {
    char name[128];
    if(!sfo_title(b, len, name, sizeof(name))) snprintf(out, size, "%s", name);
    free(b);
  }
}

/* "Itemzflow", "Itemzflow and Astro Bot", "Itemzflow, Astro Bot and Bloodborne", "Itemzflow, Astro Bot and 3 more". */
static void
names_of(const char (*titles)[16], int n, char *out, size_t size) {
  out[0] = 0;
  int shown = n > 3 ? 2 : n;
  for(int i = 0; i < shown; i++) {
    char name[96];
    title_name(titles[i], name, sizeof(name));
    size_t at = strlen(out);
    snprintf(out + at, size - at, "%s%s", i ? (i == n - 1 ? " and " : ", ") : "", name);
  }
  size_t at = strlen(out);
  if(n > shown) snprintf(out + at, size - at, " and %d more", n - shown);
}

/* What the console says once the theme is applied again for icons styled (by this, or the page) and titles whose
   pictures went back. */
void
autostyle_note(char *out, size_t size, const char (*styled)[16], int nstyled, const char (*back)[16], int nback) {
  char a[320], b[320];
  names_of(styled, nstyled, a, sizeof(a));
  names_of(back, nback, b, sizeof(b));
  int many = nstyled + nback > 1 || nback;
  if(nstyled && nback) {
    snprintf(out, size, "ps5-theme: icon%s styled for %s; the theme's pictures are back for %s. Rest mode shows them on the home row.",
             nstyled > 1 ? "s" : "", a, b);
  } else if(nstyled) {
    snprintf(out, size, "ps5-theme: icon%s styled for %s. Rest mode shows %s on the home row.", nstyled > 1 ? "s" : "", a, many ? "them" : "it");
  } else {
    snprintf(out, size, "ps5-theme: the theme's pictures are back for %s. Rest mode shows them on the home row.", b);
  }
}

/* Whether the theme is still there (it may be deleted while a title is styled). */
static int
theme_present(const char *theme) {
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s/%s", THEMES_DIR, theme);
  return fs_is_dir(dir);
}

/* The template's pixel at (x, y) of a side-by-side picture, `side` pixels. */
static void
bilinear(const uint8_t *src, uint32_t w, uint32_t h, double u, double v, uint8_t out[4]) {
  if(u < 0) u = 0;
  if(v < 0) v = 0;
  if(u > w - 1) u = w - 1;
  if(v > h - 1) v = h - 1;
  uint32_t x0 = (uint32_t)u, y0 = (uint32_t)v, x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
  double fx = u - x0, fy = v - y0;
  for(int c = 0; c < 4; c++) {
    double a = src[((size_t)y0 * w + x0) * 4 + c] * (1 - fx) + src[((size_t)y0 * w + x1) * 4 + c] * fx;
    double b = src[((size_t)y1 * w + x0) * 4 + c] * (1 - fx) + src[((size_t)y1 * w + x1) * 4 + c] * fx;
    out[c] = (uint8_t)(a * (1 - fy) + b * fy + 0.5);
  }
}

/* The styled icon at the template's size: the template, and inside the shape the title's icon over it, set into the
   square the style's size gives (the same square the page draws it in). */
static uint8_t *
compose(const uint8_t *base, const uint8_t *mask, uint32_t side, int scale, const uint8_t *icon, uint32_t iw, uint32_t ih) {
  uint8_t *out = malloc((size_t)side * side * 4);
  if(!out) return NULL;
  memcpy(out, base, (size_t)side * side * 4);
  double r = side / 2.0 * scale / 100.0, x0 = side / 2.0 - r, y0 = side / 2.0 - r;
  for(uint32_t y = 0; y < side; y++) {
    for(uint32_t x = 0; x < side; x++) {
      unsigned a = mask[((size_t)y * side + x) * 4 + 3];
      if(!a) continue;
      uint8_t px[4];
      bilinear(icon, iw, ih, (x + 0.5 - x0) / (2 * r) * iw - 0.5, (y + 0.5 - y0) / (2 * r) * ih - 0.5, px);
      uint8_t *o = out + ((size_t)y * side + x) * 4;
      for(int c = 0; c < 3; c++) o[c] = (uint8_t)((px[c] * a + o[c] * (255 - a) + 127) / 255);
      o[3] = 255;
    }
  }
  return out;
}

/* The styled icon at w x h. */
static uint8_t *
resized(const uint8_t *styled, uint32_t side, uint32_t w, uint32_t h) {
  uint8_t *out = malloc((size_t)w * h * 4);
  if(!out) return NULL;
  for(uint32_t y = 0; y < h; y++) {
    for(uint32_t x = 0; x < w; x++) {
      bilinear(styled, side, side, (x + 0.5) * side / w - 0.5, (y + 0.5) * side / h - 0.5, out + ((size_t)y * w + x) * 4);
    }
  }
  return out;
}

typedef struct { char asset[PATH_MAX], dst[PATH_MAX], part[PATH_MAX + 16]; } made_t;

/* Make the title's icon files into the theme (each checked as an upload is), from `source` (your picture kept for
   it) or else its own icon. 0 when all were made. */
static int
style_title(const char *theme, const char *title, const uint8_t *base, const uint8_t *mask, uint32_t side, int scale,
            const char *source) {
  char asset[PATH_MAX], stock[PATH_MAX], why[256];
  made_t made[4];
  int nmade = 0, rc = -1;
  size_t len;
  uint32_t iw, ih;
  uint8_t *iconfile = NULL, *icon = NULL, *styled = NULL;

  snprintf(asset, sizeof(asset), "%s/%s/icon0.png", APPMETA_DIR, title);
  stock_path(asset, stock, sizeof(stock));
  if(!(iconfile = read_file(source ? source : stock, ICON_MAX_BYTES, &len)) || !(icon = png_decode_rgba(iconfile, len, &iw, &ih))) {
    log_msg("icon style: %s's icon isn't a picture this can read; the manager's page can style it", title);
    goto done;
  }
  if(!(styled = compose(base, mask, side, scale, icon, iw, ih))) goto done;

  const char *const files[] = { APPMETA_DIR "/%s/icon0.png", APPMETA_DIR "/%s/icon0.dds", APP_DIR "/%s/sce_sys/icon0.png", APP_DIR "/%s/sce_sys/icon0.dds" };
  for(size_t i = 0; i < sizeof(files) / sizeof(*files); i++) {
    struct stat st;
    uint8_t *stockb = NULL, *pixels = NULL, *bytes = NULL;
    size_t sl = 0, bl = 0;
    int ok = 0;
    snprintf(asset, sizeof(asset), files[i], title);
    if(stat(asset, &st) || !S_ISREG(st.st_mode)) continue;
    if(!asset_path_valid(asset)) continue;
    stock_path(asset, stock, sizeof(stock));
    int png = strlen(asset) > 4 && !strcmp(asset + strlen(asset) - 4, ".png");
    if(png) {
      uint32_t w, h;
      uint8_t *sp = (stockb = read_file(stock, ICON_MAX_BYTES, &sl)) ? png_decode_rgba(stockb, sl, &w, &h) : NULL;
      free(sp);
      if(sp && (pixels = resized(styled, side, w, h))) bytes = png_encode_like(stockb, sl, pixels, &bl);
    } else {
      texture_info_t t;
      int compressed = 1;
      if(!texture_info(stock, &t, NULL, &compressed) && !compressed && (stockb = read_file(stock, ICON_MAX_BYTES, &sl)) &&
         (pixels = resized(styled, side, t.width, t.height))) {
        bytes = texture_encode_like(stockb, sl, pixels, &bl);
      }
    }
    if(bytes) {
      made_t *m = &made[nmade];
      snprintf(m->asset, sizeof(m->asset), "%s", asset);
      snprintf(m->dst, sizeof(m->dst), "%s/%s/files%s", THEMES_DIR, theme, asset);
      /* Written outside the theme, in the uploads' folder, and its folder made only as it is placed, under the
         themes lock: the theme can be moved aside meanwhile (Discard, see web.c place_upload). */
      static atomic_uint style_seq;
      const char *base = strrchr(asset, '/');
      snprintf(m->part, sizeof(m->part), "%s/style-%u-%s.part", INCOMING_DIR, atomic_fetch_add(&style_seq, 1), base ? base + 1 : asset);
      if(fs_room_for(bl)) snprintf(why, sizeof(why), "the console's storage is nearly full");
      else if(!theme_present(theme)) snprintf(why, sizeof(why), "the theme was deleted meanwhile");   /* never a folder made again */
      else if(fs_mkdirs(INCOMING_DIR) || fs_write_file(m->part, bytes, bl)) snprintf(why, sizeof(why), "could not be written");
      else if(asset_validate(m->part, stock, why, sizeof(why))) fs_unlink(m->part);
      else { nmade++; ok = 1; }
    } else {
      snprintf(why, sizeof(why), "its form is one the manager's page makes");
    }
    free(stockb);
    free(pixels);
    free(bytes);
    if(!ok) {
      log_msg("icon style: %s (%s) not styled here: %s", title, asset, why);
      goto done;
    }
  }
  if(!nmade) goto done;
  themes_lock();   /* (a delete holds it too) */
  int placed = theme_present(theme);
  for(int i = 0; i < nmade && placed; i++) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", made[i].dst);
    *strrchr(dir, '/') = 0;
    if(fs_mkdirs(dir) || fs_rename(made[i].part, made[i].dst)) placed = 0;
  }
  themes_unlock();
  if(!placed) { log_err("icon style: %s's icons could not be put in the theme", title); goto done; }
  nmade = 0;
  rc = 0;

done:
  for(int i = 0; i < nmade; i++) fs_unlink(made[i].part);
  free(iconfile);
  free(icon);
  free(styled);
  return rc;
}

/* Add `title` to the style's list of titles styled here, written whole through the write guard. */
static void
remember_made(const char *dir, const char *title) {
  char path[PATH_MAX];
  size_t len = 0;
  snprintf(path, sizeof(path), "%s/made.txt", dir);
  themes_lock();   /* the page clears the list under the same lock: never written back from a stale copy */
  uint8_t *old = read_file(path, 64u << 10, &len);
  size_t n = strlen(title);
  char *all = malloc(len + n + 2);
  if(all) {
    if(old) memcpy(all, old, len);
    memcpy(all + len, title, n);
    all[len + n] = '\n';
    if(fs_write_file(path, all, len + n + 1)) log_err("icon style: could not note %s as styled", title);
  }
  themes_unlock();
  free(all);
  free(old);
}

/* "Itemzflow is", "Itemzflow and Astro Bot are" ... */
static void
say_installing(const char (*titles)[16], int n, int again, int settle) {
  char names[320];
  int mins = settle < 60 ? 1 : (settle + 59) / 60;
  names_of(titles, n, names, sizeof(names));
  if(again) {
    notify("ps5-theme: %s %s installing again. The theme's pictures for %s go back %d minute%s after %s finishes.", names,
           n > 1 ? "are" : "is", n > 1 ? "them" : "it", mins, mins > 1 ? "s" : "", n > 1 ? "each install" : "the install");
  } else {
    notify("ps5-theme: %s %s installing. %s the theme's icon style %d minute%s after %s finishes.", names, n > 1 ? "are" : "is",
           n > 1 ? "Their icons get" : "Its icon gets", mins, mins > 1 ? "s" : "", n > 1 ? "each install" : "the install");
  }
}

/* One look at the titles: style the ones that are new and settled, put back the theme's pictures for the ones
   installed since it was applied (made again first where they are the style's), then apply the theme again. The
   console is told once of each such title still installing, and of one this can't style. */
static void
autostyle_pass(int settle) {
  char active[128], applied[128], busy[128], dir[PATH_MAX], path[PATH_MAX], value[16];
  read_active(active, sizeof(active));
  if(!applied_theme(applied, sizeof(applied)) || applying_theme(busy, sizeof(busy)) || strcmp(active, applied)) return;
  snprintf(dir, sizeof(dir), "%s/%s/%s", THEMES_DIR, applied, STYLE_DIR);
  snprintf(path, sizeof(path), "%s/style.ini", dir);
  int on = !read_ini_value(path, "auto", value, sizeof(value)) && !strcmp(value, "1");
  int scale = !read_ini_value(path, "scale", value, sizeof(value)) ? atoi(value) : 0;
  if(scale < 40 || scale > 100) on = 0;
  for(int i = told.n - 1; i >= 0; i--) if(!installed(told.id[i])) mark_list(&told, told.id[i], 0);   /* (told again next install) */

  /* The titles the theme has pictures for, installed since it was applied, once their install is over. */
  char back[AUTOSTYLE_MAX][16], again[AUTOSTYLE_MAX][16], waiting[64][16];
  int nback = 0, nagain = 0, nwaiting = titles_awaiting_install(waiting, 64);
  for(int i = 0; i < nwaiting; i++) {
    if(!installed(waiting[i])) mark_list(&backed, waiting[i], 0);   /* (uninstalled again: its next install counts) */
    else if(in_list(&backed, waiting[i])) continue;
    else if(settled(waiting[i], settle)) { if(nback < AUTOSTYLE_MAX) snprintf(back[nback++], sizeof(back[0]), "%s", waiting[i]); }
    else if(quiet_for(waiting[i]) >= 0 && nagain < AUTOSTYLE_MAX && !in_list(&told, waiting[i]) && mark_list(&told, waiting[i], 1)) {
      snprintf(again[nagain++], sizeof(again[0]), "%s", waiting[i]);
    }
  }

  char made_path[PATH_MAX], styled_path[PATH_MAX];
  size_t made_len = 0, styled_len = 0, known_len = 0;
  snprintf(made_path, sizeof(made_path), "%s/made.txt", dir);
  snprintf(styled_path, sizeof(styled_path), "%s/styled.txt", dir);
  uint8_t *made = read_file(made_path, 256u << 10, &made_len), *styled_list = read_file(styled_path, 256u << 10, &styled_len), *known = NULL;
  if(on) {
    snprintf(path, sizeof(path), "%s/known.txt", dir);
    if(!(known = read_file(path, 256u << 10, &known_len))) {   /* without it, every title would look new */
      if(strcmp(said_template, applied)) {
        log_err("icon style: theme %s's style template isn't whole; open the theme's Games and apps in the manager to keep it again", applied);
        snprintf(said_template, sizeof(said_template), "%s", applied);
      }
      on = 0;
    } else {
      said_template[0] = 0;
    }
  }

  /* New titles: not in the list of those installed when the style was used, not styled here before, and with no
     file of the theme's (one you changed yourself, or one put back above). One still installing is said once. */
  char fresh[AUTOSTYLE_MAX][16], soon[AUTOSTYLE_MAX][16];
  int nfresh = 0, nsoon = 0;
  DIR *d = on ? opendir(APPMETA_DIR) : NULL;
  struct dirent *e;
  while(d && (e = readdir(d)) && (nfresh < AUTOSTYLE_MAX || nsoon < AUTOSTYLE_MAX)) {
    if(!title_id_valid(e->d_name) || was_tried(e->d_name)) continue;
    char own[PATH_MAX];
    struct stat st;
    snprintf(own, sizeof(own), "%s/%s/files%s/%s/icon0.png", THEMES_DIR, applied, APPMETA_DIR, e->d_name);
    if(listed((const char *)known, known_len, e->d_name) || (made && listed((const char *)made, made_len, e->d_name)) ||
       !lstat(own, &st)) continue;
    if(!settled(e->d_name, settle)) {   /* (not marked tried: looked at again next time) */
      if(quiet_for(e->d_name) >= 0 && nsoon < AUTOSTYLE_MAX && !in_list(&told, e->d_name) && mark_list(&told, e->d_name, 1)) {
        snprintf(soon[nsoon++], sizeof(soon[0]), "%s", e->d_name);
      }
      continue;
    }
    if(nfresh < AUTOSTYLE_MAX) snprintf(fresh[nfresh++], sizeof(fresh[0]), "%s", e->d_name);
  }
  if(d) closedir(d);
  free(known);
  if(nsoon) say_installing((const char (*)[16])soon, nsoon, 0, settle);
  if(nagain) say_installing((const char (*)[16])again, nagain, 1, settle);

  /* Of those put back, the style's are made again with it as it is now. */
  int restyle[AUTOSTYLE_MAX] = {0}, nrestyle = 0;
  for(int i = 0; on && i < nback; i++) {
    restyle[i] = (made && listed((const char *)made, made_len, back[i])) || (styled_list && listed((const char *)styled_list, styled_len, back[i]));
    nrestyle += restyle[i];
  }
  free(made);
  free(styled_list);
  if(!nfresh && !nback) return;

  size_t bl = 0, ml = 0;
  uint32_t bw = 0, bh = 0, mw = 0, mh = 0;
  uint8_t *bf = NULL, *mf = NULL, *base = NULL, *mask = NULL;
  int unreadable = 0;
  if(nfresh || nrestyle) {
    snprintf(path, sizeof(path), "%s/base.png", dir);
    if((bf = read_file(path, TEMPLATE_MAX_BYTES, &bl))) base = png_decode_rgba(bf, bl, &bw, &bh);
    snprintf(path, sizeof(path), "%s/mask.png", dir);
    if((mf = read_file(path, TEMPLATE_MAX_BYTES, &ml))) mask = png_decode_rgba(mf, ml, &mw, &mh);
    free(bf);
    free(mf);
    if(!base || !mask || bw != bh || mw != bw || mh != bh) {
      log_err("icon style: theme %s's style template can't be read; open the theme's Games and apps in the manager to keep it again", applied);
      unreadable = 1;
      for(int i = 0; i < nback; i++) restyle[i] = 0;   /* their pictures go back as they are */
    }
  }

  int styled = 0, left = 0;
  char done[AUTOSTYLE_MAX][16], leftover[AUTOSTYLE_MAX][16];
  for(int i = 0; i < nfresh; i++) {
    mark_tried(fresh[i]);
    if(unreadable || style_title(applied, fresh[i], base, mask, bw, scale, NULL)) {
      snprintf(leftover[left++], sizeof(leftover[0]), "%s", fresh[i]);
      continue;
    }
    remember_made(dir, fresh[i]);
    log_msg("icon style: styled %s's icon in theme %s", fresh[i], applied);
    snprintf(done[styled++], sizeof(done[0]), "%s", fresh[i]);
  }
  for(int i = 0; i < nback; i++) {
    mark_list(&backed, back[i], 1);
    if(restyle[i]) {
      char own[PATH_MAX];
      snprintf(own, sizeof(own), "%s/own/%s.png", dir, back[i]);
      if(style_title(applied, back[i], base, mask, bw, scale, access(own, R_OK) ? NULL : own)) {
        log_msg("icon style: %s's icon is put back as the theme has it", back[i]);
      } else {
        log_msg("icon style: styled %s's icon in theme %s again, as the style is now", back[i], applied);
      }
    }
    log_msg("icon style: %s is installed again; theme %s is applied again for its pictures", back[i], applied);
  }
  free(base);
  free(mask);
  if(styled || nback || left) atomic_fetch_add(&changes, 1);   /* (an open manager page reads the theme again) */
  if(left) {
    char names[320];
    names_of((const char (*)[16])leftover, left, names, sizeof(names));
    notify("ps5-theme: %s%s%s can't be styled on the console. Open the theme in the manager to style %s.", left > 1 ? "the icons of " : "",
           names, left > 1 ? "" : "'s icon", left > 1 ? "them" : "it");
  }
  if(!styled && !nback) return;

  char message[1024];
  autostyle_note(message, sizeof(message), (const char (*)[16])done, styled, (const char (*)[16])back, nback);
  apply_again(applied, message);
}

void *
autostyle_thread(void *arg) {
  (void)arg;
  int poll = env_seconds("PS5THEME_TEST_AUTOSTYLE_POLL", AUTOSTYLE_POLL);
  int settle = env_seconds("PS5THEME_TEST_AUTOSTYLE_SETTLE", AUTOSTYLE_SETTLE);
  for(;;) {
    sleep((unsigned)(poll > 0 ? poll : AUTOSTYLE_POLL));
    autostyle_pass(settle);
  }
  return NULL;
}
