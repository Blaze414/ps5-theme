/* Mix and match: a new theme made of parts of others. */

#include <errno.h>
#include <fcntl.h>
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
#include "policy.h"
#include "status.h"
#include "store.h"
#include "theme.h"

/* Mix and match: a new theme made of parts of others, each taken from a theme
   as it is now. The body names what goes in, one per line:
     file\t<theme>\t<system path>     a file of that theme (its files/<path>)
     music\t<theme>\t<song 1-8>       a song of its home music (music.wav, music2.wav …)
     source\t<theme>\t<system path>   the picture one of its textures was made from
     settings\t<theme>                its home music settings (from its theme.ini)
     preview\t<theme>                 its library picture made from the background
     style\t<theme>                   its icon style's template (icon-style/: base.png, mask.png, known.txt,
                                      style.ini) and the picture behind its icons (icon-backdrop.jpg), those it has
     own\t<theme>\t<title>            your picture kept with its icon style for that title (icon-style/own/)
   The page decides which files make up each part. Every path is checked as an
   upload's is (a replaceable system file of this console) and must lie inside
   its theme's folder; the files are copied as they are, as Duplicate copies
   them, and checked against the console's own again when the theme is
   applied. The theme is built under a hidden name and renamed into place only
   once everything is in, so a mix that fails leaves nothing behind. */
#define MIX_MAX_BYTES (1u << 20)
#define MIX_MAX_LINES 8192

static int
mix_copy(const char *from, const char *dst) {
  char dir[PATH_MAX];
  struct stat st;
  snprintf(dir, sizeof(dir), "%s", dst);
  *strrchr(dir, '/') = 0;
  errno = 0;
  if(lstat(from, &st) || !S_ISREG(st.st_mode)) { errno = ENOENT; return -1; }
  return fs_mkdirs(dir) || fs_copy_file(from, dst) ? -1 : 0;   /* ENOSPC: no room left (fs_room_for) */
}

/* One line of a mix into `tmp`: 0, or -1 with `why`. */
static int
mix_line(char *line, const char *id, const char *tmp, music_settings_t *m, int *settings, int *files,
         char *why, size_t len) {
  char *kind = line, *from = strchr(line, '\t'), *arg = NULL, src[PATH_MAX], dst[PATH_MAX], root[PATH_MAX];
  if(from) { *from++ = 0; arg = strchr(from, '\t'); }
  if(arg) *arg++ = 0;
  if(!from || !theme_exists(from) || !strcmp(from, id)) {
    snprintf(why, len, "no such theme to take it from");
    return -1;
  }
  snprintf(root, sizeof(root), "%s/%s", THEMES_DIR, from);
  if(!strcmp(kind, "file") || !strcmp(kind, "source")) {
    int file = kind[0] == 'f';
    if(!arg || !asset_path_valid(arg) || asset_kind(arg) == KIND_BLOCKED || (!file && !source_kind_ok(arg))) {
      snprintf(why, len, "not a replaceable system asset: %s", arg ? arg : "");
      return -1;
    }
    if(file) {
      if(snprintf(src, sizeof(src), "%s/files%s", root, arg) >= (int)sizeof(src) ||
         snprintf(dst, sizeof(dst), "%s/files%s", tmp, arg) >= (int)sizeof(dst)) {
        snprintf(why, len, "path too long");
        return -1;
      }
    } else {
      if(source_path(from, arg, src, sizeof(src))) { snprintf(why, len, "path too long"); return -1; }
      snprintf(dst, sizeof(dst), "%s/sources/%s", tmp, strrchr(src, '/') + 1);
    }
    errno = 0;
    if(!fs_read_within(src, root) || mix_copy(src, dst)) {
      if(errno == ENOSPC) snprintf(why, len, "%s", STORAGE_FULL);
      else snprintf(why, len, "%s has no %s %s", from, file ? "file" : "original of", arg);
      return -1;
    }
    if(file) (*files)++;
    return 0;
  }
  if(!strcmp(kind, "music")) {
    int slot = arg ? atoi(arg) : 0;
    char file[24];
    if(slot < 1 || slot > MUSIC_MAX_TRACKS || strspn(arg, "0123456789") != strlen(arg)) {
      snprintf(why, len, "a song is numbered 1 to %d", MUSIC_MAX_TRACKS);
      return -1;
    }
    music_path(from, slot, src, sizeof(src));
    music_source_file(slot, file, sizeof(file));
    snprintf(dst, sizeof(dst), "%s/%s", tmp, file);
    if(mix_copy(src, dst)) {
      if(errno == ENOSPC) snprintf(why, len, "%s", STORAGE_FULL);
      else snprintf(why, len, "%s has no song %d", from, slot);
      return -1;
    }
    return 0;
  }
  if(!strcmp(kind, "settings") && !arg) {
    read_music_settings(from, m);
    *settings = 1;
    return 0;
  }
  if(!strcmp(kind, "style") && !arg) {
    static const char *const parts[] = { "icon-style/base.png", "icon-style/mask.png",
                                         "icon-style/known.txt", "icon-style/style.ini", "icon-backdrop.jpg" };
    for(size_t i = 0; i < sizeof(parts) / sizeof(*parts); i++) {
      snprintf(src, sizeof(src), "%s/%s", root, parts[i]);
      snprintf(dst, sizeof(dst), "%s/%s", tmp, parts[i]);
      if(!fs_read_within(src, root)) continue;   /* (one it hasn't) */
      if(mix_copy(src, dst)) {
        snprintf(why, len, "%s", errno == ENOSPC ? STORAGE_FULL : "could not copy its icon style");
        return -1;
      }
    }
    return 0;
  }
  if(!strcmp(kind, "own") && arg) {
    if(!style_title_ok(arg)) {
      snprintf(why, len, "own: a title ID such as PPSA01234");
      return -1;
    }
    snprintf(src, sizeof(src), "%s/icon-style/own/%s.png", root, arg);
    snprintf(dst, sizeof(dst), "%s/icon-style/own/%s.png", tmp, arg);
    errno = 0;
    if(!fs_read_within(src, root) || mix_copy(src, dst)) {
      if(errno == ENOSPC) snprintf(why, len, "%s", STORAGE_FULL);
      else snprintf(why, len, "%s has no picture kept for %s", from, arg);
      return -1;
    }
    return 0;
  }
  if(!strcmp(kind, "preview") && !arg) {
    snprintf(src, sizeof(src), "%s/preview.jpg", root);
    snprintf(dst, sizeof(dst), "%s/preview.jpg", tmp);
    if(access(src, F_OK)) return 0;
    if(!mix_copy(src, dst)) return 0;
    snprintf(why, len, "%s", errno == ENOSPC ? STORAGE_FULL : "could not copy the preview picture");
    return -1;
  }
  snprintf(why, len, "unknown part %s", kind);
  return -1;
}

