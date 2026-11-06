/* The library and a theme as a whole: the status the page polls, and creating, copying, deleting, saving,
   discarding and applying a theme. */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>

#include "api.h"
#include "apply.h"
#include "autostyle.h"
#include "config.h"
#include "fsguard.h"
#include "http.h"
#include "log.h"
#include "media.h"
#include "platform.h"
#include "policy.h"
#include "status.h"
#include "store.h"
#include "theme.h"

static void
list_files(strbuf_t *sb, const char *root, const char *rel, int *first, int *count, int depth) {
  if(depth > 32) return;
  walk_paths_t *w = malloc(sizeof(*w));
  if(!w) return;
  char *dir = w->dir, *child_rel = w->child_rel, *child = w->child;
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
  free(w);
}

/* Up to `max` image paths from a theme, for the library card preview. */
static void
preview_images(strbuf_t *sb, const char *root, const char *rel, int *n, int max, int depth) {
  if(depth > 32) return;
  walk_paths_t *w = malloc(sizeof(*w));
  if(!w) return;
  char *dir = w->dir, *child_rel = w->child_rel, *child = w->child;
  snprintf(dir, PATH_MAX, "%s%s", root, rel);

  DIR *d = opendir(dir);
  if(!d) { free(w); return; }
  struct dirent *e;
  while(*n < max && (e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    struct stat st;
    if(snprintf(child_rel, PATH_MAX, "%s/%s", rel, e->d_name) >= PATH_MAX) continue;
    snprintf(child, PATH_MAX, "%s%s", root, child_rel);
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
  free(w);
}

/* The slow part of the status: walking each theme's files to count them and pick up to three for its card. The
   manager asks every few seconds (every second during an apply), from every device that has it open; the walk is
   kept until the plugin writes anything (fs_generation, so a change made here shows at once) or for
   WALK_KEEP_SECONDS (a change made from outside, over FTP). Everything else in the status is read every time. */
#define WALK_KEEP_SECONDS 10
#define WALK_SLOTS        128
typedef struct { char id[128]; unsigned gen; time_t at; int count; char *preview; size_t len; } walk_t;
static walk_t walks[WALK_SLOTS];
static pthread_mutex_t walks_lock = PTHREAD_MUTEX_INITIALIZER;

static void
theme_walk(const char *id, const char *files, int *count, strbuf_t *sb) {
  unsigned gen = fs_generation();   /* read before the walk: a write during it makes it stale */
  time_t now = time(NULL);
  pthread_mutex_lock(&walks_lock);
  for(int i = 0; i < WALK_SLOTS; i++) {
    walk_t *w = &walks[i];
    if(!w->id[0] || strcmp(w->id, id)) continue;
    if(w->gen == gen && now - w->at < WALK_KEEP_SECONDS && now >= w->at) {
      *count = w->count;
      if(w->len) sb_append(sb, w->preview, w->len);
      pthread_mutex_unlock(&walks_lock);
      return;
    }
    break;
  }
  pthread_mutex_unlock(&walks_lock);

  int first = 1, n = 0, shown = 0;
  strbuf_t pv = {0};
  list_files(NULL, files, "", &first, &n, 0);
  preview_images(&pv, files, "", &shown, 3, 0);
  *count = n;
  if(pv.len && !pv.failed) sb_append(sb, pv.data, pv.len);

  pthread_mutex_lock(&walks_lock);
  walk_t *slot = NULL;
  for(int i = 0; i < WALK_SLOTS && !slot; i++) if(!strcmp(walks[i].id, id)) slot = &walks[i];
  for(int i = 0; i < WALK_SLOTS && !slot; i++) if(!walks[i].id[0]) slot = &walks[i];
  if(!slot) {   /* full: the one kept longest goes */
    slot = &walks[0];
    for(int i = 1; i < WALK_SLOTS; i++) if(walks[i].at < slot->at) slot = &walks[i];
  }
  free(slot->preview);
  memset(slot, 0, sizeof(*slot));
  if(!pv.failed) {
    snprintf(slot->id, sizeof(slot->id), "%s", id);
    slot->gen = gen;
    slot->at = now;
    slot->count = n;
    slot->preview = pv.data;
    slot->len = pv.len;
    pv.data = NULL;
  }
  pthread_mutex_unlock(&walks_lock);
  free(pv.data);
}

void
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
  /* Changes when a game or app is installed or removed, or the icon styler did something: an open page reads its
     titles and the theme again. */
  char stamp[48];
  titles_stamp(stamp, sizeof(stamp));
  SB_LIT(&sb, ",\"titles\":");
  sb_json_str(&sb, stamp);
  sb_printf(&sb, ",\"maxFolders\":%d", max_theme_folders());
  /* How much a theme can still take, keeping the console's share free: the page checks a file against it before
     sending it (a refusal after the start of a large upload can reach the browser as a dropped connection). */
  uint64_t avail = plat_free_bytes(BASE_DIR);
  if(avail != UINT64_MAX) sb_printf(&sb, ",\"room\":%llu", (unsigned long long)(avail > FS_RESERVE ? avail - FS_RESERVE : 0));
  SB_LIT(&sb, ",\"themes\":[");

  swap_read_begin();   /* (never a theme mid-Discard: see swap_lock) */
  DIR *d = opendir(THEMES_DIR);
  int first = 1;
  if(d) {
    struct dirent *e;
    while((e = readdir(d))) {
      char ini[PATH_MAX], files[PATH_MAX], name[128], tfw[64];
      int count = 0;
      if(!theme_name_valid(e->d_name)) continue;
      snprintf(ini, sizeof(ini), "%s/%s/theme.ini", THEMES_DIR, e->d_name);
      theme_title(e->d_name, name, sizeof(name));
      if(read_ini_value(ini, "fw", tfw, sizeof(tfw))) tfw[0] = 0;
      snprintf(files, sizeof(files), "%s/%s/files", THEMES_DIR, e->d_name);
      strbuf_t walked = {0};
      theme_walk(e->d_name, files, &count, &walked);

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
      if(walked.len && !walked.failed) sb_append(&sb, walked.data, walked.len);
      free(walked.data);
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
      long long cover_time, home_time = 0;
      const char *cover_kind = shown_preview(e->d_name, cover, sizeof(cover), &cover_time);
      struct stat hst;   /* (the home screen picture: the page draws one for a theme that has none) */
      if(!preview_path(e->d_name, "home", cover, sizeof(cover)) && !stat(cover, &hst) && S_ISREG(hst.st_mode)) home_time = (long long)hst.st_mtime;
      sb_printf(&sb, ",\"cover\":\"%s\",\"coverTime\":%lld,\"homeTime\":%lld,\"unsaved\":%s}", cover_kind, cover_time,
                home_time, theme_unsaved(e->d_name) ? "true" : "false");
    }
    closedir(d);
  }
  swap_read_end();
  SB_LIT(&sb, "]}");
  respond_json(req->fd, 200, &sb);
}

void
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

void
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

void
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

  if(rc && errno == ELOOP) {
    return respond_error(req->fd, 500, "it holds folders nested deeper than any of the console's, which the plugin "
                                       "doesn't go into: delete it over FTP");
  }
  if(rc && errno == EPERM) {
    return respond_error(req->fd, 409, "something else is mounted inside this theme's folder: restart the console, then "
                         "delete it");
  }
  if(rc) return respond_error(req->fd, 500, "delete failed");
  log_msg("deleted theme %s", id);
  respond_ok(req->fd);
}

