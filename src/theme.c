/* What a theme is on the console: its name and theme.ini, its home music settings, and the system files it may
   change. */

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

#include "theme.h"
#include "config.h"
#include "fsguard.h"
#include "platform.h"
#include "policy.h"

/* A replaceable asset: allowed prefix, existing regular file, sane name, and
   its real location is also inside an allowed prefix. */
int
asset_path_valid(const char *path) {
  char real[PATH_MAX];
  struct stat st;
  return path[0] == '/' && strlen(path) < 512 && !strchr(path, '\\') &&
         target_allowed(path) && !stat(path, &st) && S_ISREG(st.st_mode) &&
         realpath(path, real) && (target_allowed(real) || fs_read_within(real, STAGE_DIR));
}

/* Themable folders: ShellUI's React Native apps and its own VSH assets
   (home screen backgrounds, sounds), and installed titles' metadata (their
   icons), including the sce_sys folders of their installations, which most
   PS5 games' records point to, and the pictures downloaded for them from
   PlayStation Network, the console's text fonts, and the Appearance themes'
   textures in /system/vsh_asset. What may change inside them is decided by
   policy.c; code and unknown types never can. */
static const char *const allowed_prefixes[] = {
  "/system_ex/rnps/apps/",
  "/mnt/rnps/apps/",
  "/system_ex/vsh_asset/",
  APPMETA_DIR "/",
  APP_DIR "/",              /* only <title>/sce_sys/ (app_path_ok) */
  CATALOG_APPMETA_DIR "/",
  CONCEPTMETA_DIR "/",
  FONT_DIR "/",             /* only in the home screen's own views (home_view_only) */
  SYS_ASSET_DIR "/",        /* the PlayStation 1-4 Appearance themes' textures: like /system_ex/vsh_asset */
};
#define NPREFIXES (sizeof(allowed_prefixes) / sizeof(*allowed_prefixes))

void
stock_path(const char *asset, char *out, size_t size) {
  char fw[16], kept[PATH_MAX];
  struct stat st;
  fw_string(fw, sizeof(fw));
  if(snprintf(kept, sizeof(kept), "%s/%s%s", STOCK_DIR, fw, asset) < (int)sizeof(kept) &&
     !lstat(kept, &st) && S_ISREG(st.st_mode)) {
    snprintf(out, size, "%s", kept);
  } else {
    snprintf(out, size, "%s", asset);
  }
}

int
music_loop_valid(const char *loop) {
  return !strcmp(loop, "crossfade") || !strcmp(loop, "fade") || !strcmp(loop, "cut");
}

int
music_order_valid(const char *order) {
  return !strcmp(order, "music") || !strcmp(order, "loop-first") || !strcmp(order, "alternate") ||
         !strcmp(order, "music-first");
}

int
music_end_mode_valid(const char *mode) {
  return !strcmp(mode, "loop-all") || !strcmp(mode, "loop-last");
}

/* A playlist track's source file: slot 1 is music.wav, the rest music<slot>.wav. */
void
music_source_file(int slot, char *out, size_t size) {
  if(slot <= 1) snprintf(out, size, "%s", MUSIC_FILE);
  else snprintf(out, size, "music%d.wav", slot);
}

static int
ini_int(const char *path, const char *key, int def, int lo, int hi) {
  char v[16];
  if(read_ini_value(path, key, v, sizeof(v))) return def;
  int n = atoi(v);
  return n < lo ? lo : n > hi ? hi : n;
}

/* A theme's name as you gave it (theme.ini's name=), or its folder's if it has none. */
void
theme_title(const char *id, char *out, size_t size) {
  char ini[PATH_MAX];
  snprintf(ini, sizeof(ini), "%s/%s/theme.ini", THEMES_DIR, id);
  if(read_ini_value(ini, "name", out, size)) snprintf(out, size, "%s", id);
}

/* Music settings of a theme; absent keys get the defaults (whole music, a
   2-second crossfade at the loop point, the system music's loudness). */
