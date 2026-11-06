/* Themes' folders as the manager changes them: where each part of a theme is kept, the saved copy Discard puts
   back, and the lock that keeps Discard's swap of folders out of sight. Every write goes through fsguard.c and
   stays below THEMES_DIR. */

#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "store.h"
#include "config.h"
#include "fsguard.h"
#include "log.h"
#include "theme.h"

/* Discard puts a theme back by moving its folder aside and the saved one in its place: for that moment there is no
   folder. It does that holding this lock to write; whatever finds a theme's folder missing waits for it and looks
   again (theme_exists, respond_file), and the status lists the themes holding it to read, so no request ever sees a
   theme gone, or the library a card missing for a moment. Taken after themes_lock where both are. */
static pthread_rwlock_t swap_lock = PTHREAD_RWLOCK_INITIALIZER;
static _Thread_local int swap_reading;   /* this thread holds it to read: no swap can be under way, and never again */

void
swap_wait(void) {
  if(swap_reading) return;
  pthread_rwlock_rdlock(&swap_lock);
  pthread_rwlock_unlock(&swap_lock);
}

void
swap_read_begin(void) {
  pthread_rwlock_rdlock(&swap_lock);
  swap_reading = 1;
}

void
swap_read_end(void) {
  swap_reading = 0;
  pthread_rwlock_unlock(&swap_lock);
}

int
theme_exists(const char *id) {
  char dir[PATH_MAX];
  if(!theme_name_valid(id)) return 0;
  snprintf(dir, sizeof(dir), "%s/%s", THEMES_DIR, id);
  if(fs_is_dir(dir)) return 1;
  swap_wait();
  return fs_is_dir(dir);
}

int
theme_file_path(const char *id, const char *asset, char *out, size_t size) {
  return snprintf(out, size, "%s/%s/files%s", THEMES_DIR, id, asset) >= (int)size ? -1 : 0;
}

void
music_path(const char *id, int slot, char *out, size_t size) {
  char file[24];
  music_source_file(slot, file, sizeof(file));
  snprintf(out, size, "%s/%s/%s", THEMES_DIR, id, file);
}

int
preview_path(const char *id, const char *kind, char *out, size_t size) {
  const char *f = !strcmp(kind, "background") ? "preview.jpg" : !strcmp(kind, "picture") ? "picture.jpg"
                : !strcmp(kind, "iconbg") ? "icon-backdrop.jpg" : !strcmp(kind, "home") ? "home.jpg" : NULL;
  if(!f) return -1;
  snprintf(out, size, "%s/%s/%s", THEMES_DIR, id, f);
  return 0;
}

#define CHOICES_FILE "choices.json"   /* the editor's choices (api_parts.c) */

void
choices_path(const char *id, char *out, size_t size) {
  snprintf(out, size, "%s/%s/%s", THEMES_DIR, id, CHOICES_FILE);
}

