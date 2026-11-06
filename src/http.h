/* http.c: the manager's web server: a listener thread hands each connection to one of WEB_WORKERS workers, which
   reads the request and passes it to route() (routes.c). What a handler answers with is here. */
#ifndef PS5_THEME_HTTP_H
#define PS5_THEME_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "access.h"

#define IO_CHUNK (64 * 1024)   /* a worker's buffer for bodies and files */

typedef struct request {
  int  fd;
  int  slot;           /* crash breadcrumb slot of the worker */
  char *io;            /* IO_CHUNK bytes owned by the worker */
  char method[8];
  char path[256];
  char query[2048];
  long content_length;
  int  has_token;      /* it carries "X-PS5-Theme: 1" (routes.c) */
  int  gzip_ok;        /* the browser takes gzip (Accept-Encoding) */
  char if_none_match[96];   /* the version of a response it already has (If-None-Match) */
  char host[256];      /* the name the browser used for the console (Host), if it sent one */
  int  has_host;
  char cookie[512];    /* the device's key, if it has one (Cookie) */
  char ua[200];        /* what browser it is (User-Agent), to name an allowed device */
  access_who_t who;    /* who is asking (access_gate) */
  char *body;          /* bytes of the body already read with the headers */
  size_t body_len;
} request_t;

void *web_server_thread(void *arg);
void  route(request_t *req);                  /* routes.c */
void  lan_address(char *out, size_t size);    /* the console's address on its network, "" if none */

/* A growing text buffer for JSON answers; a failed allocation is remembered and the answer is an error. */
typedef struct strbuf {
  char  *data;
  size_t len, cap;
  int    failed;
} strbuf_t;
void sb_append(strbuf_t *sb, const char *s, size_t n);
#define SB_LIT(sb, lit) sb_append((sb), (lit), sizeof(lit) - 1)
void sb_printf(strbuf_t *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_json_str(strbuf_t *sb, const char *s);   /* a JSON string, quoted and escaped */
void sb_base64(strbuf_t *sb, const uint8_t *d, size_t n);

/* Answers. Each closes the exchange (Connection: close). */
int  send_all(int fd, const void *data, size_t len);   /* 0 once all is sent */
void respond(int fd, int status, const char *ctype, const void *body, size_t len);
void respond_json(int fd, int status, strbuf_t *sb);    /* frees sb */
void respond_error(int fd, int status, const char *msg);
void respond_ok(int fd);
void respond_icon(int fd, const char *ctype, const void *body, size_t len);
void respond_file(request_t *req, const char *path);
void respond_page_asset(request_t *req, int which, const char *ctype);   /* 0 the page, 1 its encoder, 2 the logo */

/* A query parameter, URL-decoded: 0 if present; -1 if not; -2 if longer than `size` (then never a cut-down value). */
int  get_param(const char *query, const char *key, char *out, size_t size);
/* An optional whole number within [lo, hi]: 0 if absent (*present 0) or valid (*out set); -1 if present but not. */
int  int_param(const char *query, const char *key, int lo, int hi, int *out, int *present);

#ifdef TEST_HOOKS
void http_break_listener(request_t *req);   /* the listening socket stops working, as rest mode can make it */
void http_deafen(request_t *req);           /* the web server stops answering for good, as 1.49's did */
#endif

#endif
