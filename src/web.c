/* Theme manager web server: serves the UI and a small JSON API on WEB_PORT.

   Every mutating request must carry "X-PS5-Theme: 1". Browsers cannot add a
   custom header cross-origin without a CORS preflight (which we never answer),
   so other web pages opened on the console cannot drive this API.

   All writes go through fsguard.c and stay below THEMES_DIR. Uploads are
   verified (length, client checksum, image format and exact pixel size against
   the stock asset) before they are moved into a theme, under the theme lock.

   A listener thread accepts connections and hands them to WEB_WORKERS
   workers through a bounded queue, so one slow client cannot stall the UI.
   Each worker owns its buffers; handlers share nothing else unlocked. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "platform.h"
#include "theme.h"


#define MAX_HEADER   (16 * 1024)
#define MAX_RESULTS      300
#define MAX_RESULTS_ALL  5000
#define IO_CHUNK     (64 * 1024)

static atomic_int listen_fd = -1;   /* the listening socket, for a test hook */
#ifdef TEST_HOOKS
static atomic_int deaf;              /* the web server stops for good, as 1.49's did */
#endif

__asm__(
  ".section .rodata\n"
  ".balign 16\n"
  ".global web_index, web_index_end, web_atrac9, web_atrac9_end, web_icon, web_icon_end\n"
  "web_index:\n"
  ".incbin \"web/index.html\"\n"
  "web_index_end:\n"
  ".balign 16\n"
  "web_atrac9:\n"
  ".incbin \"web/atrac9.js\"\n"
  "web_atrac9_end:\n"
  ".balign 16\n"
  "web_icon:\n"
  ".incbin \"assets/icon0.png\"\n"
  "web_icon_end:\n"
  ".previous\n"
);
extern const char web_index[], web_index_end[];
extern const char web_atrac9[], web_atrac9_end[];
extern const char web_icon[], web_icon_end[];



typedef struct request {
  int  fd;
  int  slot;           /* crash breadcrumb slot of the worker */
  char *io;            /* IO_CHUNK bytes owned by the worker */
  char method[8];
  char path[256];
  char query[2048];
  long content_length;
  int  has_token;
  char *body;          /* bytes of the body already read with the headers */
  size_t body_len;
} request_t;


typedef struct strbuf {
  char  *data;
  size_t len, cap;
  int    failed;
} strbuf_t;


static void
sb_append(strbuf_t *sb, const char *s, size_t n) {
  if(sb->failed) return;
  if(sb->len + n + 1 > sb->cap) {
    size_t cap = sb->cap ? sb->cap : 1024;
    while(cap < sb->len + n + 1) cap *= 2;
    char *p = realloc(sb->data, cap);
    if(!p) { sb->failed = 1; return; }
    sb->data = p;
    sb->cap = cap;
  }
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = 0;
}


#define SB_LIT(sb, lit) sb_append((sb), (lit), sizeof(lit) - 1)


static void
sb_printf(strbuf_t *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void
sb_printf(strbuf_t *sb, const char *fmt, ...) {
  char buf[1024];
  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if(n > 0) sb_append(sb, buf, MIN((size_t)n, sizeof(buf) - 1));
}


static void
sb_json_str(strbuf_t *sb, const char *s) {
  SB_LIT(sb, "\"");
  for(; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if(c == '"' || c == '\\') {
      char esc[2] = {'\\', (char)c};
      sb_append(sb, esc, 2);
    } else if(c < 0x20) {
      sb_printf(sb, "\\u%04x", c);
    } else {
      sb_append(sb, (const char *)&c, 1);
    }
  }
  SB_LIT(sb, "\"");
}


static int
send_all(int fd, const void *data, size_t len) {
  const char *p = data;
  while(len) {
    ssize_t n = write(fd, p, len);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) return -1;
    p += n;
    len -= n;
  }
  return 0;
}


static const char *
status_text(int status) {
  switch(status) {
  case 200: return "OK";
  case 400: return "Bad Request";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 409: return "Conflict";
  case 413: return "Payload Too Large";
  case 422: return "Unprocessable Entity";
  default:  return "Internal Server Error";
  }
}


static void
respond(int fd, int status, const char *ctype, const void *body, size_t len) {
  char head[256];
  int n = snprintf(head, sizeof(head),
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %zu\r\n"
                   "Cache-Control: no-store\r\n"
                   "X-Content-Type-Options: nosniff\r\n"
                   "Connection: close\r\n\r\n",
                   status, status_text(status), ctype, len);
  if(send_all(fd, head, n)) return;
  if(len) send_all(fd, body, len);
}


static void
respond_json(int fd, int status, strbuf_t *sb) {
  if(sb->failed) {
    free(sb->data);
    static const char oom[] = "{\"error\":\"out of memory\"}";
    respond(fd, 500, "application/json", oom, sizeof(oom) - 1);
    return;
  }
  respond(fd, status, "application/json", sb->data ? sb->data : "", sb->len);
  free(sb->data);
}


static void
respond_error(int fd, int status, const char *msg) {
  strbuf_t sb = {0};
  SB_LIT(&sb, "{\"error\":");
  sb_json_str(&sb, msg);
  SB_LIT(&sb, "}");
  if(status >= 500) log_err("web: %d %s", status, msg);
  else if(status == 409 || status == 413 || status == 414 || status == 422) log_msg("web: refused (%d): %s", status, msg);
  respond_json(fd, status, &sb);
}


static void
respond_ok(int fd) {
  static const char ok[] = "{\"ok\":true}";
  respond(fd, 200, "application/json", ok, sizeof(ok) - 1);
}


