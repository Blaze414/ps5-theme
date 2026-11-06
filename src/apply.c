/* The apply engine: a theme's files put on the console as read-only overlays of staged copies, and taken off.

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
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>

#include "apply.h"
#include "apply_internal.h"
#include "config.h"
#include "crash.h"
#include "fsguard.h"
#include "log.h"
#include "mounts.h"
#include "platform.h"
#include "policy.h"
#include "prefs.h"
#include "running.h"
#include "shellui.h"
#include "status.h"
#include "theme.h"

#define STAGE_SIZE_LIMIT (512ull * 1024 * 1024)

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

/* A theme's file copied or linked into a stage folder must be the very file that passed its check (`passed`): one
   swapped for another in between (only something else on the console could) is never staged. */
static int
not_the_checked(const char *src, const struct stat *st, const fs_id_t *passed) {
  fs_id_t now;
  if(!passed || !passed->ino || (!fs_id_of(st, &now) && fs_id_same(&now, passed))) return 0;
  log_err("theme: %s changed after it was checked; not staged", src);
  errno = ESTALE;
  return 1;
}

static int
copy_file(const char *src, const char *dst, uint64_t *budget, const fs_id_t *passed) {
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

  int in = open(src, O_RDONLY | O_CLOEXEC);
  if(in < 0) return -1;
  struct stat opened;
  if(fstat(in, &opened) || !S_ISREG(opened.st_mode) || not_the_checked(src, &opened, passed)) {
    close(in);
    return -1;
  }
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
link_or_copy(const char *from, const char *to, uint64_t *budget, const fs_id_t *passed) {
  static int warned;
  if(!fs_link(from, to)) {
    struct stat linked;
    if(!lstat(to, &linked) && !not_the_checked(from, &linked, passed)) return 0;
    fs_unlink(to);
    return -1;
  }
  if(!warned) {
    warned = 1;
    log_msg("hard links aren't available here (%s); stage folders hold copies, reused while unchanged",
            strerror(errno));
  }
  return copy_file(from, to, budget, passed);
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
      ret = use_links ? link_or_copy(s, t, budget, NULL) : copy_file(s, t, budget, NULL);
    }
    if(ret) log_err("copy failed: %s -> %s (%s)", s, t, strerror(errno));
  }
  closedir(d);
  return ret;
}

#define PLAN_MAX_FILES 4096

static int
plan_push(plan_t *p, const char *rel) {
  if(p->count == p->cap) {
    if(p->cap >= PLAN_MAX_FILES) return -1;
    int cap = p->cap ? p->cap * 2 : 64;
    void *n = realloc(p->rel, (size_t)cap * PATH_MAX);
    if(!n) return -1;
    p->rel = n;
    n = realloc(p->id, (size_t)cap * sizeof(*p->id));
    if(!n) return -1;
    p->id = n;
    p->cap = cap;
  }
  memset(&p->id[p->count], 0, sizeof(*p->id));
  snprintf(p->rel[p->count++], PATH_MAX, "%s", rel);
  return 0;
}