void
api_mix_themes(request_t *req) {
  char id[128], name[128], fw[64], tmp[PATH_MAX], dst[PATH_MAX], part[PATH_MAX + 24], crc_hex[9], why[256] = "", msg[320];
  int full = 0;
  uint32_t crc;

  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "name", name, sizeof(name));
  if(!theme_name_valid(id)) return respond_error(req->fd, 400, "id: use letters, digits, - and _");
  if(req->content_length <= 0) return respond_error(req->fd, 400, "nothing to mix");
  if((uint64_t)req->content_length > MIX_MAX_BYTES) return respond_error(req->fd, 413, "the list of parts is too long");
  if(!name[0]) snprintf(name, sizeof(name), "%s", id);
  for(char *p = name; *p; p++) if(*p == '\n' || *p == '\r' || *p == '=') *p = ' ';
  fw_string(fw, sizeof(fw));

  snprintf(tmp, sizeof(tmp), "%s/.mix-%s", THEMES_DIR, id);
  snprintf(dst, sizeof(dst), "%s/%s", THEMES_DIR, id);
  part_name(part, sizeof(part), tmp);
  if(receive_upload(req, part, &crc, crc_hex)) return;
  char *body = malloc((size_t)req->content_length + 1);
  int fd = open(part, O_RDONLY), got = 0;
  if(body && fd >= 0) {
    ssize_t r;
    while(got < req->content_length && (r = read(fd, body + got, (size_t)req->content_length - got)) > 0) got += (int)r;
  }
  if(fd >= 0) close(fd);
  fs_unlink(part);
  if(!body || got != req->content_length) {
    free(body);
    return respond_error(req->fd, 500, "could not read the list of parts");
  }
  body[got] = 0;

  themes_lock();
  if(fs_is_dir(dst)) {
    themes_unlock();
    free(body);
    return respond_error(req->fd, 409, "a theme with that id exists");
  }
  fs_rm_rf_under(THEMES_DIR, tmp);   /* left by a mix cut off earlier */
  char path[PATH_MAX], ini[256];
  snprintf(path, sizeof(path), "%s/files", tmp);
  int n = snprintf(ini, sizeof(ini), "name=%s\nfw=%s\nmusic_gain=0\n", name, fw);
  int ok = !fs_mkdirs(path);
  snprintf(path, sizeof(path), "%s/theme.ini", tmp);
  ok = ok && !fs_write_file(path, ini, n);
  if(!ok) snprintf(why, sizeof(why), "could not create the theme folder");

  music_settings_t m;
  int settings = 0, files = 0, lines = 0, at = 0;
  for(char *save = NULL, *line = strtok_r(body, "\n", &save); ok && line; line = strtok_r(NULL, "\n", &save)) {
    at++;
    line[strcspn(line, "\r")] = 0;
    if(!line[0]) continue;
    if(++lines > MIX_MAX_LINES) { snprintf(why, sizeof(why), "more than %d parts", MIX_MAX_LINES); ok = 0; break; }
    char text[64];
    snprintf(text, sizeof(text), "%.60s", line);
    if(mix_line(line, id, tmp, &m, &settings, &files, why, sizeof(why))) {
      if(!strcmp(why, STORAGE_FULL)) full = 1;   /* said as it is: no line of the list is at fault */
      else {
        snprintf(msg, sizeof(msg), "line %d (%s): %s", at, text, why);
        snprintf(why, sizeof(why), "%s", msg);
      }
      ok = 0;
    }
  }
  free(body);
  if(ok && fs_rename(tmp, dst)) { snprintf(why, sizeof(why), "could not put the theme in place"); ok = 0; }
  if(!ok) {
    fs_rm_rf_under(THEMES_DIR, tmp);
    themes_unlock();
    return respond_error(req->fd, full ? 507 : 422, why);
  }
  drop_snapshot(id);   /* left by a theme of the same id */
  if(settings && write_theme_ini(id, name, fw, &m)) log_err("theme %s: could not write its music settings", id);
  themes_unlock();
  log_msg("mixed theme %s (%s): %d part line(s), %d file(s)", id, name, lines, files);
  strbuf_t sb = {0};
  sb_printf(&sb, "{\"ok\":true,\"files\":%d}", files);
  respond_json(req->fd, 200, &sb);
}