static const char *
content_type(const char *path) {
  const char *ext = strrchr(path, '.');
  if(!ext) return "application/octet-stream";
  if(!strcasecmp(ext, ".png"))  return "image/png";
  if(!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";
  if(!strcasecmp(ext, ".webp")) return "image/webp";
  if(!strcasecmp(ext, ".svg"))  return "image/svg+xml";
  if(!strcasecmp(ext, ".json")) return "application/json";
  if(!strcasecmp(ext, ".txt"))  return "text/plain; charset=utf-8";
  return "application/octet-stream";
}


static void
respond_file(request_t *req, const char *path) {
  struct stat st;
  int fd = req->fd;
  int in = open(path, O_RDONLY);

  if(in < 0 || fstat(in, &st) || !S_ISREG(st.st_mode)) {
    if(in >= 0) close(in);
    respond_error(fd, 404, "file not found");
    return;
  }

  char head[256];
  int n = snprintf(head, sizeof(head),
                   "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
                   "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                   "Connection: close\r\n\r\n",
                   content_type(path), (long long)st.st_size);
  if(!send_all(fd, head, n)) {
    for(;;) {
      ssize_t r = read(in, req->io, IO_CHUNK);
      if(r < 0 && errno == EINTR) continue;
      if(r <= 0 || send_all(fd, req->io, r)) break;
    }
  }
  close(in);
}


static int
hexval(char c) {
  if(c >= '0' && c <= '9') return c - '0';
  if(c >= 'a' && c <= 'f') return c - 'a' + 10;
  if(c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}


/* Extract and URL-decode one query parameter. Returns 0 if present. */
static int
get_param(const char *query, const char *key, char *out, size_t size) {
  size_t klen = strlen(key);
  const char *p = query;

  while(p && *p) {
    if(!strncmp(p, key, klen) && p[klen] == '=') {
      const char *v = p + klen + 1;
      size_t o = 0;
      while(*v && *v != '&' && o + 1 < size) {
        if(*v == '%' && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
          out[o++] = (char)(hexval(v[1]) * 16 + hexval(v[2]));
          v += 3;
        } else {
          out[o++] = *v == '+' ? ' ' : *v;
          v++;
        }
      }
      if(*v && *v != '&') {
        /* longer than its field: never act on a cut-down value (a shorter path is a different file) */
        out[0] = 0;
        return -2;
      }
      out[o] = 0;
      return 0;
    }
    p = strchr(p, '&');
    if(p) p++;
  }
  out[0] = 0;
  return -1;
}


/* A replaceable asset: allowed prefix, existing regular file, sane name, and
   its real location is also inside an allowed prefix. */
static int
asset_path_valid(const char *path) {
  char real[PATH_MAX];
  struct stat st;
  return path[0] == '/' && strlen(path) < 512 && !strchr(path, '\\') &&
         target_allowed(path) && !stat(path, &st) && S_ISREG(st.st_mode) &&
         realpath(path, real) && (target_allowed(real) || fs_read_within(real, STAGE_DIR));
}


static int
theme_exists(const char *id) {
  char dir[PATH_MAX];
  if(!theme_name_valid(id)) return 0;
  snprintf(dir, sizeof(dir), "%s/%s", THEMES_DIR, id);
  return fs_is_dir(dir);
}


static void
list_files(strbuf_t *sb, const char *root, const char *rel, int *first, int *count, int depth) {
  char dir[PATH_MAX];
  if(depth > 32) return;
  snprintf(dir, sizeof(dir), "%s%s", root, rel);

  DIR *d = opendir(dir);
  if(!d) return;
  struct dirent *e;
  while((e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char child_rel[PATH_MAX], child[PATH_MAX];
    struct stat st;
    if(snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, e->d_name) >= (int)sizeof(child_rel)) continue;
    snprintf(child, sizeof(child), "%s%s", root, child_rel);
    if(lstat(child, &st)) continue;
    if(S_ISDIR(st.st_mode)) {
      list_files(sb, root, child_rel, first, count, depth + 1);
    } else if(S_ISREG(st.st_mode)) {
      size_t n = strlen(child_rel);
      if(n > 5 && !strcmp(child_rel + n - 5, ".part")) continue;
      if(sb) {
        if(!*first) SB_LIT(sb, ",");
        *first = 0;
        SB_LIT(sb, "{\"path\":");
        sb_json_str(sb, child_rel);
        sb_printf(sb, ",\"size\":%lld}", (long long)st.st_size);
      }
      (*count)++;
    }
  }
  closedir(d);
}


/* Up to `max` image paths from a theme, for the library card preview. */
static void
preview_images(strbuf_t *sb, const char *root, const char *rel, int *n, int max, int depth) {
  char dir[PATH_MAX];
  if(depth > 32) return;
  snprintf(dir, sizeof(dir), "%s%s", root, rel);

  DIR *d = opendir(dir);
  if(!d) return;
  struct dirent *e;
  while(*n < max && (e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char child_rel[PATH_MAX], child[PATH_MAX];
    struct stat st;
    if(snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, e->d_name) >= (int)sizeof(child_rel)) continue;
    snprintf(child, sizeof(child), "%s%s", root, child_rel);
    if(lstat(child, &st)) continue;
    if(S_ISDIR(st.st_mode)) {
      preview_images(sb, root, child_rel, n, max, depth + 1);
    } else if(S_ISREG(st.st_mode) && image_format_for_path(child) != IMG_NONE) {
      if(*n) SB_LIT(sb, ",");
      sb_json_str(sb, child_rel);
      (*n)++;
    }
  }
  closedir(d);
}


/* LAN address: connect a UDP socket (no packet is sent) and read the source. */
void
lan_address(char *out, size_t size) {
  struct sockaddr_in dst, src;
  socklen_t len = sizeof(src);
  int fd = socket(AF_INET, SOCK_DGRAM, 0);

  out[0] = 0;
  if(fd < 0) return;
  memset(&dst, 0, sizeof(dst));
  memset(&src, 0, sizeof(src));
  dst.sin_family = AF_INET;
  dst.sin_port = htons(53);
  dst.sin_addr.s_addr = htonl(0x08080808);
  if(!connect(fd, (struct sockaddr *)&dst, sizeof(dst)) &&
     !getsockname(fd, (struct sockaddr *)&src, &len)) {
    uint32_t a = ntohl(src.sin_addr.s_addr);
    if(a) snprintf(out, size, "%u.%u.%u.%u", a >> 24, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
  }
  close(fd);
}


static const char *shown_preview(const char *id, char *path, size_t size, long long *mtime);
static int theme_unsaved(const char *id);
static void drop_snapshot(const char *id);

static void
api_status(request_t *req) {
  char fw[16], active[128], applied[128], applying[128], ip[32], result[RESULT_MAX];
  strbuf_t sb = {0};

  fw_string(fw, sizeof(fw));
  read_active(active, sizeof(active));
  applied_theme(applied, sizeof(applied));
  applying_theme(applying, sizeof(applying));
  last_result(result, sizeof(result));
  lan_address(ip, sizeof(ip));

  SB_LIT(&sb, "{\"version\":\"" PLUGIN_VERSION "\",\"fw\":");
  sb_json_str(&sb, fw);
  SB_LIT(&sb, ",\"ip\":");
  sb_json_str(&sb, ip);
  SB_LIT(&sb, ",\"active\":");
  sb_json_str(&sb, active);
  SB_LIT(&sb, ",\"applied\":");
  sb_json_str(&sb, applied);
  SB_LIT(&sb, ",\"applying\":");
  sb_json_str(&sb, applying);
  char step[128];
  int pct = apply_progress(step, sizeof(step));
  if(pct < 0) {
    SB_LIT(&sb, ",\"progress\":null");
  } else {
    sb_printf(&sb, ",\"progress\":{\"percent\":%d,\"text\":", pct);
    sb_json_str(&sb, step);
    SB_LIT(&sb, "}");
  }
  SB_LIT(&sb, ",\"result\":");
  sb_json_str(&sb, result);
  SB_LIT(&sb, ",\"themes\":[");

  DIR *d = opendir(THEMES_DIR);
  int first = 1;
  if(d) {
    struct dirent *e;
    while((e = readdir(d))) {
      char ini[PATH_MAX], files[PATH_MAX], name[128], tfw[64];
      int nfirst = 1, count = 0, shown = 0;
      if(!theme_name_valid(e->d_name)) continue;
      snprintf(ini, sizeof(ini), "%s/%s/theme.ini", THEMES_DIR, e->d_name);
      if(read_ini_value(ini, "name", name, sizeof(name))) snprintf(name, sizeof(name), "%s", e->d_name);
      if(read_ini_value(ini, "fw", tfw, sizeof(tfw))) tfw[0] = 0;
      snprintf(files, sizeof(files), "%s/%s/files", THEMES_DIR, e->d_name);
      list_files(NULL, files, "", &nfirst, &count, 0);

      if(!first) SB_LIT(&sb, ",");
      first = 0;
      SB_LIT(&sb, "{\"id\":");
      sb_json_str(&sb, e->d_name);
      SB_LIT(&sb, ",\"name\":");
      sb_json_str(&sb, name);
      SB_LIT(&sb, ",\"fw\":");
      sb_json_str(&sb, tfw);
      sb_printf(&sb, ",\"compatible\":%s,\"files\":%d,\"preview\":[",
                fw_matches(tfw, fw) ? "true" : "false", count);
      preview_images(&sb, files, "", &shown, 3, 0);
      char mpath[PATH_MAX];
      wav_info_t wav;
      char why[160];
      unsigned secs = 0;
      snprintf(mpath, sizeof(mpath), "%s/%s/%s", THEMES_DIR, e->d_name, MUSIC_FILE);
      if(!access(mpath, F_OK) && music_wav_ok(mpath, &wav, why, sizeof(why))) {
        secs = (unsigned)((wav.data_size + MUSIC_RATE * 2) / (MUSIC_RATE * 4));
      }
      music_settings_t ms;
      read_music_settings(e->d_name, &ms);
      sb_printf(&sb, "],\"music\":%u,\"gain\":%d,\"musicStart\":%d,\"musicEnd\":%d,\"musicPlayFrom\":%d,"
                "\"musicLoopStart\":%d,\"musicLoopEnd\":%d,\"musicOrder\":\"%s\",\"musicLoop\":\"%s\",\"musicBlend\":%d",
                secs, ms.gain, ms.start_ms, ms.end_ms, ms.play_from_ms, ms.loop_start_ms, ms.loop_end_ms, ms.order,
                ms.loop, ms.blend_ms);
      /* Playlist: the order of source slots and each one's part, so the editor can rebuild the track list. */
      sb_printf(&sb, ",\"musicTracks\":%d,\"musicBetween\":\"%s\",\"musicEndMode\":\"%s\",\"musicSequence\":[",
                ms.tracks, ms.between, ms.end_mode);
      for(int i = 0; i < ms.tracks; i++) sb_printf(&sb, "%s%d", i ? "," : "", ms.seq[i]);
      SB_LIT(&sb, "],\"musicStarts\":[");
      for(int i = 0; i < ms.tracks; i++) { int s = ms.seq[i]; sb_printf(&sb, "%s%d", i ? "," : "", s == 1 ? ms.start_ms : ms.track_start_ms[s - 1]); }
      SB_LIT(&sb, "],\"musicEnds\":[");
      for(int i = 0; i < ms.tracks; i++) { int s = ms.seq[i]; sb_printf(&sb, "%s%d", i ? "," : "", s == 1 ? ms.end_ms : ms.track_end_ms[s - 1]); }
      SB_LIT(&sb, "]");
      char cover[PATH_MAX];
      long long cover_time;
      const char *cover_kind = shown_preview(e->d_name, cover, sizeof(cover), &cover_time);
      sb_printf(&sb, ",\"cover\":\"%s\",\"coverTime\":%lld,\"unsaved\":%s}", cover_kind, cover_time,
                theme_unsaved(e->d_name) ? "true" : "false");
    }
    closedir(d);
  }
  SB_LIT(&sb, "]}");
  respond_json(req->fd, 200, &sb);
}


static void
api_theme_files(request_t *req) {
  char id[128], root[PATH_MAX];
  strbuf_t sb = {0};
  int first = 1, count = 0;

  get_param(req->query, "id", id, sizeof(id));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");

  snprintf(root, sizeof(root), "%s/%s/files", THEMES_DIR, id);
  SB_LIT(&sb, "{\"files\":[");
  list_files(&sb, root, "", &first, &count, 0);
  SB_LIT(&sb, "]}");
  respond_json(req->fd, 200, &sb);
}


static int
stristr(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  for(; *hay; hay++) {
    if(!strncasecmp(hay, needle, n)) return 1;
  }
  return 0;
}


static int
kind_in_list(const char *list, const char *kind) {
  size_t n = strlen(kind);
  for(const char *p = list; *p; ) {
    size_t len = strcspn(p, ",");
    if(len == n && !strncmp(p, kind, n)) return 1;
    p += len;
    if(*p) p++;
  }
  return 0;
}


/* Which titles show the pictures of which concept folder
   (CONCEPTMETA_DIR/<concept>, shared by a game's editions): the console's
   app database names the folder in each title's record right after the
   title ID, as "...titleId=PPSA01968/user/catalog_downloader/conceptmeta/
   234585/icon0.png?ts=...". Read-only; the pairs found, in `out`. */
#define APP_DB        "/system_data/priv/mms/app.db"
#define MAX_CONCEPTS  256
#define CONCEPT_RUN   4096
#define CONCEPT_KEEP  512     /* of a longer run, read on with this much kept */
#define CONCEPT_BACK  256     /* how far before the folder's path its title ID may be */

typedef struct { char concept[16], title[10]; } concept_t;

static int
title_id_at(const char *p) {
  for(int i = 0; i < 9; i++) if(i < 4 ? p[i] < 'A' || p[i] > 'Z' : p[i] < '0' || p[i] > '9') return 0;
  return p[9] < '0' || p[9] > '9';
}

static int
cmp_concept(const void *a, const void *b) {
  const concept_t *x = a, *y = b;
  int c = strcmp(x->concept, y->concept);
  return c ? c : strcmp(x->title, y->title);
}

static int
concepts_in_run(char *run, size_t n, concept_t *out, int count) {
  static const char key[] = CONCEPTMETA_DIR "/";
  run[n] = 0;
  for(char *at = run; count < MAX_CONCEPTS && (at = strstr(at, key)); at += sizeof(key) - 1) {
    const char *num = at + sizeof(key) - 1, *title = NULL;
    size_t digits = strspn(num, "0123456789");
    if(!digits || digits >= sizeof(out->concept) || num[digits] != '/') continue;
    for(const char *p = at - 9; p >= run && at - p <= CONCEPT_BACK && !title; p--) {
      if(title_id_at(p) && (p == run || p[-1] < 'A' || p[-1] > 'Z')) title = p;
    }
    if(!title) continue;
    concept_t c;
    snprintf(c.concept, sizeof(c.concept), "%.*s", (int)digits, num);
    snprintf(c.title, sizeof(c.title), "%.9s", title);
    int seen = 0;
    for(int k = 0; k < count && !seen; k++) seen = !cmp_concept(&out[k], &c);
    if(!seen) out[count++] = c;
  }
  return count;
}

static int
title_concepts(concept_t *out) {
  int fd = open(APP_DB, O_RDONLY), count = 0;
  uint8_t *buf = malloc(IO_CHUNK);
  char *run = malloc(CONCEPT_RUN + 1);
  size_t n = 0;
  ssize_t r;
  while(fd >= 0 && buf && run && (r = read(fd, buf, IO_CHUNK)) > 0) {
    for(ssize_t i = 0; i < r; i++) {
      if(buf[i] >= 0x20 && buf[i] < 0x7f) {
        run[n++] = (char)buf[i];
        if(n < CONCEPT_RUN) continue;
        count = concepts_in_run(run, n, out, count);
        memmove(run, run + n - CONCEPT_KEEP, CONCEPT_KEEP);
        n = CONCEPT_KEEP;
        continue;
      }
      count = concepts_in_run(run, n, out, count);
      n = 0;
    }
  }
  if(run) count = concepts_in_run(run, n, out, count);
  if(fd >= 0) close(fd);
  free(buf);
  free(run);
  qsort(out, count, sizeof(*out), cmp_concept);
  return count;
}


static void
api_assets(request_t *req) {
  char q[128], listing[PATH_MAX], line[PATH_MAX + 32];
  strbuf_t sb = {0};
  char kinds[64], lim[16];
  int first = 1, count = 0, truncated = 0, protected = 0;

  get_param(req->query, "q", q, sizeof(q));
  get_param(req->query, "kind", kinds, sizeof(kinds));   /* e.g. "image,svg" */
  get_param(req->query, "limit", lim, sizeof(lim));
  int limit = lim[0] ? atoi(lim) : MAX_RESULTS;
  if(limit < 1) limit = 1;
  if(limit > MAX_RESULTS_ALL) limit = MAX_RESULTS_ALL;
  asset_listing_path(listing, sizeof(listing));

  FILE *f = fopen(listing, "r");
  if(!f) return respond_error(req->fd, 404, "asset listing not created yet");

  SB_LIT(&sb, "{\"assets\":[");
  while(fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = 0;
    char *path = strchr(line, '/');
    if(!path || (q[0] && !stristr(path, q))) continue;
    int kind = asset_kind(path);
    if(kind == KIND_BLOCKED) { protected++; continue; }
    if(kinds[0] && !kind_in_list(kinds, asset_kind_name(kind))) continue;
    if(count >= limit) { truncated = 1; break; }
    if(!first) SB_LIT(&sb, ",");
    first = 0;
    SB_LIT(&sb, "{\"path\":");
    sb_json_str(&sb, path);
    sb_printf(&sb, ",\"size\":%lld,\"kind\":\"%s\"}", atoll(line), asset_kind_name(kind));
    count++;
  }
  fclose(f);

  /* Installed titles come and go, so their pictures are listed live: each
     title's metadata folder, its sce_sys folder under /user/app, and the
     pictures downloaded for it and for its game (concept). */
  static const char *const title_roots[][2] = {{APPMETA_DIR, ""}, {APP_DIR, "/sce_sys"},
                                               {CATALOG_APPMETA_DIR, ""}, {CONCEPTMETA_DIR, ""}};
  for(size_t r = 0; r < sizeof(title_roots) / sizeof(*title_roots) && !truncated; r++) {
    DIR *titles = opendir(title_roots[r][0]);
    struct dirent *t;
    while(titles && !truncated && (t = readdir(titles))) {
      char dir[PATH_MAX];
      if(t->d_name[0] == '.' || snprintf(dir, sizeof(dir), "%s/%s%s", title_roots[r][0], t->d_name, title_roots[r][1]) >= (int)sizeof(dir)) continue;
      DIR *d = opendir(dir);
      struct dirent *e;
      while(d && (e = readdir(d))) {
        char path[PATH_MAX];
        struct stat st;
        if(e->d_name[0] == '.' || snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)) continue;
        if(lstat(path, &st) || !S_ISREG(st.st_mode) || (q[0] && !stristr(path, q))) continue;
        int kind = asset_kind(path);
        if(kind == KIND_BLOCKED) { protected++; continue; }
        if(kinds[0] && !kind_in_list(kinds, asset_kind_name(kind))) continue;
        if(count >= limit) { truncated = 1; break; }
        if(!first) SB_LIT(&sb, ",");
        first = 0;
        SB_LIT(&sb, "{\"path\":");
        sb_json_str(&sb, path);
        sb_printf(&sb, ",\"size\":%lld,\"kind\":\"%s\"}", (long long)st.st_size, asset_kind_name(kind));
        count++;
      }
      if(d) closedir(d);
    }
    if(titles) closedir(titles);
  }
  /* Two flat folders are listed live: the console's text fonts, and the
     Appearance themes' textures and Welcome backgrounds in /system/vsh_asset. */
  static const char *const flat_dirs[] = {FONT_DIR, SYS_ASSET_DIR};
  for(size_t r = 0; r < sizeof(flat_dirs) / sizeof(*flat_dirs) && !truncated; r++) {
    DIR *flat = opendir(flat_dirs[r]);
    struct dirent *fe;
    while(flat && (fe = readdir(flat))) {
      char path[PATH_MAX];
      struct stat st;
      if(fe->d_name[0] == '.' || snprintf(path, sizeof(path), "%s/%s", flat_dirs[r], fe->d_name) >= (int)sizeof(path)) continue;
      if(lstat(path, &st) || !S_ISREG(st.st_mode) || (q[0] && !stristr(path, q))) continue;
      int kind = asset_kind(path);
      if(kind == KIND_BLOCKED) { protected++; continue; }
      if(kinds[0] && !kind_in_list(kinds, asset_kind_name(kind))) continue;
      if(count >= limit) { truncated = 1; break; }
      if(!first) SB_LIT(&sb, ",");
      first = 0;
      SB_LIT(&sb, "{\"path\":");
      sb_json_str(&sb, path);
      sb_printf(&sb, ",\"size\":%lld,\"kind\":\"%s\"}", (long long)st.st_size, asset_kind_name(kind));
      count++;
    }
    if(flat) closedir(flat);
  }
  sb_printf(&sb, "],\"truncated\":%s,\"protected\":%d,\"concepts\":{", truncated ? "true" : "false", protected);
  concept_t *concepts = malloc(sizeof(*concepts) * MAX_CONCEPTS);
  int nc = concepts ? title_concepts(concepts) : 0;
  for(int k = 0; k < nc; k++) {
    if(k && !strcmp(concepts[k].concept, concepts[k - 1].concept)) {
      sb_printf(&sb, ",\"%s\"", concepts[k].title);
      continue;
    }
    sb_printf(&sb, "%s\"%s\":[\"%s\"", k ? "]," : "", concepts[k].concept, concepts[k].title);
  }
  free(concepts);
  sb_printf(&sb, "%s}}", nc ? "]" : "");
  respond_json(req->fd, 200, &sb);
}


static void
api_create_theme(request_t *req) {
  char id[128], name[128], fw[64], dir[PATH_MAX], path[PATH_MAX], ini[256];

  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "name", name, sizeof(name));
  get_param(req->query, "fw", fw, sizeof(fw));
  if(!theme_name_valid(id)) return respond_error(req->fd, 400, "id: use letters, digits, - and _");
  if(!name[0]) snprintf(name, sizeof(name), "%s", id);
  if(!fw[0]) fw_string(fw, sizeof(fw));
  for(char *p = name; *p; p++) if(*p == '\n' || *p == '\r' || *p == '=') *p = ' ';
  for(char *p = fw; *p; p++) if(*p == '\n' || *p == '\r' || *p == '=') *p = ' ';

  themes_lock();
  snprintf(dir, sizeof(dir), "%s/%s", THEMES_DIR, id);
  if(fs_is_dir(dir)) {
    themes_unlock();
    return respond_error(req->fd, 409, "a theme with that id exists");
  }
  drop_snapshot(id);   /* left by a theme of the same id */
  snprintf(path, sizeof(path), "%s/files", dir);
  int n = snprintf(ini, sizeof(ini), "name=%s\nfw=%s\nmusic_gain=0\n", name, fw);
  int ok = !fs_mkdirs(path);
  snprintf(path, sizeof(path), "%s/theme.ini", dir);
  ok = ok && !fs_write_file(path, ini, n);
  themes_unlock();

  if(!ok) return respond_error(req->fd, 500, "could not create the theme folder");
  log_msg("created theme %s (%s, fw %s)", id, name, fw);
  respond_ok(req->fd);
}


static void
api_delete_theme(request_t *req) {
  char id[128], active[128], dir[PATH_MAX];

  get_param(req->query, "id", id, sizeof(id));
  themes_lock();
  if(!theme_exists(id)) {
    themes_unlock();
    return respond_error(req->fd, 404, "no such theme");
  }
  read_active(active, sizeof(active));
  if(!strcmp(active, id)) {
    themes_unlock();
    return respond_error(req->fd, 409, "switch to another theme before deleting this one");
  }
  snprintf(dir, sizeof(dir), "%s/%s", THEMES_DIR, id);
  int rc = fs_rm_rf_under(THEMES_DIR, dir);
  if(!rc) drop_snapshot(id);
  themes_unlock();

  if(rc) return respond_error(req->fd, 500, "delete failed");
  log_msg("deleted theme %s", id);
  respond_ok(req->fd);
}


/* Which of a title's picture and sound files, the console's and the theme's
   copies of them, any process opens or reads in the next few minutes
   (read-only). For finding where the home screen gets a title's icon: start
   it, then rest mode and wake the console. */
#define WATCH_FILES 96

typedef struct {
  FILE *f;
  char (*paths)[PATH_MAX];
  int last_i, repeat;
  char last_what[8];
  long last_ms;
} watch_out_t;

static void
watch_seen(int i, const char *what, long ms, void *ctx) {
  watch_out_t *w = ctx;
  if(i == w->last_i && !strcmp(what, w->last_what) && ms - w->last_ms < 1000) {
    w->repeat++;
    return;
  }
  if(w->repeat) fprintf(w->f, "           (%d more)\n", w->repeat);
  w->repeat = 0;
  fprintf(w->f, "+%3ld.%03ld s  %-5s %s\n", ms / 1000, ms % 1000, what, w->paths[i]);
  w->last_i = i;
  snprintf(w->last_what, sizeof(w->last_what), "%s", what);
  w->last_ms = ms;
}

static int
watch_add_folder(char (*paths)[PATH_MAX], int n, const char *dir) {
  DIR *d = opendir(dir);
  struct dirent *e;
  while(d && n < WATCH_FILES && (e = readdir(d))) {
    const char *dot = strrchr(e->d_name, '.');
    struct stat st;
    if(e->d_name[0] == '.' || !dot || (strcasecmp(dot, ".png") && strcasecmp(dot, ".dds") && strcasecmp(dot, ".at9"))) continue;
    if(snprintf(paths[n], PATH_MAX, "%s/%s", dir, e->d_name) >= PATH_MAX || stat(paths[n], &st) || !S_ISREG(st.st_mode)) continue;
    n++;
  }
  if(d) closedir(d);
  return n;
}

static void
api_watch(request_t *req) {
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  char id[16], secs[16];
  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "seconds", secs, sizeof(secs));
  int ok = strlen(id) == 9, seconds = atoi(secs);
  for(int i = 0; ok && i < 9; i++) ok = i < 4 ? id[i] >= 'A' && id[i] <= 'Z' : id[i] >= '0' && id[i] <= '9';
  if(!ok) return respond_error(req->fd, 400, "id must be a title ID such as PPSA01234");
  if(seconds <= 0) seconds = 120;
  if(seconds > 300) seconds = 300;
  if(pthread_mutex_trylock(&lock)) return respond_error(req->fd, 409, "a watch is already running");

  static char paths[WATCH_FILES][PATH_MAX];
  char folders[8][PATH_MAX];
  int n = 0, nf = 3;
  snprintf(folders[0], PATH_MAX, "%s/%s", APPMETA_DIR, id);
  snprintf(folders[1], PATH_MAX, "%s/%s/sce_sys", APP_DIR, id);
  snprintf(folders[2], PATH_MAX, "%s/%s", CATALOG_APPMETA_DIR, id);
  concept_t *concepts = malloc(sizeof(*concepts) * MAX_CONCEPTS);
  int nc = concepts ? title_concepts(concepts) : 0;
  for(int k = 0; k < nc && nf < 8; k++) {
    if(!strcmp(concepts[k].title, id)) snprintf(folders[nf++], PATH_MAX, "%s/%s", CONCEPTMETA_DIR, concepts[k].concept);
  }
  free(concepts);
  for(int k = 0; k < nf; k++) n = watch_add_folder(paths, n, folders[k]);
  /* The theme's staged copies of those folders, as mounted. */
  FILE *live = fopen(LIVE_FILE, "r");
  char line[PATH_MAX * 2 + 64];
  while(live && fgets(line, sizeof(line), live)) {
    line[strcspn(line, "\r\n")] = 0;
    char *state = strchr(line, '\t'), *dir = state ? strchr(state + 1, '\t') : NULL, *real = dir ? strchr(dir + 1, '\t') : NULL;
    if(strncmp(line, "stage", 5) || !real) continue;
    *dir++ = 0;
    *real++ = 0;
    char *end = strchr(real, '\t');
    if(end) *end = 0;
    for(int k = 0; k < nf; k++) if(!strcmp(real, folders[k])) n = watch_add_folder(paths, n, dir);
  }
  if(live) fclose(live);

  const char *path = DUMP_DIR "/watch.txt", *tmp = DUMP_DIR "/watch.txt.tmp";
  int fd = fs_open_write(tmp);
  FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
  if(!f) {
    if(fd >= 0) close(fd);
    pthread_mutex_unlock(&lock);
    return respond_error(req->fd, 500, "could not write the report");
  }
  fprintf(f, "# watching %d file(s) of %s for %d s\n", n, id, seconds);
  for(int i = 0; i < n; i++) fprintf(f, "#   %s\n", paths[i]);
  fflush(f);
  log_msg("watching %d picture file(s) of %s for %d s", n, id, seconds);
  watch_out_t w = {f, paths, -1, 0, "", 0};
  const char *list[WATCH_FILES];
  for(int i = 0; i < n; i++) list[i] = paths[i];
  if(plat_watch_files(list, n, seconds, watch_seen, &w)) fprintf(f, "# files can't be watched here\n");
  if(w.repeat) fprintf(f, "           (%d more)\n", w.repeat);
  fprintf(f, "# done\n");
  fclose(f);
  ok = !fs_rename(tmp, path);
  pthread_mutex_unlock(&lock);
  if(ok) respond_file(req, path);
  else respond_error(req->fd, 500, "could not write the report");
}


