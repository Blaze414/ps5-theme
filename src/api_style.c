/* A theme's icon style (icon-style/), kept so the plugin can style games and apps installed later. */

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "api.h"
#include "autostyle.h"
#include "config.h"
#include "fsguard.h"
#include "http.h"
#include "log.h"
#include "media.h"
#include "status.h"
#include "store.h"
#include "theme.h"

/* ---- A theme's icon style, for games and apps installed later (autostyle.c) ----

   The page keeps the style's template with the theme when it styles the icons: base.png (the backdrop with the
   shape's shadow), mask.png (the shape), known.txt (the titles installed then, perhaps none) and style.ini (auto,
   scale). Each upload is answered with the size and checksum stored, as every upload is. The plugin's background
   styler reads them, and lists the titles it styled in made.txt, which the page reads back.

   own/<title>.png: for an icon you changed by hand and then styled, your picture as it was, so styling again puts
   it in the shape again (never a styled icon inside another), and removing the style puts it back.

   styled.txt: the titles the page's style made (installed or not): one installed again later gets the style as it
   is then, by the plugin (autostyle.c). */
#define STYLE_TEMPLATE_MAX (8u << 20)
#define STYLE_KNOWN_MAX    (128u << 10)
#define STYLE_OWN_MAX      4096   /* pictures listed */

/* A title ID as the console's folders name them: four capital letters and five digits, or letters and digits. */
int
style_title_ok(const char *t) {
  if(strlen(t) != 9) return 0;
  for(const char *c = t; *c; c++) if(!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9'))) return 0;
  return 1;
}

static void
style_path(const char *id, const char *file, char *out, size_t size) {
  snprintf(out, size, "%s/%s/icon-style%s%s", THEMES_DIR, id, file[0] ? "/" : "", file);
}

void
api_put_icon_style(request_t *req) {
  char id[128], part_kind[16], title[16], dst[PATH_MAX], part[PATH_MAX + 24], crc_hex[9];
  uint32_t crc;
  uint64_t size = 0;
  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "part", part_kind, sizeof(part_kind));
  get_param(req->query, "title", title, sizeof(title));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  int known = !strcmp(part_kind, "known") || !strcmp(part_kind, "styled"), own = !strcmp(part_kind, "own");
  int picture = !strcmp(part_kind, "base") || !strcmp(part_kind, "mask");
  if(!known && !picture && !own) return respond_error(req->fd, 400, "part: base, mask, known, styled or own");
  if(own && !style_title_ok(title)) return respond_error(req->fd, 400, "title must be a title ID such as PPSA01234");
  if(req->content_length <= 0) return respond_error(req->fd, 400, "empty upload");
  if((uint64_t)req->content_length > (known ? STYLE_KNOWN_MAX : STYLE_TEMPLATE_MAX)) return respond_error(req->fd, 413, "too large");
  char name[32];
  if(own) snprintf(name, sizeof(name), "own/%s.png", title);
  else snprintf(name, sizeof(name), "%s.%s", part_kind, known ? "txt" : "png");
  style_path(id, name, dst, sizeof(dst));
  part_name(part, sizeof(part), dst);   /* (its folder is made as it is placed: see place_upload) */
  if(receive_upload(req, part, &crc, crc_hex)) return;
  size_t len = 0;
  uint8_t *b = NULL;
  int fd = open(part, O_RDONLY);
  if(fd >= 0) {
    b = malloc((size_t)req->content_length);
    while(b && len < (size_t)req->content_length) { ssize_t n = read(fd, b + len, (size_t)req->content_length - len); if(n <= 0) break; len += (size_t)n; }
    close(fd);
  }
  const char *bad = !b || len != (size_t)req->content_length ? "could not read it back" : NULL;
  if(!bad && picture) {
    uint32_t w, h;
    uint8_t *px = png_decode_rgba(b, len, &w, &h);
    if(!px || w != h || w < 128 || w > 1024) bad = "a square 8-bit PNG of 128 to 1024 pixels";
    free(px);
  }
  if(!bad && own) {   /* read back as the page reads it: any 8-bit PNG this decodes, 16 to 2048 pixels a side */
    uint32_t w = 0, h = 0;
    uint8_t *px = png_decode_rgba(b, len, &w, &h);
    if(!px || w < 16 || h < 16) bad = "an 8-bit PNG of 16 to 2048 pixels a side";
    free(px);
  }
  for(size_t i = 0, line = 0; !bad && known && i < len; i++) {   /* title IDs, one a line */
    if(b[i] == '\n') { if(line && line != 9) bad = "title IDs, one a line"; line = 0; continue; }
    if(!((b[i] >= 'A' && b[i] <= 'Z') || (b[i] >= '0' && b[i] <= '9')) || ++line > 9) bad = "title IDs, one a line";
  }
  free(b);
  if(bad) { fs_unlink(part); return respond_error(req->fd, 422, bad); }
  /* The template is made from your choices (as the background's preview is), not a change of yours for Discard; your
     picture kept for an icon is part of styling it, which is. */
  if(place_upload(req, id, part, dst, crc, &size, own, (uint64_t)req->content_length)) return;

  strbuf_t sb = {0};   /* what was stored, as every upload answers: the page checks it against what it sent */
  sb_printf(&sb, "{\"ok\":true,\"size\":%llu,\"crc32\":\"%s\"}", (unsigned long long)size, crc_hex);
  log_msg("theme %s: icon style %s%s%s kept (%llu bytes, crc %s, verified)", id, part_kind, own ? " picture of " : "", own ? title : "",
          (unsigned long long)size, crc_hex);
  respond_json(req->fd, 200, &sb);
}

