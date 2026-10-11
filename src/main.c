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


#define MAX_TARGETS      64
#define STAGE_SIZE_LIMIT (512ull * 1024 * 1024)
#define POLL_SECONDS     1
#define WEB_STACK_SIZE   (1024 * 1024)

/* Themable folders: ShellUI's React Native apps and its own VSH assets
   (home screen backgrounds, sounds). What may change inside them is decided
   by policy.c; code and unknown types never can. */
static const char *const allowed_prefixes[] = {
  "/system_ex/rnps/apps/",
  "/mnt/rnps/apps/",
  "/system_ex/vsh_asset/",
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
static char result[512];         /* outcome of the last apply, for the UI */
static atomic_int reapply_requested;


static void
set_progress(int pct, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* What the apply is doing, for the manager. The percentage never goes back. */
static void
set_progress(int pct, const char *fmt, ...) {
  va_list ap;
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
  }
  pthread_mutex_unlock(&state_lock);
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
}


/* Rewrite theme.ini atomically. name/fw/music NULL keep the current value. */
int
write_theme_ini(const char *id, const char *name, const char *fw, const music_settings_t *music) {
  char path[PATH_MAX], cur_name[128], cur_fw[64], ini[512];
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
  if(strstr(path, "/../") || strstr(path, "/./") || strstr(path, "//")) return 0;
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

/* Progress of a long copy (the stock mirror), reported every few MB. */
static struct {
  uint64_t done, total, next;
  int lo, hi, on;                     /* on: only while a stock mirror is copied */
} copy_meter;                         /* owned by the main thread */

static void
copy_meter_add(uint64_t n) {
  if(!copy_meter.on || !copy_meter.total) return;
  copy_meter.done += n;
  if(copy_meter.done < copy_meter.next) return;
  copy_meter.next = copy_meter.done + (4u << 20);
  uint64_t done = copy_meter.done < copy_meter.total ? copy_meter.done : copy_meter.total;
  set_progress(copy_meter.lo + (int)((copy_meter.hi - copy_meter.lo) * done / copy_meter.total),
               "Keeping a copy of the system's files (first time only): %llu of %llu MB",
               (unsigned long long)(done >> 20), (unsigned long long)(copy_meter.total >> 20));
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
      ret = use_links && !fs_link(s, t) ? 0 : copy_file(s, t, budget);
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
static int
plan_files(const char *theme_dir, const char *stock_dir, const char *stock_real, const char *rel, plan_t *p) {
  char dir[PATH_MAX];
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


/* Checksum every planned theme file against its copy under other_dir. */
static int
verify_plan(const plan_t *p, const char *theme_dir, const char *other_dir, const char *what) {
  for(int i = 0; i < p->count; i++) {
    char a[PATH_MAX], b[PATH_MAX];
    uint32_t ca, cb;
    uint64_t sa, sb;
    snprintf(a, sizeof(a), "%s%s", theme_dir, p->rel[i]);
    snprintf(b, sizeof(b), "%s%s", other_dir, p->rel[i]);
    if(fs_crc32_file(a, &ca, &sa) || fs_crc32_file(b, &cb, &sb) || ca != cb || sa != sb) {
      log_err("verify (%s): %s does not match %s", what, b, a);
      return -1;
    }
  }
  return 0;
}


/* ---------------- mounts ---------------- */

/* Take every overlay down. Stage folders go too, except those whose name
   starts with `keep` (the theme being applied, staged before this runs). */
static void
revert_mounts(const char *keep) {
  static char lines[MAX_TARGETS * 4 + 64][PATH_MAX];
  int n = 0, count = 0;

  crumb(CRUMB_MAIN, "reverting mounts");
  FILE *f = fopen(MOUNTS_FILE, "r");
  if(f) {
    while(n < (int)(sizeof(lines) / sizeof(*lines)) && fgets(lines[n], PATH_MAX, f)) {
      lines[n][strcspn(lines[n], "\r\n")] = 0;
      if(lines[n][0]) n++;
    }
    fclose(f);
  }
  /* Newest first, so nested and view mounts come off before what they cover. */
  for(int i = n - 1; i >= 0; i--) {
    if(!plat_is_our_mount(lines[i], STAGE_DIR "/")) {
      log_msg("not our mount (already gone): %s", lines[i]);
      continue;
    }
    if(plat_unmount(lines[i])) {
      log_err("unmount %s failed: %s", lines[i], strerror(errno));
    } else {
      log_msg("unmounted %s", lines[i]);
      count++;
    }
  }
  fs_unlink(MOUNTS_FILE);

  /* Stage copies are disposable once nothing is mounted from them. */
  DIR *d = opendir(STAGE_DIR);
  if(d) {
    struct dirent *e;
    while((e = readdir(d))) {
      if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      if(keep && !strncmp(e->d_name, keep, strlen(keep))) continue;
      char p[PATH_MAX];
      snprintf(p, sizeof(p), "%s/%s", STAGE_DIR, e->d_name);
      if(plat_is_our_mount(p, "")) continue;  /* never delete under a live mount */
      fs_rm_rf_under(STAGE_DIR, p);
    }
    closedir(d);
  }
  if(count) log_msg("reverted %d mount(s)", count);
}


static void
revert_all(void) {
  revert_mounts(NULL);
}


static int
record_mount(const char *target) {
  char buf[64 * 1024];
  size_t len = 0;
  FILE *f = fopen(MOUNTS_FILE, "r");

  if(f) {
    len = fread(buf, 1, sizeof(buf) - PATH_MAX - 2, f);
    fclose(f);
  }
  len += snprintf(buf + len, sizeof(buf) - len, "%s\n", target);
  return fs_write_file(MOUNTS_FILE, buf, len);
}


/* Collect parent directories of every file under files_root. */
static int
collect_targets(const char *files_root, const char *rel,
                char targets[][PATH_MAX], int *count) {
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
    if(S_ISDIR(st.st_mode)) ret = collect_targets(files_root, child_rel, targets, count);
    else if(S_ISREG(st.st_mode)) has_files = 1;
  }
  closedir(d);

  if(!ret && has_files) {
    if(*count >= MAX_TARGETS) return -1;
    snprintf(targets[(*count)++], PATH_MAX, "%s", rel);
  }
  return ret;
}


#define MAX_VIEWS     256       /* views that can show a themable folder */
#define MAX_ALL_VIEWS 8192      /* every nullfs mount on the console (1,072 seen on 13.42) */
#define MAX_JOBS      (MAX_TARGETS * 8)

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
} stage_t;


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
  snprintf(inside, sizeof(inside), "%s/", from);
  return target_allowed(inside);
}


/* All views of themable folders, from the full mount table. */
static int
relevant_views(plat_view_t *out, int max) {
  plat_view_t *all = malloc(sizeof(*all) * MAX_ALL_VIEWS);
  int count = 0;
  if(!all) return 0;
  int n = plat_list_views(all, MAX_ALL_VIEWS);
  for(int i = 0; i < n && count < max; i++) {
    if(view_relevant(all[i].from)) out[count++] = all[i];
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


/* 0 with the mirror's path in `dir`; 1 if it must first be made but one of
   our overlays covers the folder; -1 if it could not be made. */
static int
ensure_mirror(const char *real, char *dir, size_t size) {
  char mark[PATH_MAX], mark_dir[PATH_MAX];
  uint64_t budget = STAGE_SIZE_LIMIT;

  mirror_paths(real, dir, size, mark, sizeof(mark));
  if(!access(mark, F_OK)) return 0;
  if(covered_by_us(real)) return 1;
  crumb(CRUMB_MAIN, "keeping a copy of %s", real);
  log_msg("keeping a verified copy of %s (once per firmware)", real);
  copy_meter.on = 1;
  int copied = !copy_tree(real, dir, &budget, 0);
  copy_meter.on = 0;
  if(!copied || same_tree(real, dir, 0)) {
    log_err("could not keep a verified copy of %s", real);
    return -1;
  }
  snprintf(mark_dir, sizeof(mark_dir), "%s", mark);
  *strrchr(mark_dir, '/') = 0;
  if(fs_mkdirs(mark_dir) || fs_write_file(mark, "ok\n", 3)) return -1;
  return 0;
}


/* The stock folder (from its mirror) plus the theme's checked files, in
   st->dir, verified. */
static int
stage_folder(stage_t *st, const char *src, const char *mirror, int *skipped) {
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
  fs_rm_rf_under(STAGE_DIR, st->dir);
  int ok = !copy_tree(mirror, st->dir, &budget, 1);
  for(int k = 0; ok && k < st->plan.count; k++) {
    char from[PATH_MAX], to[PATH_MAX];
    snprintf(from, sizeof(from), "%s%s", src, st->plan.rel[k]);
    snprintf(to, sizeof(to), "%s%s", st->dir, st->plan.rel[k]);
    ok = !copy_file(from, to, &budget);
  }
  if(!ok || verify_plan(&st->plan, src, st->dir, "staged copy")) {
    log_err("staging failed for %s", st->real);
    fs_rm_rf_under(STAGE_DIR, st->dir);
    return -1;
  }
  return 0;
}


/* The applied theme's target folders and stage folders. They stay in use
   after the apply, for the view watcher (main thread only). */
static char targets[MAX_TARGETS][PATH_MAX];
static int ntargets;
static stage_t stages[MAX_JOBS];
static int live_stages;           /* stages[] behind the live overlays */
static char live_theme[128];


static void
forget_live(void) {
  for(int k = 0; k < live_stages; k++) plan_free(&stages[k].plan);
  live_stages = 0;
  ntargets = 0;
  live_theme[0] = 0;
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
     own sandbox views, where a mount on the real path is not visible). */
  static plat_view_t views[MAX_VIEWS];
  int nviews = relevant_views(views, MAX_VIEWS);
  size_t mount_max = plat_mount_path_max();
  int njobs = 0;

  for(int i = 0; i < ntargets; i++) {
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


static int
apply_theme(const char *name) {
  char path[PATH_MAX], files_root[PATH_MAX], fw[16], want_fw[64], title[128];
  int count = 0, mounted = 0, verified = 0, failed = 0, skipped = 0;

  forget_live();   /* the previous theme's overlays are replaced below */
  if(!theme_name_valid(name)) {
    revert_all();
    set_result("Invalid theme name.");
    notify("ps5-theme: invalid theme name");
    return -1;
  }
  crumb(CRUMB_MAIN, "applying theme %s", name);

  snprintf(path, sizeof(path), "%s/%s/theme.ini", THEMES_DIR, name);
  if(read_ini_value(path, "fw", want_fw, sizeof(want_fw))) {
    revert_all();
    set_result("%s has no theme.ini with a fw= line.", name);
    notify("ps5-theme: %s has no theme.ini with fw=", name);
    return -1;
  }
  if(read_ini_value(path, "name", title, sizeof(title))) snprintf(title, sizeof(title), "%s", name);
  fw_string(fw, sizeof(fw));
  if(!fw_matches(want_fw, fw)) {
    revert_all();
    set_result("%s is for firmware %s; this console is %s.", title, want_fw, fw);
    notify("ps5-theme: '%s' is for FW %s, console is %s", title, want_fw, fw);
    return -1;
  }

  snprintf(files_root, sizeof(files_root), "%s/%s/files", THEMES_DIR, name);
  int unreadable = fs_is_dir(files_root) && collect_targets(files_root, "", targets, &count);
  ntargets = unreadable ? 0 : count;
  if(unreadable) {
    revert_all();
    set_result("Could not read %s, or it touches more than %d folders.", title, MAX_TARGETS);
    notify("ps5-theme: could not read %s", title);
    return -1;
  }

  static job_t jobs[MAX_JOBS];
  int njobs = plan_jobs(jobs, &failed, 0);

  /* Stage each distinct real folder once, while the previous theme (if any)
     is still shown. A folder without a stock mirror yet needs the previous
     theme taken down first, since the mirror is copied from the system. */
  static unsigned generation;
  char keep[32];
  snprintf(keep, sizeof(keep), "g%u-", ++generation);
  static int job_stage[MAX_JOBS];
  int nstages = 0, views_mounted = 0, reverted = 0;

  /* Progress: copying stock folders the first time (by bytes), preparing each
     folder, the swap, then checking each overlay on the console. */
  int nreal = 0;
  uint64_t to_mirror = 0;
  for(int i = 0; i < njobs; i++) {
    int seen = 0;
    for(int k = 0; k < i && !seen; k++) seen = !strcmp(jobs[k].real, jobs[i].real);
    if(seen) continue;
    nreal++;
    if(!mirror_ready(jobs[i].real)) to_mirror += tree_size(jobs[i].real, 0);
  }
  int band_mirror = to_mirror ? 55 : 0, band_stage = band_mirror + (to_mirror ? 10 : 25), band_swap = band_stage + 5;
  memset(&copy_meter, 0, sizeof(copy_meter));
  copy_meter.total = to_mirror;
  copy_meter.hi = band_mirror;
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
      revert_mounts(keep);
      reverted = 1;
      m = ensure_mirror(j->real, mirror, sizeof(mirror));
    }
    set_progress(band_mirror, "Preparing the theme (%d of %d)", nstages, nreal);
    st->state = m ? -1 : stage_folder(st, src, mirror, &skipped);
    set_progress(band_mirror + (band_stage - band_mirror) * nstages / (nreal ? nreal : 1),
                 "Preparing the theme (%d of %d)", nstages, nreal);
  }
  copy_meter.total = 0;

  /* Swap: the previous theme comes down and this one goes up straight away,
     so the stock look shows only for a moment. */
  crumb(CRUMB_MAIN, "applying theme %s: mounting", name);
  set_progress(band_stage, "Switching to the new theme");
  if(!reverted) revert_mounts(keep);
  static int job_mounted[MAX_JOBS];
  for(int i = 0; i < njobs; i++) {
    job_t *j = &jobs[i];
    stage_t *st = &stages[job_stage[i]];
    job_mounted[i] = 0;
    if(st->state < 0) {
      if(!j->view) failed++;
      continue;
    }
    if(plat_mount_ro(st->dir, j->on)) {
      log_err("mount %s -> %s failed: %s", st->dir, j->on, strerror(errno));
      if(!j->view) failed++;
      continue;
    }
    if(record_mount(j->on)) log_err("could not record mount %s", j->on);
    job_mounted[i] = 1;
  }

  /* Read back through each live mount: this is what ShellUI sees. */
  for(int i = 0; i < njobs; i++) {
    job_t *j = &jobs[i];
    stage_t *st = &stages[job_stage[i]];
    char src[PATH_MAX];
    if(!job_mounted[i]) continue;
    crumb(CRUMB_MAIN, "applying theme %s: checking %s", name, j->on);
    set_progress(band_swap + (99 - band_swap) * i / (njobs ? njobs : 1),
                 "Checking it on the console (%d of %d)", i + 1, njobs);
    snprintf(src, sizeof(src), "%s%s", files_root, j->real);
    if(verify_plan(&st->plan, src, j->on, "live mount")) {
      log_err("verification failed on %s, removing overlay", j->on);
      if(plat_unmount(j->on)) log_err("unmount %s failed: %s", j->on, strerror(errno));
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
  if(mounted) {   /* kept for the view watcher */
    live_stages = nstages;
    snprintf(live_theme, sizeof(live_theme), "%s", name);
  } else {
    for(int k = 0; k < nstages; k++) plan_free(&stages[k].plan);
  }
  log_msg("theme %s: %d overlay(s), %d in system views", name, mounted, views_mounted);

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
    set_result("%s was not applied: no folder passed verification. See Activity.%s", title, music);
    notify("ps5-theme: '%s' could not be applied, see Activity", title);
    return -1;
  }
  if(failed || skipped) {
    set_result("%s applied partly: %d files verified, %d skipped, %d of %d folders.%s See Activity.",
               title, verified, skipped, mounted, count, music);
  } else {
    set_result("%s applied: %d files verified on the console.%s", title, verified, music);
  }
  if(failed || skipped) {
    notify("ps5-theme: '%s' applied partly (%d files verified, %d skipped). See Activity.", title, verified, skipped + failed);
  } else {
    notify("ps5-theme: '%s' applied (%d files verified)", title, verified);
  }
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
    dump_listing(root, f, 0);
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


/* Called from the web thread; the main loop picks the change up. */
int
write_active(const char *name) {
  char line[128];
  int n = snprintf(line, sizeof(line), "%s\n", name);
  if(fs_write_file(ACTIVE_FILE, line, n)) return -1;
  atomic_store(&reapply_requested, 1);
  return 0;
}


/* ---------------- view watcher ----------------

   The system can make new views of a themed folder after the theme was
   applied: after rest mode, some of ShellUI's apps restart in new sandboxes,
   whose views show the stock files. While a theme is applied, the watcher
   overlays any such view from the same stage folders and checks it, within a
   second of the mount table changing. It never takes anything down. */

#define WATCH_FULL_SECONDS 30     /* look again even if the mount count is the same */
#define WATCH_RETRY_SECONDS 60    /* before trying a view that refused the overlay again */

static struct {
  char on[PATH_MAX];
  time_t until;
} watch_refused[16];

static int
watch_waiting(const char *on, time_t now) {
  for(size_t i = 0; i < sizeof(watch_refused) / sizeof(*watch_refused); i++) {
    if(watch_refused[i].until > now && !strcmp(watch_refused[i].on, on)) return 1;
  }
  return 0;
}

static void
watch_refuse(const char *on, time_t now) {
  static size_t next;
  snprintf(watch_refused[next].on, PATH_MAX, "%s", on);
  watch_refused[next].until = now + WATCH_RETRY_SECONDS;
  next = (next + 1) % (sizeof(watch_refused) / sizeof(*watch_refused));
}

static void
watch_views(int reset) {
  static int last_count = -1;
  static time_t last_full;
  static job_t jobs[MAX_JOBS];
  time_t now = time(NULL);

  if(reset || !live_stages) {
    last_count = -1;
    return;
  }
  int count = plat_mount_count();
  if(count == last_count && now - last_full < WATCH_FULL_SECONDS) return;
  last_count = count;
  last_full = now;

  int failed = 0, added = 0;
  int njobs = plan_jobs(jobs, &failed, 1);
  for(int i = 0; i < njobs; i++) {
    job_t *j = &jobs[i];
    stage_t *st = NULL;
    for(int k = 0; k < live_stages && !st; k++) {
      if(!stages[k].state && !strcmp(stages[k].real, j->real)) st = &stages[k];
    }
    if(!st || plat_is_our_mount(j->on, STAGE_DIR "/") || watch_waiting(j->on, now)) continue;

    crumb(CRUMB_MAIN, "overlaying a new view %s", j->on);
    if(plat_mount_ro(st->dir, j->on)) {
      log_err("new view %s: overlay refused (%s), trying again in %d s", j->on, strerror(errno), WATCH_RETRY_SECONDS);
      watch_refuse(j->on, now);
      continue;
    }
    if(record_mount(j->on)) log_err("could not record mount %s", j->on);
    /* Against the stage folder: the theme's own files may have unsaved edits. */
    if(verify_plan(&st->plan, st->dir, j->on, "new view")) {
      log_err("new view %s: verification failed, removing overlay", j->on);
      if(plat_unmount(j->on)) log_err("unmount %s failed: %s", j->on, strerror(errno));
      watch_refuse(j->on, now);
      continue;
    }
    log_msg("overlaid %s, a view the system made after the theme was applied (%d file(s) verified)",
            j->on, st->plan.count);
    added++;
  }
  if(added) {
    log_msg("theme %s: %d new view(s) covered", live_theme, added);
    dump_mount_table();
    last_count = plat_mount_count();   /* our own mounts changed the count */
  }
  crumb(CRUMB_MAIN, "idle");
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
  char fw[16], active[128], current[128] = {0};
  pthread_t web;
  pthread_attr_t attr;

  /* Visible before anything can fail, so a silent start is impossible. */
  crash_install();
  plat_notify("ps5-theme " PLUGIN_VERSION " starting");
  plat_klog("[ps5-theme] " PLUGIN_VERSION " starting\n");

  /* Privileges first: until then the process may only see a sandbox. */
  crumb(CRUMB_MAIN, "raising privileges");
  if(plat_init()) {
    plat_notify("ps5-theme: could not get privileges. Is etaHEN running?");
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
  crash_report_previous();

  fs_mkdirs(THEMES_DIR);
  fs_mkdirs(STAGE_DIR);
  fs_mkdirs(DUMP_DIR);

  /* 1.09's audio output choice; the plugin no longer plays audio itself. */
  if(!access(BASE_DIR "/audio-route", F_OK)) fs_unlink(BASE_DIR "/audio-route");

  /* Mounts from a previous run (plugin Stop is a hard kill) are cleaned first. */
  revert_all();
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

  time_t tile_started = time(NULL);
  int tile_warned = 0;
  char ip[32];
  lan_address(ip, sizeof(ip));
  if(ip[0]) notify("ps5-theme ready. Open the Themes tile, or http://%s:%d from a computer or phone.", ip, WEB_PORT);
  else notify("ps5-theme ready. Open the Themes tile.");
  crumb(CRUMB_MAIN, "idle");

  for(;;) {
    /* Say so if the system is slow to register the tile; nothing else waits on it. */
    if(!tile_warned && !atomic_load(&tile_done) && time(NULL) - tile_started > 45) {
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
      pthread_mutex_unlock(&state_lock);
      if(theme) set_progress(0, "Starting");

      if(theme) {
        /* At start the console shows stock until the theme is back: say so. */
        if(!current[0]) {
          char ini[PATH_MAX], title[128];
          snprintf(ini, sizeof(ini), "%s/%s/theme.ini", THEMES_DIR, active);
          if(read_ini_value(ini, "name", title, sizeof(title))) snprintf(title, sizeof(title), "%s", active);
          notify("ps5-theme: putting '%s' back...", title);
        }
        themes_lock();
        int rc = apply_theme(active);
        themes_unlock();
        pthread_mutex_lock(&state_lock);
        if(!rc) snprintf(applied, sizeof(applied), "%s", active);
        applying[0] = 0;
        pthread_mutex_unlock(&state_lock);
        set_progress(-1, "%s", "");
      } else {
        forget_live();
        revert_all();
        set_result("Stock home screen.");
        if(current[0]) notify("ps5-theme: stock theme restored");
      }
      snprintf(current, sizeof(current), "%s", active);
      crumb(CRUMB_MAIN, "idle");
      watch_views(1);
    }
    watch_views(0);
    sleep(POLL_SECONDS);
  }
  return 0;
}
