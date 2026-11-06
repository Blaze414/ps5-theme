/* A theme's other parts: its home music sources, its preview pictures and the editor's choices. None of them is
   applied to the console as it is. */

#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include "api.h"
#include "fsguard.h"
#include "http.h"
#include "log.h"
#include "media.h"
#include "policy.h"
#include "status.h"
#include "store.h"
#include "theme.h"

void
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

void
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

void
api_get_music(request_t *req) {
  char id[128], path[PATH_MAX];
  int slot = 1, has_slot = 0;
  get_param(req->query, "id", id, sizeof(id));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(int_param(req->query, "slot", 1, MUSIC_MAX_TRACKS, &slot, &has_slot)) return respond_error(req->fd, 400, "slot must be 1 to 8");
  music_path(id, slot, path, sizeof(path));
  respond_file(req, path);
}

/* A theme's preview picture for the library: its home screen as the console would show it, drawn by the page from
   the console's own row of games and apps (home.jpg), made from its background (preview.jpg), or chosen
   by the user (picture.jpg, shown first). None is ever applied to the console. The same store keeps the picture an
   icon style puts behind the games' and apps' icons (icon-backdrop.jpg), kept so the style can make the
   icons of games installed later. */
#define PREVIEW_MAX_BYTES (4u << 20)
#define PREVIEW_MAX_SIDE  4096

/* The pictures the page makes by itself from what the theme has: never a change of yours, so never kept for Discard. */
static int
preview_made(const char *kind) {
  return !strcmp(kind, "background") || !strcmp(kind, "home");
}

/* The picture the library shows for a theme: "picture", "home", "background" or "". */
const char *
shown_preview(const char *id, char *path, size_t size, long long *mtime) {
  static const char *kinds[] = {"picture", "home", "background"};
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

void
api_put_preview(request_t *req) {
  char id[128], kind[16], dst[PATH_MAX], part[PATH_MAX + 24], msg[160], crc_hex[9];
  uint32_t crc;
  uint64_t size = 0;

  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "kind", kind, sizeof(kind));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(preview_path(id, kind, dst, sizeof(dst))) return respond_error(req->fd, 400, "kind: background, home, picture or iconbg");
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
  /* (the background's and the home screen's are made by the page as the theme changes: only the others are yours) */
  if(place_upload(req, id, part, dst, crc, &size, !preview_made(kind), (uint64_t)req->content_length)) return;

  strbuf_t sb = {0};
  sb_printf(&sb, "{\"ok\":true,\"size\":%llu,\"crc32\":\"%s\"}", (unsigned long long)size, crc_hex);
  log_msg("theme %s: %s preview set (%llu bytes, crc %s, verified)", id, kind, (unsigned long long)size, crc_hex);
  respond_json(req->fd, 200, &sb);
}

void
api_delete_preview(request_t *req) {
  char id[128], kind[16], path[PATH_MAX];
  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "kind", kind, sizeof(kind));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(preview_path(id, kind, path, sizeof(path))) return respond_error(req->fd, 400, "kind: background, home, picture or iconbg");
  themes_lock();
  int rc = access(path, F_OK) || (!preview_made(kind) && keep_snapshot(id)) || fs_unlink(path);
  themes_unlock();
  if(rc) return respond_error(req->fd, 404, "no such preview picture");
  respond_ok(req->fd);
}

void
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

/* The editor's choices for a theme: how pictures are fitted, each Appearance
   piece's placement, which background areas and sections are on, and the
   playlist's song names. A small JSON object the page reads and writes, kept
   with the theme (copied with it, undone by Discard) and never applied to the
   console. The page checks every value it reads; this keeps out anything that
   isn't such a file. */
#define CHOICES_MAX_BYTES (64u << 10)

static int
is_blank(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* One JSON object as UTF-8 text, without control characters but tabs and line ends. */
static int
choices_ok(const char *path, char *why, size_t len) {
  struct stat st;
  int fd = open(path, O_RDONLY);
  if(fd < 0 || fstat(fd, &st) || st.st_size < 2 || st.st_size > (off_t)CHOICES_MAX_BYTES) {
    if(fd >= 0) close(fd);
    snprintf(why, len, "the choices must be 2 bytes to 64 KB of text");
    return 0;
  }
  size_t n = (size_t)st.st_size;
  unsigned char *b = malloc(n);
  int ok = b && pread(fd, b, n, 0) == (ssize_t)n;
  close(fd);
  for(size_t i = 0; ok && i < n; ) {
    unsigned c = b[i];
    if(c < 0x20) { ok = is_blank((unsigned char)c); i++; continue; }
    if(c < 0x80) { i++; continue; }
    size_t more = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
    if(!more || i + more >= n) { ok = 0; break; }
    for(size_t k = 1; k <= more; k++) if((b[i + k] & 0xc0) != 0x80) ok = 0;
    i += more + 1;
  }
  size_t first = 0, last = n;
  while(ok && first < n && is_blank(b[first])) first++;
  while(ok && last > first && is_blank(b[last - 1])) last--;
  if(ok && (last - first < 2 || b[first] != '{' || b[last - 1] != '}')) ok = 0;
  free(b);
  if(!ok) snprintf(why, len, "the choices must be one JSON object in UTF-8 text");
  return ok;
}

void
api_get_choices(request_t *req) {
  char id[128], path[PATH_MAX];
  get_param(req->query, "id", id, sizeof(id));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  choices_path(id, path, sizeof(path));
  if(access(path, F_OK)) {
    strbuf_t sb = {0};
    SB_LIT(&sb, "{}");
    return respond_json(req->fd, 200, &sb);
  }
  respond_file(req, path);
}

/* `quiet=1`: choices the page moved from its browser storage the first time
   the theme was opened, not a change for Discard to undo. */
void
api_put_choices(request_t *req) {
  char id[128], quiet[4], dst[PATH_MAX], part[PATH_MAX + 24], msg[160], crc_hex[9];
  uint32_t crc;
  uint64_t size = 0;

  get_param(req->query, "id", id, sizeof(id));
  int is_quiet = !get_param(req->query, "quiet", quiet, sizeof(quiet)) && !strcmp(quiet, "1");
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(req->content_length <= 0) return respond_error(req->fd, 400, "empty upload");
  if((uint64_t)req->content_length > CHOICES_MAX_BYTES) return respond_error(req->fd, 413, "the choices are larger than 64 KB");
  choices_path(id, dst, sizeof(dst));
  part_name(part, sizeof(part), dst);
  if(receive_upload(req, part, &crc, crc_hex)) return;
  if(!choices_ok(part, msg, sizeof(msg))) {
    fs_unlink(part);
    return respond_error(req->fd, 422, msg);
  }
  if(place_upload(req, id, part, dst, crc, &size, !is_quiet, (uint64_t)req->content_length)) return;
  strbuf_t sb = {0};
  sb_printf(&sb, "{\"ok\":true,\"size\":%llu,\"crc32\":\"%s\"}", (unsigned long long)size, crc_hex);
  respond_json(req->fd, 200, &sb);
}