/* A styling run in the manager, said on the console as an apply is, so whoever is at the TV knows what
   is going on: as it starts, as it goes (at most once every STYLE_NOTE_SECONDS: notifications queue up and each
   stays a few seconds) and as it ends. The words are made here from counts and title IDs, never taken from the
   request. A run for games installed since (`auto`) ends with the apply that follows, which says it. */
#define STYLE_NOTE_SECONDS 8
static atomic_llong style_noted;   /* when the last of a run's notes was said */

static void
style_note(request_t *req, const char *note) {
  char v[16], title[16] = "", name[96] = "";
  int n = 0, total = 0, failed = 0, is_auto = 0;
  if(!get_param(req->query, "n", v, sizeof(v))) n = atoi(v);
  if(!get_param(req->query, "total", v, sizeof(v))) total = atoi(v);
  if(!get_param(req->query, "failed", v, sizeof(v))) failed = atoi(v);
  if(!get_param(req->query, "auto", v, sizeof(v))) is_auto = !strcmp(v, "1");
  if(!get_param(req->query, "title", title, sizeof(title)) && title[0] && !style_title_ok(title)) {
    return respond_error(req->fd, 400, "title must be a title ID such as PPSA01234");
  }
  if(total < 1 || total > 100000 || n < 0 || n > total || failed < 0 || failed > total) {
    return respond_error(req->fd, 400, "n, total and failed: counts, n and failed at most total");
  }
  if(title[0]) title_name(title, name, sizeof(name));
  long long now = mono_seconds();
  if(!strcmp(note, "start")) {
    atomic_store(&style_noted, now);
    if(is_auto && total == 1 && name[0]) notify("ps5-theme: styling the icon of %s, installed since the theme's icon style was used...", name);
    else if(is_auto) notify("ps5-theme: styling the icons of %d games and apps installed since the theme's icon style was used...", total);
    else notify("ps5-theme: styling %d icon%s in the manager...", total, total == 1 ? "" : "s");
  } else if(!strcmp(note, "step")) {
    long long last = atomic_load(&style_noted);
    if(now - last < STYLE_NOTE_SECONDS || !atomic_compare_exchange_strong(&style_noted, &last, now)) return respond_ok(req->fd);
    notify("ps5-theme: styling icons: %d of %d...", n, total);
  } else if(!strcmp(note, "done")) {
    atomic_store(&style_noted, 0);
    int made = n - failed;
    if(is_auto && !failed && made) return respond_ok(req->fd);   /* (the apply that follows says it, by name) */
    if(!made) notify("ps5-theme: no icon could be styled. See the manager.");
    else notify("ps5-theme: %d icon%s styled%s%s.%s", made, made == 1 ? "" : "s", failed ? ", " : "",
                failed ? (failed == 1 ? "1 could not be" : "some could not be") : "",
                is_auto ? "" : " Apply the theme to see them.");
  } else if(!strcmp(note, "stopped")) {
    atomic_store(&style_noted, 0);
    notify("ps5-theme: styling stopped: %d of %d icons styled; those keep the style.", n - failed, total);
  } else {
    return respond_error(req->fd, 400, "note: start, step, done or stopped");
  }
  respond_ok(req->fd);
}