/* Every process's pid and name (read-only), to find the ones to watch. */
static void
api_procs(request_t *req) {
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  const char *path = DUMP_DIR "/procs.txt", *tmp = DUMP_DIR "/procs.txt.tmp";
  pthread_mutex_lock(&lock);
  int fd = fs_open_write(tmp);
  FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
  if(f) {
    plat_dump_processes(f);
    fclose(f);
  } else if(fd >= 0) {
    close(fd);
  }
  int ok = f && !fs_rename(tmp, path);
  if(ok) respond_file(req, path);
  pthread_mutex_unlock(&lock);
  if(!ok) respond_error(req->fd, 500, "could not list the processes");
}


static void
api_apply(request_t *req) {
  char id[128];

  get_param(req->query, "id", id, sizeof(id));
  if(strcmp(id, "none") && !theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(write_active(id)) return respond_error(req->fd, 500, "could not save the selection");
  log_msg("selection changed to %s", id);
  respond_ok(req->fd);
}


static int
theme_file_path(const char *id, const char *asset, char *out, size_t size) {
  return snprintf(out, size, "%s/%s/files%s", THEMES_DIR, id, asset) >= (int)size ? -1 : 0;
}


/* The temporary file an upload is received into: its own for each request, so two uploads of the same file at
   once never write into each other. It ends in ".part", which listings and checks skip. */
static atomic_uint upload_seq;

static void
part_name(char *out, size_t size, const char *dst) {
  snprintf(out, size, "%s.%u.part", dst, atomic_fetch_add(&upload_seq, 1));
}


/* Stream the request body into `part` (no lock held while the network is
   slow), checking it against the browser's checksum. 0 on success; on failure
   the error has been sent and `part` removed. */
static int
receive_upload(request_t *req, const char *part, uint32_t *crc_out, char crc_hex[9]) {
  char want_crc[16];
  int out = fs_open_write(part);
  if(out < 0) {
    respond_error(req->fd, 500, "could not open file");
    return -1;
  }

  long remaining = req->content_length;
  size_t first = MIN(req->body_len, (size_t)remaining);
  uint32_t crc = crc32_update(0, req->body, first);
  int ok = send_all(out, req->body, first) == 0;
  remaining -= first;
  while(ok && remaining > 0) {
    ssize_t n = read(req->fd, req->io, MIN((size_t)IO_CHUNK, (size_t)remaining));
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) { ok = 0; break; }
    crc = crc32_update(crc, req->io, n);
    ok = send_all(out, req->io, n) == 0;
    remaining -= n;
  }
  if(fsync(out)) ok = 0;
  if(close(out)) ok = 0;
  if(!ok) {
    fs_unlink(part);
    respond_error(req->fd, 400, "upload was interrupted");
    return -1;
  }

  /* Verification 1: the bytes are what the browser sent. */
  snprintf(crc_hex, 9, "%08x", crc);
  get_param(req->query, "crc", want_crc, sizeof(want_crc));
  if(want_crc[0] && strcasecmp(want_crc, crc_hex)) {
    fs_unlink(part);
    respond_error(req->fd, 422, "upload was corrupted in transfer (checksum mismatch)");
    return -1;
  }
  *crc_out = crc;
  return 0;
}


