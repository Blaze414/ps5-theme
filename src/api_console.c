/* The console's own files, for the manager: the list of replaceable files, its app database, its interface icons,
   textures and sounds. Read-only. */

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <sys/stat.h>

#include "api.h"
#include "config.h"
#include "http.h"
#include "media.h"
#include "policy.h"
#include "theme.h"

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
#define CONCEPT_RUN   4096
#define CONCEPT_KEEP  512     /* of a longer run, read on with this much kept */
#define CONCEPT_BACK  256     /* how far before the folder's path its title ID may be */

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

int
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

#define MAX_RESULTS      300
#define MAX_RESULTS_ALL  5000

void
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

/* The console's app database as it is, for the manager's preview: which games and apps the home screen shows, in the
   order it shows them (most recently used first). Read-only, for allowed devices only, like every other request. */
void
api_app_db(request_t *req) {
  respond_file(req, APP_DB);
}

void
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

/* The manager draws its list of a theme's parts with the console's own icons (iconid_*), read from the console's
   copy of its main interface file: a theme's version of the file is never used for them, and nothing is written. */
#define UI_RCO "/system_ex/vsh_asset/Sce.PlayStation.PUI_UI3.rco"
void
api_ui_icon(request_t *req) {
  char id[64], stock[PATH_MAX];
  uint8_t *d;
  size_t n;
  get_param(req->query, "id", id, sizeof(id));
  if(strncmp(id, "iconid_", 7) || !id[7] || strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789_") != strlen(id)) {
    return respond_error(req->fd, 400, "not an interface icon");
  }
  stock_path(UI_RCO, stock, sizeof(stock));
  if(rco_icon(stock, id, &d, &n)) return respond_error(req->fd, 404, "no such icon on this firmware");
  static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  respond_icon(req->fd, n >= 8 && !memcmp(d, png_sig, 8) ? "image/png" : "image/svg+xml", d, n);
  free(d);
}

/* A quarter-size picture of one of the console's own textures (texture_preview), from its own copy: the manager's
   previews draw the console's artwork where a theme leaves it. Sent as its width and height (32-bit, little-endian)
   followed by RGBA pixels. */
void
api_texture_picture(request_t *req) {
  char asset[512], stock[PATH_MAX];
  uint8_t *rgba;
  uint32_t w, h;
  get_param(req->query, "path", asset, sizeof(asset));
  if(!asset_path_valid(asset) || asset_kind(asset) != KIND_TEXTURE) return respond_error(req->fd, 403, "not a system texture");
  stock_path(asset, stock, sizeof(stock));
  if(texture_preview(stock, &rgba, &w, &h)) return respond_error(req->fd, 422, "this texture can't be drawn");
  size_t n = (size_t)w * h * 4 + 8;
  uint8_t *out = malloc(n);
  if(!out) { free(rgba); return respond_error(req->fd, 500, "out of memory"); }
  for(int i = 0; i < 4; i++) { out[i] = (uint8_t)(w >> (8 * i)); out[4 + i] = (uint8_t)(h >> (8 * i)); }
  memcpy(out + 8, rgba, n - 8);
  free(rgba);
  respond_icon(req->fd, "application/octet-stream", out, n);
  free(out);
}

/* Format of a system sound, so the builder can prepare a matching source. */
void
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
void
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