void
read_music_settings(const char *id, music_settings_t *m) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s/theme.ini", THEMES_DIR, id);
  m->gain = ini_int(path, "music_gain", 0, GAIN_MIN, GAIN_MAX);
  m->start_ms = ini_int(path, "music_start", 0, 0, MUSIC_MAX_MS);
  m->end_ms = ini_int(path, "music_end", 0, 0, MUSIC_MAX_MS);
  if(m->end_ms && m->end_ms <= m->start_ms) m->end_ms = 0;
  m->play_from_ms = ini_int(path, "music_play_from", -1, -1, MUSIC_MAX_MS);
  if(m->play_from_ms >= 0 && (m->play_from_ms < m->start_ms || (m->end_ms && m->play_from_ms >= m->end_ms))) m->play_from_ms = -1;
  m->loop_start_ms = ini_int(path, "music_loop_start", -1, -1, MUSIC_MAX_MS);
  m->loop_end_ms = ini_int(path, "music_loop_end", -1, -1, MUSIC_MAX_MS);
  if(m->loop_start_ms < 0 || m->loop_end_ms < 0 || (m->loop_end_ms && m->loop_end_ms <= m->loop_start_ms)) {
    m->loop_start_ms = m->loop_end_ms = -1;
  }
  if(read_ini_value(path, "music_order", m->order, sizeof(m->order)) || !music_order_valid(m->order) ||
     m->loop_start_ms < 0) {
    snprintf(m->order, sizeof(m->order), "music");
  }
  m->blend_ms = ini_int(path, "music_blend", 2000, 0, BLEND_MAX_MS);
  if(read_ini_value(path, "music_loop", m->loop, sizeof(m->loop)) || !music_loop_valid(m->loop)) {
    snprintf(m->loop, sizeof(m->loop), "crossfade");
  }

  /* Playlist: each slot's part, the play order, and the between/end settings.
     A single-song theme (no music_sequence) reads as one track, slot 1. */
  for(int s = 1; s <= MUSIC_MAX_TRACKS; s++) {
    char ks[24], ke[24];
    if(s == 1) { snprintf(ks, sizeof(ks), "music_start"); snprintf(ke, sizeof(ke), "music_end"); }
    else { snprintf(ks, sizeof(ks), "music%d_start", s); snprintf(ke, sizeof(ke), "music%d_end", s); }
    int st = ini_int(path, ks, 0, 0, MUSIC_MAX_MS);
    int en = ini_int(path, ke, 0, 0, MUSIC_MAX_MS);
    if(en && en <= st) en = 0;
    m->track_start_ms[s - 1] = st;
    m->track_end_ms[s - 1] = en;
  }
  m->track_start_ms[0] = m->start_ms;      /* slot 1's part is the single-song part */
  m->track_end_ms[0] = m->end_ms;
  m->tracks = 0;
  char seq[128];
  if(!read_ini_value(path, "music_sequence", seq, sizeof(seq))) {
    for(char *p = seq; *p && m->tracks < MUSIC_MAX_TRACKS; ) {
      while(*p == ',' || *p == ' ') p++;
      if(!*p) break;
      int n = atoi(p);
      if(n >= 1 && n <= MUSIC_MAX_TRACKS) m->seq[m->tracks++] = n;
      while(*p && *p != ',') p++;
    }
  }
  if(m->tracks < 1) { m->tracks = 1; m->seq[0] = 1; }
  if(read_ini_value(path, "music_between", m->between, sizeof(m->between)) || !music_loop_valid(m->between)) {
    snprintf(m->between, sizeof(m->between), "%s", m->loop);
  }
  if(read_ini_value(path, "music_end_mode", m->end_mode, sizeof(m->end_mode)) || !music_end_mode_valid(m->end_mode)) {
    snprintf(m->end_mode, sizeof(m->end_mode), "loop-all");
  }
}

/* Rewrite theme.ini atomically. name/fw/music NULL keep the current value. */
int
write_theme_ini(const char *id, const char *name, const char *fw, const music_settings_t *music) {
  char path[PATH_MAX], cur_name[128], cur_fw[64], ini[1024];
  music_settings_t m;

  snprintf(path, sizeof(path), "%s/%s/theme.ini", THEMES_DIR, id);
  theme_title(id, cur_name, sizeof(cur_name));
  if(read_ini_value(path, "fw", cur_fw, sizeof(cur_fw))) fw_string(cur_fw, sizeof(cur_fw));
  if(music) m = *music;
  else read_music_settings(id, &m);
  int n = snprintf(ini, sizeof(ini),
                   "name=%s\nfw=%s\nmusic_gain=%d\nmusic_start=%d\nmusic_end=%d\nmusic_play_from=%d\n"
                   "music_loop_start=%d\nmusic_loop_end=%d\nmusic_order=%s\nmusic_loop=%s\nmusic_blend=%d\n",
                   name ? name : cur_name, fw ? fw : cur_fw, m.gain, m.start_ms, m.end_ms, m.play_from_ms,
                   m.loop_start_ms, m.loop_end_ms, m.order, m.loop, m.blend_ms);
  if(n < 0 || n >= (int)sizeof(ini)) return -1;
  int tracks = m.tracks >= 1 && m.tracks <= MUSIC_MAX_TRACKS ? m.tracks : 1;
  n += snprintf(ini + n, sizeof(ini) - n, "music_tracks=%d\nmusic_between=%s\nmusic_end_mode=%s\nmusic_sequence=",
                tracks, m.between[0] ? m.between : "crossfade", m.end_mode[0] ? m.end_mode : "loop-all");
  if(n < 0 || n >= (int)sizeof(ini)) return -1;
  for(int i = 0; i < tracks; i++) {
    int s = i < MUSIC_MAX_TRACKS ? m.seq[i] : 1;
    n += snprintf(ini + n, sizeof(ini) - n, "%s%d", i ? "," : "", s);
    if(n < 0 || n >= (int)sizeof(ini)) return -1;
  }
  n += snprintf(ini + n, sizeof(ini) - n, "\n");
  if(n < 0 || n >= (int)sizeof(ini)) return -1;
  for(int i = 0; i < tracks; i++) {                 /* each added slot's part (slot 1 uses music_start/end above) */
    int s = i < MUSIC_MAX_TRACKS ? m.seq[i] : 0;
    if(s <= 1 || s > MUSIC_MAX_TRACKS) continue;
    n += snprintf(ini + n, sizeof(ini) - n, "music%d_start=%d\nmusic%d_end=%d\n",
                  s, m.track_start_ms[s - 1], s, m.track_end_ms[s - 1]);
    if(n < 0 || n >= (int)sizeof(ini)) return -1;
  }
  return fs_write_file(path, ini, n);
}