static void
plan_free(plan_t *p) {
  free(p->rel);
  free(p->id);
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
    fs_id_t passed;
    if(asset_validate_id(theme, stock, why, sizeof(why), &passed)) {
      log_err("theme: skipping %s: %s", theme, why);
      p->skipped++;
      continue;
    }
    if(plan_push(p, r)) { ret = -1; break; }
    p->id[p->count - 1] = passed;
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
long long
mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Every planned theme file must be what other_dir shows: the same file, or
   if not, the same bytes (checksum and size). */
int
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

static mount_rec_t records[MAX_RECORDS];   /* (scratch for the records: main thread only) */

/* Is `dir` the source of an overlay still mounted (one that stayed because
   it was in use)? Such a folder must not be deleted or changed. */
static int
source_in_use(const char *dir) {
  int n = read_mount_records(records, MAX_RECORDS);
  for(int i = 0; i < n; i++) if(!strcmp(records[i].src, dir)) return 1;
  return 0;
}

static void mirror_paths(const char *real, char *dir, size_t dsize, char *mark, size_t msize);

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

#define MAX_ALL_VIEWS 8192      /* every nullfs mount on the console (1,072 seen on 13.42) */
#define MAX_PREV      (MAX_TARGETS * 2)   /* stage folders kept: the applied theme's and the previous one's */

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
  const char *p;
  for(size_t i = 0; (p = theme_folder(i)); i++) {
    if(!strncmp(p, from, n) && p[n] == '/') return 1;
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
int
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
    else ok = !link_or_copy(from, to, &budget, &st->plan.id[k]);
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

const char *
live_theme_name(void) {
  return live_theme;
}

int
live_stage_count(void) {
  return live_stages;
}

const stage_t *
live_stage_for(const char *real) {
  for(int k = 0; k < live_stages; k++) {
    if(!stages[k].state && !strcmp(stages[k].real, real)) return &stages[k];
  }
  return NULL;
}

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

/* The console's own look: the applied theme's overlays come down, its stage folders kept as spares. */
void
apply_none(void) {
  static prev_stage_t prev[MAX_PREV];
  int nprev = load_prev_stages(prev, MAX_PREV);
  forget_live(1);
  revert_keep_spares(prev, nprev);
}

/* At start, when a previous copy's overlays can't be carried on with (after a reboot they are gone): any left come
   down, and the last apply's stage folders stay for the next apply to reuse if nothing changed. */
void
apply_after_reboot(void) {
  static prev_stage_t kept[MAX_PREV];
  load_prev_stages(kept, MAX_PREV);
  forget_live(0);
  revert_mounts(NULL, NULL, 0, NULL);
  nkept_stages = 0;
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

/* plat_mount_ro, but never into a closed sandbox, nor into a ShellUI sandbox but the watched one: checked before,
   and after, as the guard thread may close it meanwhile (it stops watching it, then marks it, before it lists what
   to take out, so one of the two sees the mount). Every overlay of ours goes on through here. 1 if skipped. */
int
mount_overlay(const char *src, const char *on) {
  if(sandbox_closed(on) || shellui_box_unwatched(on) || restart_hold(on)) return 1;
  /* The folder shown is the one built: held open from the plugin's folder down (never through a link) while the
     system mounts it by name, and the overlay's top folder compared with it afterwards (an overlay shows its folder's
     inode). A stage folder swapped as it is mounted never stays. */
  struct stat built, shown;
  int held = fs_open_dir(src);
  if(held < 0 || fstat(held, &built)) {
    if(held >= 0) close(held);
    return -1;
  }
  int rc = plat_mount_ro(src, on);
  if(!rc && (stat(on, &shown) || shown.st_ino != built.st_ino)) {
    log_err("overlay: %s is not the folder that was built; taken off %s", src, on);
    plat_unmount_once(on);
    rc = -1;
    errno = EPERM;
  }
  close(held);
  if(rc) return -1;
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


static char mount_points[MAX_RECORDS][1024];   /* main thread only */

/* The records of overlays the guard took out (see release_sandbox in shellui.c): those
   in a closed sandbox that are no longer mounted go. Main thread. */
void
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
   running to take them out when ShellUI exits (see release_sandbox in shellui.c). Newest first;
   one still in use stays. The number taken out. */
int
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

/* Carry on with the overlays a previous copy left for theme `active`. 0 if
   they are all there and each is one this version would make (a newer
   version may overlay fewer places); otherwise nothing is kept and the
   caller takes them down and applies. */
int
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
int
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

/* Whether a folder holds a file at any depth: stops at the first. */
static int
any_file_under(char *path, size_t len, int depth) {
  if(depth > THEME_MAX_DEPTH) return 0;
  DIR *d = opendir(path);
  if(!d) return 0;
  struct dirent *e;
  int found = 0;
  while(!found && (e = readdir(d))) {
    size_t n = strlen(e->d_name);
    struct stat st;
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (n > 5 && !strcmp(e->d_name + n - 5, ".part"))) continue;
    if(len + 1 + n >= PATH_MAX) continue;
    path[len] = '/';
    memcpy(path + len + 1, e->d_name, n + 1);
    if(!lstat(path, &st)) found = S_ISDIR(st.st_mode) ? any_file_under(path, len + 1 + n, depth + 1) : S_ISREG(st.st_mode);
    path[len] = 0;
  }
  closedir(d);
  return found;
}

/* Whether `name` has anything to put on the console (an emptied theme isn't applied again: it would fail). */
int
theme_has_files(const char *name) {
  char *path = malloc(PATH_MAX);
  if(!path) return 1;
  int len = snprintf(path, PATH_MAX, "%s/%s/files", THEMES_DIR, name);
  int found = len > 0 && len < PATH_MAX && any_file_under(path, (size_t)len, 0);
  free(path);
  return found;
}

/* Whether `name`'s files are exactly the ones the console shows (applied last, and not changed since). */
int
applied_files_unchanged(const char *name) {
  return applied_files_print && !strcmp(applied_files_name, name) && files_fingerprint(name) == applied_files_print;
}

int
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
  theme_title(name, title, sizeof(title));
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
    set_result("%s was not applied: none of its files passed the checks. The log in Settings says why.%s%s", title, music, games);
    toast("Theme not applied", "%s: none of its files passed the checks. The log in the manager's Settings says why.", title);
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
  if(icons && !shellui_found()) {
    snprintf(games, sizeof(games), " Its game and app pictures wait until the home screen's process is found.");
    icons = 0;
  }
  if(font_hint && !shellui_found()) {
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
    set_result("%s applied partly: %d %s checked, %d skipped.%s%s%s The log in Settings says why.%s",
               title, verified, verified == 1 ? "file" : "files", skipped, folders, music, games, hints);
  } else {
    set_result("%s applied: %d %s checked on the console.%s%s%s", title, verified, verified == 1 ? "file" : "files",
               music, games, hints);
  }
  if(status_apply_quiet()) {   /* applied again by itself: what for, and rest mode, said once */
    if(failed || skipped) toast("Theme applied partly", "%s: %d skipped. The log in the manager's Settings says why.", title, skipped + failed);
    say_apply_note();
    return 0;
  }
  /* One notification, not one per thing to do: what was applied on its first line, what to do to see all of it on
   the second (unless that is turned off in the manager's Settings). */
  char next[400] = "";
  if(pref_on("tips")) {
    const char *see[3];
    int nsee = 0;
    if(icons) see[nsee++] = "game and app icons";
    if(font_hint) see[nsee++] = "font";
    if(artwork) see[nsee++] = "Appearance artwork";
    if(nsee) {
      snprintf(next, sizeof(next), " Put the console in rest mode to see its new %s%s%s%s%s.", see[0],
               nsee == 3 ? ", " : nsee == 2 ? " and " : "", nsee > 1 ? see[1] : "", nsee == 3 ? " and " : "", nsee == 3 ? see[2] : "");
    }
    if(home_music) {
      snprintf(next + strlen(next), sizeof(next) - strlen(next), " Its music plays after a game's or app's own music has.");
    }
  }
  if(failed || skipped) {
    toast("Theme applied partly", "%s: %d skipped. The log in the manager's Settings says why.%s", title, skipped + failed, next);
  } else {
    toast("Theme applied", "%s.%s", title, next);
  }
  say_apply_note();
  return 0;
}
