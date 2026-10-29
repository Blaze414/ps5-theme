/* Linux implementation of platform.h for the end-to-end test rig.
   Read-only bind mounts stand in for nullfs. Runs as root in a privileged
   container with a fake /system_ex, /data and /user/app.

   With EMULATE_MOUNTS set (for sandboxes that cannot mount, e.g. a docker
   build step) an overlay is emulated by moving the folder aside and putting a
   symlink to the staged copy in its place. Reads behave the same; the overlay
   is not read-only, so the test suite skips its EROFS assertions then. */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

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
  /* As the console's nullfs: a folder is never stacked on a view of itself
     (FreeBSD's "lock against myself" check), EDEADLK. */
  struct stat a, b;
  if(!stat(from, &a) && !stat(on, &b) && a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
    errno = EDEADLK;
    return -1;
  }
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
  /* Like the PS5's: never detached while in use (EBUSY). */
  return umount2(on, 0);
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


static int proc_times(const char *pid, unsigned long long *start, unsigned long long *cpu_us);

/* (not one that has exited and is waiting to be reaped, as on the console) */
int
plat_find_process(const char *name) {
  DIR *d = opendir("/proc");
  struct dirent *e;
  int pid = -1;
  while(d && pid < 0 && (e = readdir(d))) {
    char path[64], comm[64] = "";
    unsigned long long start, cpu;
    if(e->d_name[0] < '1' || e->d_name[0] > '9') continue;
    snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
    FILE *f = fopen(path, "r");
    if(!f) continue;
    if(fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\n")] = 0;
    fclose(f);
    if(!strcmp(comm, name) && !proc_times(e->d_name, &start, &cpu)) pid = atoi(e->d_name);
  }
  if(d) closedir(d);
  return pid;
}


/* A process's start (as plat_process_start gives it) and processor time from /proc/<pid>/stat: its user and
   system time in clock ticks, as the console's kernel keeps ki_runtime. Plus one microsecond, as the start is plus
   one: a process that has used less than a tick has a time that is known, and 0 means one that can't be told.
   0, or -1 if it has gone. */
static int
proc_times(const char *pid, unsigned long long *start, unsigned long long *cpu_us) {
  char path[64], line[1024] = "";
  snprintf(path, sizeof(path), "/proc/%s/stat", pid);
  FILE *f = fopen(path, "r");
  char *rest = f && fgets(line, sizeof(line), f) ? strrchr(line, ')') : NULL;
  if(f) fclose(f);
  unsigned long long ut = 0, st = 0, began = 0;
  char state = 0;
  if(!rest || sscanf(rest + 1, " %c %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %llu %llu %*s %*s %*s %*s %*s %*s %llu",
                     &state, &ut, &st, &began) != 4 || state == 'Z') {
    return -1;
  }
  long tick = sysconf(_SC_CLK_TCK);
  *start = began + 1;
  *cpu_us = (ut + st) * 1000000ull / (unsigned long long)(tick > 0 ? tick : 100) + 1;
  return 0;
}

int
plat_processes_named(const char *name, plat_proc_t *out, int max) {
  DIR *d = opendir("/proc");
  if(!d) return -1;
  struct dirent *e;
  int n = 0;
  while(n < max && (e = readdir(d))) {
    char path[64], comm[64] = "";
    if(e->d_name[0] < '1' || e->d_name[0] > '9') continue;
    snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
    FILE *f = fopen(path, "r");
    if(!f) continue;
    if(fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\n")] = 0;
    fclose(f);
    if(strcmp(comm, name) || proc_times(e->d_name, &out[n].start, &out[n].cpu_us)) continue;
    out[n++].pid = atoi(e->d_name);
  }
  closedir(d);
  return n;
}


void
plat_dump_processes(FILE *out) {
  DIR *d = opendir("/proc");
  struct dirent *e;
  while(d && (e = readdir(d))) {
    char path[64], comm[64] = "";
    unsigned long long start = 0, cpu = 0;
    if(e->d_name[0] < '1' || e->d_name[0] > '9') continue;
    snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
    FILE *f = fopen(path, "r");
    if(!f) continue;
    if(fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\n")] = 0;
    fclose(f);
    proc_times(e->d_name, &start, &cpu);
    fprintf(out, "%s %s cpu=%llu.%03llus\n", e->d_name, comm, cpu / 1000000, cpu / 1000 % 1000);
  }
  if(d) closedir(d);
}


/* Mount table changes show as POLLPRI on /proc/self/mountinfo, and a
   process's exit as POLLIN on its pidfd: the same events the PS5 gets from
   kqueue. */
int
plat_wait_events(int pid, int ms) {
  static int mounts = -1, pidfd = -1, watched = -1;
  if(mounts < 0) mounts = open("/proc/self/mountinfo", O_RDONLY);
  if(pid != watched) {
    if(pidfd >= 0) close(pidfd);
    pidfd = pid > 0 ? (int)syscall(SYS_pidfd_open, pid, 0) : -1;
    watched = pidfd >= 0 ? pid : -1;
  }
  struct pollfd pf[2] = {{mounts, POLLPRI, 0}, {pidfd, POLLIN, 0}};
  int k = poll(pf, 2, ms), events = 0;
  if(k > 0 && (pf[0].revents & (POLLPRI | POLLERR))) {
    char buf[4096];
    events |= PLAT_EV_MOUNTS;
    lseek(mounts, 0, SEEK_SET);
    while(read(mounts, buf, sizeof(buf)) > 0) {}
  }
  if(k > 0 && pidfd >= 0 && (pf[1].revents & POLLIN)) {
    events |= PLAT_EV_EXIT;
    close(pidfd);
    pidfd = -1;
    watched = -1;
  }
  return events;
}


int
plat_wait_exit(int pid, int ms) {
  static __thread int pidfd = -1, watched = -1;
  if(pid != watched) {
    if(pidfd >= 0) close(pidfd);
    /* PS5THEME_TEST_NO_EXIT_EVENTS: as when the console's kqueue won't watch the process */
    const char *no_events = getenv("PS5THEME_TEST_NO_EXIT_EVENTS");
    pidfd = no_events && *no_events ? -1 : (int)syscall(SYS_pidfd_open, pid, 0);
    if(pidfd < 0) {
      if(no_events && *no_events) errno = EPERM;
      watched = -1;
      if(errno == ESRCH) return 1;
      /* can't be watched: wait a little all the same, then look (never straight back, which would spin; never
         long, as an exit is acted on at once) */
      unsigned long long start, cpu;
      char name[16];
      usleep((ms < 50 ? ms : 50) * 1000);
      snprintf(name, sizeof(name), "%d", pid);
      return proc_times(name, &start, &cpu) != 0;
    }
    watched = pid;
  }
  struct pollfd pf = {pidfd, POLLIN, 0};
  if(poll(&pf, 1, ms) > 0 && (pf.revents & POLLIN)) {
    close(pidfd);
    pidfd = -1;
    watched = -1;
    return 1;
  }
  return 0;
}


int
plat_unmount_once(const char *on) {
  if(emulated()) return plat_unmount(on);
  return umount2(on, 0);
}


/* inotify sees opens and reads of a file through any bind mount of it, as
   kqueue's vnode events do on the PS5 through nullfs. */
int
plat_watch_files(const char *const *paths, int n, int seconds,
                 void (*seen)(int, const char *, long, void *), void *ctx) {
  int in = inotify_init1(IN_NONBLOCK), *wds = calloc(n ? n : 1, sizeof(int));
  if(in < 0 || !wds) {
    if(in >= 0) close(in);
    free(wds);
    return -1;
  }
  for(int i = 0; i < n; i++) wds[i] = inotify_add_watch(in, paths[i], IN_OPEN | IN_ACCESS | IN_CLOSE);
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for(;;) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
    if(ms >= seconds * 1000L) break;
    struct pollfd pf = {in, POLLIN, 0};
    poll(&pf, 1, (int)(seconds * 1000L - ms));
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len = read(in, buf, sizeof(buf));
    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
    for(char *p = buf; len > 0 && p < buf + len;) {
      struct inotify_event *ev = (struct inotify_event *)p;
      for(int i = 0; i < n; i++) {
        if(wds[i] != ev->wd) continue;
        if(ev->mask & IN_OPEN) seen(i, "open", ms, ctx);
        if(ev->mask & IN_ACCESS) seen(i, "read", ms, ctx);
        if(ev->mask & IN_CLOSE) seen(i, "close", ms, ctx);
      }
      p += sizeof(*ev) + ev->len;
    }
  }
  free(wds);
  close(in);
  return 0;
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
#ifdef TEST_HOOKS
    /* Like the PS5: /user is shown again as /original_user, and sandboxes
       show folders of /original_user (Linux reports the underlying folder). */
    if(getenv("PS5THEME_TEST_ORIGINAL_USER") && !strncmp(point, "/mnt/sandbox/", 13) && !strncmp(root, "/user/", 6)) {
      snprintf(out[count].from, sizeof(out[count].from), "/original_user%s", root + 5);
      snprintf(out[count].on, sizeof(out[count].on), "%s", point);
      count++;
      continue;
    }
#endif
    snprintf(out[count].from, sizeof(out[count].from), "%s", root);
    snprintf(out[count].on, sizeof(out[count].on), "%s", point);
    count++;
  }
  fclose(f);
  return count;
}


/* Field 22 of /proc/<pid>/stat (start time in clock ticks since boot). */
unsigned long long
plat_process_start(int pid) {
  char path[64], line[1024] = "";
  snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  FILE *f = pid > 0 ? fopen(path, "r") : NULL;
  char *rest = f && fgets(line, sizeof(line), f) ? strrchr(line, ')') : NULL;
  if(f) fclose(f);
  unsigned long long start = 0;
  char state = 0;
  if(!rest || sscanf(rest + 1, " %c %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %llu",
                     &state, &start) != 2 || state == 'Z') {
    return 0;
  }
  return start + 1;
}


int
plat_our_top_mounts(const char *stage_prefix, const char *under, char (*out)[1024], int max, int *buried) {
  /* per call, as two threads ask: the table's mount points in the order made, whether each is ours */
  char (*points)[512] = malloc(4096 * 512), *mine = malloc(4096);
  int *ours = malloc(4096 * sizeof(int));
  char line[4096], root[PATH_MAX], point[PATH_MAX];
  int count = 0, n = 0, nours = 0;
  *buried = 0;
  FILE *f = emulated() || !points || !mine || !ours ? NULL : fopen("/proc/self/mountinfo", "r");
  if(!f) { free(points); free(mine); free(ours); return -1; }
  while(n < 4096 && fgets(line, sizeof(line), f)) {
    if(sscanf(line, "%*s %*s %*s %4095s %4095s", root, point) != 2) continue;
    snprintf(points[n], sizeof(points[n]), "%s", point);
    mine[n] = !strncmp(root, stage_prefix, strlen(stage_prefix));
    if(mine[n] && !strncmp(point, under, strlen(under))) ours[nours++] = n;
    n++;
  }
  fclose(f);
  for(int i = 0; i < nours && count < max; i++) {
    int top = 1;   /* nothing but ours over it */
    for(int k = ours[i] + 1; k < n && top; k++) top = strcmp(points[k], points[ours[i]]) || mine[k];
    if(top) snprintf(out[count++], 1024, "%s", points[ours[i]]);
    else (*buried)++;
  }
  free(points);
  free(mine);
  free(ours);
  return count;
}

int
plat_our_mounts(const char *stage_prefix, const char *under, char (*out)[1024], int max) {
  char line[4096], root[PATH_MAX], point[PATH_MAX];
  int count = 0;
  if(emulated()) return -1;
  FILE *f = fopen("/proc/self/mountinfo", "r");
  if(!f) return -1;
  while(count < max && fgets(line, sizeof(line), f)) {
    if(sscanf(line, "%*s %*s %*s %4095s %4095s", root, point) != 2) continue;
    if(strncmp(root, stage_prefix, strlen(stage_prefix)) || strncmp(point, under, strlen(under))) continue;
    snprintf(out[count++], 1024, "%s", point);
  }
  fclose(f);
  return count;
}


void
plat_dump_threads(FILE *out) {
  DIR *procs = opendir("/proc");
  struct dirent *p;
  while(procs && (p = readdir(procs))) {
    int pid = atoi(p->d_name);
    char path[64];
    if(pid <= 0) continue;
    snprintf(path, sizeof(path), "/proc/%d/task", pid);
    DIR *tasks = opendir(path);
    struct dirent *t;
    while(tasks && (t = readdir(tasks))) {
      char file[300], comm[64] = "";
      if(t->d_name[0] == '.') continue;
      snprintf(file, sizeof(file), "%s/%s/comm", path, t->d_name);
      FILE *f = fopen(file, "r");
      if(f && fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\n")] = 0;
      if(f) fclose(f);
      fprintf(out, "%d %s %s\n", pid, t->d_name, comm);
    }
    if(tasks) closedir(tasks);
  }
  if(procs) closedir(procs);
}


/* /proc/<pid>/task/<tid>/comm holds each thread's name; a process that has
   exited but not been waited for ("Z" in its stat) doesn't count. */
int
plat_find_named_thread(const char *name, int except) {
  /* As on the console, where this found nothing (13.42). */
  if(getenv("PS5THEME_TEST_NO_THREAD_NAMES")) return -1;
  DIR *procs = opendir("/proc");
  struct dirent *p;
  int found = -1;
  while(procs && found < 0 && (p = readdir(procs))) {
    int pid = atoi(p->d_name);
    char path[128], line[512] = "";
    if(pid <= 0 || pid == except) continue;
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *st = fopen(path, "r");
    char *close_paren = st && fgets(line, sizeof(line), st) ? strrchr(line, ')') : NULL;
    if(st) fclose(st);
    if(!close_paren || close_paren[1] != ' ' || close_paren[2] == 'Z') continue;
    snprintf(path, sizeof(path), "/proc/%d/task", pid);
    DIR *tasks = opendir(path);
    struct dirent *t;
    while(tasks && found < 0 && (t = readdir(tasks))) {
      char comm[64] = "", file[300];
      if(t->d_name[0] == '.') continue;
      snprintf(file, sizeof(file), "%s/%s/comm", path, t->d_name);
      FILE *f = fopen(file, "r");
      if(f && fgets(comm, sizeof(comm), f)) {
        comm[strcspn(comm, "\n")] = 0;
        if(!strcmp(comm, name)) found = pid;
      }
      if(f) fclose(f);
    }
    if(tasks) closedir(tasks);
  }
  if(procs) closedir(procs);
  return found;
}


void
plat_set_name(const char *name) {
  prctl(PR_SET_NAME, name, 0, 0, 0);
}


int
plat_mount_count(void) {
  char line[4096];
  int n = 0;
  FILE *f = fopen("/proc/self/mountinfo", "r");
  if(!f) return -1;
  while(fgets(line, sizeof(line), f)) n++;
  fclose(f);
  return n;
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


/* (x86-64 as the console is; ARM64 too, so the suite also runs natively there: ThreadSanitizer needs that) */
void
plat_crash_regs(void *ucontext, uint64_t *rip, uint64_t *rsp, uint64_t *rbp) {
  ucontext_t *uc = ucontext;
#if defined(__x86_64__)
  *rip = uc ? (uint64_t)uc->uc_mcontext.gregs[REG_RIP] : 0;
  *rsp = uc ? (uint64_t)uc->uc_mcontext.gregs[REG_RSP] : 0;
  *rbp = uc ? (uint64_t)uc->uc_mcontext.gregs[REG_RBP] : 0;
#elif defined(__aarch64__)
  *rip = uc ? (uint64_t)uc->uc_mcontext.pc : 0;
  *rsp = uc ? (uint64_t)uc->uc_mcontext.sp : 0;
  *rbp = uc ? (uint64_t)uc->uc_mcontext.regs[29] : 0;
#else
  (void)uc;
  *rip = *rsp = *rbp = 0;
#endif
}


void
plat_socket_nosigpipe(int fd) {
  (void)fd;  /* SIGPIPE is ignored process-wide; send errors surface as EPIPE */
}




/* What the test says is free (the file PS5THEME_TEST_FREE_BYTES names, while it exists), else the real figure. */
uint64_t
plat_free_bytes(const char *path) {
  const char *name = getenv("PS5THEME_TEST_FREE_BYTES");
  FILE *f = name ? fopen(name, "r") : NULL;
  if(f) {
    unsigned long long v = 0;
    int ok = fscanf(f, "%llu", &v) == 1;
    fclose(f);
    if(ok) return v;
  }
  struct statvfs sv;
  if(statvfs(path, &sv)) return UINT64_MAX;
  return (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
}