void
api_post_icon_style(request_t *req) {
  char id[128], dir[PATH_MAX], path[PATH_MAX], line[64], forget[16], note[16];
  int on, has_on, scale, has_scale, clear, has_clear;
  get_param(req->query, "id", id, sizeof(id));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(!get_param(req->query, "note", note, sizeof(note)) && note[0]) return style_note(req, note);
  if(!get_param(req->query, "forget", forget, sizeof(forget)) && forget[0]) {   /* one title's kept picture: you changed its icon */
    if(!style_title_ok(forget)) return respond_error(req->fd, 400, "forget must be a title ID such as PPSA01234");
    char name[32];
    snprintf(name, sizeof(name), "own/%s.png", forget);
    style_path(id, name, path, sizeof(path));
    themes_lock();
    int rc = !access(path, F_OK) && fs_unlink(path);
    themes_unlock();
    if(rc) return respond_error(req->fd, 500, "could not forget the picture");
    return respond_ok(req->fd);
  }
  if(int_param(req->query, "auto", 0, 1, &on, &has_on) || int_param(req->query, "scale", 40, 100, &scale, &has_scale) ||
     int_param(req->query, "clear", 0, 1, &clear, &has_clear)) {
    return respond_error(req->fd, 400, "auto 0 or 1, scale 40 to 100, clear 0 or 1");
  }
  style_path(id, "", dir, sizeof(dir));
  style_path(id, "style.ini", path, sizeof(path));
  char cur[16];
  themes_lock();   /* read and written together: two changes at once never lose one */
  if(!has_on) on = !read_ini_value(path, "auto", cur, sizeof(cur)) && !strcmp(cur, "1");
  if(!has_scale) scale = !read_ini_value(path, "scale", cur, sizeof(cur)) ? atoi(cur) : 72;
  int n = snprintf(line, sizeof(line), "auto=%d\nscale=%d\n", on, scale);
  int rc = fs_mkdirs(dir) || fs_write_file(path, line, (size_t)n);
  if(!rc && has_clear && clear) {
    /* The lists of titles styled, and your pictures kept for them: back in the theme now, except for a title the
       theme still has an icon for (one not installed when the style was removed, whose picture can only go back
       once it is): its picture stays until then. */
    style_path(id, "made.txt", path, sizeof(path));
    if(!access(path, F_OK)) rc = fs_unlink(path);
    style_path(id, "styled.txt", path, sizeof(path));
    if(!rc && !access(path, F_OK)) rc = fs_unlink(path);
    style_path(id, "own", path, sizeof(path));
    DIR *d = !rc ? opendir(path) : NULL;
    struct dirent *e;
    int left = 0;
    while(d && (e = readdir(d))) {
      char t[10], icon[PATH_MAX], pic[PATH_MAX];
      size_t n = strlen(e->d_name);
      if(n != 13 || strcmp(e->d_name + 9, ".png")) { left += e->d_name[0] != '.'; continue; }
      memcpy(t, e->d_name, 9);
      t[9] = 0;
      snprintf(icon, sizeof(icon), "%s/%s/files" APPMETA_DIR "/%s/icon0.png", THEMES_DIR, id, t);
      snprintf(pic, sizeof(pic), "%s/%s", path, e->d_name);
      if(style_title_ok(t) && !access(icon, F_OK)) { left++; continue; }
      if(fs_unlink(pic)) rc = -1;
    }
    if(d) closedir(d);
    if(!rc && !left && fs_is_dir(path)) rc = fs_rmdir(path);
  }
  themes_unlock();
  if(rc) return respond_error(req->fd, 500, "could not keep the icon style");
  log_msg("theme %s: icon style for titles installed later %s (size %d%%)%s", id, on ? "on" : "off", scale, has_clear && clear ? ", its list cleared" : "");
  respond_ok(req->fd);
}

