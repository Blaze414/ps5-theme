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
     - never during an apply, and never while a game or app is being played: its process (named after the program
       it runs, eboot.bin) used the processor between two looks. The console suspends a game left on the home
       screen, and a suspended game uses none, so it holds nothing up; the styling, and the apply after it, wait
       only while one is played. Each title is tried once per run of the plugin.

   The title's own icon (an 8-bit PNG, png_decode_rgba) is set into the shape over the template, then written as
   each of its icon files is (png_encode_like, texture_encode_like: the console's own form), and checked exactly as
   an upload is (asset_validate) before it goes into the theme. Then the theme is applied again, and the console
   says so. A title whose icons can't all be made this way (a compressed texture, a picture of another kind) is left
   for the manager's page, which handles every form. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>

#include "platform.h"
#include "theme.h"


#define AUTOSTYLE_POLL     60
#define AUTOSTYLE_SETTLE   600
#define AUTOSTYLE_TRIED    512
#define STYLE_DIR          "icon-style"
#define TEMPLATE_MAX_BYTES (8u << 20)
#define ICON_MAX_BYTES     (32u << 20)

#define GAME_PROCESS       "eboot.bin"   /* a game's or app's process, named after the program it runs */
#define GAMES_SEEN         16
#define PLAYED_PERCENT     2             /* of one processor core between two looks; a game being played uses far more */

static char tried[AUTOSTYLE_TRIED][16];
static int ntried;
static char owed[128];          /* a theme to apply again once no game is being played */
static char owed_names[160];    /* and the titles styled for it, for the console's message */
static int owed_count;
static int said_waiting;
static char said_template[128]; /* the theme whose incomplete template was last logged (said once, not each minute) */

/* (the styler's thread alone uses these) */
static plat_proc_t games_seen[GAMES_SEEN];
static int ngames_seen;
static struct timespec games_seen_at;

/* Whether a game or app is being played: a process of one used more than PLAYED_PERCENT of a processor core since
   the last look. A game suspended on the home screen uses none. One seen for the first time, or whose time can't
   be read, counts as played until the next look; so does every one when the process list can't be read. */
static int
game_played(void) {
  plat_proc_t now[GAMES_SEEN];
  struct timespec at;
  clock_gettime(CLOCK_MONOTONIC, &at);
  int n = plat_processes_named(GAME_PROCESS, now, GAMES_SEEN), played = n < 0;
  double us = (double)(at.tv_sec - games_seen_at.tv_sec) * 1e6 + (double)(at.tv_nsec - games_seen_at.tv_nsec) / 1e3;
  for(int i = 0; i < n && !played; i++) {
    const plat_proc_t *was = NULL;
    for(int k = 0; k < ngames_seen && !was; k++) {
      if(games_seen[k].pid == now[i].pid && games_seen[k].start == now[i].start) was = &games_seen[k];
    }
    played = !was || !now[i].cpu_us || now[i].cpu_us < was->cpu_us || us <= 0 ||
             (double)(now[i].cpu_us - was->cpu_us) * 100 >= us * PLAYED_PERCENT;
  }
  if(n >= 0) {
    memcpy(games_seen, now, sizeof(*now) * (size_t)n);
    ngames_seen = n;
  }
  games_seen_at = at;
  return played;
}

/* Apply the theme again for the titles styled, unless a game is being played: then it is owed. */
static void
apply_styled(const char *theme, int styled, const char *names, int playing) {
  if(playing) {
    if(strcmp(owed, theme)) { owed_count = 0; owed_names[0] = 0; }
    snprintf(owed, sizeof(owed), "%s", theme);
    snprintf(owed_names + strlen(owed_names), sizeof(owed_names) - strlen(owed_names), "%s%s", owed_names[0] ? ", " : "", names);
    owed_count += styled;
    log_msg("icon style: a game is being played; theme %s is applied again once it is closed or left on the home screen", theme);
    return;
  }
  if(reapply_active(theme)) {   /* another theme was chosen meanwhile: it is applied without these */
    log_msg("icon style: theme %s is no longer the one chosen; its styled icons wait for its next apply", theme);
    owed[0] = 0; owed_names[0] = 0; owed_count = 0;
    return;
  }
  notify("ps5-theme: styled the icon of %d new %s (%s). Rest mode shows %s on the home row.", styled,
         styled == 1 ? "game or app" : "games and apps", names, styled == 1 ? "it" : "them");
  owed[0] = 0;
  owed_names[0] = 0;
  owed_count = 0;
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

static time_t
newest(const char *path, time_t t) {
  struct stat st;
  return !stat(path, &st) && st.st_mtime > t ? st.st_mtime : t;
}

/* Whether nothing in the title's folders has changed for `settle` seconds, and its icon is there. The game's own
   package (app.pkg, or a patch's) grows while it downloads without its folder changing, so it is looked at too. */
static int
settled(const char *title, int settle) {
  char p[PATH_MAX];
  time_t t = 0;
  struct stat st;
  snprintf(p, sizeof(p), "%s/%s/icon0.png", APPMETA_DIR, title);
  if(stat(p, &st) || !S_ISREG(st.st_mode)) return 0;
  const char *const parts[] = { "", "/icon0.png", "/icon0.dds", "/param.json", "/param.sfo" };
  for(size_t i = 0; i < sizeof(parts) / sizeof(*parts); i++) {
    snprintf(p, sizeof(p), "%s/%s%s", APPMETA_DIR, title, parts[i]);
    t = newest(p, t);
    snprintf(p, sizeof(p), "%s/%s/sce_sys%s", APP_DIR, title, parts[i]);
    t = newest(p, t);
  }
  const char *const others[] = { APP_DIR "/%s", APP_DIR "/%s/app.pkg", PATCH_DIR "/%s", PATCH_DIR "/%s/patch.pkg" };
  for(size_t i = 0; i < sizeof(others) / sizeof(*others); i++) {
    snprintf(p, sizeof(p), others[i], title);
    t = newest(p, t);
  }
  return time(NULL) - t >= settle;
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

/* Make the title's icon files into the theme (each checked as an upload is). 0 when all were made. */
static int
style_title(const char *theme, const char *title, const uint8_t *base, const uint8_t *mask, uint32_t side, int scale) {
  char asset[PATH_MAX], stock[PATH_MAX], why[256];
  made_t made[4];
  int nmade = 0, rc = -1;
  size_t len;
  uint32_t iw, ih;
  uint8_t *iconfile = NULL, *icon = NULL, *styled = NULL;

  snprintf(asset, sizeof(asset), "%s/%s/icon0.png", APPMETA_DIR, title);
  stock_path(asset, stock, sizeof(stock));
  if(!(iconfile = read_file(stock, ICON_MAX_BYTES, &len)) || !(icon = png_decode_rgba(iconfile, len, &iw, &ih))) {
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
      snprintf(m->part, sizeof(m->part), "%s/%s/files%s.style.part", THEMES_DIR, theme, asset);
      char dir[PATH_MAX];
      snprintf(dir, sizeof(dir), "%s", m->dst);
      *strrchr(dir, '/') = 0;
      if(fs_room_for(bl)) snprintf(why, sizeof(why), "the console's storage is nearly full");
      else if(!theme_present(theme)) snprintf(why, sizeof(why), "the theme was deleted meanwhile");   /* never a folder made again */
      else if(fs_mkdirs(dir) || fs_write_file(m->part, bytes, bl)) snprintf(why, sizeof(why), "could not be written");
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
  for(int i = 0; i < nmade && placed; i++) if(fs_rename(made[i].part, made[i].dst)) placed = 0;
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

/* One look at the titles: style the ones that are new and settled, then apply the theme again. */
static void
autostyle_pass(int settle) {
  char active[128], applied[128], busy[128], dir[PATH_MAX], path[PATH_MAX], value[16];
  int playing = game_played();   /* looked at every pass: a game's use of the processor is between two looks */
  read_active(active, sizeof(active));
  if(!applied_theme(applied, sizeof(applied)) || applying_theme(busy, sizeof(busy)) || strcmp(active, applied)) return;
  if(owed[0]) {   /* an apply owed from while a game was played */
    if(playing) return;
    char names[160];
    snprintf(names, sizeof(names), "%s", owed_names);
    if(!strcmp(owed, applied)) apply_styled(applied, owed_count, names, 0);
    else owed[0] = 0;   /* another theme since: its own apply covered it */
    return;
  }
  snprintf(dir, sizeof(dir), "%s/%s/%s", THEMES_DIR, applied, STYLE_DIR);
  snprintf(path, sizeof(path), "%s/style.ini", dir);
  if(read_ini_value(path, "auto", value, sizeof(value)) || strcmp(value, "1")) return;
  int scale = !read_ini_value(path, "scale", value, sizeof(value)) ? atoi(value) : 0;
  if(scale < 40 || scale > 100) return;

  char known_path[PATH_MAX], made_path[PATH_MAX];
  size_t known_len = 0, made_len = 0;
  snprintf(known_path, sizeof(known_path), "%s/known.txt", dir);
  snprintf(made_path, sizeof(made_path), "%s/made.txt", dir);
  uint8_t *known = read_file(known_path, 256u << 10, &known_len), *made = read_file(made_path, 256u << 10, &made_len);
  if(!known) {   /* without the titles installed when the style was used, every title would look new */
    if(strcmp(said_template, applied)) {
      log_err("icon style: theme %s's style template isn't whole; open the theme's Games and apps in the manager to keep it again", applied);
      snprintf(said_template, sizeof(said_template), "%s", applied);
    }
    free(made);
    return;
  }
  said_template[0] = 0;
  DIR *d = opendir(APPMETA_DIR);
  if(!d) { free(known); free(made); return; }
  char titles[16][16];
  int n = 0;
  struct dirent *e;
  while((e = readdir(d)) && n < 16) {
    if(!title_id_valid(e->d_name)) continue;
    int seen = 0;
    for(int i = 0; i < ntried && !seen; i++) seen = !strcmp(tried[i], e->d_name);
    if(seen) continue;
    char own[PATH_MAX];
    struct stat st;
    snprintf(own, sizeof(own), "%s/%s/files%s/%s/icon0.png", THEMES_DIR, applied, APPMETA_DIR, e->d_name);
    if((known && listed((const char *)known, known_len, e->d_name)) || (made && listed((const char *)made, made_len, e->d_name)) ||
       !lstat(own, &st)) continue;
    if(!settled(e->d_name, settle)) continue;   /* (not marked tried: looked at again next time) */
    snprintf(titles[n++], sizeof(titles[0]), "%s", e->d_name);
  }
  closedir(d);
  free(known);
  free(made);
  if(!n) return;
  if(playing) {   /* nothing while a game is being played: the titles are styled once it is closed or suspended */
    if(!said_waiting) {
      log_msg("icon style: %s%s waits while a game is being played (one left on the home screen doesn't count)",
              titles[0], n > 1 ? " and others" : "");
    }
    said_waiting = 1;
    return;
  }
  said_waiting = 0;

  size_t bl, ml;
  uint32_t bw, bh, mw, mh;
  uint8_t *bf = NULL, *mf = NULL, *base = NULL, *mask = NULL;
  snprintf(path, sizeof(path), "%s/base.png", dir);
  if((bf = read_file(path, TEMPLATE_MAX_BYTES, &bl))) base = png_decode_rgba(bf, bl, &bw, &bh);
  snprintf(path, sizeof(path), "%s/mask.png", dir);
  if((mf = read_file(path, TEMPLATE_MAX_BYTES, &ml))) mask = png_decode_rgba(mf, ml, &mw, &mh);
  free(bf);
  free(mf);
  if(!base || !mask || bw != bh || mw != bw || mh != bh) {
    log_err("icon style: theme %s's style template can't be read; open the theme's Games and apps in the manager to keep it again", applied);
    free(base);
    free(mask);
    for(int i = 0; i < n && ntried < AUTOSTYLE_TRIED; i++) snprintf(tried[ntried++], sizeof(tried[0]), "%s", titles[i]);
    return;
  }
  int styled = 0;
  char names[160] = "";
  for(int i = 0; i < n; i++) {
    if(ntried < AUTOSTYLE_TRIED) snprintf(tried[ntried++], sizeof(tried[0]), "%s", titles[i]);
    if(style_title(applied, titles[i], base, mask, bw, scale)) continue;
    remember_made(dir, titles[i]);
    log_msg("icon style: styled %s's icon in theme %s", titles[i], applied);
    snprintf(names + strlen(names), sizeof(names) - strlen(names), "%s%s", styled ? ", " : "", titles[i]);
    styled++;
  }
  free(base);
  free(mask);
  if(!styled) return;
  apply_styled(applied, styled, names, game_played());   /* looked at again: a game may have started meanwhile */
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
