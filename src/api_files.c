/* A theme's files (files/<system path>), each checked against the console's own before it goes in, and the
   pictures its textures were made from (sources/). */

#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include "api.h"
#include "config.h"
#include "fsguard.h"
#include "http.h"
#include "log.h"
#include "media.h"
#include "policy.h"
#include "status.h"
#include "store.h"
#include "theme.h"

/* Receive, verify, then atomically place one replacement file. */
void
api_put_file(request_t *req) {
  char id[128], asset[512], dst[PATH_MAX], part[PATH_MAX + 24], msg[256], crc_hex[9];
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

  part_name(part, sizeof(part), dst);   /* (its folder is made as it is placed: see place_upload) */
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

/* Check a theme's file against the system file again, as applying it would
   (files made by an older builder can fall outside what the console plays). */
void
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

void
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

/* Pictures and textures are made from the user's picture (fitted, cropped, padded), so their original can be kept. */
int
source_kind_ok(const char *asset) {
  int k = asset_kind(asset);
  return k == KIND_TEXTURE || k == KIND_IMAGE || k == KIND_SVG;
}

void
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

void
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
  part_name(part, sizeof(part), dst);   /* (its folder is made as it is placed: see place_upload) */
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

void
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

/* The theme's files (paths as the page lists them) whose original picture is kept. */
static void
sources_in(strbuf_t *sb, const char *id, const char *root, const char *rel, int *first, int depth) {
  if(depth > 32) return;
  walk_paths_t *w = malloc(sizeof(*w));
  if(!w) return;
  char *dir = w->dir, *child_rel = w->child_rel, *child = w->child, *kept = w->kept;
  snprintf(dir, PATH_MAX, "%s%s", root, rel);
  DIR *d = opendir(dir);
  if(!d) { free(w); return; }
  struct dirent *e;
  while((e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    struct stat st;
    if(snprintf(child_rel, PATH_MAX, "%s/%s", rel, e->d_name) >= PATH_MAX) continue;
    snprintf(child, PATH_MAX, "%s%s", root, child_rel);
    if(lstat(child, &st)) continue;
    if(S_ISDIR(st.st_mode)) sources_in(sb, id, root, child_rel, first, depth + 1);
    else if(S_ISREG(st.st_mode) && asset_path_valid(child_rel) && source_kind_ok(child_rel) &&
            !source_path(id, child_rel, kept, PATH_MAX) && !access(kept, F_OK)) {
      if(!*first) SB_LIT(sb, ",");
      *first = 0;
      sb_json_str(sb, child_rel);
    }
  }
  closedir(d);
  free(w);
}

void
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
void
api_delete_sources(request_t *req) {
  char id[128], dir[PATH_MAX];

  get_param(req->query, "id", id, sizeof(id));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  themes_lock();
  snprintf(dir, sizeof(dir), "%s/%s/sources", THEMES_DIR, id);
  fs_rm_rf_under(THEMES_DIR, dir);
  drop_snapshot_part(id, "sources");
  themes_unlock();
  respond_ok(req->fd);
}
