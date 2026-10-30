/* ps5-theme: apply custom home-screen assets on a jailbroken PS5 (etaHEN plugin).

   Immutability:
   - Every write goes through fsguard.c and can only land in BASE_DIR or the
     tile's TILE_DIR. Nothing under /system or /system_ex is ever opened for
     writing, even though etaHEN remounts them read-write.
   - A theme is applied with read-only overlay mounts of staged copies under
     BASE_DIR. Mounts live in kernel memory only; a reboot restores stock.
   - Only mounts provably created by this plugin are ever unmounted.

   Verification: each theme file is checksummed after staging and again after
   reading it back through the live mount; a folder that fails is unmounted.

   Layout on the console:
     /data/ps5-theme/active                    selected theme ("none" = stock)
     /data/ps5-theme/themes/<id>/theme.ini     name=, fw=
     /data/ps5-theme/themes/<id>/files/<absolute system path>
     /data/ps5-theme/stage/                    staged copies (managed)
     /data/ps5-theme/mounts                    mount points we created (managed)
     /data/ps5-theme/logs/                     log.txt, crash-*.txt
     /data/ps5-theme/dump/rnps-files-<fw>.txt  stock asset listing
*/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>

#include "platform.h"
#include "theme.h"


/* The folders a theme may change files in. An icon style takes two a game (its metadata and its sce_sys) and some
   thirty for the system tiles, so 64 (before 1.74) was passed by a theme of 18 games with its tiles styled. 256 holds
   a library of over a hundred; each costs about 2 KB of the tables below for every view of it. */
#define MAX_TARGETS      256
#define STAGE_SIZE_LIMIT (512ull * 1024 * 1024)
#define POLL_SECONDS     1
#define UPDATE_CHECK_SECONDS 10
#define HANDOFF_ASK_SECONDS  30    /* for a newer copy this one started to ask to take over */
#define HANDOFF_WAIT_MS  180000    /* for an older copy to step aside (it may be applying) */
#define TAKEOVER_ASK_SECONDS 10    /* for the running copy's web server to answer */
#define WEB_STACK_SIZE   (1024 * 1024)

/* ShellUI's process and sandbox, watched by a thread of its own
   (shellui_guard), so its exit is handled at once even while the main
   thread is busy applying a theme. */
#define SHELLUI_PROCESS  "SceShellUI"
#define SHELLUI_SANDBOX  "/mnt/sandbox/NPXS40087_"
#define SANDBOX_ROOT     "/mnt/sandbox"
static atomic_int shellui_seen;     /* found at least once (for the apply's hints) */
/* The ShellUI sandbox whose process the guard is watching (/mnt/sandbox/NPXS40087_<n>), -1 when none. Nothing of
   ours goes into a ShellUI sandbox but this one (see mount_overlay). */
static atomic_int watched_box = -1;
static atomic_int guard_hunting;    /* ShellUI exited moments ago: its successor is looked for, and covered, at once */
static atomic_int shellui_between;  /* from ShellUI's exit until the next one is watched: no sandbox is touched */

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

/* Threads: main runs the engine below, the web thread serves the UI.
   state_lock guards applied/result. themes_mutex is held by the engine for a
   whole apply and by the web thread for any change to a theme's files, so a
   theme never changes underneath staging and verification. Large static
   buffers are each owned by a single thread. */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t themes_mutex = PTHREAD_MUTEX_INITIALIZER;
static char applied[128];        /* theme currently mounted, "" if stock */
static char applying[128];       /* theme being applied, "" when idle */
static int  progress_pct = -1;   /* how far the apply is, -1 when idle */
static char progress_text[128];  /* what it is doing, in plain words */
static char result[RESULT_MAX];  /* outcome of the last apply, for the UI */
static atomic_int reapply_requested;
/* An apply asked for by the icon styler or the page's own styling of new icons (reapply_quietly): said once on the
   console when it is done, with what it was for, and not step by step as one you start: a game may be open. One you
   ask for meanwhile (write_active) is said as usual, the note after it. */
static char quiet_note[1024];    /* (state_lock) said once the next apply of quiet_theme is done */
static char quiet_theme[128];    /* (state_lock) */
static int  loud_wanted;         /* (state_lock) an apply asked for by you since the last one began */
static atomic_int apply_quiet;   /* this apply is said only once it is done */
static char apply_note[1024];    /* (main thread) and with this */
static void say_apply_note(void);


