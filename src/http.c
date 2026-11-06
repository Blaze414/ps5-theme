/* The manager's web server. A listener thread accepts connections and hands them to WEB_WORKERS workers through a
   bounded queue, so one slow client cannot stall the page. A worker reads the request's headers and passes it to
   route() (routes.c); the answers are made here. Each worker owns its buffers. */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "http.h"
#include "config.h"
#include "crash.h"
#include "fsguard.h"
#include "log.h"
#include "platform.h"
#include "store.h"

#define MAX_HEADER   (16 * 1024)
#define HEADER_SECONDS   10

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
  ".incbin \"build-meta/index.html\"\n"
  "web_index_end:\n"
  ".balign 16\n"
  "web_atrac9:\n"
  ".incbin \"web/atrac9.js\"\n"
  "web_atrac9_end:\n"
  ".balign 16\n"
  "web_icon:\n"
  ".incbin \"assets/icon0.png\"\n"
  "web_icon_end:\n"
  /* The page and the encoder compressed when the plugin is built (gzip -9 -n): a third of the size to send. */
  ".balign 16\n"
  ".global web_index_gz, web_index_gz_end, web_atrac9_gz, web_atrac9_gz_end\n"
  "web_index_gz:\n"
  ".incbin \"build-meta/index.html.gz\"\n"
  "web_index_gz_end:\n"
  ".balign 16\n"
  "web_atrac9_gz:\n"
  ".incbin \"build-meta/atrac9.js.gz\"\n"
  "web_atrac9_gz_end:\n"
  ".previous\n"
);
extern const char web_index[], web_index_end[];
extern const char web_atrac9[], web_atrac9_end[];
extern const char web_icon[], web_icon_end[];
extern const char web_index_gz[], web_index_gz_end[];
extern const char web_atrac9_gz[], web_atrac9_gz_end[];

void
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

void
sb_printf(strbuf_t *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

void
sb_printf(strbuf_t *sb, const char *fmt, ...) {
  char buf[1024];
  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if(n > 0) sb_append(sb, buf, MIN((size_t)n, sizeof(buf) - 1));
}

void
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

void
sb_base64(strbuf_t *sb, const uint8_t *d, size_t n) {
  static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for(size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    char out[4] = { tab[v >> 18], tab[(v >> 12) & 63], i + 1 < n ? tab[(v >> 6) & 63] : '=', i + 2 < n ? tab[v & 63] : '=' };
    sb_append(sb, out, 4);
  }
}

int
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
  case 401: return "Unauthorized";
  case 429: return "Too Many Requests";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 409: return "Conflict";
  case 413: return "Payload Too Large";
  case 414: return "URI Too Long";
  case 422: return "Unprocessable Entity";
  case 503: return "Service Unavailable";
  case 507: return "Insufficient Storage";
  default:  return "Internal Server Error";
  }
}

void
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

void
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

void
respond_error(int fd, int status, const char *msg) {
  strbuf_t sb = {0};
  SB_LIT(&sb, "{\"error\":");
  sb_json_str(&sb, msg);
  SB_LIT(&sb, "}");
  if(status >= 500) log_err("web: %d %s", status, msg);
  else if(status == 409 || status == 413 || status == 414 || status == 422) log_msg("web: refused (%d): %s", status, msg);
  respond_json(fd, status, &sb);
}

/* One of the console's own pictures (an interface icon, a texture's preview): it may be kept for an hour (the
   console's own copy changes only with the firmware), and as an SVG it can never run anything, even opened on its
   own. */
void
respond_icon(int fd, const char *ctype, const void *body, size_t len) {
  char head[384];
  int n = snprintf(head, sizeof(head),
                   "HTTP/1.1 200 OK\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %zu\r\n"
                   "Cache-Control: private, max-age=3600\r\n"
                   "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'\r\n"
                   "X-Content-Type-Options: nosniff\r\n"
                   "Connection: close\r\n\r\n",
                   ctype, len);
  if(send_all(fd, head, n)) return;
  send_all(fd, body, len);
}

/* Whether the browser already has the response this version names: If-None-Match is one version, a list of them,
   or "*". */
static int
not_modified(const request_t *req, const char *etag) {
  return req->if_none_match[0] && (strstr(req->if_none_match, etag) || !strcmp(req->if_none_match, "*"));
}

