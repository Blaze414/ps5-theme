/* What the plugin found on the console, written to DUMP_DIR to find out why something went wrong. */

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include "diag.h"
#include "apply_internal.h"
#include "config.h"
#include "crash.h"
#include "fsguard.h"
#include "log.h"
#include "platform.h"
#include "theme.h"

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
void
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
void
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
  const char *p;
  for(size_t i = 0; (p = theme_folder(i)); i++) {
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s", p);
    root[strlen(root) - 1] = 0;
    if(!home_view_only(p) && strcmp(p, SYS_ASSET_DIR "/")) {
      dump_listing(root, f, 0);   /* titles, fonts and /system/vsh_asset are listed live */
    }
  }
  fclose(f);
  fs_rename(tmp, path);
  log_msg("wrote asset listing to %s", path);
}

/* The console's mount table, for diagnosing where the system reads from. */
void
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