void
fw_string(char *out, size_t size) {
  uint32_t fw = plat_fw_version();
  if(!fw) {
    snprintf(out, size, "unknown");
    return;
  }
  snprintf(out, size, "%x.%02x", (fw >> 24) & 0xff, (fw >> 16) & 0xff);
}

void
asset_listing_path(char *out, size_t size) {
  char fw[16];
  fw_string(fw, sizeof(fw));
  snprintf(out, size, "%s/assets-v2-%s.txt", DUMP_DIR, fw);
}

/* A folder of an installed game's or app's pictures. */
int
title_target(const char *dir) {
  return !strncmp(dir, APPMETA_DIR "/", sizeof(APPMETA_DIR)) || !strncmp(dir, APP_DIR "/", sizeof(APP_DIR)) ||
         !strncmp(dir, CATALOG_DIR "/", sizeof(CATALOG_DIR));
}

/* The Appearance themes' textures (and the welcome backgrounds beside them): read as the interface starts. */
int
artwork_target(const char *dir) {
  size_t n = strlen(SYS_ASSET_DIR);
  return !strncmp(dir, SYS_ASSET_DIR, n) && (!dir[n] || dir[n] == '/');
}

/* The console's text fonts folder. */
int
font_target(const char *dir) {
  size_t n = strlen(FONT_DIR);
  return !strncmp(dir, FONT_DIR, n) && (!dir[n] || dir[n] == '/');
}

/* Folders overlaid only in the home screen's own views: installed titles', and the text fonts (which games read
   too, so a font on the real folder would reach them). */
int
home_view_only(const char *dir) {
  return title_target(dir) || font_target(dir);
}

const char *
theme_folder(size_t i) {
  return i < NPREFIXES ? allowed_prefixes[i] : NULL;
}

int
target_allowed(const char *path) {
  if(strstr(path, "/../") || strstr(path, "/./") || strstr(path, "//") || !app_path_ok(path)) return 0;
  for(size_t i = 0; i < NPREFIXES; i++) {
    if(!strncmp(path, allowed_prefixes[i], strlen(allowed_prefixes[i]))) return 1;
  }
  return 0;
}

int
theme_name_valid(const char *name) {
  size_t n = strlen(name);
  if(!n || n > 64) return 0;
  for(size_t i = 0; i < n; i++) {
    char c = name[i];
    if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '_')) return 0;
  }
  return strcmp(name, "none") != 0;
}

/* theme.ini fw= is "any" or a comma/space separated list of exact versions. */
int
fw_matches(const char *list, const char *fw) {
  size_t n = strlen(fw);
  const char *p = list;

  if(!strcmp(list, "any")) return 1;
  while(*p) {
    p += strspn(p, ", ");
    size_t len = strcspn(p, ", ");
    if(len && len == n && !strncmp(p, fw, n)) return 1;
    p += len;
  }
  return 0;
}

int
read_ini_value(const char *path, const char *key, char *out, size_t size) {
  char line[256];
  size_t klen = strlen(key);
  FILE *f = fopen(path, "r");

  if(!f) return -1;
  while(fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\r\n")] = 0;
    if(!strncmp(line, key, klen) && line[klen] == '=') {
      snprintf(out, size, "%s", line + klen + 1);
      fclose(f);
      return 0;
    }
  }
  fclose(f);
  return -1;
}