static void
respond_not_modified(int fd, const char *etag) {
  char head[256];
  int n = snprintf(head, sizeof(head),
                   "HTTP/1.1 304 Not Modified\r\nETag: %s\r\nCache-Control: no-cache\r\nVary: Accept-Encoding\r\n"
                   "Connection: close\r\n\r\n", etag);
  send_all(fd, head, n);
}

/* The page and its encoder: the same bytes until the plugin is updated. Asked again each time it is opened (no-cache),
   so an updated plugin's page is never an old one, and answered "not modified" in a few bytes while it is the same;
   sent compressed to a browser that takes it. The version is the plugin's and a checksum of the very bytes. */
typedef struct { const char *data, *end, *gz, *gz_end; char etag[48]; } asset_t;
static asset_t page_assets[3];
static pthread_once_t page_assets_once = PTHREAD_ONCE_INIT;

static void
page_assets_init(void) {
  page_assets[0] = (asset_t){ web_index, web_index_end, web_index_gz, web_index_gz_end, "" };
  page_assets[1] = (asset_t){ web_atrac9, web_atrac9_end, web_atrac9_gz, web_atrac9_gz_end, "" };
  page_assets[2] = (asset_t){ web_icon, web_icon_end, NULL, NULL, "" };   /* (a PNG: already compressed) */
  for(int i = 0; i < 3; i++) {
    asset_t *a = &page_assets[i];
    snprintf(a->etag, sizeof(a->etag), "\"%s-%08x\"", PLUGIN_VERSION, crc32_update(0, a->data, a->end - a->data));
  }
}

void
respond_page_asset(request_t *req, int which, const char *ctype) {
  pthread_once(&page_assets_once, page_assets_init);
  const asset_t *a = &page_assets[which];
  if(not_modified(req, a->etag)) return respond_not_modified(req->fd, a->etag);
  int gz = req->gzip_ok && a->gz && a->gz_end > a->gz;
  const char *body = gz ? a->gz : a->data;
  size_t len = gz ? (size_t)(a->gz_end - a->gz) : (size_t)(a->end - a->data);
  char head[384];
  int n = snprintf(head, sizeof(head),
                   "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%s"
                   "Cache-Control: no-cache\r\nETag: %s\r\nVary: Accept-Encoding\r\n"
                   "X-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
                   ctype, len, gz ? "Content-Encoding: gzip\r\n" : "", a->etag);
  if(send_all(req->fd, head, n)) return;
  send_all(req->fd, body, len);
}

void
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

/* The plugin's own files are reached as a write is, never through a link (one put in a theme can't serve a file
   from elsewhere); the console's own, as the system has them. */
static int
open_served(const char *path) {
  if(!strncmp(path, BASE_DIR "/", sizeof(BASE_DIR))) return fs_open_read(path, BASE_DIR);
  return open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
}