void
api_apply(request_t *req) {
  char id[128], list[400];

  get_param(req->query, "id", id, sizeof(id));
  if(strcmp(id, "none") && !theme_exists(id)) return respond_error(req->fd, 404, "no such theme");
  /* The page styled new games' icons by itself and applies the theme again for them: said once on the console when
     it is done, by their names, as the plugin's own styling is (a game may be open). */
  if(strcmp(id, "none") && !get_param(req->query, "styled", list, sizeof(list)) && list[0]) {
    char titles[32][16];
    int n = 0;
    for(char *t = list, *next; t && *t; t = next) {
      next = strchr(t, ',');
      if(next) *next++ = 0;
      if(!style_title_ok(t) || n >= 32) return respond_error(req->fd, 400, "styled: up to 32 title IDs, such as PPSA01234,CUSA09999");
      snprintf(titles[n++], sizeof(titles[0]), "%s", t);
    }
    char note[1024];
    autostyle_note(note, sizeof(note), (const char (*)[16])titles, n, NULL, 0);
    if(!reapply_quietly(id, note)) {
      log_msg("theme %s: applied again for %d icon(s) the manager styled", id, n);
      return respond_ok(req->fd);
    }
  }
  if(write_active(id)) return respond_error(req->fd, 500, "could not save the selection");
  log_msg("selection changed to %s", id);
  respond_ok(req->fd);
}

