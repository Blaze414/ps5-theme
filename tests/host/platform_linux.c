/* Linux implementation of platform.h for the end-to-end test rig.
   Read-only bind mounts stand in for nullfs. Runs as root in a privileged
   container with a fake /system_ex, /data and /user/app.

   With EMULATE_MOUNTS set (for sandboxes that cannot mount, e.g. a docker
   build step) an overlay is emulated by moving the folder aside and putting a
   symlink to the staged copy in its place. Reads behave the same; the overlay
   is not read-only, so the test suite skips its EROFS assertions then. */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include <sys/mount.h>
#include <sys/stat.h>

#include "platform.h"


int
plat_init(void) {
  return getenv("FAKE_INIT_FAIL") ? -1 : 0;
}


uint32_t
plat_fw_version(void) {
  const char *fw = getenv("FAKE_FW");
  return fw ? (uint32_t)strtoul(fw, NULL, 16) : 0x13420000u;
}


void
plat_notify(const char *msg) {
  FILE *f = fopen("/tmp/notifications.txt", "a");
  if(f) {
    fprintf(f, "%s\n", msg);
    fclose(f);
  }
}


static int
emulated(void) {
  return getenv("EMULATE_MOUNTS") != NULL;
}


static void
under_path(const char *on, char *out, size_t size) {
  snprintf(out, size, "%s.ps5theme-under", on);
}


void
plat_klog(const char *msg) {
  fputs(msg, stderr);
}


int
plat_mount_ro(const char *from, const char *on) {
  if(emulated()) {
    char under[PATH_MAX];
    under_path(on, under, sizeof(under));
    if(rename(on, under)) return -1;
    if(symlink(from, on)) {
      int saved = errno;
      rename(under, on);
      errno = saved;
      return -1;
    }
    return 0;
  }
  if(mount(from, on, NULL, MS_BIND, NULL)) return -1;
  if(mount(NULL, on, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID, NULL)) {
    int saved = errno;
    umount2(on, MNT_DETACH);
    errno = saved;
    return -1;
  }
  return 0;
}


size_t
plat_mount_path_max(void) {
  const char *v = getenv("FAKE_MNAMELEN");
  return v ? (size_t)atoi(v) : 4095;
}


int
plat_unmount(const char *on) {
  if(emulated()) {
    char under[PATH_MAX];
    under_path(on, under, sizeof(under));
    if(unlink(on)) return -1;
    return rename(under, on);
  }
  if(!umount2(on, 0)) return 0;
  return umount2(on, MNT_DETACH);
}


/* /proc/self/mountinfo: id parent major:minor root mountpoint ... For a bind
   mount, "root" is the source directory within its filesystem. The last
   matching line is the topmost mount. */
int
plat_is_our_mount(const char *on, const char *stage_prefix) {
  char line[4096], root[PATH_MAX], point[PATH_MAX];
  int found = 0;

  if(emulated()) {
    char under[PATH_MAX];
    struct stat st;
    ssize_t n = readlink(on, root, sizeof(root) - 1);
    if(n < 0) return 0;
    root[n] = 0;
    under_path(on, under, sizeof(under));
    return !lstat(under, &st) && !strncmp(root, stage_prefix, strlen(stage_prefix));
  }

  FILE *f = fopen("/proc/self/mountinfo", "r");

  if(!f) return 0;
  while(fgets(line, sizeof(line), f)) {
    if(sscanf(line, "%*s %*s %*s %4095s %4095s", root, point) != 2) continue;
    if(strcmp(point, on)) continue;
    found = !strncmp(root, stage_prefix, strlen(stage_prefix));
  }
  fclose(f);
  return found;
}


/* Bind mounts stand in for nullfs views: in mountinfo, "root" is the source. */
int
plat_list_views(plat_view_t *out, int max) {
  char line[4096], root[PATH_MAX], point[PATH_MAX];
  int count = 0;
  FILE *f = fopen("/proc/self/mountinfo", "r");

  if(!f) return 0;
  while(count < max && fgets(line, sizeof(line), f)) {
    if(sscanf(line, "%*s %*s %*s %4095s %4095s", root, point) != 2) continue;
    if(!strcmp(root, "/") || root[0] != '/' || !strcmp(root, point)) continue;
    if(!strncmp(root, "/data/ps5-theme/stage/", 22)) continue;
    snprintf(out[count].from, sizeof(out[count].from), "%s", root);
    snprintf(out[count].on, sizeof(out[count].on), "%s", point);
    count++;
  }
  fclose(f);
  return count;
}


void
plat_dump_mounts(FILE *out) {
  char line[4096];
  FILE *f = fopen("/proc/self/mountinfo", "r");
  if(!f) return;
  while(fgets(line, sizeof(line), f)) fputs(line, out);
  fclose(f);
}


int
plat_register_tile(const char *title_id) {
  FILE *f = fopen("/tmp/tile-registered", "w");
  if(!f) return -1;
  fprintf(f, "%s\n", title_id);
  fclose(f);
  return 0;
}


void
plat_crash_regs(void *ucontext, uint64_t *rip, uint64_t *rsp, uint64_t *rbp) {
  ucontext_t *uc = ucontext;
  *rip = uc ? (uint64_t)uc->uc_mcontext.gregs[REG_RIP] : 0;
  *rsp = uc ? (uint64_t)uc->uc_mcontext.gregs[REG_RSP] : 0;
  *rbp = uc ? (uint64_t)uc->uc_mcontext.gregs[REG_RBP] : 0;
}


void
plat_socket_nosigpipe(int fd) {
  (void)fd;  /* SIGPIPE is ignored process-wide; send errors surface as EPIPE */
}