void
respond_file(request_t *req, const char *path) {
  struct stat st;
  int fd = req->fd;
  int in = open_served(path);
  if(in < 0 && errno == ENOENT) {   /* (a theme's folder mid-Discard: see swap_lock) */
    swap_wait();
    in = open_served(path);
  }

  if(in >= 0 && (fstat(in, &st) || !S_ISREG(st.st_mode))) {
    close(in);
    in = -1;
  }
  if(in < 0) {
    respond_error(fd, 404, "file not found");
    return;
  }

  /* A file is only ever replaced (a new one renamed over it), never changed in place: its identity, size and time
     name this version of it. The browser asks again each time it shows it, and an unchanged one is "not modified":
     a page redrawing its pictures costs a few bytes each, not the pictures. */
  char etag[80];
  snprintf(etag, sizeof(etag), "\"%llx-%llx-%llx.%lx\"", (unsigned long long)st.st_ino, (unsigned long long)st.st_size,
           (unsigned long long)st.st_mtim.tv_sec, (unsigned long)st.st_mtim.tv_nsec);
  if(not_modified(req, etag)) {
    close(in);
    return respond_not_modified(fd, etag);
  }
  char head[384];
  int n = snprintf(head, sizeof(head),
                   "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
                   "Cache-Control: no-cache\r\nETag: %s\r\nX-Content-Type-Options: nosniff\r\n"
                   "Connection: close\r\n\r\n",
                   content_type(path), (long long)st.st_size, etag);
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
int
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

/* An optional integer parameter within [lo, hi]. 0 if absent or valid, -1 if
   present but invalid. */
int
int_param(const char *query, const char *key, int lo, int hi, int *out, int *present) {
  char v[16], *end = NULL;
  *present = !get_param(query, key, v, sizeof(v)) && v[0];
  if(!*present) return 0;
  long n = strtol(v, &end, 10);
  if(*end || n < lo || n > hi) return -1;
  *out = (int)n;
  return 0;
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

/* "gzip" among the encodings, and not turned off with q=0. Only the line's own text is read. */
static int
accepts_gzip(const char *v) {
  const char *e = strstr(v, "\r\n");
  size_t n = e ? (size_t)(e - v) : strlen(v);
  char line[256];
  if(n >= sizeof(line)) n = sizeof(line) - 1;
  memcpy(line, v, n);
  line[n] = 0;
  for(char *c = line; *c; c++) *c = (char)tolower((unsigned char)*c);
  char *g = strstr(line, "gzip");
  if(!g) return 0;
  const char *q = strstr(g, "q=");
  const char *comma = strchr(g, ',');
  if(q && (!comma || q < comma)) return strtod(q + 2, NULL) > 0;
  return 1;
}

/* A header's value, without the spaces around it, into out; -1 if it doesn't fit (then it is left empty). */
static int
header_value(const char *v, char *out, size_t size) {
  while(*v == ' ' || *v == '\t') v++;
  const char *e = strstr(v, "\r\n");
  size_t n = e ? (size_t)(e - v) : strlen(v);
  while(n && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
  out[0] = 0;
  if(n >= size) return -1;
  memcpy(out, v, n);
  out[n] = 0;
  return 0;
}

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

  /* Each read may wait 5 s, and the whole request line and headers get 10 s: a client sending a byte now and then
     can't hold one of the few workers for long (a browser sends them at once). */
  long long started = mono_seconds();
  while(len < MAX_HEADER) {
    ssize_t n = read(fd, buf + len, MAX_HEADER - len);
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) return;
    len += n;
    if((end = header_end(buf, len))) break;
    if(mono_seconds() - started > HEADER_SECONDS) return;
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
    else if(!strncasecmp(line, "Host:", 5)) req.has_host = !header_value(line + 5, req.host, sizeof(req.host));
    else if(!strncasecmp(line, "Cookie:", 7)) header_value(line + 7, req.cookie, sizeof(req.cookie));
    else if(!strncasecmp(line, "User-Agent:", 11)) header_value(line + 11, req.ua, sizeof(req.ua));
    else if(!strncasecmp(line, "Accept-Encoding:", 16)) req.gzip_ok = accepts_gzip(line + 16);
    else if(!strncasecmp(line, "If-None-Match:", 14)) {
      const char *v = line + 14, *e = strstr(v, "\r\n");
      while(*v == ' ' || *v == '\t') v++;
      size_t n = e ? (size_t)(e - v) : strlen(v);
      if(n < sizeof(req.if_none_match)) { memcpy(req.if_none_match, v, n); req.if_none_match[n] = 0; }
    }
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

#ifdef TEST_HOOKS
void
http_break_listener(request_t *req) {
  log_msg("debug: breaking the listening socket on request");
  respond_ok(req->fd);
  shutdown(atomic_load(&listen_fd), SHUT_RDWR);
}

void
http_deafen(request_t *req) {
  log_msg("debug: the web server stops answering on request");
  respond_ok(req->fd);
  atomic_store(&deaf, 1);
}
#endif

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

  long long checked = mono_seconds(), reported = -1000000;   /* (the first is always said) */
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
    } else if(ready > 0 || mono_seconds() - checked >= LISTEN_CHECK_SECONDS) {
      /* Also when nothing seems to arrive: a socket broken by rest mode
         may not say so to poll(). */
      checked = mono_seconds();
      fd = accept(srv, NULL, NULL);
      if(fd < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNABORTED) {
        broken = 1;
        err = errno;
      }
    }
    if(broken) {
      if(mono_seconds() - reported >= 60) {
        log_msg("web: the listening socket stopped working (%s, errno %d), as rest mode can do; listening again",
                strerror(err), err);
      }
      atomic_store(&listen_fd, -1);
      close(srv);
      crumb(CRUMB_LISTEN, "listening again");
      srv = web_listen(0);
      atomic_store(&listen_fd, srv);
      if(mono_seconds() - reported >= 60) log_msg("web: listening on port %d again", WEB_PORT);
      reported = mono_seconds();
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