/* Rename, change the firmware list, or change the music settings: gain (dB),
   start/end (ms, end 0 = to the end), play_from (ms, where the "music" order
   starts playing; -1 = the start), loop_start/loop_end (ms, the loop part;
   both -1 = none), order (music, loop-first, alternate, music-first), loop
   (crossfade, fade, cut), blend (ms). */
void
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

/* Keep the changes made since the last save. The theme chosen for the console is
   applied again, so the console shows what was saved: only when its files aren't
   the ones the console shows (not after a change to choices or music settings,
   nor when the changes were applied before saving), as Discard does. */
void
api_save_theme(request_t *req) {
  char id[128], active[128];
  get_param(req->query, "id", id, sizeof(id));
  themes_lock();
  int exists = theme_exists(id), had = exists && theme_unsaved(id);
  if(had) drop_snapshot(id);
  int unchanged = !had || applied_files_unchanged(id);
  int empty = had && !unchanged && !theme_has_files(id);   /* (applying it would fail: none of its files passed) */
  themes_unlock();
  if(!exists) return respond_error(req->fd, 404, "no such theme");

  read_active(active, sizeof(active));
  int selected = !strcmp(active, id);
  int again = selected && !unchanged && !empty && !write_active(id);
  if(had) log_msg("theme %s: changes saved%s", id, again ? ", applying it again" : !selected ? ""
                  : empty ? "; nothing is left in it, so the console keeps the last one applied"
                  : unchanged ? "; the console already shows these files" : "");
  strbuf_t sb = {0};
  sb_printf(&sb, "{\"ok\":true,\"reapplied\":%s,\"empty\":%s}", again ? "true" : "false", selected && empty ? "true" : "false");
  respond_json(req->fd, 200, &sb);
}

/* Put the theme back as it was at the last save. An applied theme is
   applied again, so the console shows the same. */
void
api_discard_theme(request_t *req) {
  char id[128], active[128];
  get_param(req->query, "id", id, sizeof(id));
  themes_lock();
  if(!theme_exists(id) || !theme_unsaved(id)) {
    themes_unlock();
    return respond_error(req->fd, 409, "there are no changes to discard");
  }
  int rc = restore_snapshot(id);
  /* The console needs the theme again only if the files put back aren't the ones it shows:
     not after a change to choices, music settings or pictures it doesn't use, or one never applied. */
  int unchanged = !rc && applied_files_unchanged(id);
  themes_unlock();
  if(rc) return respond_error(req->fd, 500, "could not discard the changes");

  read_active(active, sizeof(active));
  int selected = !strcmp(active, id);
  int again = selected && !unchanged && !write_active(id);
  log_msg("theme %s: changes discarded%s", id, again ? ", applying it again" : selected ? "; the console already shows these files" : "");
  strbuf_t sb = {0};
  sb_printf(&sb, "{\"ok\":true,\"reapplied\":%s}", again ? "true" : "false");
  respond_json(req->fd, 200, &sb);
}

void
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
  int full = rc < 0 && errno == ENOSPC;
  if(!rc && name[0]) rc = write_theme_ini(new_id, name, NULL, NULL);
  if(rc < 0) fs_rm_rf_under(THEMES_DIR, dst);
  themes_unlock();
  if(rc > 0) return respond_error(req->fd, 409, "a theme with that id exists");
  if(full) return respond_error(req->fd, 507, STORAGE_FULL);
  if(rc) return respond_error(req->fd, 500, "could not duplicate the theme");
  log_msg("duplicated theme %s as %s", id, new_id);
  respond_ok(req->fd);
}