int
source_path(const char *id, const char *asset, char *out, size_t size) {
  uint64_t h = 1469598103934665603ull;
  for(const char *p = asset; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ull;
  return snprintf(out, size, "%s/%s/sources/%016llx.src", THEMES_DIR, id, (unsigned long long)h) >= (int)size ? -1 : 0;
}

/* Verification 3: atomic move under the theme lock, then read back. */
/* ---- unsaved changes ----
   The first change to a theme keeps a snapshot of it under SNAP_DIR, made of
   hard links (theme files are only ever replaced, never changed in place, so
   the snapshot stays as it was). Save drops the snapshot; Discard puts it
   back. Call with themes_lock held. */
static void
snapshot_path(const char *id, char *out, size_t size) {
  snprintf(out, size, "%s/%s", SNAP_DIR, id);
}

int
theme_unsaved(const char *id) {
  char path[PATH_MAX];
  snapshot_path(id, path, sizeof(path));
  return fs_is_dir(path);
}

int
keep_snapshot(const char *id) {
  char src[PATH_MAX], dst[PATH_MAX], part[PATH_MAX + 8];
  snapshot_path(id, dst, sizeof(dst));
  if(fs_is_dir(dst)) return 0;
  snprintf(src, sizeof(src), "%s/%s", THEMES_DIR, id);
  snprintf(part, sizeof(part), "%s.part", dst);
  fs_mkdirs(SNAP_DIR);
  fs_rm_rf_under(SNAP_DIR, part);
  if(fs_link_tree(src, part) || fs_rename(part, dst)) {
    fs_rm_rf_under(SNAP_DIR, part);
    log_err("theme %s: could not keep a copy to undo changes", id);
    return -1;
  }
  return 0;
}

void
drop_snapshot(const char *id) {
  char path[PATH_MAX];
  snapshot_path(id, path, sizeof(path));
  if(fs_is_dir(path)) fs_rm_rf_under(SNAP_DIR, path);
}

/* Discard: the theme's folder goes aside as <snapshot>.trash and the snapshot takes its place, under swap_lock so
   no reader sees the moment between (see swap_wait). If the second step fails the theme's own files go back. */
int
restore_snapshot(const char *id) {
  char snap[PATH_MAX], dir[PATH_MAX], trash[PATH_MAX + 8];
  snapshot_path(id, snap, sizeof(snap));
  snprintf(dir, sizeof(dir), "%s/%s", THEMES_DIR, id);
  snprintf(trash, sizeof(trash), "%s.trash", snap);
  fs_rm_rf_under(SNAP_DIR, trash);   /* (one left by a Discard that finished: the theme's folder is there) */
  pthread_rwlock_wrlock(&swap_lock);
  int rc = fs_rename(dir, trash);
  if(!rc && fs_rename(snap, dir)) {
    rc = -1;
    if(fs_rename(trash, dir)) log_err("theme %s: discarding failed and its files are in %s; they go back at the next start", id, trash);
  }
  pthread_rwlock_unlock(&swap_lock);
  if(!rc) fs_rm_rf_under(SNAP_DIR, trash);
  return rc;
}

/* Removes one part of a theme's snapshot too (a folder such as "sources"), so the space is free at once. */
void
drop_snapshot_part(const char *id, const char *part) {
  char path[PATH_MAX];
  snapshot_path(id, path, sizeof(path));
  if(!fs_is_dir(path)) return;
  size_t n = strlen(path);
  if(n + 1 + strlen(part) >= sizeof(path)) return;
  snprintf(path + n, sizeof(path) - n, "/%s", part);
  fs_rm_rf_under(SNAP_DIR, path);
}

/* At start: a Discard cut off between its two steps (a crash or a power loss) left the theme's own files aside as
   <snapshot>.trash and no folder in its place. They go back, so the theme is never lost; a .trash beside a theme that
   is there is a Discard's leftover, and goes. Main thread, before the web server starts. */
void
snapshots_recover(void) {
  DIR *d = opendir(SNAP_DIR);
  if(!d) return;
  struct dirent *e;
  while((e = readdir(d))) {
    size_t n = strlen(e->d_name);
    if(n <= 6 || strcmp(e->d_name + n - 6, ".trash")) continue;
    char id[128], trash[PATH_MAX], dir[PATH_MAX];
    if(n - 6 >= sizeof(id)) continue;
    memcpy(id, e->d_name, n - 6);
    id[n - 6] = 0;
    if(!theme_name_valid(id)) continue;
    snprintf(trash, sizeof(trash), "%s/%s", SNAP_DIR, e->d_name);
    snprintf(dir, sizeof(dir), "%s/%s", THEMES_DIR, id);
    if(fs_is_dir(dir)) fs_rm_rf_under(SNAP_DIR, trash);
    else if(!fs_rename(trash, dir)) log_msg("theme %s: put back as it was before a Discard that was cut off", id);
    else log_err("theme %s: could not put back %s", id, trash);
  }
  closedir(d);
}