/* Verification 3: atomic move under the theme lock, then read back. */
/* ---- unsaved changes ----
   The first change to a theme keeps a snapshot of it under SNAP_DIR, made of
   hard links (theme files are only ever replaced, never changed in place, so
   the snapshot stays as it was). Save drops the snapshot; Discard puts it
   back. Call with themes_lock held. */
static void
snapshot_path(const char *id, char *out, size_t size) {
  snprintf(out, size, "%s/%s", SNAP_DIR, id);
}


static int
theme_unsaved(const char *id) {
  char path[PATH_MAX];
  snapshot_path(id, path, sizeof(path));
  return fs_is_dir(path);
}


static int
keep_snapshot(const char *id) {
  char src[PATH_MAX], dst[PATH_MAX], part[PATH_MAX + 8];
  snapshot_path(id, dst, sizeof(dst));
  if(fs_is_dir(dst)) return 0;
  snprintf(src, sizeof(src), "%s/%s", THEMES_DIR, id);
  snprintf(part, sizeof(part), "%s.part", dst);
  fs_mkdirs(SNAP_DIR);
  fs_rm_rf_under(SNAP_DIR, part);
  if(fs_link_tree(src, part) || fs_rename(part, dst)) {
    fs_rm_rf_under(SNAP_DIR, part);
    log_err("theme %s: could not keep a copy to undo changes", id);
    return -1;
  }
  return 0;
}


static void
drop_snapshot(const char *id) {
  char path[PATH_MAX];
  snapshot_path(id, path, sizeof(path));
  if(fs_is_dir(path)) fs_rm_rf_under(SNAP_DIR, path);
}


/* `keep`: the upload is a change Discard can undo (not a picture the
   library made by itself). `length` is what the file must measure: what was
   received, or its length after the check wrote it again (a texture's frame). */
static int
place_upload(request_t *req, const char *id, const char *part, const char *dst, uint32_t crc,
             uint64_t *size, int keep, uint64_t length) {
  uint32_t stored = 0;
  themes_lock();
  int moved = theme_exists(id) && (!keep || !keep_snapshot(id)) && !fs_rename(part, dst);
  int placed = moved && !fs_crc32_file(dst, &stored, size) && stored == crc && *size == length;
  if(moved && !placed) fs_unlink(dst);   /* it didn't read back as sent: never leave it in the theme */
  themes_unlock();
  if(!placed) {
    fs_unlink(part);
    respond_error(req->fd, 500, "could not store the file");
    return -1;
  }
  return 0;
}


/* Receive, verify, then atomically place one replacement file. */
static void
api_put_file(request_t *req) {
  char id[128], asset[512], dst[PATH_MAX], part[PATH_MAX + 24], dir[PATH_MAX], msg[256], crc_hex[9];
  uint32_t crc;
  uint64_t size = 0;

  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "path", asset, sizeof(asset));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(!asset_path_valid(asset)) return respond_error(req->fd, 403, "not a replaceable system asset");
  int kind = asset_kind(asset);
  if(kind == KIND_BLOCKED) return respond_error(req->fd, 403, "this file type is protected and cannot be themed");
  if(req->content_length <= 0) return respond_error(req->fd, 400, "empty upload");
  if((uint64_t)req->content_length > asset_size_limit(kind)) {
    snprintf(msg, sizeof(msg), "file is larger than the %llu MB limit for %s files",
             (unsigned long long)(asset_size_limit(kind) >> 20), asset_kind_name(kind));
    return respond_error(req->fd, 413, msg);
  }
  if(theme_file_path(id, asset, dst, sizeof(dst))) return respond_error(req->fd, 400, "path too long");

  snprintf(dir, sizeof(dir), "%s", dst);
  *strrchr(dir, '/') = 0;
  part_name(part, sizeof(part), dst);
  if(fs_mkdirs(dir)) return respond_error(req->fd, 500, "could not create folder");
  if(receive_upload(req, part, &crc, crc_hex)) return;

  /* Verification 2: the asset policy against the stock file (images: same
     format and exact pixel size, so every output resolution stays correct). */
  char stock[PATH_MAX];
  stock_path(asset, stock, sizeof(stock));
  if(asset_validate(part, stock, msg, sizeof(msg))) {
    fs_unlink(part);
    return respond_error(req->fd, 422, msg);
  }
  /* A texture whose zstd frame was written again with the system file's header
     is a different file from the one received: its checksum and length are the new ones. */
  uint64_t length = (uint64_t)req->content_length;
  if(kind == KIND_TEXTURE) {
    uint32_t now = 0;
    if(fs_crc32_file(part, &now, &length)) {
      fs_unlink(part);
      return respond_error(req->fd, 500, "could not store the file");
    }
    crc = now;
    snprintf(crc_hex, sizeof(crc_hex), "%08x", crc);
  }
  if(place_upload(req, id, part, dst, crc, &size, 1, length)) return;

  strbuf_t sb = {0};
  sb_printf(&sb, "{\"ok\":true,\"kind\":\"%s\",\"size\":%llu,\"crc32\":\"%s\"",
            asset_kind_name(kind), (unsigned long long)size, crc_hex);
  image_info_t got;
  if(kind == KIND_IMAGE && !image_probe(dst, &got)) {
    sb_printf(&sb, ",\"format\":\"%s\",\"width\":%u,\"height\":%u",
              image_format_name(got.format), got.width, got.height);
  }
  SB_LIT(&sb, "}");
  log_msg("theme %s: replaced %s (%llu bytes, crc %s, verified)", id, asset,
          (unsigned long long)size, crc_hex);
  respond_json(req->fd, 200, &sb);
}


static int int_param(const char *query, const char *key, int lo, int hi, int *out, int *present);

static void
music_path(const char *id, int slot, char *out, size_t size) {
  char file[24];
  music_source_file(slot, file, sizeof(file));
  snprintf(out, size, "%s/%s/%s", THEMES_DIR, id, file);
}


static void
api_put_music(request_t *req) {
  char id[128], dst[PATH_MAX], part[PATH_MAX + 24], msg[200], crc_hex[9];
  uint32_t crc;
  uint64_t size = 0;
  wav_info_t wav;
  int slot = 1, has_slot = 0;

  get_param(req->query, "id", id, sizeof(id));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(int_param(req->query, "slot", 1, MUSIC_MAX_TRACKS, &slot, &has_slot)) return respond_error(req->fd, 400, "slot must be 1 to 8");
  if(req->content_length <= 0) return respond_error(req->fd, 400, "empty upload");
  if((uint64_t)req->content_length > MUSIC_MAX_BYTES) {
    return respond_error(req->fd, 413, "music is larger than 128 MB (about 11 minutes)");
  }
  music_path(id, slot, dst, sizeof(dst));
  part_name(part, sizeof(part), dst);
  if(receive_upload(req, part, &crc, crc_hex)) return;
  if(!music_wav_ok(part, &wav, msg, sizeof(msg))) {
    fs_unlink(part);
    return respond_error(req->fd, 422, msg);
  }
  if(place_upload(req, id, part, dst, crc, &size, 1, (uint64_t)req->content_length)) return;

  unsigned secs = (unsigned)((wav.data_size + MUSIC_RATE * 2) / (MUSIC_RATE * 4));
  strbuf_t sb = {0};
  sb_printf(&sb, "{\"ok\":true,\"seconds\":%u,\"size\":%llu,\"crc32\":\"%s\"}",
            secs, (unsigned long long)size, crc_hex);
  log_msg("theme %s: music set (slot %d, %u s, crc %s, verified)", id, slot, secs, crc_hex);
  respond_json(req->fd, 200, &sb);
}


static void
api_delete_music(request_t *req) {
  char id[128], path[PATH_MAX];
  int slot = 1, has_slot = 0;
  get_param(req->query, "id", id, sizeof(id));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(int_param(req->query, "slot", 1, MUSIC_MAX_TRACKS, &slot, &has_slot)) return respond_error(req->fd, 400, "slot must be 1 to 8");
  music_path(id, slot, path, sizeof(path));
  themes_lock();
  int rc = access(path, F_OK) || keep_snapshot(id) || fs_unlink(path);
  themes_unlock();
  if(rc) return respond_error(req->fd, 404, "this theme has no music in that slot");
  respond_ok(req->fd);
}


static void
api_get_music(request_t *req) {
  char id[128], path[PATH_MAX];
  int slot = 1, has_slot = 0;
  get_param(req->query, "id", id, sizeof(id));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(int_param(req->query, "slot", 1, MUSIC_MAX_TRACKS, &slot, &has_slot)) return respond_error(req->fd, 400, "slot must be 1 to 8");
  music_path(id, slot, path, sizeof(path));
  respond_file(req, path);
}


/* A theme's preview picture for the library: made from its background
   (preview.jpg) or chosen by the user (picture.jpg, shown first). Neither is
   ever applied to the console. */
#define PREVIEW_MAX_BYTES (4u << 20)
#define PREVIEW_MAX_SIDE  4096

static int
preview_path(const char *id, const char *kind, char *out, size_t size) {
  const char *f = !strcmp(kind, "background") ? "preview.jpg" : !strcmp(kind, "picture") ? "picture.jpg" : NULL;
  if(!f) return -1;
  snprintf(out, size, "%s/%s/%s", THEMES_DIR, id, f);
  return 0;
}


/* The picture the library shows for a theme: "picture", "background" or "". */
static const char *
shown_preview(const char *id, char *path, size_t size, long long *mtime) {
  static const char *kinds[] = {"picture", "background"};
  struct stat st;
  for(size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
    preview_path(id, kinds[i], path, size);
    if(!stat(path, &st) && S_ISREG(st.st_mode)) {
      *mtime = (long long)st.st_mtime;
      return kinds[i];
    }
  }
  *mtime = 0;
  return "";
}


/* A complete JPEG: a header our parser accepts, a sane size, and the end marker. */
static int
preview_ok(const char *path, char *why, size_t len) {
  image_info_t info;
  uint8_t tail[2] = {0};
  struct stat st;
  int fd = open(path, O_RDONLY);
  int ended = fd >= 0 && !fstat(fd, &st) && st.st_size >= 4 &&
              pread(fd, tail, 2, st.st_size - 2) == 2 && tail[0] == 0xff && tail[1] == 0xd9;
  if(fd >= 0) close(fd);
  if(image_probe(path, &info) || info.format != IMG_JPEG || !ended) {
    snprintf(why, len, "the preview picture must be a complete JPEG");
    return 0;
  }
  if(info.width < 16 || info.height < 16 || info.width > PREVIEW_MAX_SIDE || info.height > PREVIEW_MAX_SIDE) {
    snprintf(why, len, "the preview picture is %ux%u; it must be 16 to %u pixels a side",
             info.width, info.height, PREVIEW_MAX_SIDE);
    return 0;
  }
  return 1;
}