void
api_get_icon_style(request_t *req) {
  char id[128], path[PATH_MAX], cur[16], line[64], title[16];
  get_param(req->query, "id", id, sizeof(id));
  if(!theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  if(!get_param(req->query, "own", title, sizeof(title)) && title[0]) {   /* your picture kept for a title */
    if(!style_title_ok(title)) return respond_error(req->fd, 400, "own must be a title ID such as PPSA01234");
    char name[32];
    snprintf(name, sizeof(name), "own/%s.png", title);
    style_path(id, name, path, sizeof(path));
    return respond_file(req, path);
  }
  if(!get_param(req->query, "part", title, sizeof(title)) && title[0]) {   /* the template as kept: the page tells a styled icon by it */
    if(strcmp(title, "base") && strcmp(title, "mask")) return respond_error(req->fd, 400, "part: base or mask");
    char name[16];
    snprintf(name, sizeof(name), "%s.png", title);
    style_path(id, name, path, sizeof(path));
    return respond_file(req, path);
  }
  style_path(id, "style.ini", path, sizeof(path));
  int on = !read_ini_value(path, "auto", cur, sizeof(cur)) && !strcmp(cur, "1");
  int scale = !read_ini_value(path, "scale", cur, sizeof(cur)) ? atoi(cur) : 0;
  char base[PATH_MAX], mask[PATH_MAX], known[PATH_MAX], styled[PATH_MAX];
  style_path(id, "base.png", base, sizeof(base));
  style_path(id, "mask.png", mask, sizeof(mask));
  style_path(id, "known.txt", known, sizeof(known));
  style_path(id, "styled.txt", styled, sizeof(styled));
  strbuf_t sb = {0};   /* the template is whole with all three: the styler uses none of them without the others */
  sb_printf(&sb, "{\"auto\":%s,\"scale\":%d,\"template\":%s,\"styledList\":%s,\"made\":[", on ? "true" : "false", scale,
            !access(base, F_OK) && !access(mask, F_OK) && !access(known, F_OK) ? "true" : "false",
            !access(styled, F_OK) ? "true" : "false");
  style_path(id, "made.txt", path, sizeof(path));
  FILE *f = fopen(path, "r");
  for(int first = 1, k = 0; f && k < 4096 && fgets(line, sizeof(line), f); k++) {
    line[strcspn(line, "\r\n")] = 0;
    if(!line[0]) continue;
    if(!first) SB_LIT(&sb, ",");
    first = 0;
    sb_json_str(&sb, line);
  }
  if(f) fclose(f);
  SB_LIT(&sb, "],\"own\":[");
  style_path(id, "own", path, sizeof(path));
  DIR *d = opendir(path);
  struct dirent *e;
  for(int first = 1, k = 0; d && k < STYLE_OWN_MAX && (e = readdir(d)); ) {
    size_t n = strlen(e->d_name);
    if(n != 13 || strcmp(e->d_name + 9, ".png")) continue;   /* <title>.png; never a .part */
    char t[10];
    memcpy(t, e->d_name, 9);
    t[9] = 0;
    if(!style_title_ok(t)) continue;
    if(!first) SB_LIT(&sb, ",");
    first = 0;
    k++;
    sb_json_str(&sb, t);
  }
  if(d) closedir(d);
  /* Titles still installing (something in their folders changed lately): seconds until they count as installed.
     The page styles none of them, as the plugin doesn't, until then. */
  sb_printf(&sb, "],\"settle\":%d,\"waiting\":{", autostyle_settle());
  d = opendir(APPMETA_DIR);
  for(int first = 1, k = 0; d && k < STYLE_OWN_MAX && (e = readdir(d)); ) {
    int left = autostyle_wait(e->d_name);
    if(left <= 0) continue;
    sb_printf(&sb, "%s", first ? "" : ",");
    first = 0;
    k++;
    sb_json_str(&sb, e->d_name);
    sb_printf(&sb, ":%d", left);
  }
  if(d) closedir(d);
  SB_LIT(&sb, "}}");
  respond_json(req->fd, 200, &sb);
}