static void
set_progress(int pct, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Console notifications queue up and each stays a few seconds, so a long
   apply gets at most one progress notification every PROGRESS_NOTE_SECONDS
   (and none in its first seconds), on top of the start and the result. */
#define PROGRESS_NOTE_SECONDS 8
static long long progress_started, progress_noted;   /* main thread only (forward-only clock) */

/* What the apply is doing, for the manager and the console. The percentage
   never goes back. */
static void
set_progress(int pct, const char *fmt, ...) {
  va_list ap;
  char text[sizeof(progress_text)];
  int shown = -1;
  pthread_mutex_lock(&state_lock);
  if(pct < 0) {
    progress_pct = -1;
    progress_text[0] = 0;
  } else {
    if(pct > 100) pct = 100;
    if(pct > progress_pct) progress_pct = pct;
    va_start(ap, fmt);
    vsnprintf(progress_text, sizeof(progress_text), fmt, ap);
    va_end(ap);
    shown = progress_pct;
    snprintf(text, sizeof(text), "%s", progress_text);
  }
  pthread_mutex_unlock(&state_lock);

  long long now = mono_seconds();
  if(shown < 0) {
    progress_started = 0;
  } else if(!progress_started) {
    progress_started = progress_noted = now;
  } else if(now - progress_noted >= PROGRESS_NOTE_SECONDS && shown < 100 && !atomic_load(&apply_quiet)) {
    progress_noted = now;
    notify("ps5-theme: %s... %d%%", text, shown);
  }
}


int
apply_progress(char *text, size_t size) {
  pthread_mutex_lock(&state_lock);
  int pct = progress_pct;
  snprintf(text, size, "%s", progress_text);
  pthread_mutex_unlock(&state_lock);
  return pct;
}


static void
set_result(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
set_result(const char *fmt, ...) {
  va_list ap;
  pthread_mutex_lock(&state_lock);
  va_start(ap, fmt);
  vsnprintf(result, sizeof(result), fmt, ap);
  va_end(ap);
  pthread_mutex_unlock(&state_lock);
}


void themes_lock(void)   { pthread_mutex_lock(&themes_mutex); }
void themes_unlock(void) { pthread_mutex_unlock(&themes_mutex); }


void
last_result(char *out, size_t size) {
  pthread_mutex_lock(&state_lock);
  snprintf(out, size, "%s", result);
  pthread_mutex_unlock(&state_lock);
}


int
applied_theme(char *out, size_t size) {
  pthread_mutex_lock(&state_lock);
  snprintf(out, size, "%s", applied);
  pthread_mutex_unlock(&state_lock);
  return out[0] != 0;
}


int
applying_theme(char *out, size_t size) {
  pthread_mutex_lock(&state_lock);
  snprintf(out, size, "%s", applying);
  pthread_mutex_unlock(&state_lock);
  return out[0] != 0;
}


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
  if(read_ini_value(path, "name", cur_name, sizeof(cur_name))) snprintf(cur_name, sizeof(cur_name), "%s", id);
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


/* ---------------- staging ---------------- */

static uint8_t copy_buf[64 * 1024];  /* owned by the main thread */

/* Progress of a long copy (the stock mirror, or a stage folder where hard
   links aren't available), reported every few MB. */
typedef struct {
  uint64_t done, total, next;
  int lo, hi;
  char label[96];
} copy_meter_t;
static copy_meter_t mirror_meter, stage_meter;   /* owned by the main thread */
static copy_meter_t *copy_meter;                 /* the one counting now, if any */

static void
copy_meter_add(uint64_t n) {
  copy_meter_t *m = copy_meter;
  if(!m || m->total < (4u << 20)) return;   /* small copies are quick */
  m->done += n;
  if(m->done < m->next) return;
  m->next = m->done + (4u << 20);
  uint64_t done = m->done < m->total ? m->done : m->total;
  set_progress(m->lo + (int)((m->hi - m->lo) * done / m->total), "%s: %llu of %llu MB", m->label,
               (unsigned long long)(done >> 20), (unsigned long long)(m->total >> 20));
}

static int
copy_file(const char *src, const char *dst, uint64_t *budget) {
  struct stat st, dst_st;
  int ret = 0;

  if(stat(src, &st) || !S_ISREG(st.st_mode)) return -1;
  if((uint64_t)st.st_size > *budget) {
    log_err("stage size limit reached at %s", src);
    return -1;
  }
  if(fs_room_for((uint64_t)st.st_size)) return -1;   /* logged */
  *budget -= st.st_size;

  /* Always a new file: dst may be a copied symlink, or a link to a stock
     mirror file, which must never change. */
  if(!lstat(dst, &dst_st)) {
    if(S_ISDIR(dst_st.st_mode) || fs_unlink(dst)) return -1;
  }

  int in = open(src, O_RDONLY);
  if(in < 0) return -1;
  int out = fs_open_write(dst);
  if(out < 0) {
    close(in);
    return -1;
  }
  uint64_t copied = 0;
  for(;;) {
    ssize_t n = read(in, copy_buf, sizeof(copy_buf));
    if(n < 0 && errno == EINTR) continue;
    if(n < 0) { ret = -1; break; }
    if(n == 0) break;
    ssize_t off = 0;
    while(off < n) {
      ssize_t w = write(out, copy_buf + off, n - off);
      if(w < 0 && errno == EINTR) continue;
      if(w <= 0) { ret = -1; break; }
      off += w;
    }
    if(ret) break;
    copied += n;
    copy_meter_add(n);
  }
  close(in);
  if(close(out)) ret = -1;
  if(!ret && copied != (uint64_t)st.st_size) {
    log_err("short copy %s: %llu of %lld bytes", src, (unsigned long long)copied, (long long)st.st_size);
    ret = -1;
  }
  return ret;
}


/* A hard link, or a copy where the filesystem can't link (said once). */
static int
link_or_copy(const char *from, const char *to, uint64_t *budget) {
  static int warned;
  if(!fs_link(from, to)) return 0;
  if(!warned) {
    warned = 1;
    log_msg("hard links aren't available here (%s); stage folders hold copies, reused while unchanged",
            strerror(errno));
  }
  return copy_file(from, to, budget);
}


/* Recursively copy a stock system folder (to its mirror, or a mirror into the
   stage). Symlinks are part of the stock layout and are copied as links; the
   write guard makes sure no later write can travel through one. With
   `use_links`, regular files become hard links where the filesystem allows. */
static int
copy_tree(const char *src, const char *dst, uint64_t *budget, int use_links) {
  struct stat st;

  if(fs_mkdirs(dst) || lstat(dst, &st) || !S_ISDIR(st.st_mode)) {
    log_err("stage: %s is not a plain directory", dst);
    return -1;
  }

  DIR *d = opendir(src);
  if(!d) return -1;
  struct dirent *e;
  int ret = 0;
  while(!ret && (e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char s[PATH_MAX], t[PATH_MAX];
    if(snprintf(s, sizeof(s), "%s/%s", src, e->d_name) >= (int)sizeof(s) ||
       snprintf(t, sizeof(t), "%s/%s", dst, e->d_name) >= (int)sizeof(t)) {
      ret = -1;
      break;
    }
    if(lstat(s, &st)) { ret = -1; break; }

    if(S_ISDIR(st.st_mode)) {
      ret = copy_tree(s, t, budget, use_links);
    } else if(S_ISLNK(st.st_mode)) {
      char link[PATH_MAX];
      ssize_t n = readlink(s, link, sizeof(link) - 1);
      if(n < 0) { ret = -1; break; }
      link[n] = 0;
      struct stat old;   /* a mirror made again over an older one */
      ret = !lstat(t, &old) && (S_ISDIR(old.st_mode) || fs_unlink(t)) ? -1 : fs_symlink(link, t);
    } else if(S_ISREG(st.st_mode)) {
      ret = use_links ? link_or_copy(s, t, budget) : copy_file(s, t, budget);
    }
    if(ret) log_err("copy failed: %s -> %s (%s)", s, t, strerror(errno));
  }
  closedir(d);
  return ret;
}


/* Files of one theme folder that passed the pre-checks, as paths relative to
   the overlaid folder ("/sub/file.png"). */
typedef struct plan {
  char (*rel)[PATH_MAX];
  int count, cap, skipped;
} plan_t;

#define PLAN_MAX_FILES 4096


static void
plan_free(plan_t *p) {
  free(p->rel);
  memset(p, 0, sizeof(*p));
}


/* A theme file is used only if it replaces an existing stock file of a
   themable kind and passes asset_validate() against it (for images: exactly
   the stock format and pixel size). Matching each stock file exactly is what
   keeps a theme correct at every output resolution the console renders at. */
/* A theme's files mirror the console's folders, none of which is 32 levels deep. Deeper is never walked: each level
   takes stack (see fsguard.c). */
#define THEME_MAX_DEPTH 32

static int
rel_depth(const char *rel) {
  int n = 0;
  for(; *rel; rel++) n += *rel == '/';
  return n;
}

static int
plan_files(const char *theme_dir, const char *stock_dir, const char *stock_real, const char *rel, plan_t *p) {
  char dir[PATH_MAX];
  if(rel_depth(rel) > THEME_MAX_DEPTH) {
    log_err("theme: skipping %s%s, deeper than any folder of the console", theme_dir, rel);
    p->skipped++;
    return 0;
  }
  snprintf(dir, sizeof(dir), "%s%s", theme_dir, rel);

  DIR *d = opendir(dir);
  if(!d) return -1;
  struct dirent *e;
  int ret = 0;
  while(!ret && (e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char r[PATH_MAX], theme[PATH_MAX], stock[PATH_MAX];
    struct stat st, sst;
    if(snprintf(r, sizeof(r), "%s/%s", rel, e->d_name) >= (int)sizeof(r)) continue;
    snprintf(theme, sizeof(theme), "%s%s", theme_dir, r);
    snprintf(stock, sizeof(stock), "%s%s", stock_dir, r);
    if(lstat(theme, &st)) continue;

    if(S_ISDIR(st.st_mode)) {
      ret = plan_files(theme_dir, stock_dir, stock_real, r, p);
      continue;
    }
    /* An upload being received: not a file of the theme. One untouched for an hour was cut off (a live upload
       writes at least every 15 seconds, or gives up), and is removed. */
    size_t len = strlen(e->d_name);
    if(S_ISREG(st.st_mode) && len > 5 && !strcmp(e->d_name + len - 5, ".part")) {
      if(time(NULL) - st.st_mtime > 3600 && !fs_unlink(theme)) log_msg("theme: removed %s, an upload that was cut off", theme);
      continue;
    }
    if(!S_ISREG(st.st_mode)) {
      log_err("theme: ignoring %s (not a regular file)", theme);
      p->skipped++;
      continue;
    }
    if(stat(stock, &sst) || !S_ISREG(sst.st_mode)) {
      log_err("theme: skipping %s, no such system file on this firmware", stock);
      p->skipped++;
      continue;
    }
    char real[PATH_MAX], expect[PATH_MAX];
    snprintf(expect, sizeof(expect), "%s%s", stock_real, r);
    if(!realpath(stock, real) || strcmp(real, expect)) {
      /* Staging it would mean writing through a symlink; the guard would
         refuse anyway, but skipping keeps the rest of the folder usable. */
      log_err("theme: skipping %s, it is reached through a symlink", stock);
      p->skipped++;
      continue;
    }
    char why[200];
    if(asset_validate(theme, stock, why, sizeof(why))) {
      log_err("theme: skipping %s: %s", theme, why);
      p->skipped++;
      continue;
    }
    if(p->count == p->cap) {
      if(p->cap >= PLAN_MAX_FILES) { ret = -1; break; }
      int cap = p->cap ? p->cap * 2 : 64;
      void *n = realloc(p->rel, (size_t)cap * PATH_MAX);
      if(!n) { ret = -1; break; }
      p->rel = n;
      p->cap = cap;
    }
    snprintf(p->rel[p->count++], PATH_MAX, "%s", r);
  }
  closedir(d);
  return ret;
}


/* Is `b` the very file `a` is (seen through a link or an overlay)? The same
   inode, size and modification time: an overlay passes those through, and a
   system file sharing all of them with a theme file does not happen. */
static int
same_file(const char *a, const char *b) {
  struct stat sa, sb;
  return !stat(a, &sa) && !stat(b, &sb) && S_ISREG(sa.st_mode) && S_ISREG(sb.st_mode) &&
         sa.st_ino == sb.st_ino && sa.st_size == sb.st_size &&
         sa.st_mtim.tv_sec == sb.st_mtim.tv_sec && sa.st_mtim.tv_nsec == sb.st_mtim.tv_nsec;
}

static int verified_by_content;   /* files that needed a full comparison (main thread) */

/* Milliseconds on a clock that only goes forward, for timing the steps of an apply. */
static long long
mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Every planned theme file must be what other_dir shows: the same file, or
   if not, the same bytes (checksum and size). */
static int
verify_plan(const plan_t *p, const char *theme_dir, const char *other_dir, const char *what) {
  for(int i = 0; i < p->count; i++) {
    char a[PATH_MAX], b[PATH_MAX];
    uint32_t ca, cb;
    uint64_t sa, sb;
    snprintf(a, sizeof(a), "%s%s", theme_dir, p->rel[i]);
    snprintf(b, sizeof(b), "%s%s", other_dir, p->rel[i]);
    if(same_file(a, b)) continue;
    verified_by_content++;
    if(fs_crc32_file(a, &ca, &sa) || fs_crc32_file(b, &cb, &sb) || ca != cb || sa != sb) {
      log_err("verify (%s): %s does not match %s", what, b, a);
      return -1;
    }
  }
  return 0;
}


/* ---------------- mounts ---------------- */

/* Stage folders to keep through a revert, so a later apply can reuse them:
   the applied theme's (after a reboot) and the spares of the theme shown
   before it. Main thread only. */
static char kept_stages[MAX_TARGETS * 2][PATH_MAX];
static int nkept_stages;

/* MOUNTS_FILE holds one line per overlay this plugin mounted, oldest first:
   "<mount point>\t<source>\t<system folder it shows>" (copies before 1.39
   wrote the mount point alone). A mount point can be listed more than once:
   an overlay still in use when it was due to come down stays, and the next
   one goes on top of it (see revert_mounts). */
#define MAX_RECORDS 2048

typedef struct {
  char on[PATH_MAX];
  char src[256];
  char real[512];
} mount_rec_t;

static mount_rec_t records[MAX_RECORDS];   /* main thread only */

static int records_torn;   /* the last read found a last line cut short (main thread) */

static int
read_mount_records(mount_rec_t *recs, int max) {
  char line[PATH_MAX + 256 + 512 + 8];
  int n = 0;
  FILE *f = fopen(MOUNTS_FILE, "r");
  if(!f) return 0;
  records_torn = 0;
  while(n < max && fgets(line, sizeof(line), f)) {
    if(!strchr(line, '\n')) { records_torn = 1; break; }   /* a last line cut short: not a record (see record_mount) */
    line[strcspn(line, "\r\n")] = 0;
    if(!line[0]) continue;
    char *src = strchr(line, '\t'), *real = src ? strchr(src + 1, '\t') : NULL;
    if(src) *src++ = 0;
    if(real) *real++ = 0;
    snprintf(recs[n].on, sizeof(recs[n].on), "%s", line);
    snprintf(recs[n].src, sizeof(recs[n].src), "%s", src ? src : "");
    snprintf(recs[n].real, sizeof(recs[n].real), "%s", real ? real : "");
    n++;
  }
  fclose(f);
  return n;
}

static int nrecorded = -1;   /* lines in MOUNTS_FILE, -1 until read (main thread) */

static int
write_mount_records(const mount_rec_t *recs, int n) {
  nrecorded = -1;
  if(!n) return fs_unlink(MOUNTS_FILE) && errno != ENOENT ? -1 : 0;
  size_t cap = (size_t)n * (sizeof(*recs) + 3), len = 0;
  char *buf = malloc(cap);
  if(!buf) return -1;
  for(int i = 0; i < n; i++) len += snprintf(buf + len, cap - len, "%s\t%s\t%s\n", recs[i].on, recs[i].src, recs[i].real);
  int ret = fs_write_file(MOUNTS_FILE, buf, len);
  free(buf);
  if(!ret) nrecorded = n;
  return ret;
}

static int
record_mount(const char *on, const char *src, const char *real) {
  /* One line added, not the whole record written again and waited for at each overlay (217 of them took seconds
     on the console, while the home screen showed its own look): the record matters only while the console runs. */
  if(nrecorded < 0) {
    int n = read_mount_records(records, MAX_RECORDS);
    /* A last line cut short (written as the power went): the record is written whole again first, so the next line
       never runs on from it. */
    if(records_torn && write_mount_records(records, n)) return -1;
    nrecorded = n;
  }
  if(nrecorded >= MAX_RECORDS) return -1;
  char line[PATH_MAX + 256 + 512 + 8];
  int len = snprintf(line, sizeof(line), "%s\t%s\t%s\n", on, src, real);
  if(len < 0 || len >= (int)sizeof(line) || fs_append_file(MOUNTS_FILE, line, (size_t)len)) {
    nrecorded = -1;
    return -1;
  }
  nrecorded++;
  return 0;
}

/* Is `dir` the source of an overlay still mounted (one that stayed because
   it was in use)? Such a folder must not be deleted or changed. */
static int
source_in_use(const char *dir) {
  int n = read_mount_records(records, MAX_RECORDS);
  for(int i = 0; i < n; i++) if(!strcmp(records[i].src, dir)) return 1;
  return 0;
}

static void mirror_paths(const char *real, char *dir, size_t dsize, char *mark, size_t msize);
static int  mount_overlay(const char *src, const char *on);

/* A verified copy of the system's folder `real` over `on`, where an overlay
   still in use shows a theme, so the console shows its own files there.
   0 if done, with the new overlay in `rec`. */
static int
cover_with_stock(const char *on, const char *real, mount_rec_t *rec) {
  char mirror[PATH_MAX], mark[PATH_MAX];
  if(real[0]) mirror_paths(real, mirror, sizeof(mirror), mark, sizeof(mark));
  if(!real[0] || access(mark, F_OK)) {
    log_err("%s still shows the theme: no verified copy of the system's folder to put over it", on);
    return -1;
  }
  int r = mount_overlay(mirror, on);
  if(r) {
    if(r < 0) log_err("%s still shows the theme: mounting the system's copy over it failed: %s", on, strerror(errno));
    return -1;
  }
  log_msg("covered %s with the system's own files", on);
  snprintf(rec->on, sizeof(rec->on), "%s", on);
  snprintf(rec->src, sizeof(rec->src), "%s", mirror);
  snprintf(rec->real, sizeof(rec->real), "%s", real);
  return 0;
}

/* Carry on with the overlay that stayed in use on `on` as this apply's,
   recorded under its folder's new name `dir`: it already shows that very
   folder (an unchanged folder reused for the same theme, renamed from the
   name it was mounted under). 0 if done. */
static int
adopt_left_overlay(const char *on, const char *dir) {
  int n = read_mount_records(records, MAX_RECORDS), top = -1;
  for(int i = 0; i < n; i++) if(!strcmp(records[i].on, on)) top = i;
  if(top < 0) return -1;
  snprintf(records[top].src, sizeof(records[top].src), "%s", dir);
  return write_mount_records(records, n);
}

/* The same, recorded in MOUNTS_FILE: for an apply whose new overlay didn't
   go on where revert_mounts left the previous theme under it. */
static void
cover_left_theme(const char *on, const char *real) {
  mount_rec_t rec;
  if(!cover_with_stock(on, real, &rec) && record_mount(rec.on, rec.src, rec.real)) log_err("could not record mount %s", on);
}

/* Take every overlay down. Stage folders go too, except those whose name
   starts with `keep` (the theme being applied, staged before this runs),
   those in kept_stages, and those an overlay still uses.

   An overlay is never forced off: on 13.42 that broke the system's own views
   stacked on it ("Bad file descriptor") and the apps using them, until a
   restart. One still in use stays, with everything under it at that mount
   point, and a verified copy of the system's folder goes on top so the
   console looks stock; the next revert tries again from the top.

   `covers` (`ncovers` of them, or none): the mount points an apply puts its
   new overlays on straight after. Where one of them is still in use, the
   new overlay goes straight on top and no copy of the system's folder goes
   in between (each apply while ShellUI holds its interface folder used to
   add two layers there); deferred[k] is set for covers[k], so the apply
   puts that copy on only if its own overlay doesn't go on there. */
static void
revert_mounts(const char *keep, const char *const *covers, int ncovers, int *deferred) {
  static mount_rec_t left[MAX_RECORDS];
  static int gone[MAX_RECORDS];
  int n = read_mount_records(records, MAX_RECORDS), count = 0, busy = 0, nleft = 0, under_new = 0;
  for(int k = 0; k < ncovers; k++) deferred[k] = 0;

  crumb(CRUMB_MAIN, "reverting mounts");
  /* Newest first, so nested and view mounts come off before what they cover. */
  for(int i = n - 1; i >= 0; i--) {
    gone[i] = 0;
    int under = 0;
    for(int k = i + 1; k < n && !under; k++) under = !gone[k] && !strcmp(records[k].on, records[i].on);
    if(under) continue;   /* below one that stays */
    if(!plat_is_our_mount(records[i].on, BASE_DIR "/")) {
      log_msg("not our mount (already gone): %s", records[i].on);
      gone[i] = 1;
      continue;
    }
    /* Where the new theme goes on straight after, one try: an overlay in use then simply stays under the new one.
       Waiting for it (3 s each on the console) bought nothing there, as the home screen keeps its font and
       interface pictures open for as long as it runs: four such waits added 12 s to every apply. */
    int replaced = 0;
    for(int k = 0; k < ncovers && !replaced; k++) replaced = !strcmp(covers[k], records[i].on);
    if(replaced ? plat_unmount_once(records[i].on) : plat_unmount(records[i].on)) {
      if(errno == EBUSY) busy++;
      if(replaced && errno == EBUSY) log_msg("unmount %s: still in use; the new theme's overlay goes on top of it", records[i].on);
      else log_err("unmount %s: %s; it stays until it is no longer in use or the console restarts",
                   records[i].on, errno == EBUSY ? "still in use" : strerror(errno));
      continue;
    }
    log_msg("unmounted %s", records[i].on);
    gone[i] = 1;
    count++;
  }
  for(int i = 0; i < n; i++) if(!gone[i]) left[nleft++] = records[i];

  /* Stock on top of each overlay that stayed and shows a theme. */
  char stock_prefix[PATH_MAX];
  snprintf(stock_prefix, sizeof(stock_prefix), "%s/", STOCK_DIR);
  for(int i = 0, n0 = nleft; i < n0; i++) {
    int top = 1;
    for(int k = i + 1; k < n0 && top; k++) top = strcmp(left[k].on, left[i].on) != 0;
    if(!top || !strncmp(left[i].src, stock_prefix, strlen(stock_prefix))) continue;
    int k = 0;
    while(k < ncovers && strcmp(covers[k], left[i].on)) k++;
    if(k < ncovers) {
      deferred[k] = 1;
      under_new++;
      continue;
    }
    if(nleft == MAX_RECORDS) {
      log_err("%s still shows the theme: too many overlays recorded", left[i].on);
      continue;
    }
    if(!cover_with_stock(left[i].on, left[i].real, &left[nleft])) nleft++;
  }
  if(write_mount_records(left, nleft)) log_err("could not record the overlays that stayed");

  /* Stage copies are disposable once nothing is mounted from them. */
  DIR *d = opendir(STAGE_DIR);
  if(d) {
    struct dirent *e;
    while((e = readdir(d))) {
      if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      if(keep && !strncmp(e->d_name, keep, strlen(keep))) continue;
      char p[PATH_MAX];
      snprintf(p, sizeof(p), "%s/%s", STAGE_DIR, e->d_name);
      int kept = 0;
      for(int k = 0; k < nkept_stages && !kept; k++) kept = !strcmp(kept_stages[k], p);
      for(int k = 0; k < nleft && !kept; k++) kept = !strcmp(left[k].src, p);
      if(kept) continue;
      if(plat_is_our_mount(p, "")) continue;  /* never delete under a live mount */
      fs_rm_rf_under(STAGE_DIR, p);
    }
    closedir(d);
  }
  if(count) log_msg("reverted %d mount(s)", count);
  if(busy) log_msg("%d overlay(s) were still in use and stay until they are free or the console restarts", busy);
  if(under_new) log_msg("%d of them stay under the new theme's overlay, which goes straight on top", under_new);
}


/* Installed games' and apps' folders (/user/appmeta/<title>,
   /user/app/<title>/sce_sys, CATALOG_DIR/appmeta/<title> and
   CATALOG_DIR/conceptmeta/<concept>). */

#define ICONS_HINT "To see the new game and app icons on the home screen, put the console in rest mode and wake it. " \
                   "The Game Library and the backgrounds show them already."
#define MUSIC_HINT "To hear the new home music, highlight a game or app that plays its own music, let it play, " \
                   "then go back to the home screen."
#define FONTS_HINT "To see the new font, put the console in rest mode and wake it. If the text looks wrong, " \
                   "choose No theme in the manager."
#define ARTWORK_HINT "To see the new Appearance artwork, put the console in rest mode and wake it."

static int
title_target(const char *dir) {
  return !strncmp(dir, APPMETA_DIR "/", sizeof(APPMETA_DIR)) || !strncmp(dir, APP_DIR "/", sizeof(APP_DIR)) ||
         !strncmp(dir, CATALOG_DIR "/", sizeof(CATALOG_DIR));
}

/* The console's text fonts folder. */
/* The Appearance themes' textures (and the welcome backgrounds beside them): read as the interface starts. */
static int
artwork_target(const char *dir) {
  size_t n = strlen(SYS_ASSET_DIR);
  return !strncmp(dir, SYS_ASSET_DIR, n) && (!dir[n] || dir[n] == '/');
}

static int
font_target(const char *dir) {
  size_t n = strlen(FONT_DIR);
  return !strncmp(dir, FONT_DIR, n) && (!dir[n] || dir[n] == '/');
}

/* Folders overlaid only in the home screen's own views: installed titles',
   and the text fonts (which games read too, so a font on the real folder
   would reach them). */
static int
home_view_only(const char *dir) {
  return title_target(dir) || font_target(dir);
}

/* 1 while the home screen is restarting (from its exit until the next one is found and watched, as when the console
   goes into rest mode or wakes): then nothing goes into any app's sandbox. The system builds the next home screen's
   sandbox and those of the apps that start with it meanwhile, and on 13.42 the next home screen never started, so
   the console couldn't wake, after the plugin covered two such apps' views just as they were made (1.79). Their views
   are covered once the new home screen runs (the guard asks for that). */
static int
restart_hold(const char *on) {
  return !strncmp(on, "/mnt/sandbox/", 13) && atomic_load(&shellui_between);
}

/* Is `on` in a ShellUI sandbox other than the one whose process the guard is watching? Nothing of ours goes into
   one (mount_overlay): a sandbox the system is still building (its process not started), one it is taking down,
   or one whose ShellUI is gone. On 13.42 (2026-10-29), the font overlaid in each new ShellUI sandbox while the
   system was still building it, as it woke from rest mode: two instances in a row never started, the system was
   left unable to take their sandboxes down (our overlay on its font folder), and no ShellUI started again until
   the console was restarted. Watched, every overlay in it comes out the moment it exits (shellui_guard). */
static int
shellui_box_unwatched(const char *on) {
  if(strncmp(on, SHELLUI_SANDBOX, sizeof(SHELLUI_SANDBOX) - 1)) return 0;
  const char *n = on + sizeof(SHELLUI_SANDBOX) - 1;
  if(*n < '0' || *n > '9') return 1;
  return atoi(n) != atomic_load(&watched_box);
}

/* Where such a folder is overlaid: only in ShellUI's own views (`view`
   on `on`), the one place it reads games' and apps' pictures and its fonts
   from, and only in the instance being watched, so they come out of its
   sandbox the moment it exits (shellui_guard). On 13.42 an overlay left in
   its view of /user when it exited (rest mode) stopped every game and app
   from starting (CE-105733-9) until the console restarted; overlays on the
   real folders stopped the game itself from starting. */
static int
title_job_ok(int view, const char *on) {
  return view && !strncmp(on, SHELLUI_SANDBOX, sizeof(SHELLUI_SANDBOX) - 1) && !shellui_box_unwatched(on);
}

#ifdef TEST_HOOKS
/* For the test of ShellUI exiting in the middle of an apply: while the file
   PS5THEME_TEST_HOLD_APPLY names exists and says `where` ("before" or
   "after" mounting), the apply waits there. */
static void
test_hold(const char *where) {
  const char *hold = getenv("PS5THEME_TEST_HOLD_APPLY");
  char buf[16];
  int fd = hold ? open(hold, O_RDONLY) : -1;
  if(fd < 0) return;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  buf[n > 0 ? n : 0] = 0;
  buf[strcspn(buf, "\n")] = 0;
  if(strcmp(buf, where)) return;
  log_msg("test: holding the apply %s mounting", where);
  while(!access(hold, F_OK)) usleep(50 * 1000);
}

/* For the test of an overlay that fails its check where the previous theme's
   stays under it: 1 if the file PS5THEME_TEST_FAIL_VERIFY names holds `on`. */
static int
test_fail_verify(const char *on) {
  const char *name = getenv("PS5THEME_TEST_FAIL_VERIFY");
  char buf[PATH_MAX];
  int fd = name ? open(name, O_RDONLY) : -1;
  if(fd < 0) return 0;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  buf[n > 0 ? n : 0] = 0;
  buf[strcspn(buf, "\n")] = 0;
  return !strcmp(buf, on);
}
#endif

/* Games and apps the applied theme has pictures for that weren't installed when it was applied (collect_targets):
   the background styler applies the theme again once one is (autostyle.c). The main thread writes them, the
   styler reads them, under the lock. */
static pthread_mutex_t awaiting_lock = PTHREAD_MUTEX_INITIALIZER;
static char awaiting[64][16];
static int nawaiting;

static void
awaiting_clear(void) {
  pthread_mutex_lock(&awaiting_lock);
  nawaiting = 0;
  pthread_mutex_unlock(&awaiting_lock);
}

/* `rel` is one of a title's folders ("/user/appmeta/<ID>", "/user/app/<ID>/sce_sys"): its title, if the title
   itself isn't installed (its metadata folder isn't there: not one of its folders missing, as a homebrew app's
   sce_sys can be). */
static void
awaiting_note(const char *rel) {
  const char *id = !strncmp(rel, APPMETA_DIR "/", sizeof(APPMETA_DIR)) ? rel + sizeof(APPMETA_DIR)
                 : !strncmp(rel, APP_DIR "/", sizeof(APP_DIR)) ? rel + sizeof(APP_DIR) : NULL;
  char t[16], meta[PATH_MAX];
  struct stat st;
  if(!id || strlen(id) < 9 || (id[9] && id[9] != '/')) return;
  memcpy(t, id, 9);
  t[9] = 0;
  snprintf(meta, sizeof(meta), "%s/%s", APPMETA_DIR, t);
  if(!stat(meta, &st) && S_ISDIR(st.st_mode)) return;
  pthread_mutex_lock(&awaiting_lock);
  int seen = 0;
  for(int i = 0; i < nawaiting && !seen; i++) seen = !strcmp(awaiting[i], t);
  if(!seen && nawaiting < (int)(sizeof(awaiting) / sizeof(*awaiting))) snprintf(awaiting[nawaiting++], sizeof(awaiting[0]), "%s", t);
  pthread_mutex_unlock(&awaiting_lock);
}

int
titles_awaiting_install(char (*out)[16], int max) {
  pthread_mutex_lock(&awaiting_lock);
  int n = nawaiting < max ? nawaiting : max;
  memcpy(out, awaiting, sizeof(*out) * (size_t)n);
  pthread_mutex_unlock(&awaiting_lock);
  return n;
}

/* Collect parent directories of every file under files_root. */
static int targets_overflow;   /* (main thread) the last collect_targets stopped at MAX_TARGETS */

int max_theme_folders(void) { return MAX_TARGETS; }

static int
collect_targets(const char *files_root, const char *rel,
                char targets[][PATH_MAX], int *count) {
  if(!*rel) targets_overflow = 0;
  if(rel_depth(rel) > THEME_MAX_DEPTH) {
    log_err("theme: skipping %s%s, deeper than any folder of the console", files_root, rel);
    return 0;
  }
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s%s", files_root, rel);

  DIR *d = opendir(dir);
  if(!d) return -1;
  struct dirent *e;
  int has_files = 0, ret = 0;
  while(!ret && (e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char child_rel[PATH_MAX], child[PATH_MAX];
    struct stat st;
    snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, e->d_name);
    snprintf(child, sizeof(child), "%s%s", files_root, child_rel);
    if(lstat(child, &st)) continue;
    size_t nl = strlen(e->d_name);
    if(S_ISDIR(st.st_mode)) ret = collect_targets(files_root, child_rel, targets, count);
    else if(S_ISREG(st.st_mode) && !(nl > 5 && !strcmp(e->d_name + nl - 5, ".part"))) has_files = 1;   /* (one being written isn't) */
  }
  closedir(d);

  if(!ret && has_files) {
    /* A game or app that isn't installed (any more, or yet): its pictures stay in the theme for when it is, and
       aren't a failure of the apply. Icons styled for games installed later make this common after an uninstall. */
    struct stat ts;
    if(title_target(rel) && (stat(rel, &ts) || !S_ISDIR(ts.st_mode))) {
      log_msg("theme: %s isn't installed; its pictures stay in the theme for when it is", rel);
      awaiting_note(rel);
      return 0;
    }
    if(*count >= MAX_TARGETS) { targets_overflow = 1; return -1; }
    snprintf(targets[(*count)++], PATH_MAX, "%s", rel);
  }
  return ret;
}


#define MAX_VIEWS     256       /* views that can show a themable folder */
#define MAX_ALL_VIEWS 8192      /* every nullfs mount on the console (1,072 seen on 13.42) */
#define MAX_JOBS      (MAX_TARGETS * 6)   /* (a game's folders have two or three views; the system's about a dozen) */
#define MAX_PREV      (MAX_TARGETS * 2)   /* stage folders kept: the applied theme's and the previous one's */

typedef struct job {
  char on[PATH_MAX];      /* mount point */
  char real[PATH_MAX];    /* real folder whose themed copy is mounted there */
  int  view;              /* 1 if on is a nullfs view of real, not real itself */
} job_t;

typedef struct stage {
  char   real[PATH_MAX];
  char   dir[PATH_MAX];
  plan_t plan;
  int    state;           /* 0 staged and verified, -1 failed */
  int    counted;
  unsigned long long fp_inputs;   /* the stock copy and the theme files it was built from */
  unsigned long long fp_staged;   /* the theme files as they are in dir */
} stage_t;

/* A stage folder of an earlier apply, which may be reused: the applied
   theme's (live) or the one shown before it (a spare). */
typedef struct prev_stage {
  char dir[PATH_MAX], real[PATH_MAX];
  unsigned long long fp_inputs, fp_staged;
  int live, used;         /* used: reused by this apply */
} prev_stage_t;


/* Does a view of `from` show part of a themable folder? True when `from` is
   the root, an ancestor of an allowed prefix (e.g. /system_ex or /mnt/rnps,
   which app sandboxes mirror whole), or a folder inside one. */
static int
view_relevant(const char *from) {
  size_t n = strlen(from);
  char inside[PATH_MAX];

  if(!strcmp(from, "/")) return 1;
  for(size_t i = 0; i < NPREFIXES; i++) {
    if(!strncmp(allowed_prefixes[i], from, n) && allowed_prefixes[i][n] == '/') return 1;
  }
  /* A title's folder under /user/app holds its sce_sys folder. */
  if(!strncmp(from, APP_DIR "/", sizeof(APP_DIR)) && from[sizeof(APP_DIR)] && !strchr(from + sizeof(APP_DIR), '/')) return 1;
  snprintf(inside, sizeof(inside), "%s/", from);
  return target_allowed(inside);
}


/* A view of a view, in terms of the real folder. The console shows /user
   again as /original_user, and ShellUI's sandboxes then show
   /original_user/appmeta: its source is /user/appmeta. Only views of /user
   are followed, the one place this happens. */
static void
view_source(const plat_view_t *all, int n, char *from, size_t size) {
  for(int hop = 0; hop < 4; hop++) {
    int changed = 0;
    for(int j = 0; j < n && !changed; j++) {
      size_t on = strlen(all[j].on);
      if(strncmp(all[j].from, "/user", 5) || (all[j].from[5] && all[j].from[5] != '/')) continue;
      if(strncmp(from, all[j].on, on) || (from[on] && from[on] != '/')) continue;
      char next[PATH_MAX];
      if(snprintf(next, sizeof(next), "%s%s", all[j].from, from + on) >= (int)sizeof(next)) return;
      snprintf(from, size, "%s", next);
      changed = 1;
    }
    if(!changed) return;
  }
}


/* All views of themable folders, from the full mount table. */
static int
relevant_views(plat_view_t *out, int max) {
  plat_view_t *all = malloc(sizeof(*all) * MAX_ALL_VIEWS);
  int count = 0;
  if(!all) return 0;
  int n = plat_list_views(all, MAX_ALL_VIEWS);
  for(int i = 0; i < n && count < max; i++) {
    plat_view_t v = all[i];
    view_source(all, n, v.from, sizeof(v.from));
    if(view_relevant(v.from)) out[count++] = v;
  }
  free(all);
  return count;
}


static int
cmp_job(const void *a, const void *b) {
  return strcmp(((const job_t *)a)->on, ((const job_t *)b)->on);
}


/* ---------------- stock mirrors ----------------

   A verified copy of each system folder a theme overlays, kept per firmware at
   STOCK_DIR/<fw><folder>, which is also where stock_path() finds the system
   version of a file. It is made once, while none of our overlays covers the
   folder. Staging then links its files instead of copying the folder again,
   so it takes seconds and can run while the previous theme is still shown. */

static void
mirror_paths(const char *real, char *dir, size_t dsize, char *mark, size_t msize) {
  char fw[16];
  fw_string(fw, sizeof(fw));
  snprintf(dir, dsize, "%s/%s%s", STOCK_DIR, fw, real);
  snprintf(mark, msize, "%s/%s/.complete%s.ok", STOCK_DIR, fw, real);
}


/* Is `path` shown through one of our overlays (on it or on a parent)? */
static int
covered_by_us(const char *path) {
  char p[PATH_MAX];
  snprintf(p, sizeof(p), "%s", path);
  for(;;) {
    if(plat_is_our_mount(p, STAGE_DIR "/")) return 1;
    char *slash = strrchr(p, '/');
    if(!slash || slash == p) return 0;
    *slash = 0;
  }
}


/* Every file and link under `live` is the same under `copy`. */
static int
same_tree(const char *live, const char *copy, int depth) {
  if(depth > 32) return -1;
  DIR *d = opendir(live);
  if(!d) return -1;
  struct dirent *e;
  int ret = 0;
  while(!ret && (e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char a[PATH_MAX], b[PATH_MAX], la[PATH_MAX], lb[PATH_MAX];
    struct stat sa, sb;
    if(snprintf(a, sizeof(a), "%s/%s", live, e->d_name) >= (int)sizeof(a) ||
       snprintf(b, sizeof(b), "%s/%s", copy, e->d_name) >= (int)sizeof(b) ||
       lstat(a, &sa) || lstat(b, &sb) || (sa.st_mode & S_IFMT) != (sb.st_mode & S_IFMT)) {
      ret = -1;
    } else if(S_ISDIR(sa.st_mode)) {
      ret = same_tree(a, b, depth + 1);
    } else if(S_ISLNK(sa.st_mode)) {
      ssize_t na = readlink(a, la, sizeof(la) - 1), nb = readlink(b, lb, sizeof(lb) - 1);
      ret = na < 0 || na != nb || memcmp(la, lb, na) ? -1 : 0;
    } else if(S_ISREG(sa.st_mode)) {
      uint32_t ca, cb;
      uint64_t za, zb;
      ret = fs_crc32_file(a, &ca, &za) || fs_crc32_file(b, &cb, &zb) || ca != cb || za != zb ? -1 : 0;
    }
    if(ret) log_err("stock copy: %s differs from %s", b, a);
  }
  closedir(d);
  return ret;
}


/* Bytes in the regular files under `dir`. */
static uint64_t
tree_size(const char *dir, int depth) {
  uint64_t total = 0;
  if(depth > 32) return 0;
  DIR *d = opendir(dir);
  if(!d) return 0;
  struct dirent *e;
  while((e = readdir(d))) {
    char p[PATH_MAX];
    struct stat st;
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if(snprintf(p, sizeof(p), "%s/%s", dir, e->d_name) >= (int)sizeof(p) || lstat(p, &st)) continue;
    if(S_ISDIR(st.st_mode)) total += tree_size(p, depth + 1);
    else if(S_ISREG(st.st_mode)) total += (uint64_t)st.st_size;
  }
  closedir(d);
  return total;
}


static int
mirror_ready(const char *real) {
  char dir[PATH_MAX], mark[PATH_MAX];
  mirror_paths(real, dir, sizeof(dir), mark, sizeof(mark));
  return !access(mark, F_OK);
}


static void fingerprint_walk(char *path, size_t len, size_t root, uint64_t *sum, uint64_t *count);

/* A game's or app's folder as it is now: each file's name, identity, size and time, no file read. 0 if it can't be
   told. Kept beside the copy's mark (never in it: the mark's own identity is part of what a stage folder was built
   from, and changing it would rebuild them all). */
static uint64_t
live_fingerprint(const char *real) {
  char *path = malloc(PATH_MAX);
  uint64_t sum = 0, count = 0;
  if(!path) return 0;
  int len = snprintf(path, PATH_MAX, "%s", real);
  if(len > 0 && len < PATH_MAX) fingerprint_walk(path, (size_t)len, (size_t)len, &sum, &count);
  free(path);
  return count ? (sum ^ (count * 0xff51afd7ed558ccdull)) | 1 : 0;
}

static uint64_t
read_fingerprint(const char *mark) {
  char path[PATH_MAX], line[32] = "";
  snprintf(path, sizeof(path), "%s.fp", mark);
  FILE *f = fopen(path, "r");
  if(!f) return 0;
  if(!fgets(line, sizeof(line), f)) line[0] = 0;
  fclose(f);
  return strtoull(line, NULL, 16);
}

static void
write_fingerprint(const char *mark, uint64_t fp) {
  char path[PATH_MAX], line[32];
  if(!fp) return;
  snprintf(path, sizeof(path), "%s.fp", mark);
  int n = snprintf(line, sizeof(line), "%016llx\n", (unsigned long long)fp);
  if(fs_write_file(path, line, (size_t)n)) log_err("could not write %s", path);
}

/* 0 with the mirror's path in `dir`; 1 if it must first be made but one of
   our overlays covers the folder; -1 if it could not be made. */
static int
ensure_mirror(const char *real, char *dir, size_t size) {
  char mark[PATH_MAX], mark_dir[PATH_MAX];
  uint64_t budget = STAGE_SIZE_LIMIT;

  mirror_paths(real, dir, size, mark, sizeof(mark));
  if(!access(mark, F_OK)) {
    /* A game's or app's folder changes when it is installed again or
       updated: a copy that no longer matches is made again, while none of
       our overlays covers the folder (system folders only change with the
       firmware). */
    if(!title_folder(real) || covered_by_us(real) || source_in_use(dir)) return 0;
    /* Unchanged since it was last found the same (each file's identity, size and time): nothing is read. Comparing
       every file of every game's folder, both ways, at every apply took 30 s on the console (1.77): the theme's
       overlays are in the home screen's view, never on the folders themselves, so none of them was ever skipped. */
    uint64_t now = live_fingerprint(real);
    if(now && now == read_fingerprint(mark)) return 0;
    if(!same_tree(real, dir, 0) && !same_tree(dir, real, 0)) {
      write_fingerprint(mark, now);
      return 0;
    }
    log_msg("%s changed since its copy was kept; keeping a new one", real);
    if(fs_unlink(mark) || fs_rm_rf_under(STOCK_DIR, dir)) return -1;
  }
  if(covered_by_us(real)) return 1;
  crumb(CRUMB_MAIN, "keeping a copy of %s", real);
  log_msg("keeping a verified copy of %s (once per firmware)", real);
  copy_meter = &mirror_meter;
  int copied = !copy_tree(real, dir, &budget, 0);
  copy_meter = NULL;
  if(!copied || same_tree(real, dir, 0)) {
    log_err("could not keep a verified copy of %s", real);
    return -1;
  }
  snprintf(mark_dir, sizeof(mark_dir), "%s", mark);
  *strrchr(mark_dir, '/') = 0;
  if(fs_mkdirs(mark_dir) || fs_write_file(mark, "ok\n", 3)) return -1;
  if(title_folder(real)) write_fingerprint(mark, live_fingerprint(real));   /* (as it was copied) */
  return 0;
}


/* Fingerprints of the files a stage folder holds or was built from: each
   planned file's name and identity (inode, size, modification time). Theme,
   stock and stage files are only ever replaced, never changed in place, so an
   equal fingerprint means the very same files. */
static unsigned long long
fp_mix(unsigned long long h, const void *data, size_t n) {
  const unsigned char *b = data;
  while(n--) {
    h ^= *b++;
    h *= 1099511628211ull;
  }
  return h;
}

static unsigned long long
fp_file(unsigned long long h, const char *path) {
  struct stat st;
  if(stat(path, &st)) return fp_mix(h, "missing", 7);
  unsigned long long v[4] = { st.st_ino, (unsigned long long)st.st_size,
                              (unsigned long long)st.st_mtim.tv_sec, (unsigned long long)st.st_mtim.tv_nsec };
  return fp_mix(h, v, sizeof(v));
}

static unsigned long long
plan_fp(const plan_t *p, const char *base, const char *also) {
  unsigned long long h = 14695981039346656037ull;
  char path[PATH_MAX];
  if(also) h = fp_file(h, also);
  for(int i = 0; i < p->count; i++) {
    h = fp_mix(h, p->rel[i], strlen(p->rel[i]) + 1);
    snprintf(path, sizeof(path), "%s%s", base, p->rel[i]);
    h = fp_file(h, path);
  }
  return h;
}

static int stages_reused;   /* this apply (main thread) */

/* The stock folder (from its mirror) plus the theme's checked files, in
   st->dir, verified. An earlier stage folder for the same folder, built
   from the very same files and unchanged since, is reused instead (renamed
   to st->dir): nothing is copied or read again. */
static int
stage_folder(stage_t *st, const char *src, const char *mirror, const char *mark, int *skipped,
             prev_stage_t *prev, int nprev) {
  uint64_t budget = STAGE_SIZE_LIMIT;
  char mirror_real[PATH_MAX];

  if(!realpath(mirror, mirror_real) || plan_files(src, mirror, mirror_real, "", &st->plan)) {
    log_err("could not check theme files for %s", st->real);
    return -1;
  }
  *skipped += st->plan.skipped;
  if(!st->plan.count) {
    log_err("nothing usable for %s", st->real);
    return -1;
  }
  st->fp_inputs = plan_fp(&st->plan, src, mark);
  for(int k = 0; k < nprev; k++) {
    prev_stage_t *pv = &prev[k];
    if(pv->used || strcmp(pv->real, st->real) || pv->fp_inputs != st->fp_inputs || !pv->fp_staged ||
       !fs_is_dir(pv->dir) || plan_fp(&st->plan, pv->dir, NULL) != pv->fp_staged) {
      continue;
    }
    if(strcmp(pv->dir, st->dir) && fs_rename(pv->dir, st->dir)) break;
    pv->used = 1;
    st->fp_staged = pv->fp_staged;
    stages_reused++;
    return 0;
  }
  fs_rm_rf_under(STAGE_DIR, st->dir);
  stage_meter.total = tree_size(mirror, 0) + tree_size(src, 0);   /* counts only if copied */
  copy_meter = &stage_meter;
  int ok = !copy_tree(mirror, st->dir, &budget, 1);
  /* The theme's files are linked in too (they are only ever replaced, never
     changed in place); copied where the filesystem can't link. */
  for(int k = 0; ok && k < st->plan.count; k++) {
    char from[PATH_MAX], to[PATH_MAX];
    snprintf(from, sizeof(from), "%s%s", src, st->plan.rel[k]);
    snprintf(to, sizeof(to), "%s%s", st->dir, st->plan.rel[k]);
    if(!access(to, F_OK) && fs_unlink(to)) ok = 0;
    else ok = !link_or_copy(from, to, &budget);
  }
  copy_meter = NULL;
  if(!ok || verify_plan(&st->plan, src, st->dir, "staged copy")) {
    log_err("staging failed for %s", st->real);
    fs_rm_rf_under(STAGE_DIR, st->dir);
    return -1;
  }
  st->fp_staged = plan_fp(&st->plan, st->dir, NULL);
  return 0;
}


/* The applied theme's target folders and stage folders. They stay in use
   after the apply, for the view watcher (main thread only). */
static char targets[MAX_TARGETS][PATH_MAX];
static int ntargets;
static stage_t stages[MAX_JOBS];
static int live_stages;           /* stages[] behind the live overlays */
static char live_theme[128];


#define SPARE_FILE BASE_DIR "/spare"   /* stage folders of the theme shown before it */

static void awaiting_clear(void);

/* Drop the applied theme's state; with `drop_record`, LIVE_FILE too. */
static void
forget_live(int drop_record) {
  awaiting_clear();
  for(int k = 0; k < live_stages; k++) plan_free(&stages[k].plan);
  live_stages = 0;
  ntargets = 0;
  live_theme[0] = 0;
  if(drop_record && !access(LIVE_FILE, F_OK)) fs_unlink(LIVE_FILE);
}


static int
plan_push(plan_t *p, const char *rel) {
  if(p->count == p->cap) {
    if(p->cap >= PLAN_MAX_FILES) return -1;
    int cap = p->cap ? p->cap * 2 : 64;
    void *n = realloc(p->rel, (size_t)cap * PATH_MAX);
    if(!n) return -1;
    p->rel = n;
    p->cap = cap;
  }
  snprintf(p->rel[p->count++], PATH_MAX, "%s", rel);
  return 0;
}


/* Written after every apply, so a new copy of the plugin (an update, or a
   restart after a hard kill) can carry on with the overlays in place instead
   of taking them down. Lines: "theme\t<id>", "target\t<path>",
   "stage\t<state>\t<dir>\t<real>" and "file\t<rel>" for that stage. */
static void
save_live(void) {
  char tmp[] = LIVE_FILE ".tmp";
  int fd = fs_open_write(tmp);
  FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
  if(!f) {
    if(fd >= 0) close(fd);
    return;
  }
  fprintf(f, "theme\t%s\n", live_theme);
  for(int i = 0; i < ntargets; i++) fprintf(f, "target\t%s\n", targets[i]);
  for(int k = 0; k < live_stages; k++) {
    fprintf(f, "stage\t%d\t%s\t%s\t%llx\t%llx\n", stages[k].state, stages[k].dir, stages[k].real,
            stages[k].fp_inputs, stages[k].fp_staged);
    for(int r = 0; r < stages[k].plan.count; r++) fprintf(f, "file\t%s\n", stages[k].plan.rel[r]);
  }
  int ok = !ferror(f);
  if(fclose(f) || !ok || fs_rename(tmp, LIVE_FILE)) fs_unlink(tmp);
}


/* Stage records ("stage\t0\t<dir>\t<real>\t<fp_inputs>\t<fp_staged>") from
   `file`, added to prev[n..max) and to kept_stages. */
static int
read_stage_records(const char *file, int live, prev_stage_t *prev, int n, int max) {
  char line[PATH_MAX * 2 + 96];
  FILE *f = fopen(file, "r");
  if(!f) return n;
  while(n < max && fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = 0;
    if(strncmp(line, "stage\t", 6)) continue;
    char *state = line + 6, *dir = strchr(state, '\t'), *real = dir ? strchr(dir + 1, '\t') : NULL;
    char *fps = real ? strchr(real + 1, '\t') : NULL;
    if(!fps || atoi(state)) continue;
    *dir++ = 0;
    *real++ = 0;
    *fps++ = 0;
    prev_stage_t *pv = &prev[n];
    memset(pv, 0, sizeof(*pv));
    if(sscanf(fps, "%llx\t%llx", &pv->fp_inputs, &pv->fp_staged) != 2 ||
       strncmp(dir, STAGE_DIR "/", sizeof(STAGE_DIR)) || !fs_is_dir(dir)) {
      continue;
    }
    snprintf(pv->dir, sizeof(pv->dir), "%s", dir);
    snprintf(pv->real, sizeof(pv->real), "%s", real);
    pv->live = live;
    if(nkept_stages < (int)(sizeof(kept_stages) / sizeof(*kept_stages))) {
      snprintf(kept_stages[nkept_stages++], PATH_MAX, "%s", dir);
    }
    n++;
  }
  fclose(f);
  return n;
}


/* The stage folders of the last apply (LIVE_FILE) and the spares
   (SPARE_FILE), which the next apply may reuse. Their names go to
   kept_stages too, so a revert keeps them. */
static int
load_prev_stages(prev_stage_t *prev, int max) {
  nkept_stages = 0;
  int n = read_stage_records(LIVE_FILE, 1, prev, 0, max);
  return read_stage_records(SPARE_FILE, 0, prev, n, max);
}


/* Before the shown theme's overlays come down: its stage folders that this
   apply did not reuse become the spares, so switching back to it is as quick
   as a re-apply. If it left none (the same theme again), the older spares
   stay. Only these survive the revert that follows (kept_stages). */
static void
keep_spares(prev_stage_t *prev, int nprev) {
  int outgoing = 0;
  for(int k = 0; k < nprev; k++) outgoing += prev[k].live && !prev[k].used && fs_is_dir(prev[k].dir);

  char tmp[] = SPARE_FILE ".tmp";
  int fd = fs_open_write(tmp);
  FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
  if(!f && fd >= 0) close(fd);
  nkept_stages = 0;
  for(int k = 0; k < nprev; k++) {
    prev_stage_t *pv = &prev[k];
    if(pv->used || pv->live != (outgoing > 0) || !fs_is_dir(pv->dir)) continue;
    if(nkept_stages >= (int)(sizeof(kept_stages) / sizeof(*kept_stages))) break;
    snprintf(kept_stages[nkept_stages++], PATH_MAX, "%s", pv->dir);
    if(f) fprintf(f, "stage\t0\t%s\t%s\t%llx\t%llx\n", pv->dir, pv->real, pv->fp_inputs, pv->fp_staged);
  }
  if(f) {
    int ok = !ferror(f);
    if(fclose(f) || !ok || fs_rename(tmp, SPARE_FILE)) fs_unlink(tmp);
  }
  if(!f) nkept_stages = 0;   /* unrecorded folders would never be reused */
}


/* Take the shown theme down, keeping its folders as spares. */
static void
revert_keep_spares(prev_stage_t *prev, int nprev) {
  keep_spares(prev, nprev);
  revert_mounts(NULL, NULL, 0, NULL);
}


/* A stage folder prefix ("g<N>-") no folder has yet: kept folders outlive
   the plugin, and the counter does not. */
static unsigned
next_generation(void) {
  static unsigned generation;
  DIR *d = opendir(STAGE_DIR);
  struct dirent *e;
  while(d && (e = readdir(d))) {
    unsigned n;
    if(sscanf(e->d_name, "g%u-", &n) == 1 && n > generation) generation = n;
  }
  if(d) closedir(d);
  return ++generation;
}


/* Sandboxes (/mnt/sandbox/<app>_<n>/) whose app has exited: never covered
   again, nor any older one of the app, so the launcher's teardown of them
   finds nothing of ours (see shellui_guard). Marked by the guard thread,
   read by both. */
static pthread_mutex_t dead_lock = PTHREAD_MUTEX_INITIALIZER;
static char dead_sandboxes[32][40];
static int ndead, dead_next;

/* The sandbox a mount point is in, "/mnt/sandbox/<app>_<n>/" written to
   `out`; 0 if it is in none. */
static int
sandbox_of(const char *on, char *out, size_t size) {
  static const char prefix[] = "/mnt/sandbox/";
  if(strncmp(on, prefix, sizeof(prefix) - 1)) return 0;
  const char *name = on + sizeof(prefix) - 1, *slash = strchr(name, '/');
  if(!slash || !strchr(name, '_') || strchr(name, '_') > slash) return 0;
  return snprintf(out, size, "%.*s/", (int)(slash - on), on) < (int)size;
}

static void
sandbox_mark_dead(const char *box) {
  pthread_mutex_lock(&dead_lock);
  snprintf(dead_sandboxes[dead_next], sizeof(dead_sandboxes[0]), "%s", box);
  dead_next = (dead_next + 1) % 32;
  if(ndead < 32) ndead++;
  pthread_mutex_unlock(&dead_lock);
}

/* Is `on` in a sandbox not newer than one whose app was seen exiting? */
static int
sandbox_closed(const char *on) {
  char box[64];
  int closed = 0;
  if(!sandbox_of(on, box, sizeof(box))) return 0;
  const char *us = strrchr(box, '_');
  size_t app = (size_t)(us - box);
  int mine = atoi(us + 1);
  pthread_mutex_lock(&dead_lock);
  for(int i = 0; i < ndead && !closed; i++) {
    closed = !strncmp(dead_sandboxes[i], box, app + 1) && atoi(dead_sandboxes[i] + app + 1) >= mine;
  }
  pthread_mutex_unlock(&dead_lock);
  return closed;
}

/* plat_mount_ro, but never into a closed sandbox, nor into a ShellUI sandbox but the watched one: checked before,
   and after, as the guard thread may close it meanwhile (it stops watching it, then marks it, before it lists what
   to take out, so one of the two sees the mount). Every overlay of ours goes on through here. 1 if skipped. */
static int
mount_overlay(const char *src, const char *on) {
  if(sandbox_closed(on) || shellui_box_unwatched(on) || restart_hold(on)) return 1;
  if(plat_mount_ro(src, on)) return -1;
  if(!sandbox_closed(on) && !shellui_box_unwatched(on) && !restart_hold(on)) return 0;
  if(plat_is_our_mount(on, BASE_DIR "/")) plat_unmount_once(on);
  return 1;
}

/* Only the newest sandbox of each app is covered: older ones belong to
   instances that have exited (after rest mode, or a restart) and are being
   or have been taken down by the system, which leaves some behind for good
   (13.42 keeps old ShellUI sandboxes' /system_ex views). Nor one that is
   not newer than an instance seen exiting: between ShellUI exiting and its
   next sandbox appearing, a leftover would otherwise be the newest. */
static int
sandbox_current(const plat_view_t *views, int nviews, const char *on) {
  char box[64];
  if(!sandbox_of(on, box, sizeof(box))) return 1;
  if(sandbox_closed(on)) return 0;
  const char *us = strrchr(box, '_');
  size_t app = (size_t)(us - box);
  int mine = atoi(us + 1);
  for(int v = 0; v < nviews; v++) {
    char other[64];
    if(!sandbox_of(views[v].on, other, sizeof(other)) || strncmp(other, box, app + 1)) continue;
    if(atoi(other + app + 1) > mine) return 0;
  }
  return 1;
}

static int plan_jobs(job_t *jobs, int *failed, int quiet);

static atomic_int guard_looked;     /* the guard's first look is done */
static atomic_int records_stale;    /* the guard took overlays out: the main thread tidies the records */
static atomic_int replan_now;       /* the guard found a new ShellUI: cover its views at once */
static char mount_points[MAX_RECORDS][1024];   /* main thread only */


/* The newest ShellUI sandbox there is (/mnt/sandbox/NPXS40087_<n>, the
   system keeps older ones' folders), which is the running instance's.
   -1 if there is none. */
static int
newest_shellui_box(void) {
  DIR *d = opendir(SANDBOX_ROOT);
  struct dirent *e;
  int newest = -1;
  const char *name = &SHELLUI_SANDBOX[sizeof(SANDBOX_ROOT)];   /* "NPXS40087_" */
  while(d && (e = readdir(d))) {
    if(strncmp(e->d_name, name, strlen(name))) continue;
    int inst = atoi(e->d_name + strlen(name));
    if(inst > newest) newest = inst;
  }
  if(d) closedir(d);
  return newest;
}


/* ShellUI has exited (rest mode, or a crash): its sandbox `inst` (if not
   known, the newest with an overlay of ours) is closed, and our overlays in
   it come out at once, by the mount table, before the launcher
   (SceLncService, unmountAllForShellCoreInternal) takes it down. On 13.42
   an overlay left inside its view of /user made that fail with EBUSY, and
   no game or app would start until the console restarted. The sandbox is
   never covered again; its successor's views are covered as soon as they
   appear. The records are tidied by the main thread. */
static void
release_sandbox(int inst) {
  static char points[MAX_RECORDS][1024];   /* guard thread only */
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  if(inst < 0) {
    int n = plat_our_mounts(BASE_DIR "/", SHELLUI_SANDBOX, points, MAX_RECORDS);
    for(int i = 0; i < n; i++) {
      int k = atoi(points[i] + sizeof(SHELLUI_SANDBOX) - 1);
      if(k > inst) inst = k;
    }
    if(inst < 0) return;
  }
  char box[40];
  snprintf(box, sizeof(box), "%s%03d/", SHELLUI_SANDBOX, inst);
  sandbox_mark_dead(box);   /* first: see mount_overlay */
  int stuck, out = release_overlays(box, NULL, NULL, 0, &stuck);
  atomic_store(&records_stale, 1);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
  log_msg("ShellUI exited: %d overlay(s) taken out of its sandbox %.*s in %ld ms", out,
          (int)strlen(box) - 1 - (int)strlen("/mnt/sandbox/"), box + strlen("/mnt/sandbox/"), ms);
  if(stuck) log_err("ShellUI exited: %d overlay(s) in its sandbox could not be taken out", stuck);
}


/* On a fatal signal (crash.c's handler: system calls and static memory only): our overlays out of ShellUI's
   sandbox, newest first, as the guard takes them out when ShellUI exits. With no copy running, one left there
   would stop the system taking the sandbox down at rest mode (no home screen, or no app starting, until the
   console restarts). From the records, read whole into a static buffer; only one the path shows as ours comes
   off, never a mount of the system's. */
static void
crash_release(void) {
  static char buf[512 << 10];
  static char *ons[MAX_RECORDS];
  int fd = open(MOUNTS_FILE, O_RDONLY), n = 0;
  if(fd < 0) return;
  ssize_t len = 0, r;
  while(len < (ssize_t)sizeof(buf) - 1 && (r = read(fd, buf + len, sizeof(buf) - 1 - (size_t)len)) > 0) len += r;
  close(fd);
  buf[len] = 0;
  for(char *p = buf; *p && n < MAX_RECORDS;) {
    char *end = strchr(p, '\n'), *tab = strchr(p, '\t');
    if(end) *end = 0;
    if(tab && (!end || tab < end)) *tab = 0;
    if(!strncmp(p, SHELLUI_SANDBOX, sizeof(SHELLUI_SANDBOX) - 1)) ons[n++] = p;
    if(!end) break;
    p = end + 1;
  }
  atomic_store(&watched_box, -1);   /* and nothing more goes in */
  for(int i = n - 1; i >= 0; i--) if(plat_is_our_mount(ons[i], BASE_DIR "/")) plat_unmount_once(ons[i]);
}


static void *
shellui_guard(void *arg) {
  int logged = -1, after_exit = 0;
  long long exited = 0;
  (void)arg;
  crash_thread_init(CRUMB_GUARD);
  for(;;) {
    crumb(CRUMB_GUARD, "looking for ShellUI");
    int pid = plat_find_process(SHELLUI_PROCESS);
    if(pid <= 0) {
      atomic_store(&guard_looked, 1);
      /* Soon after it exits, a new one starts: looked for often then, so its views are covered as soon as its
         process is there (see watched_box), early enough for what it reads as it starts. */
      int soon = after_exit && mono_seconds() - exited < 30;
      atomic_store(&guard_hunting, soon);
      usleep(soon ? 10 * 1000 : 1000 * 1000);
      continue;
    }
    /* Its process runs in the newest of its sandboxes: the system builds one before starting it, and only one
       ShellUI runs at a time. Only now does anything of ours go in. */
    int inst = newest_shellui_box();
    atomic_store(&watched_box, inst);
    atomic_store(&shellui_between, 0);   /* (after: its own sandbox is the watched one first) */
    atomic_store(&guard_hunting, 0);
    atomic_store(&guard_looked, 1);
    if(pid != logged) log_msg("watching ShellUI (pid %d, sandbox _%03d) to take overlays out of its sandbox when it exits", pid, inst);
    logged = pid;
    if(!atomic_exchange(&shellui_seen, 1) || after_exit) atomic_store(&replan_now, 1);
    after_exit = 0;
    crumb(CRUMB_GUARD, "watching ShellUI");
    while(!plat_wait_exit(pid, 5000)) {
      if(inst < 0) atomic_store(&watched_box, inst = newest_shellui_box());
    }
    atomic_store(&shellui_between, 1);   /* first: nothing goes into any sandbox until the next one runs */
    atomic_store(&watched_box, -1);      /* nor more into this one (see mount_overlay) */
    release_sandbox(inst);
    after_exit = 1;
    exited = mono_seconds();
    atomic_store(&guard_hunting, 1);
  }
  return NULL;
}


/* The records of overlays the guard took out (see release_sandbox): those
   in a closed sandbox that are no longer mounted go. Main thread. */
static void
tidy_records(void) {
  int n = read_mount_records(records, MAX_RECORDS), left = 0;
  for(int i = 0; i < n; i++) {
    if(!sandbox_closed(records[i].on) || plat_is_our_mount(records[i].on, BASE_DIR "/")) records[left++] = records[i];
  }
  if(left != n && write_mount_records(records, left)) log_err("could not update the overlay records");
}

/* Stopping on request (ps5-theme-stop, SIGUSR2): the theme stays for the
   next copy to carry on with, except the overlays of games' and apps'
   pictures and of fonts, which must not stay in ShellUI's view with no copy
   running to take them out when ShellUI exits (see release_sandbox). Newest first;
   one still in use stays. The number taken out. */
static int
release_titles(int *stuck) {
  int n = read_mount_records(records, MAX_RECORDS), nrec = 0, left = 0;
  for(int i = 0; i < n; i++) if(title_mount_point(records[i].on)) snprintf(mount_points[nrec++], 1024, "%s", records[i].on);
  int out = release_overlays("", title_mount_point, mount_points, nrec, stuck);
  for(int i = 0; i < n; i++) {
    if(!title_mount_point(records[i].on) || plat_is_our_mount(records[i].on, BASE_DIR "/")) records[left++] = records[i];
  }
  if(write_mount_records(records, left)) log_err("could not record the overlays that stayed");
  return out;
}


/* Ask the running copy to step aside: over the web API when it answers
   (`running`, its version, is then known), and otherwise by SIGUSR1 to its
   process, found by its main thread's name (rest mode can leave its web
   server down for a while, see web.c). Copies before 1.50 end on that
   signal, which leaves their overlays in place, as stepping aside does.
   0 once this copy holds the lock. */
static int
take_over(int running) {
  if(running >= 0) {
    log_msg("taking over from the running copy (%d.%02d)", running / 100, running % 100);
    if(!ask_handoff()) return instance_lock(HANDOFF_WAIT_MS);
  }
  int pid = running_copy(getpid());
  if(pid <= 0) {
    /* What this copy saw, to find out why. */
    int fd = !fs_mkdirs(DUMP_DIR) ? fs_open_write(DUMP_DIR "/takeover.txt") : -1;
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if(f) {
      plat_dump_threads(f);
      fclose(f);
    } else if(fd >= 0) {
      close(fd);
    }
    log_err("the running copy doesn't answer on port %d and can't be found (threads listed in %s/takeover.txt)",
            WEB_PORT, DUMP_DIR);
    return -1;
  }
  if(kill(pid, SIGUSR1)) {
    log_err("could not signal the running copy (pid %d): %s", pid, strerror(errno));
    return -1;
  }
  log_msg("taking over from the running copy (pid %d), which doesn't answer on port %d: asked it by a signal", pid, WEB_PORT);
  return instance_lock(HANDOFF_WAIT_MS);
}


/* Carry on with the overlays a previous copy left for theme `active`. 0 if
   they are all there and each is one this version would make (a newer
   version may overlay fewer places); otherwise nothing is kept and the
   caller takes them down and applies. */
static int
adopt_live(const char *active) {
  char line[PATH_MAX * 2 + 64], theme[128] = "";
  int ours = 0, bad = 0;
  FILE *f = fopen(LIVE_FILE, "r");
  if(!f) return -1;
  for(int k = 0; k < live_stages; k++) plan_free(&stages[k].plan);
  live_stages = 0;
  ntargets = 0;
  while(!bad && fgets(line, sizeof(line), f)) {
    line[strcspn(line, "\n")] = 0;
    char *tab = strchr(line, '\t');
    if(!tab) continue;
    *tab++ = 0;
    if(!strcmp(line, "theme")) {
      snprintf(theme, sizeof(theme), "%s", tab);
    } else if(!strcmp(line, "target") && ntargets < MAX_TARGETS) {
      snprintf(targets[ntargets++], PATH_MAX, "%s", tab);
    } else if(!strcmp(line, "stage") && live_stages < MAX_JOBS) {
      char *dir = strchr(tab, '\t'), *real = dir ? strchr(dir + 1, '\t') : NULL;
      if(!dir || !real) { bad = 1; break; }
      *dir++ = 0;
      *real++ = 0;
      char *fps = strchr(real, '\t');
      if(fps) *fps++ = 0;
      stage_t *st = &stages[live_stages++];
      memset(st, 0, sizeof(*st));
      st->state = atoi(tab);
      snprintf(st->dir, sizeof(st->dir), "%s", dir);
      snprintf(st->real, sizeof(st->real), "%s", real);
      if(fps) sscanf(fps, "%llx\t%llx", &st->fp_inputs, &st->fp_staged);
      if(!st->state && (strncmp(dir, STAGE_DIR "/", sizeof(STAGE_DIR)) || !fs_is_dir(dir))) bad = 1;
    } else if(!strcmp(line, "file") && live_stages) {
      if(plan_push(&stages[live_stages - 1].plan, tab)) bad = 1;
    }
  }
  fclose(f);

  /* The recorded overlays must still be ours, and each one this version
     would make, apart from leftovers in an app's older sandbox (one the
     system kept after rest mode, which only the newest instance's views
     replace): they stay recorded, and come down with the next revert.
     Games' and apps' pictures left in one are taken down at once, by
     applying again. */
  int nrec = bad ? 0 : read_mount_records(records, MAX_RECORDS);
  for(int i = 0; i < nrec; i++) if(plat_is_our_mount(records[i].on, STAGE_DIR "/")) ours++;
  if(!bad && ours) {
    static job_t planned[MAX_JOBS];
    static plat_view_t views[MAX_VIEWS];
    int failed = 0, n = plan_jobs(planned, &failed, 1), nviews = relevant_views(views, MAX_VIEWS);
    for(int i = 0; i < nrec && !bad; i++) {
      if(!plat_is_our_mount(records[i].on, STAGE_DIR "/")) continue;
      int found = 0;
      for(int j = 0; j < n && !found; j++) found = !strcmp(planned[j].on, records[i].on);
      if(!found && !title_mount_point(records[i].on) && !sandbox_current(views, nviews, records[i].on)) continue;
      if(!found) {
        log_msg("theme %s: %s is an overlay this version doesn't make; applying again", theme, records[i].on);
        bad = 1;
      }
    }
  }
  if(bad || !ours || strcmp(theme, active) || !ntargets || !live_stages) {
    for(int k = 0; k < live_stages; k++) plan_free(&stages[k].plan);
    live_stages = 0;
    ntargets = 0;
    return -1;
  }
  snprintf(live_theme, sizeof(live_theme), "%s", theme);
  log_msg("theme %s: carrying on with %d overlay(s) left by the previous copy", theme, ours);
  return 0;
}


/* Every place the system shows a target folder: the real path, plus each
   nullfs view of it (ShellUI reads the system through its own sandbox views,
   where a mount on the real path is not visible). One job per mount point;
   one nested inside another is already covered. `quiet` leaves out the log
   lines, for the view watcher. */
static int
plan_jobs(job_t *jobs, int *failed, int quiet) {
  /* Every place the system shows a target folder gets an overlay: the real
     path, plus each nullfs view of it (ShellUI reads the system through its
     own sandbox views, where a mount on the real path is not visible).
     Installed games' and apps' folders, and the fonts, only in ShellUI's
     own views (title_job_ok). */
  static plat_view_t views[MAX_VIEWS];
  int nviews = relevant_views(views, MAX_VIEWS);
  size_t mount_max = plat_mount_path_max();
  int njobs = 0;

  for(int i = 0; i < ntargets; i++) {
    int title = home_view_only(targets[i]);
    for(int v = -1; v < nviews; v++) {
      const char *from = v < 0 ? "" : views[v].from;
      const char *on = v < 0 ? "" : views[v].on;
      size_t flen = strlen(from);
      if(v >= 0 && strcmp(targets[i], from) &&
         (strncmp(targets[i], from, flen) || (flen > 1 && targets[i][flen] != '/'))) continue;
      if(v >= 0 && !strcmp(from, "/")) flen = 0;

      char mount_on[PATH_MAX], real[PATH_MAX], check[PATH_MAX];
      snprintf(real, sizeof(real), "%s", targets[i]);
      if(snprintf(mount_on, sizeof(mount_on), "%s%s", on, targets[i] + flen) >= (int)sizeof(mount_on)) continue;

      /* The kernel keeps a mount point's name in a fixed field (88 bytes on the
         PS5): a longer path is overlaid from the nearest parent that fits. */
      size_t wanted_len = strlen(mount_on);
      while(strlen(mount_on) > mount_max) {
        char *a = strrchr(mount_on, '/'), *b = strrchr(real, '/');
        if(!a || !b || a == mount_on || b == real) break;
        *a = 0;
        *b = 0;
      }
      if(title && !title_job_ok(v >= 0, mount_on)) continue;
      if(v >= 0 && !sandbox_current(views, nviews, mount_on)) continue;
      snprintf(check, sizeof(check), "%s/", real);
      if(strlen(mount_on) > mount_max || !target_allowed(check) ||
         (v >= 0 && strlen(mount_on) < strlen(on))) {
        if(!quiet) log_err("skip %s%s: no folder short enough to mount", v >= 0 ? "view " : "", mount_on);
        if(v < 0) (*failed)++;
        continue;
      }
      if(!quiet && strlen(mount_on) != wanted_len) {
        log_msg("mounting %s from its parent %s (path length limit)", targets[i], mount_on);
      }
      if(njobs == MAX_JOBS) break;
      snprintf(jobs[njobs].on, PATH_MAX, "%s", mount_on);
      snprintf(jobs[njobs].real, PATH_MAX, "%s", real);
      jobs[njobs].view = v >= 0;
      njobs++;
    }
  }

  /* One overlay per mount point; one nested inside another is already covered. */
  qsort(jobs, njobs, sizeof(*jobs), cmp_job);
  int kept = 0;
  for(int i = 0; i < njobs; i++) {
    if(kept) {
      size_t plen = strlen(jobs[kept - 1].on);
      if(!strcmp(jobs[i].on, jobs[kept - 1].on)) continue;
      if(!strncmp(jobs[i].on, jobs[kept - 1].on, plen) && jobs[i].on[plen] == '/') continue;
    }
    if(kept != i) jobs[kept] = jobs[i];
    kept++;
  }
  njobs = kept;
  return njobs;
}


/* ---- what the console shows ----
   A fingerprint of the files a theme had when it was last applied: each
   file's path under files/ and its identity (device, inode, size, change
   time). Theme files are only ever replaced, never changed in place, and the
   copy Discard puts back is made of hard links, so the files as last applied
   and the same files put back by Discard have the same fingerprint, and the
   theme needs no apply then. Read and written with themes_lock held. */
static char applied_files_name[128];
static uint64_t applied_files_print;

static void
fingerprint_walk(char *path, size_t len, size_t root, uint64_t *sum, uint64_t *count) {
  if(rel_depth(path + root) > THEME_MAX_DEPTH) { *count += 1ull << 40; return; }   /* not walked: never the same */
  DIR *d = opendir(path);
  if(!d) return;
  struct dirent *e;
  while((e = readdir(d))) {
    size_t n = strlen(e->d_name);
    struct stat st;
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (n > 5 && !strcmp(e->d_name + n - 5, ".part"))) continue;
    if(len + 1 + n >= PATH_MAX) { *count += 1ull << 40; continue; }   /* can't be told: never the same */
    path[len] = '/';
    memcpy(path + len + 1, e->d_name, n + 1);
    if(!lstat(path, &st)) {
      if(S_ISDIR(st.st_mode)) fingerprint_walk(path, len + 1 + n, root, sum, count);
      else {
        uint64_t h = 1469598103934665603ull;
        for(const char *p = path + root; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ull;
        uint64_t id[5] = { (uint64_t)st.st_dev, (uint64_t)st.st_ino, (uint64_t)st.st_size,
                           (uint64_t)st.st_mtim.tv_sec, (uint64_t)st.st_mtim.tv_nsec };
        for(int i = 0; i < 5; i++) h = (h ^ id[i]) * 1099511628211ull;
        *sum += h * 0x9e3779b97f4a7c15ull + (h >> 29);   /* a sum: the order files are listed in doesn't matter */
        *count += 1;
      }
    }
    path[len] = 0;
  }
  closedir(d);
}


static uint64_t
files_fingerprint(const char *name) {
  char *path = malloc(PATH_MAX);
  uint64_t sum = 0, count = 0;
  if(!path) return 0;
  int len = snprintf(path, PATH_MAX, "%s/%s/files", THEMES_DIR, name);
  if(len > 0 && len < PATH_MAX) fingerprint_walk(path, (size_t)len, (size_t)len, &sum, &count);
  free(path);
  return (sum ^ (count * 0xff51afd7ed558ccdull)) | 1;   /* never 0, which stands for "not known" */
}


void
applied_files_record(const char *name) {
  snprintf(applied_files_name, sizeof(applied_files_name), "%s", name);
  applied_files_print = files_fingerprint(name);
}


void
applied_files_forget(void) {
  applied_files_name[0] = 0;
  applied_files_print = 0;
}


/* Whether `name`'s files are exactly the ones the console shows (applied last, and not changed since). */
int
applied_files_unchanged(const char *name) {
  return applied_files_print && !strcmp(applied_files_name, name) && files_fingerprint(name) == applied_files_print;
}


static int
apply_theme(const char *name) {
  char path[PATH_MAX], files_root[PATH_MAX], fw[16], want_fw[64], title[128];
  int count = 0, mounted = 0, verified = 0, failed = 0, skipped = 0;
  long long t_start = mono_ms(), t_prepared = t_start, t_switched = t_start;

  static prev_stage_t prev[MAX_PREV];
  int nprev = load_prev_stages(prev, MAX_PREV);   /* before the record is cleared */
  forget_live(1);   /* the previous theme's overlays are replaced below */
  stages_reused = 0;
  if(!theme_name_valid(name)) {
    revert_keep_spares(prev, nprev);
    set_result("Invalid theme name.");
    notify("ps5-theme: invalid theme name");
    return -1;
  }
  crumb(CRUMB_MAIN, "applying theme %s", name);

  snprintf(path, sizeof(path), "%s/%s/theme.ini", THEMES_DIR, name);
  if(read_ini_value(path, "fw", want_fw, sizeof(want_fw))) {
    revert_keep_spares(prev, nprev);
    set_result("%s has no theme.ini with a fw= line.", name);
    notify("ps5-theme: %s has no theme.ini with fw=", name);
    return -1;
  }
  if(read_ini_value(path, "name", title, sizeof(title))) snprintf(title, sizeof(title), "%s", name);
  fw_string(fw, sizeof(fw));
  if(!fw_matches(want_fw, fw)) {
    revert_keep_spares(prev, nprev);
    set_result("%s is for firmware %s; this console is %s.", title, want_fw, fw);
    notify("ps5-theme: '%s' is for FW %s, console is %s", title, want_fw, fw);
    return -1;
  }

  snprintf(files_root, sizeof(files_root), "%s/%s/files", THEMES_DIR, name);
  awaiting_clear();
  int unreadable = fs_is_dir(files_root) && collect_targets(files_root, "", targets, &count);
  ntargets = unreadable ? 0 : count;
  if(unreadable) {
    revert_keep_spares(prev, nprev);
    if(targets_overflow) {
      set_result("%s changes files in more than %d folders, more than the plugin applies at once. "
                 "Take some of its pictures out (or style fewer icons), then apply it again.", title, MAX_TARGETS);
      notify("ps5-theme: '%s' changes too many folders to apply. See the manager.", title);
    } else {
      set_result("Could not read %s's files on the console.", title);
      notify("ps5-theme: could not read %s", title);
    }
    return -1;
  }
  char games[200] = "";
  static job_t jobs[MAX_JOBS];
  int njobs = plan_jobs(jobs, &failed, 0);

  /* Stage each distinct real folder once, while the previous theme (if any)
     is still shown. A folder without a stock mirror yet needs the previous
     theme taken down first, since the mirror is copied from the system. */
  char keep[32];
  snprintf(keep, sizeof(keep), "g%u-", next_generation());
  static int job_stage[MAX_JOBS];
  int nstages = 0, views_mounted = 0;

  /* Progress: copying stock folders the first time (by bytes), preparing each
     folder (by bytes where it is copied), the swap, then checking each
     overlay on the console. Copying takes most of the time. */
  int nreal = 0;
  uint64_t to_mirror = 0;
  for(int i = 0; i < njobs; i++) {
    int seen = 0;
    for(int k = 0; k < i && !seen; k++) seen = !strcmp(jobs[k].real, jobs[i].real);
    if(seen) continue;
    nreal++;
    if(!mirror_ready(jobs[i].real)) to_mirror += tree_size(jobs[i].real, 0);
  }
  int band_mirror = to_mirror ? 45 : 0, band_stage = to_mirror ? 85 : 80, band_swap = band_stage + 5;
  memset(&mirror_meter, 0, sizeof(mirror_meter));
  mirror_meter.total = to_mirror;
  mirror_meter.hi = band_mirror;
  snprintf(mirror_meter.label, sizeof(mirror_meter.label), "Keeping a copy of the system's files (first time only)");
  set_progress(1, "Checking the theme's files");

  for(int i = 0; i < njobs; i++) {
    job_t *j = &jobs[i];
    stage_t *st = NULL;
    for(int k = 0; k < nstages; k++) if(!strcmp(stages[k].real, j->real)) { st = &stages[k]; job_stage[i] = k; }
    if(st) continue;
    job_stage[i] = nstages;
    st = &stages[nstages++];
    memset(st, 0, sizeof(*st));
    snprintf(st->real, sizeof(st->real), "%s", j->real);
    snprintf(st->dir, sizeof(st->dir), "%s/%s%d", STAGE_DIR, keep, nstages - 1);
    crumb(CRUMB_MAIN, "applying theme %s: staging %s", name, j->real);

    char src[PATH_MAX], mirror[PATH_MAX];
    snprintf(src, sizeof(src), "%s%s", files_root, j->real);
    struct stat dir_st;   /* followed: the folder may be under the previous theme's overlay */
    if(stat(j->real, &dir_st) || !S_ISDIR(dir_st.st_mode)) {
      log_err("skip (no such folder on this firmware): %s", j->real);
      st->state = -1;
      continue;
    }
    int m = ensure_mirror(j->real, mirror, sizeof(mirror));
    if(m > 0) {
      revert_mounts(keep, NULL, 0, NULL);
      m = ensure_mirror(j->real, mirror, sizeof(mirror));
    }
    memset(&stage_meter, 0, sizeof(stage_meter));
    stage_meter.lo = band_mirror + (band_stage - band_mirror) * (nstages - 1) / (nreal ? nreal : 1);
    stage_meter.hi = band_mirror + (band_stage - band_mirror) * nstages / (nreal ? nreal : 1);
    if(nreal > 1) snprintf(stage_meter.label, sizeof(stage_meter.label), "Preparing the theme (%d of %d)", nstages, nreal);
    else snprintf(stage_meter.label, sizeof(stage_meter.label), "Preparing the theme");
    set_progress(stage_meter.lo, "%s", stage_meter.label);
    char mirror_dir[PATH_MAX], mark[PATH_MAX];
    mirror_paths(j->real, mirror_dir, sizeof(mirror_dir), mark, sizeof(mark));
    st->state = m ? -1 : stage_folder(st, src, mirror, mark, &skipped, prev, nprev);
    set_progress(stage_meter.hi, "%s", stage_meter.label);
  }

  t_prepared = mono_ms();
  /* Swap: the previous theme comes down and this one goes up straight away,
     so the stock look shows only for a moment. The previous theme's folders
     are kept as spares; other old ones go (also after an earlier revert). */
  crumb(CRUMB_MAIN, "applying theme %s: mounting", name);
  set_progress(band_stage, "Switching to the new theme");
  keep_spares(prev, nprev);
  /* Where the previous theme's overlay is still in use, this one goes straight on top of it; where this one then
     doesn't go on, the system's own files do (job_left). */
  static const char *covers[MAX_JOBS];
  static int cover_job[MAX_JOBS], deferred[MAX_JOBS], job_left[MAX_JOBS];
  int ncovers = 0;
  for(int i = 0; i < njobs; i++) {
    job_left[i] = 0;
    if(stages[job_stage[i]].state >= 0) { cover_job[ncovers] = i; covers[ncovers++] = jobs[i].on; }
  }
  revert_mounts(keep, covers, ncovers, deferred);
  for(int k = 0; k < ncovers; k++) job_left[cover_job[k]] = deferred[k];
#ifdef TEST_HOOKS
  test_hold("before");
#endif
  static int job_mounted[MAX_JOBS], job_adopted[MAX_JOBS];
  for(int i = 0; i < njobs; i++) {
    job_t *j = &jobs[i];
    stage_t *st = &stages[job_stage[i]];
    job_mounted[i] = job_adopted[i] = 0;
    if(st->state < 0) {
      if(!j->view) failed++;
      continue;
    }
    int r = mount_overlay(st->dir, j->on);
    /* nullfs won't stack a folder on a view of itself (EDEADLK): the overlay
       that stayed in use there already shows this very folder, so it carries
       on as this one, and nothing goes on top. */
    if(r < 0 && errno == EDEADLK && job_left[i] && !adopt_left_overlay(j->on, st->dir)) {
      log_msg("%s already shows this theme's folder (an overlay still in use): it carries on", j->on);
      job_mounted[i] = job_adopted[i] = 1;
      continue;
    }
    if(r) {
      if(r < 0) log_err("mount %s -> %s failed: %s", st->dir, j->on, strerror(errno));
      if(r < 0 && !j->view) failed++;
      if(job_left[i]) cover_left_theme(j->on, j->real);
      continue;
    }
    if(record_mount(j->on, st->dir, st->real)) log_err("could not record mount %s", j->on);
    job_mounted[i] = 1;
  }

#ifdef TEST_HOOKS
  test_hold("after");
#endif
  t_switched = mono_ms();

  /* Read back through each live mount: this is what ShellUI sees. */
  for(int i = 0; i < njobs; i++) {
    job_t *j = &jobs[i];
    stage_t *st = &stages[job_stage[i]];
    char src[PATH_MAX];
    if(!job_mounted[i] || sandbox_closed(j->on)) continue;   /* taken out since: its app exited */
    crumb(CRUMB_MAIN, "applying theme %s: checking %s", name, j->on);
    set_progress(band_swap + (99 - band_swap) * i / (njobs ? njobs : 1),
                 "Checking it on the console (%d of %d)", i + 1, njobs);
    snprintf(src, sizeof(src), "%s%s", files_root, j->real);
    /* Against the stage folder: the very folder that is mounted. */
    int bad = verify_plan(&st->plan, st->dir, j->on, "live mount");
#ifdef TEST_HOOKS
    bad = bad || test_fail_verify(j->on);
#endif
    if(bad) {
      log_err("verification failed on %s, removing overlay", j->on);
      if(job_adopted[i]) cover_left_theme(j->on, j->real);   /* the one in use stays; the system's files go on top */
      else if(plat_unmount(j->on)) log_err("unmount %s failed: %s", j->on, strerror(errno));
      else if(job_left[i]) cover_left_theme(j->on, j->real);
      if(!j->view) failed++;
      continue;
    }
    log_msg("overlaid %s%s (%d file(s) verified)", j->on, j->view ? " (system view)" : "", st->plan.count);
    if(!st->counted) {
      verified += st->plan.count;
      st->counted = 1;
    }
    if(j->view) views_mounted++;
    mounted++;
  }
  if(mounted) {   /* kept for the view watcher, and for a new copy to carry on with */
    live_stages = nstages;
    snprintf(live_theme, sizeof(live_theme), "%s", name);
    save_live();
  } else {
    for(int k = 0; k < nstages; k++) plan_free(&stages[k].plan);
  }
  long long t_end = mono_ms();
  log_msg("theme %s: %d overlay(s), %d in system views; %d of %d folder(s) reused; "
          "%d check(s) remembered, %d file(s) compared by content; %.1f s (preparing %.1f, switching %.1f, checking %.1f)",
          name, mounted, views_mounted, stages_reused, nstages, asset_checks_remembered(), verified_by_content,
          (t_end - t_start) / 1000.0, (t_prepared - t_start) / 1000.0, (t_switched - t_prepared) / 1000.0,
          (t_end - t_switched) / 1000.0);
  verified_by_content = 0;

  crumb(CRUMB_MAIN, "idle");

  /* Music the browser has not encoded into the home music yet (themes from
     before 1.10 kept only the source). */
  char music[128] = "";
  char home[PATH_MAX];
  snprintf(path, sizeof(path), "%s/%s/%s", THEMES_DIR, name, MUSIC_FILE);
  snprintf(home, sizeof(home), "%s/%s/files%s", THEMES_DIR, name, HOME_MUSIC);
  if(!access(path, F_OK) && access(home, F_OK)) {
    snprintf(music, sizeof(music), " The music is not converted to home music yet: open the theme in the editor.");
  }
  if(!mounted) {
    set_result("%s was not applied: none of its files passed the checks. See Activity.%s%s", title, music, games);
    notify("ps5-theme: '%s' could not be applied, see Activity", title);
    return -1;
  }
  /* What to do to see the changes the home screen only picks up later:
     the home row reads its icons once, as ShellUI starts, and the home
     music starts again only after another app's music has played. */
  int titles = 0, fonts = 0, artwork = 0;
  for(int i = 0; i < count; i++) {
    titles += title_target(targets[i]);
    fonts += font_target(targets[i]);
    artwork += artwork_target(targets[i]);
  }
  int icons = titles > 0, font_hint = fonts > 0, home_music = !access(home, F_OK);
  char hints[640] = "";
  if(icons && !atomic_load(&shellui_seen)) {
    snprintf(games, sizeof(games), " Its game and app pictures wait until the home screen's process is found.");
    icons = 0;
  }
  if(font_hint && !atomic_load(&shellui_seen)) {
    snprintf(games + strlen(games), sizeof(games) - strlen(games), " Its font waits until the home screen's process is found.");
    font_hint = 0;
  }
  if(icons) {
    snprintf(hints + strlen(hints), sizeof(hints) - strlen(hints), " %s", ICONS_HINT);
  }
  if(font_hint) {
    snprintf(hints + strlen(hints), sizeof(hints) - strlen(hints), " %s", FONTS_HINT);
  }
  if(artwork) {
    snprintf(hints + strlen(hints), sizeof(hints) - strlen(hints), " %s", ARTWORK_HINT);
  }
  if(home_music) {
    snprintf(hints + strlen(hints), sizeof(hints) - strlen(hints), " %s", MUSIC_HINT);
  }
  if(failed || skipped) {
    char folders[96] = "";
    if(failed) snprintf(folders, sizeof(folders), " %d of its %d folders could not be used.", failed, count);
    set_result("%s applied partly: %d %s checked, %d skipped.%s%s%s See Activity.%s",
               title, verified, verified == 1 ? "file" : "files", skipped, folders, music, games, hints);
  } else {
    set_result("%s applied: %d %s checked on the console.%s%s%s", title, verified, verified == 1 ? "file" : "files",
               music, games, hints);
  }
  if(atomic_load(&apply_quiet)) {   /* applied again by itself: what for, and rest mode, said once */
    if(failed || skipped) notify("ps5-theme: '%s' applied partly (%d skipped). See Activity.", title, skipped + failed);
    say_apply_note();
    return 0;
  }
  if(failed || skipped) {
    notify("ps5-theme: '%s' applied partly (%d skipped). See Activity.", title, skipped + failed);
  } else {
    notify("ps5-theme: '%s' applied", title);
  }
  say_apply_note();
  if(icons) notify("ps5-theme: %s", ICONS_HINT);
  if(font_hint) notify("ps5-theme: %s", FONTS_HINT);
  if(artwork) notify("ps5-theme: %s", ARTWORK_HINT);
  if(home_music) notify("ps5-theme: %s", MUSIC_HINT);
  return 0;
}


/* ---------------- asset listing ---------------- */

static void
dump_listing(const char *dir, FILE *out, int depth) {
  if(depth > 32) return;
  DIR *d = opendir(dir);
  if(!d) return;
  struct dirent *e;
  while((e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char p[PATH_MAX];
    struct stat st;
    if(snprintf(p, sizeof(p), "%s/%s", dir, e->d_name) >= (int)sizeof(p)) continue;
    if(lstat(p, &st)) continue;
    if(S_ISDIR(st.st_mode)) dump_listing(p, out, depth + 1);
    else if(S_ISREG(st.st_mode)) fprintf(out, "%10lld %s\n", (long long)st.st_size, p);
  }
  closedir(d);
}


/* Diagnostic: the home screen's own files (ShellUI, VSH assets). Read-only
   listing of names and sizes, to find what draws the wallpaper and icons. */
static void
dump_shell_files(void) {
  static const char *const roots[] = { "/system_ex/vsh_asset", "/system_ex/app/NPXS40087" };
  char tmp[] = DUMP_DIR "/shell-files.txt.tmp";
  int fd = fs_open_write(tmp);
  if(fd < 0) return;
  FILE *f = fdopen(fd, "w");
  if(!f) {
    close(fd);
    return;
  }
  for(size_t i = 0; i < sizeof(roots) / sizeof(*roots); i++) dump_listing(roots[i], f, 0);
  fclose(f);
  fs_rename(tmp, DUMP_DIR "/shell-files.txt");
}


/* Runs before any theme is mounted, so it lists stock files. */
static void
dump_assets_once(void) {
  char path[PATH_MAX], tmp[PATH_MAX];

  asset_listing_path(path, sizeof(path));
  if(!access(path, F_OK)) return;

  crumb(CRUMB_MAIN, "listing system assets");
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  int fd = fs_open_write(tmp);
  if(fd < 0) return;
  FILE *f = fdopen(fd, "w");
  if(!f) {
    close(fd);
    return;
  }
  for(size_t i = 0; i < NPREFIXES; i++) {
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s", allowed_prefixes[i]);
    root[strlen(root) - 1] = 0;
    if(!home_view_only(allowed_prefixes[i]) && strcmp(allowed_prefixes[i], SYS_ASSET_DIR "/")) {
      dump_listing(root, f, 0);   /* titles, fonts and /system/vsh_asset are listed live */
    }
  }
  fclose(f);
  fs_rename(tmp, path);
  log_msg("wrote asset listing to %s", path);
}


/* The console's mount table, for diagnosing where the system reads from. */
static void
dump_mount_table(void) {
  char tmp[] = DUMP_DIR "/mounts.txt.tmp";
  int fd = fs_open_write(tmp);
  if(fd < 0) return;
  FILE *f = fdopen(fd, "w");
  if(!f) {
    close(fd);
    return;
  }
  plat_dump_mounts(f);
  static plat_view_t views[MAX_VIEWS];
  int n = relevant_views(views, MAX_VIEWS);
  fprintf(f, "\n# %d view(s) of themable folders; overlays are mirrored into each:\n", n);
  for(int i = 0; i < n; i++) fprintf(f, "# %s -> %s\n", views[i].from, views[i].on);
  fclose(f);
  fs_rename(tmp, DUMP_DIR "/mounts.txt");
  log_msg("mount table: %d view(s) of themable folders, written to %s/mounts.txt", n, DUMP_DIR);
}


/* ---------------- selection ---------------- */

void
read_active(char *out, size_t size) {
  out[0] = 0;
  FILE *f = fopen(ACTIVE_FILE, "r");
  if(f) {
    if(fgets(out, size, f)) out[strcspn(out, "\r\n \t")] = 0;
    fclose(f);
  }
  if(!out[0]) snprintf(out, size, "none");
}


/* Apply again the theme that is chosen, if it is still `expect`: for the icon styler, which works for a while
   after reading what was applied, and the page's own styling of new icons. It never writes the choice, so a theme
   chosen meanwhile is never undone; if one is chosen between the check and the request, the request applies that
   one, as it was going to be anyway, and the note, about this one, is dropped. Notes asked for before the apply
   begins are said together. */
int
reapply_quietly(const char *expect, const char *note) {
  char cur[128];
  read_active(cur, sizeof(cur));
  if(strcmp(cur, expect)) return -1;
  pthread_mutex_lock(&state_lock);
  size_t have = strlen(quiet_note);
  if(strcmp(quiet_theme, expect)) have = 0;   /* (one left for a theme no longer chosen) */
  snprintf(quiet_theme, sizeof(quiet_theme), "%s", expect);
  if(!have) snprintf(quiet_note, sizeof(quiet_note), "%s", note);
  else if(!strstr(quiet_note, note) && have + 1 + strlen(note) < sizeof(quiet_note)) snprintf(quiet_note + have, sizeof(quiet_note) - have, "\n%s", note);
  pthread_mutex_unlock(&state_lock);
  atomic_store(&reapply_requested, 1);
  return 0;
}

/* Called from the web thread; the main loop picks the change up. */
int
write_active(const char *name) {
  char line[128];
  int n = snprintf(line, sizeof(line), "%s\n", name);
  if(fs_write_file(ACTIVE_FILE, line, n)) return -1;
  pthread_mutex_lock(&state_lock);
  loud_wanted = 1;
  pthread_mutex_unlock(&state_lock);
  atomic_store(&reapply_requested, 1);
  return 0;
}

/* Each line of the note taken for this apply, on the console. */
static void
say_apply_note(void) {
  for(char *line = apply_note, *next; line && *line; line = next) {
    next = strchr(line, '\n');
    if(next) *next++ = 0;
    notify("%s", line);
  }
  apply_note[0] = 0;
}


/* ---------------- view watcher ----------------

   The system can make new views of a themed folder after the theme was
   applied: after rest mode, some of ShellUI's apps restart in new sandboxes,
   whose views show the stock files. While a theme is applied, the watcher
   overlays any such view from the same stage folders and checks it, within a
   second of the mount table changing.

   The same views show when a system app keeps crashing as it starts: each
   restart is a new sandbox (NPXS40087_002, _003, ...). If one app restarts
   CRASH_RESTARTS times within CRASH_WINDOW seconds while a theme is applied,
   the theme is the likely cause and is taken down, before the console gives
   up on the app. So is a slower loop, CRASH_SLOW_RESTARTS within
   CRASH_SLOW_WINDOW: on 13.42 ShellUI once restarted about once a minute
   for five minutes, and apps wouldn't start meanwhile. A rest mode cycle
   restarts each app once. Sandboxes of
   earlier restarts can stay in the mount table (ShellUI's _000, _001 and
   _002 at once on the console), so only sandboxes newer than any seen at the
   first look count. */

#define WATCH_FULL_SECONDS 30     /* look again even if the mount count is the same */
#define WATCH_RETRY_SECONDS 60    /* before trying a view that refused the overlay again */

static struct {
  char on[PATH_MAX];
  long long until;   /* (forward-only clock) */
} watch_refused[16];

static int
watch_waiting(const char *on, long long now) {
  for(size_t i = 0; i < sizeof(watch_refused) / sizeof(*watch_refused); i++) {
    if(watch_refused[i].until > now && !strcmp(watch_refused[i].on, on)) return 1;
  }
  return 0;
}

#define CRASH_RESTARTS      3
#define CRASH_WINDOW        60
#define CRASH_SLOW_RESTARTS 5
#define CRASH_SLOW_WINDOW   600

static struct {
  char   title[16];
  int    instance;                /* the newest sandbox seen, e.g. 2 for _002 */
  time_t at[CRASH_SLOW_RESTARTS]; /* the latest restarts, newest first */
  int    restarts;
  unsigned first_look;            /* the look at the mount table that found it */
} apps_seen[64];
static int napps_seen;
static unsigned looks;
static char crash_app[16];        /* the app that kept restarting (main thread) */
static int crash_count, crash_window;   /* how often, within how many seconds */
static int crash_fast_window = CRASH_WINDOW, crash_slow_window = CRASH_SLOW_WINDOW;

/* Note a view of a sandbox (/mnt/sandbox/<title>_<instance>/...). 1 if its
   app has now restarted too often. Every sandbox of an app at the first look
   that finds it sets its baseline, the newest of them. */
static int
note_view(const char *on, time_t now) {
  static const char prefix[] = "/mnt/sandbox/";
  if(strncmp(on, prefix, sizeof(prefix) - 1)) return 0;
  const char *t = on + sizeof(prefix) - 1, *us = strchr(t, '_'), *slash = us ? strchr(us, '/') : NULL;
  if(!us || !slash || us == t || us - t >= 16 || slash - us < 2) return 0;
  int instance = atoi(us + 1);
  int k = 0;
  while(k < napps_seen && (strncmp(apps_seen[k].title, t, us - t) || apps_seen[k].title[us - t])) k++;
  if(k == napps_seen) {
    if(napps_seen == (int)(sizeof(apps_seen) / sizeof(*apps_seen))) return 0;
    memset(&apps_seen[k], 0, sizeof(apps_seen[k]));
    memcpy(apps_seen[k].title, t, us - t);
    apps_seen[k].instance = instance;
    apps_seen[k].first_look = looks;
    napps_seen++;
    return 0;
  }
  if(instance <= apps_seen[k].instance) return 0;
  if(apps_seen[k].first_look == looks) {
    apps_seen[k].instance = instance;
    return 0;
  }
  apps_seen[k].instance = instance;
  memmove(apps_seen[k].at + 1, apps_seen[k].at, sizeof(time_t) * (CRASH_SLOW_RESTARTS - 1));
  apps_seen[k].at[0] = now;
  if(apps_seen[k].restarts < CRASH_SLOW_RESTARTS) apps_seen[k].restarts++;
  int r = apps_seen[k].restarts;
  /* A gap below zero means the clock was set back meanwhile (the console sets it from the network): never within. */
  time_t fast = now - apps_seen[k].at[CRASH_RESTARTS - 1], slow = now - apps_seen[k].at[CRASH_SLOW_RESTARTS - 1];
  if(r >= CRASH_RESTARTS && fast >= 0 && fast <= crash_fast_window) {
    crash_count = CRASH_RESTARTS;
    crash_window = crash_fast_window;
  } else if(r >= CRASH_SLOW_RESTARTS && slow >= 0 && slow <= crash_slow_window) {
    crash_count = CRASH_SLOW_RESTARTS;
    crash_window = crash_slow_window;
  } else {
    return 0;
  }
  snprintf(crash_app, sizeof(crash_app), "%s", apps_seen[k].title);
  return 1;
}

/* 1 if what the watcher has to say about `on` was said already since the theme was applied: a view that stays
   as it is gets looked at again every minute, but said once (on the console, a game's views left under another
   mount wrote the same lines every minute, 5,800 a day, pushing what mattered out of the log). */
static char watch_said_on[64][PATH_MAX];
static int nwatch_said;

static int
watch_said(const char *on) {
  for(int i = 0; i < nwatch_said; i++) if(!strcmp(watch_said_on[i], on)) return 1;
  if(nwatch_said < (int)(sizeof(watch_said_on) / sizeof(*watch_said_on))) {
    snprintf(watch_said_on[nwatch_said++], PATH_MAX, "%s", on);
  }
  return 0;
}

/* A view in an app's sandbox is covered only once it has been there VIEW_SETTLE_MS: the system builds a sandbox
   view by view as the app starts, and an overlay put in while it does can keep the app from starting (1.67 with the
   home screen's font; 1.79 with two apps that start with the home screen, and the console didn't wake). The home
   screen's own views are the exception: they go in as soon as its process is watched, before it reads the home
   row's icons, and its sandbox is whole by then (it starts after it). */
#define VIEW_SETTLE_MS 2000
static struct { char on[PATH_MAX]; long long first; } young[64];
static int nyoung;

static int
view_settling(const char *on, long long now_ms) {
  if(strncmp(on, "/mnt/sandbox/", 13)) return 0;
  if(!strncmp(on, SHELLUI_SANDBOX, sizeof(SHELLUI_SANDBOX) - 1) && !strstr(on, "/sce_sbx/")) return 0;
  int oldest = 0;
  for(int i = 0; i < nyoung; i++) {
    if(!strcmp(young[i].on, on)) return now_ms - young[i].first < VIEW_SETTLE_MS;
    if(young[i].first < young[oldest].first) oldest = i;
  }
  int k = nyoung < (int)(sizeof(young) / sizeof(*young)) ? nyoung++ : oldest;
  snprintf(young[k].on, PATH_MAX, "%s", on);
  young[k].first = now_ms;
  return 1;
}

static void
watch_refuse(const char *on, long long now) {
  static size_t next;
  snprintf(watch_refused[next].on, PATH_MAX, "%s", on);
  watch_refused[next].until = now + WATCH_RETRY_SECONDS;
  next = (next + 1) % (sizeof(watch_refused) / sizeof(*watch_refused));
}

/* 1 if an app keeps restarting while the theme is applied (see above). */
static int watch_force;   /* plan again now, though the mount table looks the same */

static int
watch_views(int reset) {
  static int last_count = -1;
  static long long last_full;
  static job_t jobs[MAX_JOBS];
  time_t now = time(NULL);   /* app restarts: by the wall clock, so rest mode counts between them */
  long long tick = mono_seconds();   /* waits */

  if(reset || !live_stages) {
    last_count = -1;
    napps_seen = 0;
    nwatch_said = 0;
    nyoung = 0;
    return 0;
  }
  int count = plat_mount_count();
  if(count == last_count && tick - last_full < WATCH_FULL_SECONDS && !watch_force) return 0;
  watch_force = 0;
  last_count = count;
  last_full = tick;

  int failed = 0, added = 0, settling = 0;
  long long now_ms = mono_ms();
  int njobs = plan_jobs(jobs, &failed, 1);
  looks++;
  /* Our overlays by the mount table, read once a look and only when needed: one of ours may be under another
     mount at the same point, where the path no longer shows it. Never a second one there. */
  static char ours[MAX_RECORDS][1024];
  int nours = -2;
  for(int i = 0; i < njobs; i++) {
    if(note_view(jobs[i].on, now)) {
      log_err("theme %s: the system app %s restarted %d times within %d s, as when it crashes on start; taking the theme down",
              live_theme, crash_app, crash_count, crash_window);
      return 1;
    }
  }
  for(int i = 0; i < njobs; i++) {
    job_t *j = &jobs[i];
    stage_t *st = NULL;
    for(int k = 0; k < live_stages && !st; k++) {
      if(!stages[k].state && !strcmp(stages[k].real, j->real)) st = &stages[k];
    }
    if(!st || plat_is_our_mount(j->on, STAGE_DIR "/") || watch_waiting(j->on, tick)) continue;
    if(nours == -2) nours = plat_our_mounts(STAGE_DIR "/", "/", ours, MAX_RECORDS);
    int stacked = 0;
    for(int k = 0; k < nours && !stacked; k++) stacked = !strcmp(ours[k], j->on);
    if(stacked) {
      if(!watch_said(j->on)) {
        log_msg("new view %s: an overlay of ours is already there, under another mount; left as it is "
                "(the theme shows there again after rest mode)", j->on);
      }
      watch_refuse(j->on, tick);
      continue;
    }

    if(view_settling(j->on, now_ms)) {   /* (looked at again within a second) */
      settling++;
      continue;
    }
    crumb(CRUMB_MAIN, "overlaying a new view %s", j->on);
    int r = mount_overlay(st->dir, j->on);
    if(r > 0) continue;
    if(r) {
      if(!watch_said(j->on)) log_err("new view %s: overlay refused (%s), trying again every %d s", j->on, strerror(errno), WATCH_RETRY_SECONDS);
      watch_refuse(j->on, tick);
      continue;
    }
    if(record_mount(j->on, st->dir, st->real)) log_err("could not record mount %s", j->on);
    /* Against the stage folder: the theme's own files may have unsaved edits. */
    if(verify_plan(&st->plan, st->dir, j->on, "new view")) {
      log_err("new view %s: verification failed, removing overlay", j->on);
      if(plat_unmount(j->on)) log_err("unmount %s failed: %s", j->on, strerror(errno));
      watch_refuse(j->on, tick);
      continue;
    }
    log_msg("overlaid %s, a view the system made after the theme was applied (%d file(s) verified)",
            j->on, st->plan.count);
    added++;
  }
  if(settling) last_count = -1;   /* a view still being built: looked at again on the next round */
  if(added) {
    log_msg("theme %s: %d new view(s) covered", live_theme, added);
    dump_mount_table();
    /* Look once more on the next round rather than taking the count now: our own
       mounts changed it, but so may a view the system made while these were being
       covered (a sandbox's views come one after another), which taking the count
       would hide until the next full look, up to WATCH_FULL_SECONDS later. Our own
       mounts are skipped on that look. */
    last_count = -1;
  }
  crumb(CRUMB_MAIN, "idle");
  return 0;
}


static atomic_int tile_done;

static void *
tile_thread(void *arg) {
  (void)arg;
  crash_thread_init(CRUMB_TILE);
  crumb(CRUMB_TILE, "registering the home-screen tile");
  tile_install();
  atomic_store(&tile_done, 1);
  crumb(CRUMB_TILE, "done");
  crash_thread_exit(CRUMB_TILE);
  return NULL;
}


int
main(void) {
  char fw[16], active[128], current[128] = {0}, taken_down[256] = "";
  pthread_t web;
  pthread_attr_t attr;

  /* Visible before anything can fail, so a silent start is impossible. */
  crash_install();
  crash_on_fatal(crash_release);
  plat_notify("ps5-theme " PLUGIN_VERSION " starting");
  plat_klog("[ps5-theme] " PLUGIN_VERSION " starting\n");

  /* Privileges first: until then the process may only see a sandbox. */
  crumb(CRUMB_MAIN, "raising privileges");
  if(plat_init()) {
    plat_notify("ps5-theme: could not get privileges. Is the console jailbroken?");
    plat_klog("[ps5-theme] could not raise privileges\n");
    return 1;
  }
  if(fs_guard_init()) {
    plat_notify("ps5-theme: cannot create " BASE_DIR);
    plat_klog("[ps5-theme] cannot create " BASE_DIR "\n");
    return 1;
  }
  log_init();
  fw_string(fw, sizeof(fw));
  log_msg("ps5-theme %s starting, pid %d, firmware %s", PLUGIN_VERSION, getpid(), fw);
#ifdef TEST_HOOKS
  /* Shorter crash-loop windows ("fast,slow" seconds), so the tests needn't
     wait minutes. */
  const char *windows = getenv("PS5THEME_TEST_CRASH_WINDOWS");
  if(windows) sscanf(windows, "%d,%d", &crash_fast_window, &crash_slow_window);
#endif

  /* One copy at a time. A newer copy takes over from an older one (an
     update); an older or equal one leaves the running copy alone. */
  if(instance_lock(0)) {
    /* Its web server may be starting, or coming back after rest mode. */
    int running = running_version(), mine = my_version();
    for(int i = 0; running < 0 && i < TAKEOVER_ASK_SECONDS; i++) {
      sleep(1);
      running = running_version();
    }
    if(running >= mine) {
      log_msg("ps5-theme %d.%02d is already running; this copy exits", running / 100, running % 100);
      return 0;
    }
    if(take_over(running)) {
      log_err("the running copy did not hand over; this copy exits");
      notify("ps5-theme: the running copy did not hand over. Start ps5-theme-stop, then ps5-theme again.");
      return 1;
    }
  } else if(running_version() >= 0) {
    /* A copy from before 1.23 holds no lock and can't hand over. */
    log_err("an older copy without hand-over is running; this copy exits");
    notify("ps5-theme: an older copy is still running. Restart the console to finish the update.");
    return 1;
  }
  record_running();
  plat_set_name(TITLE_ID);   /* the name etaHEN knows a running plugin by */
  /* Uploads cut off by the last copy's end (a crash, a power loss): none is being received now, as only the copy
     holding the lock receives any. */
  if(fs_is_dir(INCOMING_DIR) && fs_rm_rf_under(BASE_DIR, INCOMING_DIR)) log_err("could not clear %s", INCOMING_DIR);
  snapshots_recover();

  /* ShellUI is watched from the start: overlays a previous copy left in
     its view must come out if it exits while this one starts up. */
  pthread_t guard;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  if(pthread_create(&guard, &attr, shellui_guard, NULL)) {
    log_err("could not start watching ShellUI; games' and apps' pictures stay off");
  } else {
    pthread_detach(guard);
  }
  pthread_attr_destroy(&attr);
  crash_report_previous();

  fs_mkdirs(THEMES_DIR);
  fs_mkdirs(STAGE_DIR);
  /* What the storage check works from, so a figure that looks wrong can be seen in Activity. */
  uint64_t avail = plat_free_bytes(BASE_DIR);
  if(avail == UINT64_MAX) log_msg("storage: the free space can't be told; nothing is refused for room");
  else log_msg("storage: %llu MB free, %llu MB of it kept for the console", (unsigned long long)(avail >> 20),
               (unsigned long long)(FS_RESERVE >> 20));
  fs_mkdirs(DUMP_DIR);
  fs_mkdirs(UPDATE_DIR);

  /* 1.09's audio output choice; the plugin no longer plays audio itself. */
  if(!access(BASE_DIR "/audio-route", F_OK)) fs_unlink(BASE_DIR "/audio-route");
  /* 1.42 to 1.52's choice of where games' pictures go: there is one way now. */
  if(!access(BASE_DIR "/title-placement", F_OK)) fs_unlink(BASE_DIR "/title-placement");

  /* Overlays from a previous copy (an update, or plugin Stop, which is a hard
     kill) are kept when they are complete and for the selected theme;
     otherwise they are taken down and the theme is applied again. */
  char listing[PATH_MAX];
  asset_listing_path(listing, sizeof(listing));
  read_active(active, sizeof(active));
  int adopted = strcmp(active, "none") && !access(listing, F_OK) && !adopt_live(active);
  if(adopted) {
    char ini[PATH_MAX], title[128];
    snprintf(ini, sizeof(ini), "%s/%s/theme.ini", THEMES_DIR, active);
    if(read_ini_value(ini, "name", title, sizeof(title))) snprintf(title, sizeof(title), "%s", active);
    pthread_mutex_lock(&state_lock);
    snprintf(applied, sizeof(applied), "%s", active);
    pthread_mutex_unlock(&state_lock);
    snprintf(current, sizeof(current), "%s", active);
    set_result("%s is applied (kept through the plugin's restart).", title);
  } else {
    /* After a reboot the overlays are gone; the last apply's stage folders
       stay, for the apply below to reuse if nothing changed. */
    static prev_stage_t kept[MAX_PREV];
    load_prev_stages(kept, MAX_PREV);
    forget_live(0);
    revert_mounts(NULL, NULL, 0, NULL);
    nkept_stages = 0;
  }
  dump_mount_table();

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, WEB_STACK_SIZE);
  if(pthread_create(&web, &attr, web_server_thread, NULL)) {
    log_err("could not start the web server");
  } else {
    pthread_detach(web);
  }
  pthread_attr_destroy(&attr);

  /* Stock listing for the editor; runs before any theme is mounted. */
  dump_assets_once();
  dump_shell_files();

  /* The icon style for titles installed later: it looks every minute, and works only between applies. */
  pthread_t styler;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  if(pthread_create(&styler, &attr, autostyle_thread, NULL)) log_err("could not start the icon styler");
  else pthread_detach(styler);
  pthread_attr_destroy(&attr);

  /* System app registration can block; keep it off the engine thread. */
  pthread_t tile;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  if(pthread_create(&tile, &attr, tile_thread, NULL)) {
    log_err("could not start the tile installer");
  } else {
    pthread_detach(tile);
  }
  pthread_attr_destroy(&attr);

  long long tile_started = mono_seconds();
  int tile_warned = 0;
  char ip[32];
  lan_address(ip, sizeof(ip));
  if(ip[0]) notify("ps5-theme ready. Open the Themes tile, or http://%s:%d from a computer or phone.", ip, WEB_PORT);
  else notify("ps5-theme ready. Open the Themes tile.");
  crumb(CRUMB_MAIN, "idle");

  /* The guard's first look decides whether games' pictures may go into
     ShellUI's view in the first apply. */
  for(int i = 0; i < 20 && !atomic_load(&guard_looked); i++) usleep(100 * 1000);

  long long last_update_check = -1000000;   /* (looked at once at start) */
  for(;;) {
    if(handoff_due()) {
      log_msg("handing over to the new copy; the theme stays as it is");
      _exit(0);   /* the overlays stay; the lock is released with the process */
    }
    if(stop_requested()) {
      int stuck, out = release_titles(&stuck);
      log_msg("stopping on request: the theme stays; %d overlay(s) of games' and apps' pictures taken out until the next copy starts", out);
      if(stuck) log_err("%d overlay(s) of games' and apps' pictures are in use and stay", stuck);
      notify("ps5-theme stopped. The theme stays; start ps5-theme again to open the manager.");
      _exit(0);
    }
    if(mono_seconds() - last_update_check >= UPDATE_CHECK_SECONDS) {
      last_update_check = mono_seconds();
      if(update_check()) {
        /* The newer copy asks to take over as soon as it is up. Wait for
           that instead of starting an apply (at boot, putting the theme
           back) that it would then have to wait for. */
        for(int i = 0; i < HANDOFF_ASK_SECONDS * 10 && !handoff_requested(); i++) usleep(100 * 1000);
        continue;
      }
    }
    /* Say so if the system is slow to register the tile; nothing else waits on it. */
    if(!tile_warned && !atomic_load(&tile_done) && mono_seconds() - tile_started > 45) {
      tile_warned = 1;
      log_err("tile: registration has not finished after 45 s");
      if(ip[0]) notify("ps5-theme: the Themes tile is slow to register. The manager works now at http://%s:%d", ip, WEB_PORT);
      else notify("ps5-theme: the Themes tile is slow to register; the theme manager still works.");
    }
    read_active(active, sizeof(active));
    int forced = atomic_exchange(&reapply_requested, 0);

    if(forced || strcmp(active, current)) {
      int theme = strcmp(active, "none") != 0;
      pthread_mutex_lock(&state_lock);
      applied[0] = 0;
      snprintf(applying, sizeof(applying), "%s", theme ? active : "");
      progress_pct = -1;
      /* A note asked for with this theme's apply is said once it is done; quietly, unless you asked for one too. */
      int noted = theme && quiet_note[0] && !strcmp(quiet_theme, active);
      if(quiet_note[0] && !noted) log_msg("not said, as '%s' is applied instead: %s", active, quiet_note);
      snprintf(apply_note, sizeof(apply_note), "%s", noted ? quiet_note : "");
      atomic_store(&apply_quiet, noted && !loud_wanted && current[0]);
      quiet_note[0] = quiet_theme[0] = 0;
      loud_wanted = 0;
      pthread_mutex_unlock(&state_lock);
      if(theme) set_progress(0, "Starting");

      if(theme) {
        /* Say on the console what is happening: at start it shows stock until
           the theme is back. */
        char ini[PATH_MAX], title[128];
        snprintf(ini, sizeof(ini), "%s/%s/theme.ini", THEMES_DIR, active);
        if(read_ini_value(ini, "name", title, sizeof(title))) snprintf(title, sizeof(title), "%s", active);
        if(!current[0]) notify("ps5-theme: putting '%s' back...", title);
        else if(!atomic_load(&apply_quiet)) notify("ps5-theme: applying '%s'...", title);
        themes_lock();
        int rc = apply_theme(active);
        if(!rc) applied_files_record(active);
        else applied_files_forget();
        themes_unlock();
        pthread_mutex_lock(&state_lock);
        if(!rc) snprintf(applied, sizeof(applied), "%s", active);
        applying[0] = 0;
        pthread_mutex_unlock(&state_lock);
        if(rc && apply_note[0]) log_msg("not said, as the apply didn't finish: %s", apply_note);
        apply_note[0] = 0;
        atomic_store(&apply_quiet, 0);
        set_progress(-1, "%s", "");
      } else {
        static prev_stage_t prev[MAX_PREV];
        int nprev = load_prev_stages(prev, MAX_PREV);
        forget_live(1);
        revert_keep_spares(prev, nprev);
        themes_lock();
        applied_files_forget();
        themes_unlock();
        if(taken_down[0]) {
          set_result("%s", taken_down);
          taken_down[0] = 0;
        } else {
          set_result("No theme: the console's own look.");
          if(current[0]) notify("ps5-theme: stock theme restored");
        }
      }
      snprintf(current, sizeof(current), "%s", active);
      crumb(CRUMB_MAIN, "idle");
      watch_views(1);
    }
    if(atomic_exchange(&records_stale, 0)) tidy_records();
    if(atomic_exchange(&replan_now, 0)) watch_force = 1;
    if(watch_views(0)) {
      /* The console's interface keeps crashing with this theme: back to
         stock at once, and not again at the next start. */
      char ini[PATH_MAX], title[128];
      snprintf(ini, sizeof(ini), "%s/%s/theme.ini", THEMES_DIR, current);
      if(read_ini_value(ini, "name", title, sizeof(title))) snprintf(title, sizeof(title), "%s", current);
      snprintf(taken_down, sizeof(taken_down),
               "%s was taken down: the console's interface kept restarting with it applied (%s).", title, crash_app);
      notify("ps5-theme: '%s' was taken down because the console's interface kept restarting with it", title);
      if(write_active("none")) log_err("could not select the stock theme");
      continue;
    }
    /* Wait for the next second or a mount change: a new view to cover at
       once (ShellUI reads the home row's icons as it starts). Right after
       ShellUI exited, a few times a second: its successor's views are
       covered as soon as the guard has found its process. */
    plat_wait_events(-1, atomic_load(&replan_now) ? 0 : atomic_load(&guard_hunting) ? 20 : POLL_SECONDS * 1000);
  }
  return 0;
}