static void
api_put_preview(request_t *req) {
  char id[128], kind[16], dst[PATH_MAX], part[PATH_MAX + 24], msg[160], crc_hex[9];
  uint32_t crc;
  uint64_t size = 0;

  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "kind", kind, sizeof(kind));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(preview_path(id, kind, dst, sizeof(dst))) return respond_error(req->fd, 400, "kind: background or picture");
  if(req->content_length <= 0) return respond_error(req->fd, 400, "empty upload");
  if((uint64_t)req->content_length > PREVIEW_MAX_BYTES) {
    return respond_error(req->fd, 413, "the preview picture is larger than 4 MB");
  }
  part_name(part, sizeof(part), dst);
  if(receive_upload(req, part, &crc, crc_hex)) return;
  if(!preview_ok(part, msg, sizeof(msg))) {
    fs_unlink(part);
    return respond_error(req->fd, 422, msg);
  }
  if(place_upload(req, id, part, dst, crc, &size, !strcmp(kind, "picture"), (uint64_t)req->content_length)) return;

  strbuf_t sb = {0};
  sb_printf(&sb, "{\"ok\":true,\"size\":%llu,\"crc32\":\"%s\"}", (unsigned long long)size, crc_hex);
  log_msg("theme %s: %s preview set (%llu bytes, crc %s, verified)", id, kind, (unsigned long long)size, crc_hex);
  respond_json(req->fd, 200, &sb);
}


static void
api_delete_preview(request_t *req) {
  char id[128], kind[16], path[PATH_MAX];
  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "kind", kind, sizeof(kind));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(preview_path(id, kind, path, sizeof(path))) return respond_error(req->fd, 400, "kind: background or picture");
  themes_lock();
  int rc = access(path, F_OK) || (!strcmp(kind, "picture") && keep_snapshot(id)) || fs_unlink(path);
  themes_unlock();
  if(rc) return respond_error(req->fd, 404, "no such preview picture");
  respond_ok(req->fd);
}


/* The shown picture, or the one named by `kind`. */
/* A newer copy of the plugin asks this one to step aside. Only a process on
   the console itself may ask. */
static void
api_handoff(request_t *req) {
  struct sockaddr_in peer;
  socklen_t len = sizeof(peer);
  if(getpeername(req->fd, (struct sockaddr *)&peer, &len) || peer.sin_family != AF_INET ||
     ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK) {
    return respond_error(req->fd, 403, "only the console itself can ask for this");
  }
  char pid[16] = "";
  get_param(req->query, "pid", pid, sizeof(pid));
  log_msg("a newer copy of the plugin asked to take over");
  respond_ok(req->fd);
  request_handoff(atoi(pid));
}


/* The live mount table, for diagnosing what the system shows where (read-only). */
static void
api_mounts(request_t *req) {
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  const char *path = DUMP_DIR "/mounts-now.txt", *tmp = DUMP_DIR "/mounts-now.txt.tmp";
  pthread_mutex_lock(&lock);
  int fd = fs_open_write(tmp);
  FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
  if(f) {
    plat_dump_mounts(f);
    fclose(f);
  } else if(fd >= 0) {
    close(fd);
  }
  int ok = f && !fs_rename(tmp, path);
  if(ok) respond_file(req, path);
  pthread_mutex_unlock(&lock);
  if(!ok) respond_error(req->fd, 500, "could not read the mount table");
}


/* Where the system keeps a title's pictures, for finding what the home
   screen draws (read-only, names and sizes only): the files under the
   folders ShellUI can see whose path names the title, and the strings in
   the system's databases that name it. On 13.42, PS4 games' tiles followed
   /user/appmeta/<title>/icon0.png and PS5 games' didn't. */

#define WHERE_LINES   3000
#define WHERE_VISITS  200000
#define WHERE_STRINGS 400
#define WHERE_RUN     8192    /* longer runs are cut; around the title ID is shown */

typedef struct {
  FILE *f;
  const char *id;
  int lines, visits, strings;
  uint32_t seen[WHERE_STRINGS];
  uint8_t buf[1 << 16];
} where_t;

static void
where_walk(where_t *w, const char *dir, int depth, int named) {
  DIR *d = opendir(dir);
  if(!d) {
    if(!named) fprintf(w->f, "# %s: %s\n", dir, strerror(errno));
    return;
  }
  struct dirent *e;
  while((e = readdir(d)) && w->lines < WHERE_LINES && w->visits < WHERE_VISITS) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    w->visits++;
    char path[PATH_MAX];
    struct stat st;
    if(snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path) || lstat(path, &st)) continue;
    int here = named || strstr(e->d_name, w->id);
    if(S_ISDIR(st.st_mode)) {
      /* A folder named after the title is listed two levels down. */
      int next = here && !named ? 2 : depth - 1;
      if(next >= 0) where_walk(w, path, next, here);
      else if(here) { fprintf(w->f, "%s/ (not listed)\n", path); w->lines++; }
    } else if(here) {
      fprintf(w->f, "%s %lld %lld%s\n", path, (long long)st.st_size, (long long)st.st_mtime, S_ISLNK(st.st_mode) ? " symlink" : "");
      w->lines++;
    }
  }
  closedir(d);
}

/* A printable run from a database: kept if it names the title, once, as
   up to 300 characters either side of the first mention. */
static void
where_run(where_t *w, const char *path, char *run, size_t n) {
  if(n < strlen(w->id) || w->strings >= WHERE_STRINGS) return;
  run[n] = 0;
  char *at = strstr(run, w->id);
  if(!at) return;
  uint32_t h = crc32_update(0, run, n);
  for(int k = 0; k < w->strings; k++) if(w->seen[k] == h) return;
  w->seen[w->strings++] = h;
  char *from = at - run > 300 ? at - 300 : run;
  int len = (int)(run + n - from) > 600 + (int)strlen(w->id) ? 600 + (int)strlen(w->id) : (int)(run + n - from);
  fprintf(w->f, "%s: %s%.*s%s\n", path, from > run ? "..." : "", len, from, from + len < run + n ? "..." : "");
}

static void
where_strings(where_t *w, const char *path) {
  int fd = open(path, O_RDONLY);
  if(fd < 0) { fprintf(w->f, "# %s: %s\n", path, strerror(errno)); return; }
  char run[WHERE_RUN + 1];
  size_t n = 0;
  ssize_t r;
  while(w->strings < WHERE_STRINGS && (r = read(fd, w->buf, sizeof(w->buf))) > 0) {
    for(ssize_t i = 0; i < r; i++) {
      if(w->buf[i] >= 0x20 && w->buf[i] < 0x7f) {
        if(n < WHERE_RUN) run[n++] = (char)w->buf[i];
        continue;
      }
      where_run(w, path, run, n);
      n = 0;
    }
  }
  where_run(w, path, run, n);
  close(fd);
}

static void
where_databases(where_t *w, const char *dir, int depth) {
  DIR *d = opendir(dir);
  if(!d) { fprintf(w->f, "# %s: %s\n", dir, strerror(errno)); return; }
  struct dirent *e;
  while((e = readdir(d)) && w->strings < WHERE_STRINGS) {
    char path[PATH_MAX];
    struct stat st;
    if(e->d_name[0] == '.' || snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path) || lstat(path, &st)) continue;
    if(S_ISDIR(st.st_mode) && depth > 0) where_databases(w, path, depth - 1);
    else if(S_ISREG(st.st_mode) && st.st_size <= (128 << 20) && (strstr(e->d_name, ".db") || strstr(e->d_name, ".json"))) where_strings(w, path);
  }
  closedir(d);
}

static void
api_title_where(request_t *req) {
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  static const char *roots[] = {
    "/system_data/priv/appmeta", "/system_data/priv/home", "/system_data/priv/cache", "/system_data/priv/game",
    "/system_data/priv/mms", "/system_data/nobackup/temp", "/user/appmeta", "/user/temp", "/user/av_contents",
    "/user/bgft", "/user/app", "/user/patch", "/user/catalog_downloader", "/user/download", "/user/home",
  };
  char id[16];
  get_param(req->query, "id", id, sizeof(id));
  int ok = strlen(id) == 9;
  for(int i = 0; ok && i < 9; i++) ok = i < 4 ? id[i] >= 'A' && id[i] <= 'Z' : id[i] >= '0' && id[i] <= '9';
  if(!ok) return respond_error(req->fd, 400, "id must be a title ID such as PPSA01234");

  const char *path = DUMP_DIR "/where.txt", *tmp = DUMP_DIR "/where.txt.tmp";
  pthread_mutex_lock(&lock);
  int fd = fs_open_write(tmp);
  FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
  if(f) {
    where_t *w = calloc(1, sizeof(*w));
    if(w) {
      w->f = f;
      w->id = id;
      fprintf(f, "# files whose path names %s\n", id);
      for(size_t i = 0; i < sizeof(roots) / sizeof(*roots); i++) where_walk(w, roots[i], 4, 0);
      if(w->lines >= WHERE_LINES || w->visits >= WHERE_VISITS) fprintf(f, "# stopped after %d file(s), %d visited\n", w->lines, w->visits);
      fprintf(f, "# strings naming %s in the system's databases\n", id);
      where_databases(w, "/system_data/priv/mms", 1);
      where_databases(w, "/system_data/priv/home", 2);
      free(w);
    }
    fclose(f);
  } else if(fd >= 0) {
    close(fd);
  }
  ok = f && !fs_rename(tmp, path);
  if(ok) respond_file(req, path);
  pthread_mutex_unlock(&lock);
  if(!ok) respond_error(req->fd, 500, "could not write the listing");
}


static void
api_get_preview(request_t *req) {
  char id[128], kind[16], path[PATH_MAX];
  long long mtime;
  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "kind", kind, sizeof(kind));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(kind[0] ? preview_path(id, kind, path, sizeof(path)) : !shown_preview(id, path, sizeof(path), &mtime)[0]) {
    return respond_error(req->fd, 404, "no preview picture");
  }
  respond_file(req, path);
}


/* An optional integer parameter within [lo, hi]. 0 if absent or valid, -1 if
   present but invalid. */
static int
int_param(const char *query, const char *key, int lo, int hi, int *out, int *present) {
  char v[16], *end = NULL;
  *present = !get_param(query, key, v, sizeof(v)) && v[0];
  if(!*present) return 0;
  long n = strtol(v, &end, 10);
  if(*end || n < lo || n > hi) return -1;
  *out = (int)n;
  return 0;
}


/* Rename, change the firmware list, or change the music settings: gain (dB),
   start/end (ms, end 0 = to the end), play_from (ms, where the "music" order
   starts playing; -1 = the start), loop_start/loop_end (ms, the loop part;
   both -1 = none), order (music, loop-first, alternate, music-first), loop
   (crossfade, fade, cut), blend (ms). */
static void
api_theme_settings(request_t *req) {
  char id[128], name[128], fw[64], loop[16], order[16], between[16], end_mode[16], seq[128];
  int has_gain, has_start, has_end, has_blend, has_lstart, has_lend, has_from, has_keep, keep = 1;
  int has_tracks = 0, tracks_v = 0, has_playlist = 0;
  music_settings_t m;

  get_param(req->query, "id", id, sizeof(id));
  int has_name = !get_param(req->query, "name", name, sizeof(name)) && name[0];
  int has_fw = !get_param(req->query, "fw", fw, sizeof(fw)) && fw[0];
  int has_loop = !get_param(req->query, "loop", loop, sizeof(loop)) && loop[0];
  int has_order = !get_param(req->query, "order", order, sizeof(order)) && order[0];
  int has_between = !get_param(req->query, "between", between, sizeof(between)) && between[0];
  int has_endmode = !get_param(req->query, "end_mode", end_mode, sizeof(end_mode)) && end_mode[0];
  int has_seq = !get_param(req->query, "sequence", seq, sizeof(seq)) && seq[0];
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  for(char *p = name; *p; p++) if(*p == '\n' || *p == '\r' || *p == '=') *p = ' ';
  for(char *p = fw; *p; p++) if(*p == '\n' || *p == '\r' || *p == '=') *p = ' ';

  themes_lock();
  read_music_settings(id, &m);
  const char *bad = NULL;
  if(int_param(req->query, "gain", GAIN_MIN, GAIN_MAX, &m.gain, &has_gain)) bad = "gain must be -12 to 12 dB";
  else if(int_param(req->query, "start", 0, MUSIC_MAX_MS, &m.start_ms, &has_start)) bad = "start must be 0 to 3600000 ms";
  else if(int_param(req->query, "end", 0, MUSIC_MAX_MS, &m.end_ms, &has_end)) bad = "end must be 0 to 3600000 ms";
  else if(int_param(req->query, "blend", 0, BLEND_MAX_MS, &m.blend_ms, &has_blend)) bad = "blend must be 0 to 8000 ms";
  else if(int_param(req->query, "play_from", -1, MUSIC_MAX_MS, &m.play_from_ms, &has_from)) bad = "play_from must be -1 to 3600000 ms";
  else if(int_param(req->query, "loop_start", -1, MUSIC_MAX_MS, &m.loop_start_ms, &has_lstart)) bad = "loop_start must be -1 to 3600000 ms";
  else if(int_param(req->query, "loop_end", -1, MUSIC_MAX_MS, &m.loop_end_ms, &has_lend)) bad = "loop_end must be -1 to 3600000 ms";
  else if(int_param(req->query, "keep_originals", 0, 1, &keep, &has_keep)) bad = "keep_originals must be 0 or 1";
  else if(int_param(req->query, "tracks", 1, MUSIC_MAX_TRACKS, &tracks_v, &has_tracks)) bad = "tracks must be 1 to 8";
  else if(has_loop && !music_loop_valid(loop)) bad = "loop must be crossfade, fade or cut";
  else if(has_order && !music_order_valid(order)) bad = "order must be music, loop-first, alternate or music-first";
  else if(has_between && !music_loop_valid(between)) bad = "between must be crossfade, fade or cut";
  else if(has_endmode && !music_end_mode_valid(end_mode)) bad = "end_mode must be loop-all or loop-last";
  else if(m.end_ms && m.end_ms <= m.start_ms) bad = "the end must come after the start";
  else if((m.loop_start_ms < 0) != (m.loop_end_ms < 0)) bad = "loop_start and loop_end are set together";
  else if(m.loop_start_ms >= 0 && m.loop_end_ms && m.loop_end_ms <= m.loop_start_ms) bad = "the loop must end after it starts";
  else if(has_order && strcmp(order, "music") && m.loop_start_ms < 0) bad = "this order needs a loop part";
  else if(m.play_from_ms >= 0 && (m.play_from_ms < m.start_ms || (m.end_ms && m.play_from_ms >= m.end_ms))) {
    bad = "play_from must be within the music part";
  }
  /* Playlist: each added slot's part (start2/end2 … start8/end8), then the order. */
  for(int s = 2; s <= MUSIC_MAX_TRACKS && !bad; s++) {
    char ks[16], ke[16];
    int hs = 0, he = 0;
    snprintf(ks, sizeof(ks), "start%d", s);
    snprintf(ke, sizeof(ke), "end%d", s);
    if(int_param(req->query, ks, 0, MUSIC_MAX_MS, &m.track_start_ms[s - 1], &hs)) bad = "a track start is out of range";
    else if(int_param(req->query, ke, 0, MUSIC_MAX_MS, &m.track_end_ms[s - 1], &he)) bad = "a track end is out of range";
    else if(m.track_end_ms[s - 1] && m.track_end_ms[s - 1] <= m.track_start_ms[s - 1]) bad = "a track end must come after its start";
    if(hs || he) has_playlist = 1;
  }
  if(!bad && has_seq) {
    int t = 0;
    for(char *p = seq; *p && t < MUSIC_MAX_TRACKS; ) {
      while(*p == ',' || *p == ' ') p++;
      if(!*p) break;
      int nv = atoi(p);
      if(nv < 1 || nv > MUSIC_MAX_TRACKS) { bad = "sequence has an invalid track"; break; }
      m.seq[t++] = nv;
      while(*p && *p != ',') p++;
    }
    if(!bad) { if(t < 1) bad = "sequence is empty"; else m.tracks = t; }
  } else if(!bad && has_tracks) {
    m.tracks = tracks_v;
    for(int i = 0; i < tracks_v; i++) m.seq[i] = i + 1;
  }
  if(bad) {
    themes_unlock();
    return respond_error(req->fd, 400, bad);
  }
  if(has_loop) snprintf(m.loop, sizeof(m.loop), "%s", loop);
  if(has_order) snprintf(m.order, sizeof(m.order), "%s", order);
  if(m.loop_start_ms < 0 && m.tracks <= 1) snprintf(m.order, sizeof(m.order), "music");
  if(has_between) snprintf(m.between, sizeof(m.between), "%s", between);
  if(has_endmode) snprintf(m.end_mode, sizeof(m.end_mode), "%s", end_mode);
  has_playlist = has_playlist || has_tracks || has_seq || has_between || has_endmode;
  int music = has_gain || has_start || has_end || has_blend || has_loop || has_lstart || has_lend || has_order || has_from || has_playlist;
  int rc = keep_snapshot(id) || write_theme_ini(id, has_name ? name : NULL, has_fw ? fw : NULL, music ? &m : NULL);
  if(!rc && has_keep) {
    /* keeping the pictures a theme's textures were made from: on unless the marker file is there */
    char marker[PATH_MAX];
    snprintf(marker, sizeof(marker), "%s/%s/originals-off", THEMES_DIR, id);
    rc = keep ? (access(marker, F_OK) ? 0 : fs_unlink(marker)) : fs_write_file(marker, "1\n", 2);
  }
  themes_unlock();
  if(rc) return respond_error(req->fd, 500, "could not save the settings");
  respond_ok(req->fd);
}


/* Keep the changes made since the last save. */
static void
api_save_theme(request_t *req) {
  char id[128];
  get_param(req->query, "id", id, sizeof(id));
  themes_lock();
  int exists = theme_exists(id), had = exists && theme_unsaved(id);
  if(had) drop_snapshot(id);
  themes_unlock();
  if(!exists) return respond_error(req->fd, 404, "no such theme");
  if(had) log_msg("theme %s: changes saved", id);
  respond_ok(req->fd);
}


/* Put the theme back as it was at the last save. An applied theme is
   applied again, so the console shows the same. */
static void
api_discard_theme(request_t *req) {
  char id[128], snap[PATH_MAX], dir[PATH_MAX], trash[PATH_MAX + 8], active[128];
  get_param(req->query, "id", id, sizeof(id));
  themes_lock();
  if(!theme_exists(id) || !theme_unsaved(id)) {
    themes_unlock();
    return respond_error(req->fd, 409, "there are no changes to discard");
  }
  snapshot_path(id, snap, sizeof(snap));
  snprintf(dir, sizeof(dir), "%s/%s", THEMES_DIR, id);
  snprintf(trash, sizeof(trash), "%s.trash", snap);
  fs_rm_rf_under(SNAP_DIR, trash);
  int rc = fs_rename(dir, trash);
  if(!rc && fs_rename(snap, dir)) {
    fs_rename(trash, dir);   /* put the edited theme back rather than lose it */
    rc = -1;
  }
  if(!rc) fs_rm_rf_under(SNAP_DIR, trash);
  themes_unlock();
  if(rc) return respond_error(req->fd, 500, "could not discard the changes");

  read_active(active, sizeof(active));
  int again = !strcmp(active, id) && !write_active(id);
  log_msg("theme %s: changes discarded%s", id, again ? ", applying it again" : "");
  strbuf_t sb = {0};
  sb_printf(&sb, "{\"ok\":true,\"reapplied\":%s}", again ? "true" : "false");
  respond_json(req->fd, 200, &sb);
}


static void
api_duplicate_theme(request_t *req) {
  char id[128], new_id[128], name[128], src[PATH_MAX], dst[PATH_MAX];

  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "new_id", new_id, sizeof(new_id));
  get_param(req->query, "name", name, sizeof(name));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(!theme_name_valid(new_id)) return respond_error(req->fd, 400, "new_id: use letters, digits, - and _");
  for(char *p = name; *p; p++) if(*p == '\n' || *p == '\r' || *p == '=') *p = ' ';

  snprintf(src, sizeof(src), "%s/%s", THEMES_DIR, id);
  snprintf(dst, sizeof(dst), "%s/%s", THEMES_DIR, new_id);
  themes_lock();
  int rc = fs_is_dir(dst) ? 1 : fs_link_tree(src, dst);
  if(!rc && name[0]) rc = write_theme_ini(new_id, name, NULL, NULL);
  if(rc < 0) fs_rm_rf_under(THEMES_DIR, dst);
  themes_unlock();
  if(rc > 0) return respond_error(req->fd, 409, "a theme with that id exists");
  if(rc) return respond_error(req->fd, 500, "could not duplicate the theme");
  log_msg("duplicated theme %s as %s", id, new_id);
  respond_ok(req->fd);
}


static int source_path(const char *id, const char *asset, char *out, size_t size);

/* Pictures and textures are made from the user's picture (fitted, cropped, padded), so their original can be kept. */
static int
source_kind_ok(const char *asset) {
  int k = asset_kind(asset);
  return k == KIND_TEXTURE || k == KIND_IMAGE || k == KIND_SVG;
}

static void
api_delete_file(request_t *req) {
  char id[128], asset[512], dst[PATH_MAX];

  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "path", asset, sizeof(asset));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(asset[0] != '/' || !target_allowed(asset) || theme_file_path(id, asset, dst, sizeof(dst))) {
    return respond_error(req->fd, 403, "bad path");
  }
  themes_lock();
  int rc = access(dst, F_OK) || keep_snapshot(id) || fs_unlink(dst);
  char kept[PATH_MAX];
  if(!rc && source_kind_ok(asset) && !source_path(id, asset, kept, sizeof(kept)) && !access(kept, F_OK)) fs_unlink(kept);
  themes_unlock();
  if(rc) return respond_error(req->fd, 404, "file not in theme");
  respond_ok(req->fd);
}


/* The picture a texture or picture was made from, kept in its theme (themes/<id>/sources/
   <hash of the path>.src), so the page can make the texture again when a
   placement option changes without the picture being attached again. It is
   never applied: only files/ is. */
#define SOURCE_MAX_BYTES (32ull << 20)
#define SOURCE_MAX_SIDE  16384

static int
source_path(const char *id, const char *asset, char *out, size_t size) {
  uint64_t h = 1469598103934665603ull;
  for(const char *p = asset; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ull;
  return snprintf(out, size, "%s/%s/sources/%016llx.src", THEMES_DIR, id, (unsigned long long)h) >= (int)size ? -1 : 0;
}

/* A theme can switch keeping originals off (to save space): the marker file is then in its folder, and the
   plugin refuses new originals. Kept ones stay until they are removed. */
static int
originals_off(const char *id) {
  char path[PATH_MAX];
  return snprintf(path, sizeof(path), "%s/%s/originals-off", THEMES_DIR, id) < (int)sizeof(path) && !access(path, F_OK);
}

/* What the kept originals of a theme take on the disk: each distinct file once (where the filesystem has hard
   links, one picture attached at several sizes is a set of links to one file; elsewhere each is a copy). */
static uint64_t
sources_bytes(const char *id) {
  char dir[PATH_MAX];
  uint64_t total = 0;
  ino_t seen[512];
  int nseen = 0;
  snprintf(dir, sizeof(dir), "%s/%s/sources", THEMES_DIR, id);
  DIR *d = opendir(dir);
  if(!d) return 0;
  struct dirent *e;
  while((e = readdir(d))) {
    char path[PATH_MAX];
    struct stat st;
    if(e->d_name[0] == '.' || snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path) || lstat(path, &st) || !S_ISREG(st.st_mode)) continue;
    int dup = 0;
    for(int i = 0; i < nseen && !dup; i++) dup = seen[i] == st.st_ino;
    if(dup) continue;
    if(nseen < (int)(sizeof(seen) / sizeof(*seen))) seen[nseen++] = st.st_ino;
    total += (uint64_t)st.st_size;
  }
  closedir(d);
  return total;
}


static void
api_put_source(request_t *req) {
  char id[128], asset[512], dst[PATH_MAX], part[PATH_MAX + 24], dir[PATH_MAX], crc_hex[9];
  uint32_t crc;
  uint64_t size = 0;
  image_info_t info;

  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "path", asset, sizeof(asset));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(!asset_path_valid(asset) || !source_kind_ok(asset)) return respond_error(req->fd, 403, "not a system picture or texture");
  if(originals_off(id)) return respond_error(req->fd, 409, "keeping the original pictures is switched off for this theme");
  char same[512];
  get_param(req->query, "same_as", same, sizeof(same));
  if(same[0]) {
    /* the same picture as one already kept for another file of this theme (one picture attached at several
       sizes): linked or copied here, not sent again */
    char from[PATH_MAX];
    if(!asset_path_valid(same) || !source_kind_ok(same) || source_path(id, same, from, sizeof(from)) || access(from, F_OK) ||
       source_path(id, asset, dst, sizeof(dst))) {
      return respond_error(req->fd, 404, "no picture kept for that file");
    }
    snprintf(dir, sizeof(dir), "%s", dst);
    *strrchr(dir, '/') = 0;
    themes_lock();
    int rc = !theme_exists(id) || fs_mkdirs(dir) || keep_snapshot(id);
    if(!rc && strcmp(from, dst)) {
      /* a hard link where the filesystem has them (the console's /data hasn't), else a copy made here, so the
         picture never crosses the network twice */
      fs_unlink(dst);
      rc = fs_link(from, dst) && fs_copy_file(from, dst);
    }
    themes_unlock();
    if(rc) return respond_error(req->fd, 500, "could not keep the picture");
    respond_ok(req->fd);
    return;
  }
  if(req->content_length <= 0) return respond_error(req->fd, 400, "empty upload");
  if((uint64_t)req->content_length > SOURCE_MAX_BYTES) return respond_error(req->fd, 413, "the picture is larger than 32 MB");
  if(source_path(id, asset, dst, sizeof(dst))) return respond_error(req->fd, 400, "path too long");
  snprintf(dir, sizeof(dir), "%s", dst);
  *strrchr(dir, '/') = 0;
  part_name(part, sizeof(part), dst);
  if(fs_mkdirs(dir)) return respond_error(req->fd, 500, "could not create folder");
  if(receive_upload(req, part, &crc, crc_hex)) return;
  if(image_probe(part, &info) || !info.width || !info.height || info.width > SOURCE_MAX_SIDE || info.height > SOURCE_MAX_SIDE) {
    fs_unlink(part);
    return respond_error(req->fd, 422, "not a picture (PNG, JPEG or WebP)");
  }
  if(place_upload(req, id, part, dst, crc, &size, 1, (uint64_t)req->content_length)) return;
  strbuf_t sb = {0};
  sb_printf(&sb, "{\"ok\":true,\"size\":%llu,\"crc32\":\"%s\"}", (unsigned long long)size, crc_hex);
  respond_json(req->fd, 200, &sb);
}


static void
api_get_source(request_t *req) {
  char id[128], asset[512], dst[PATH_MAX];

  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "path", asset, sizeof(asset));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(!asset_path_valid(asset) || !source_kind_ok(asset) || source_path(id, asset, dst, sizeof(dst)) ||
     !fs_read_within(dst, THEMES_DIR)) {
    return respond_error(req->fd, 404, "no picture kept for this texture");
  }
  respond_file(req, dst);
}


/* Which of the given textures (paths, comma-separated, up to 32) have a
   picture kept in the theme. */
/* The theme's files (paths as the page lists them) whose original picture is kept. */
static void
sources_in(strbuf_t *sb, const char *id, const char *root, const char *rel, int *first, int depth) {
  char dir[PATH_MAX];
  if(depth > 32) return;
  snprintf(dir, sizeof(dir), "%s%s", root, rel);
  DIR *d = opendir(dir);
  if(!d) return;
  struct dirent *e;
  while((e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char child_rel[PATH_MAX], child[PATH_MAX], kept[PATH_MAX];
    struct stat st;
    if(snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, e->d_name) >= (int)sizeof(child_rel)) continue;
    snprintf(child, sizeof(child), "%s%s", root, child_rel);
    if(lstat(child, &st)) continue;
    if(S_ISDIR(st.st_mode)) sources_in(sb, id, root, child_rel, first, depth + 1);
    else if(S_ISREG(st.st_mode) && asset_path_valid(child_rel) && source_kind_ok(child_rel) &&
            !source_path(id, child_rel, kept, sizeof(kept)) && !access(kept, F_OK)) {
      if(!*first) SB_LIT(sb, ",");
      *first = 0;
      sb_json_str(sb, child_rel);
    }
  }
  closedir(d);
}


static void
api_sources(request_t *req) {
  char id[128], list[4096], dst[PATH_MAX], root[PATH_MAX];
  strbuf_t sb = {0};
  int first = 1;

  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "paths", list, sizeof(list));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  SB_LIT(&sb, "{\"have\":[");
  if(!list[0]) {
    /* every file of the theme whose original is kept */
    snprintf(root, sizeof(root), "%s/%s/files", THEMES_DIR, id);
    sources_in(&sb, id, root, "", &first, 0);
  } else {
    char *save = NULL;
    int n = 0;
    for(char *p = strtok_r(list, ",", &save); p && n < 32; p = strtok_r(NULL, ",", &save), n++) {
      if(!asset_path_valid(p) || !source_kind_ok(p) || source_path(id, p, dst, sizeof(dst)) || access(dst, F_OK)) continue;
      if(!first) SB_LIT(&sb, ",");
      first = 0;
      sb_json_str(&sb, p);
    }
  }
  sb_printf(&sb, "],\"keep\":%s,\"bytes\":%llu}", originals_off(id) ? "false" : "true", (unsigned long long)sources_bytes(id));
  respond_json(req->fd, 200, &sb);
}


/* Remove every kept original of a theme (also from the copy Discard would restore, so the space is free at once). */
static void
api_delete_sources(request_t *req) {
  char id[128], dir[PATH_MAX], snap[PATH_MAX];

  get_param(req->query, "id", id, sizeof(id));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  themes_lock();
  snprintf(dir, sizeof(dir), "%s/%s/sources", THEMES_DIR, id);
  fs_rm_rf_under(THEMES_DIR, dir);
  snapshot_path(id, snap, sizeof(snap));
  if(fs_is_dir(snap) && strlen(snap) + 9 < sizeof(snap)) {
    strcat(snap, "/sources");
    fs_rm_rf_under(SNAP_DIR, snap);
  }
  themes_unlock();
  respond_ok(req->fd);
}


/* Check a theme's file against the system file again, as applying it would
   (files made by an older builder can fall outside what the console plays). */
static void
api_check_file(request_t *req) {
  char id[128], asset[512], file[PATH_MAX], stock[PATH_MAX], why[256];

  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "path", asset, sizeof(asset));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(!asset_path_valid(asset) || theme_file_path(id, asset, file, sizeof(file)) ||
     !fs_read_within(file, THEMES_DIR)) {
    return respond_error(req->fd, 404, "file not in theme");
  }
  stock_path(asset, stock, sizeof(stock));
  strbuf_t sb = {0};
  if(asset_validate(file, stock, why, sizeof(why))) {
    SB_LIT(&sb, "{\"ok\":false,\"error\":");
    sb_json_str(&sb, why);
    SB_LIT(&sb, "}");
  } else {
    SB_LIT(&sb, "{\"ok\":true}");
  }
  respond_json(req->fd, 200, &sb);
}


static void
api_get_file(request_t *req) {
  char id[128], asset[512], dst[PATH_MAX];

  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "path", asset, sizeof(asset));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(asset[0] != '/' || !target_allowed(asset) || theme_file_path(id, asset, dst, sizeof(dst)) ||
     !fs_read_within(dst, THEMES_DIR)) {
    return respond_error(req->fd, 404, "file not in theme");
  }
  respond_file(req, dst);
}


static void
api_get_asset(request_t *req) {
  char asset[512];

  get_param(req->query, "path", asset, sizeof(asset));
  /* A title's param.json or param.sfo may be read (for its name), never
     replaced. */
  const char *base = strrchr(asset, '/');
  int title_info = !strncmp(asset, APPMETA_DIR "/", sizeof(APPMETA_DIR)) && base &&
                   (!strcmp(base, "/param.json") || !strcmp(base, "/param.sfo"));
  if(!asset_path_valid(asset) || (asset_kind(asset) == KIND_BLOCKED && !title_info)) {
    return respond_error(req->fd, 403, "not a themable system asset");
  }
  char stock[PATH_MAX];
  stock_path(asset, stock, sizeof(stock));
  respond_file(req, stock);
}


static void
sb_base64(strbuf_t *sb, const uint8_t *d, size_t n) {
  static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for(size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    char out[4] = { tab[v >> 18], tab[(v >> 12) & 63], i + 1 < n ? tab[(v >> 6) & 63] : '=', i + 2 < n ? tab[v & 63] : '=' };
    sb_append(sb, out, 4);
  }
}


/* Format of a system sound, so the builder can prepare a matching source. */
static void
api_audio(request_t *req) {
  static const uint8_t atrac9[16] = {0xd2, 0x42, 0xe1, 0x47, 0xba, 0x36, 0x8d, 0x4d,
                                     0x88, 0xfc, 0x61, 0x65, 0x4f, 0x8c, 0x83, 0x6c};
  char asset[512];
  wav_info_t w;

  get_param(req->query, "path", asset, sizeof(asset));
  if(!asset_path_valid(asset) || asset_kind(asset) != KIND_AUDIO) return respond_error(req->fd, 403, "not a system sound");
  char stock[PATH_MAX];
  stock_path(asset, stock, sizeof(stock));
  if(wav_probe(stock, &w)) return respond_error(req->fd, 422, "cannot read this sound");
  const char *codec = w.format == 1 ? "PCM" : w.format == 0xfffe && !memcmp(w.subformat, atrac9, 16) ? "ATRAC9" : "other";
  strbuf_t sb = {0};
  sb_printf(&sb, "{\"codec\":\"%s\",\"channels\":%u,\"rate\":%u,\"bits\":%u}", codec, w.channels, w.rate, w.bits);
  respond_json(req->fd, 200, &sb);
}


/* Everything the builder needs to encode a replacement texture: the stock
   header bytes (reused verbatim) and the format. */
static void
api_texture(request_t *req) {
  char asset[512];
  texture_info_t t;
  uint8_t header[148];

  get_param(req->query, "path", asset, sizeof(asset));
  if(!asset_path_valid(asset) || asset_kind(asset) != KIND_TEXTURE) return respond_error(req->fd, 403, "not a system texture");
  char stock[PATH_MAX];
  stock_path(asset, stock, sizeof(stock));
  int compressed = 0;
  if(texture_info(stock, &t, header, &compressed)) return respond_error(req->fd, 422, "cannot read this texture");
  uint8_t frame[16];
  size_t frame_len = compressed ? texture_frame_head(stock, frame) : 0;

  strbuf_t sb = {0};
  sb_printf(&sb, "{\"width\":%u,\"height\":%u,\"mips\":%u,\"array\":%u,\"dxgi\":%u,\"format\":",
            t.width, t.height, t.mips, t.array, t.dxgi);
  sb_json_str(&sb, texture_format_name(&t));
  sb_printf(&sb, ",\"dataLength\":%llu,\"zstd\":%s,\"header\":\"",
            (unsigned long long)t.data_len, compressed ? "true" : "false");
  /* (by the file's content: some compressed textures have a plain .dds name) */
  sb_base64(&sb, header, t.header_len);
  SB_LIT(&sb, "\",\"frame\":\"");
  sb_base64(&sb, frame, frame_len);   /* the system file's zstd frame header: a replacement starts the same */
  SB_LIT(&sb, "\"}");
  respond_json(req->fd, 200, &sb);
}


static void
api_log(request_t *req) {
  int in = open(LOG_FILE, O_RDONLY);
  ssize_t n = 0;

  if(in >= 0) {
    off_t size = lseek(in, 0, SEEK_END);
    lseek(in, size > (off_t)IO_CHUNK ? size - (off_t)IO_CHUNK : 0, SEEK_SET);
    n = read(in, req->io, IO_CHUNK);
    close(in);
  }
  respond(req->fd, 200, "text/plain; charset=utf-8", req->io, n > 0 ? n : 0);
}


static int
crash_name_valid(const char *name) {
  size_t n = strlen(name);
  if(n < 11 || n > 40 || strncmp(name, "crash-", 6) || strcmp(name + n - 4, ".txt")) return 0;
  for(size_t i = 6; i < n - 4; i++) if(name[i] < '0' || name[i] > '9') return 0;
  return 1;
}


static void
api_crashes(request_t *req) {
  strbuf_t sb = {0};
  int first = 1;
  DIR *d = opendir(LOG_DIR);

  SB_LIT(&sb, "{\"crashes\":[");
  if(d) {
    struct dirent *e;
    while((e = readdir(d))) {
      if(!crash_name_valid(e->d_name)) continue;
      if(!first) SB_LIT(&sb, ",");
      first = 0;
      sb_json_str(&sb, e->d_name);
    }
    closedir(d);
  }
  SB_LIT(&sb, "]}");
  respond_json(req->fd, 200, &sb);
}


static void
api_crash(request_t *req) {
  char name[64], path[PATH_MAX];

  get_param(req->query, "name", name, sizeof(name));
  if(!crash_name_valid(name)) return respond_error(req->fd, 404, "no such report");
  snprintf(path, sizeof(path), "%s/%s", LOG_DIR, name);
  respond_file(req, path);
}


static void
route(request_t *req) {
  const char *m = req->method, *p = req->path;
  int get = !strcmp(m, "GET"), post = !strcmp(m, "POST");
  int put = !strcmp(m, "PUT"), del = !strcmp(m, "DELETE");

  if(get && (!strcmp(p, "/") || !strcmp(p, "/index.html"))) {
    respond(req->fd, 200, "text/html; charset=utf-8", web_index, web_index_end - web_index);
    return;
  }
  if(get && !strcmp(p, "/atrac9.js")) {
    respond(req->fd, 200, "text/javascript; charset=utf-8", web_atrac9, web_atrac9_end - web_atrac9);
    return;
  }
  if(get && !strcmp(p, "/icon.png")) {
    respond(req->fd, 200, "image/png", web_icon, web_icon_end - web_icon);
    return;
  }
  if(get && !strcmp(p, "/api/status"))      return api_status(req);
  if(get && !strcmp(p, "/api/assets"))      return api_assets(req);
  if(get && !strcmp(p, "/api/asset"))       return api_get_asset(req);
  if(get && !strcmp(p, "/api/asset/texture")) return api_texture(req);
  if(get && !strcmp(p, "/api/asset/audio"))  return api_audio(req);
  if(get && !strcmp(p, "/api/theme/files")) return api_theme_files(req);
  if(get && !strcmp(p, "/api/theme/file"))  return api_get_file(req);
  if(get && !strcmp(p, "/api/theme/check")) return api_check_file(req);
  if(get && !strcmp(p, "/api/theme/source")) return api_get_source(req);
  if(get && !strcmp(p, "/api/theme/sources")) return api_sources(req);
  if(get && !strcmp(p, "/api/log"))         return api_log(req);
  if(get && !strcmp(p, "/api/crashes"))     return api_crashes(req);
  if(get && !strcmp(p, "/api/crash"))       return api_crash(req);
  if(get && !strcmp(p, "/api/theme/music")) return api_get_music(req);
  if(get && !strcmp(p, "/api/theme/preview")) return api_get_preview(req);
  if(get && !strcmp(p, "/api/mounts"))      return api_mounts(req);
  if(get && !strcmp(p, "/api/title/where")) return api_title_where(req);
  if(get && !strcmp(p, "/api/diag/watch")) return api_watch(req);
  if(get && !strcmp(p, "/api/diag/procs")) return api_procs(req);

  if((post || put || del) && !req->has_token) {
    return respond_error(req->fd, 403, "missing X-PS5-Theme header");
  }
  if(post && !strcmp(p, "/api/apply"))      return api_apply(req);
  if(post && !strcmp(p, "/api/themes"))     return api_create_theme(req);
  if(del && !strcmp(p, "/api/themes"))      return api_delete_theme(req);
  if(put && !strcmp(p, "/api/theme/file"))  return api_put_file(req);
  if(put && !strcmp(p, "/api/theme/source")) return api_put_source(req);
  if(del && !strcmp(p, "/api/theme/file"))  return api_delete_file(req);
  if(del && !strcmp(p, "/api/theme/sources")) return api_delete_sources(req);
  if(put && !strcmp(p, "/api/theme/music")) return api_put_music(req);
  if(del && !strcmp(p, "/api/theme/music")) return api_delete_music(req);
  if(put && !strcmp(p, "/api/theme/preview")) return api_put_preview(req);
  if(del && !strcmp(p, "/api/theme/preview")) return api_delete_preview(req);
  if(post && !strcmp(p, "/api/theme/settings")) return api_theme_settings(req);
  if(post && !strcmp(p, "/api/themes/duplicate")) return api_duplicate_theme(req);
  if(post && !strcmp(p, "/api/theme/save")) return api_save_theme(req);
  if(post && !strcmp(p, "/api/handoff"))     return api_handoff(req);
  if(post && !strcmp(p, "/api/theme/discard")) return api_discard_theme(req);
#ifdef TEST_HOOKS
  if(post && !strcmp(p, "/api/debug/break-listener")) {
    /* As rest mode does on the PS5: the listening socket stops working. */
    log_msg("debug: breaking the listening socket on request");
    respond_ok(req->fd);
    shutdown(atomic_load(&listen_fd), SHUT_RDWR);
    return;
  }
  if(post && !strcmp(p, "/api/debug/deafen")) {
    log_msg("debug: the web server stops answering on request");
    respond_ok(req->fd);
    atomic_store(&deaf, 1);
    return;
  }
#endif
#ifdef HOST_TEST
  if(post && !strcmp(p, "/api/debug/crash")) {
    log_msg("debug: crashing on request");
    raise(SIGSEGV);
  }
#endif

  respond_error(req->fd, 404, "not found");
}


/* Find the end of the request headers in raw bytes (NUL-safe). */
static char *
header_end(char *buf, size_t len) {
  for(size_t i = 0; i + 3 < len; i++) {
    if(buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return buf + i;
  }
  return NULL;
}


typedef struct worker {
  int  slot;
  char header[MAX_HEADER + 1];
  char io[IO_CHUNK];
} worker_t;


static void
handle(worker_t *w, int fd) {
  char *buf = w->header;
  request_t req;
  size_t len = 0;
  char *end = NULL;

  memset(&req, 0, sizeof(req));
  req.fd = fd;
  req.slot = w->slot;
  req.io = w->io;

  while(len < MAX_HEADER) {
    ssize_t n = read(fd, buf + len, MAX_HEADER - len);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) return;
    len += n;
    if((end = header_end(buf, len))) break;
  }
  if(!end) return respond_error(fd, 400, "headers too large or incomplete");
  if(memchr(buf, 0, end - buf)) return respond_error(fd, 400, "malformed request");

  *end = 0;
  req.body = end + 4;
  req.body_len = len - (req.body - buf);

  char target[2304];
  if(sscanf(buf, "%7s %2303s", req.method, target) != 2) {
    return respond_error(fd, 400, "bad request line");
  }
  char *q = strchr(target, '?');
  if(q) *q = 0;
  /* An address longer than its fields is refused, never cut: a cut-down path or value is a different one. */
  if(strlen(target) + (q ? strlen(q + 1) + 1 : 0) >= sizeof(target) - 1 || strlen(target) >= sizeof(req.path) ||
     (q && strlen(q + 1) >= sizeof(req.query))) {
    return respond_error(fd, 414, "the request's address is too long");
  }
  if(q) snprintf(req.query, sizeof(req.query), "%s", q + 1);
  snprintf(req.path, sizeof(req.path), "%s", target);

  for(char *line = strstr(buf, "\r\n"); line; line = strstr(line, "\r\n")) {
    line += 2;
    if(!strncasecmp(line, "Content-Length:", 15)) req.content_length = atol(line + 15);
    else if(!strncasecmp(line, "X-PS5-Theme:", 12)) req.has_token = 1;
  }

  /* Headers get a short deadline; a body may take longer. */
  struct timeval tv = {15, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  crumb(w->slot, "%s %s?%.200s", req.method, req.path, req.query);
  route(&req);
  crumb(w->slot, "idle");
}


/* ---- worker pool ---- */

#define QUEUE_SIZE 32

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
static int queue[QUEUE_SIZE];
static int queue_head, queue_len;


static int
enqueue(int fd) {
  pthread_mutex_lock(&queue_lock);
  if(queue_len == QUEUE_SIZE) {
    pthread_mutex_unlock(&queue_lock);
    return -1;
  }
  queue[(queue_head + queue_len++) % QUEUE_SIZE] = fd;
  pthread_cond_signal(&queue_cond);
  pthread_mutex_unlock(&queue_lock);
  return 0;
}


static int
dequeue(void) {
  pthread_mutex_lock(&queue_lock);
  while(!queue_len) pthread_cond_wait(&queue_cond, &queue_lock);
  int fd = queue[queue_head];
  queue_head = (queue_head + 1) % QUEUE_SIZE;
  queue_len--;
  pthread_mutex_unlock(&queue_lock);
  return fd;
}


static void *
worker_main(void *arg) {
  worker_t *w = arg;
  crash_thread_init(w->slot);
  crumb(w->slot, "idle");
  for(;;) {
    int fd = dequeue();
    handle(w, fd);
    close(fd);
  }
  return NULL;
}


static int
start_workers(void) {
  pthread_attr_t attr;
  int started = 0;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 512 * 1024);
  for(int i = 0; i < WEB_WORKERS; i++) {
    worker_t *w = calloc(1, sizeof(*w));
    pthread_t t;
    if(!w) break;
    w->slot = CRUMB_WEB + i;
    if(pthread_create(&t, &attr, worker_main, w)) {
      free(w);
      break;
    }
    pthread_detach(t);
    started++;
  }
  pthread_attr_destroy(&attr);
  return started;
}


/* The listening socket. On the PS5, rest mode can leave it failing every
   accept() for good (errno 0xA3, as etaHEN's own servers see), which took
   the manager and the hand-over to a newer copy with it: it is then closed
   and opened again. */
#define LISTEN_CHECK_SECONDS 5

/* Up to `attempts` tries a second apart (0: until it works). */
static int
web_listen(int attempts) {
  struct sockaddr_in addr;
  int one = 1;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(WEB_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  for(int tries = 0; !attempts || tries < attempts; tries++) {
    if(tries) sleep(1);   /* after a hard Stop the old socket can linger briefly */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if(srv < 0) continue;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if(!bind(srv, (struct sockaddr *)&addr, sizeof(addr)) && !listen(srv, 32)) {
      int fl = fcntl(srv, F_GETFL);
      if(fl >= 0) fcntl(srv, F_SETFL, fl | O_NONBLOCK);
      return srv;
    }
    int saved = errno;
    close(srv);
    errno = saved;
  }
  return -1;
}


void *
web_server_thread(void *arg) {
  (void)arg;
  crash_thread_init(CRUMB_LISTEN);
  crumb(CRUMB_LISTEN, "starting");

  if(!start_workers()) {
    log_err("web: could not start any worker");
    return NULL;
  }
  int srv = web_listen(30);
  if(srv < 0) {
    log_err("web: bind :%d failed: %s", WEB_PORT, strerror(errno));
    return NULL;
  }
  atomic_store(&listen_fd, srv);
  log_msg("web: listening on port %d with %d workers", WEB_PORT, WEB_WORKERS);
  crumb(CRUMB_LISTEN, "accepting");

  time_t checked = time(NULL), reported = 0;
  for(;;) {
#ifdef TEST_HOOKS
    if(atomic_load(&deaf)) {
      atomic_store(&listen_fd, -1);
      close(srv);
      for(;;) sleep(60);
    }
#endif
    struct pollfd pf = {srv, POLLIN, 0};
    int ready = poll(&pf, 1, 1000), fd = -1, broken = 0, err = 0;
    socklen_t len = sizeof(err);
    if(ready > 0 && (pf.revents & (POLLERR | POLLHUP | POLLNVAL))) {
      broken = 1;
      if(getsockopt(srv, SOL_SOCKET, SO_ERROR, &err, &len) || !err) err = EPIPE;
    } else if(ready > 0 || time(NULL) - checked >= LISTEN_CHECK_SECONDS) {
      /* Also when nothing seems to arrive: a socket broken by rest mode
         may not say so to poll(). */
      checked = time(NULL);
      fd = accept(srv, NULL, NULL);
      if(fd < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNABORTED) {
        broken = 1;
        err = errno;
      }
    }
    if(broken) {
      if(time(NULL) - reported >= 60) {
        log_msg("web: the listening socket stopped working (%s, errno %d), as rest mode can do; listening again",
                strerror(err), err);
      }
      atomic_store(&listen_fd, -1);
      close(srv);
      crumb(CRUMB_LISTEN, "listening again");
      srv = web_listen(0);
      atomic_store(&listen_fd, srv);
      if(time(NULL) - reported >= 60) log_msg("web: listening on port %d again", WEB_PORT);
      reported = time(NULL);
      crumb(CRUMB_LISTEN, "accepting");
      continue;
    }
    if(fd < 0) continue;
    /* The BSDs pass non-blocking mode on from the listening socket. */
    int fl = fcntl(fd, F_GETFL);
    if(fl >= 0 && (fl & O_NONBLOCK)) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 15;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    plat_socket_nosigpipe(fd);
    if(enqueue(fd)) {
      static const char busy[] =
        "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\n"
        "Content-Length: 18\r\nRetry-After: 1\r\nConnection: close\r\n\r\n{\"error\":\"busy\"}\r\n";
      send_all(fd, busy, sizeof(busy) - 1);
      close(fd);
    }
  }
  return NULL;
}
