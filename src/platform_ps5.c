/* PS5 implementation of platform.h (ps5-payload-sdk).

   Module loading follows etaHEN's own spawned services: the libraries they
   link at load time (SystemService, UserService, Sysmodule, AppInstUtil) are
   linked here too, and their functions are looked up in that inherited scope.
   Nothing is ever dlopen()ed: loading a system module again from a spawned
   process can hang and block the dynamic linker for every thread.
   Functions are never called through direct imports, so an export missing on
   some firmware disables one feature instead of stopping the plugin. */

#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/event.h>
#include <sys/mount.h>
#include <sys/user.h>
#include <sys/thr.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <sys/uio.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "platform.h"
#include "theme.h"


typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);


/* ---- run-time symbol lookup ---- */

static pthread_mutex_t sym_lock = PTHREAD_MUTEX_INITIALIZER;

/* Look `sym` up among modules already loaded in this process. Never loads. */
static void *
lookup(const char *lib, const char *sym) {
  void *fn = dlsym(RTLD_DEFAULT, sym);
  if(fn) return fn;

  pthread_mutex_lock(&sym_lock);
  uint32_t handle = 0;
  if(!kernel_dynlib_handle(getpid(), lib, &handle)) {
    fn = (void *)kernel_dynlib_dlsym(getpid(), handle, sym);
  }
  pthread_mutex_unlock(&sym_lock);
  if(!fn) log_err("platform: %s not found in %s", sym, lib);
  return fn;
}


int
plat_init(void) {
  uint8_t caps[16];
  pid_t pid = getpid();

  memset(caps, 0xff, sizeof(caps));
  if(kernel_set_ucred_authid(pid, 0x4800000000010003l)) return -1;
  if(kernel_set_ucred_caps(pid, caps)) return -1;
  if(kernel_set_ucred_uid(pid, 0)) return -1;
  if(kernel_set_ucred_ruid(pid, 0)) return -1;

  /* See the real filesystem root, not a sandbox view. */
  intptr_t root = kernel_get_root_vnode();
  if(!root) return -1;
  if(kernel_set_proc_rootdir(pid, root)) return -1;
  if(kernel_set_proc_jaildir(pid, root)) return -1;
  return 0;
}


/* The kernel's sdk_version (as etaHEN reads it), else the SDK's libkernel lookup. */
uint32_t
plat_fw_version(void) {
  uint32_t fw = 0;
  size_t len = sizeof(fw);

  if(sysctlbyname("kern.sdk_version", &fw, &len, NULL, 0) || len != sizeof(fw) || !fw) {
    fw = kernel_get_fw_version();
  }
  return fw;
}


void
plat_notify(const char *msg) {
  notify_request_t req;

  memset(&req, 0, sizeof(req));
  snprintf(req.message, sizeof(req.message), "%s", msg);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}


void
plat_klog(const char *msg) {
  klog_puts(msg);
}


int
plat_mount_ro(const char *from, const char *on) {
  struct iovec iov[] = {
    {"fstype", sizeof("fstype")}, {"nullfs", sizeof("nullfs")},
    {"fspath", sizeof("fspath")}, {(void *)on, strlen(on) + 1},
    {"from", sizeof("from")},     {(void *)from, strlen(from) + 1},
  };
  return nmount(iov, sizeof(iov) / sizeof(*iov), MNT_RDONLY | MNT_NOSUID);
}


/* The PS5 kernel keeps FreeBSD 11's MNAMELEN of 88 (seen on 13.42: mount
   points of 88+ characters fail with ENAMETOOLONG, 87 and shorter work). */
size_t
plat_mount_path_max(void) {
  return 87;
}


/* An app may still be reading from the overlay: wait a little for it, and
   never pull the files away (EBUSY then). Forcing it broke the system's own
   views stacked on the overlay until a restart. */
int
plat_unmount(const char *on) {
  for(int i = 0; i < 15; i++) {
    if(!unmount(on, 0)) return 0;
    if(errno != EBUSY) return -1;
    usleep(200 * 1000);
  }
  errno = EBUSY;
  return -1;
}


int
plat_watch_files(const char *const *paths, int n, int seconds,
                 void (*seen)(int, const char *, long, void *), void *ctx) {
  int kq = kqueue(), *fds = calloc(n ? n : 1, sizeof(int));
  if(kq < 0 || !fds) {
    if(kq >= 0) close(kq);
    free(fds);
    return -1;
  }
  for(int i = 0; i < n; i++) {
    struct kevent ev;
    fds[i] = open(paths[i], O_RDONLY);
    if(fds[i] < 0) continue;
    EV_SET(&ev, fds[i], EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_OPEN | NOTE_READ | NOTE_CLOSE, 0, (void *)(intptr_t)i);
    if(kevent(kq, &ev, 1, NULL, 0, NULL)) {
      close(fds[i]);
      fds[i] = -1;
    }
  }
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for(;;) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
    if(ms >= seconds * 1000L) break;
    long left = seconds * 1000L - ms;
    struct timespec wait = {left / 1000, (left % 1000) * 1000000};
    struct kevent got[32];
    int k = kevent(kq, NULL, 0, got, 32, &wait);
    if(k < 0 && errno != EINTR) break;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
    for(int e = 0; e < k; e++) {
      int i = (int)(intptr_t)got[e].udata;
      if(got[e].fflags & NOTE_OPEN) seen(i, "open", ms, ctx);
      if(got[e].fflags & NOTE_READ) seen(i, "read", ms, ctx);
      if(got[e].fflags & NOTE_CLOSE) seen(i, "close", ms, ctx);
    }
  }
  for(int i = 0; i < n; i++) if(fds[i] >= 0) close(fds[i]);
  free(fds);
  close(kq);
  return 0;
}


/* The kernel's process list (kern.proc), walked by each record's own size:
   the console's records may be a different size from this SDK's. */
/* Each record is whole up to its command name (ki_comm), and so are the fields before it the callers read: ki_pid,
   ki_runtime, ki_start and ki_stat. 0, or -1 if the list can't be read. */
static int
each_process(void (*fn)(const struct kinfo_proc *p, const char *name, void *ctx), void *ctx) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t len = 0;
  if(sysctl(mib, 4, NULL, &len, NULL, 0) || !len) return -1;
  len += len / 4;   /* processes may start meanwhile */
  char *buf = malloc(len);
  if(!buf) return -1;
  int rc = sysctl(mib, 4, buf, &len, NULL, 0) ? -1 : 0;
  if(!rc) {
    size_t need = offsetof(struct kinfo_proc, ki_comm) + COMMLEN + 1;
    for(size_t at = 0; at + need <= len;) {
      const struct kinfo_proc *p = (const struct kinfo_proc *)(buf + at);
      if(p->ki_structsize < (int)need) break;
      char name[COMMLEN + 1];
      memcpy(name, p->ki_comm, COMMLEN);
      name[COMMLEN] = 0;
      fn(p, name, ctx);
      at += (size_t)p->ki_structsize;
    }
  }
  free(buf);
  return rc;
}

typedef struct { const char *name; int pid; } find_t;

#ifndef SZOMB
#define SZOMB 5   /* ki_stat of a process that has exited (FreeBSD's sys/proc.h) */
#endif

/* (not one that has exited and is waiting to be reaped: the guard would watch a ShellUI already gone) */
static void
find_one(const struct kinfo_proc *p, const char *name, void *ctx) {
  find_t *f = ctx;
  if(f->pid < 0 && p->ki_stat != SZOMB && !strcmp(name, f->name)) f->pid = p->ki_pid;
}

int
plat_find_process(const char *name) {
  find_t f = {name, -1};
  each_process(find_one, &f);
  return f.pid;
}


/* Every thread of every process comes with KERN_PROC_ALL, each with its
   own name; records are walked by their size here too. */
/* kern.proc.all is missing on 13.42 (ENOENT); kern.proc.proc_td, every
   process with its threads, is tried first. The records, or NULL. */
static char *
thread_records(size_t *len, const char **which) {
  static const struct { int oid; const char *name; } nodes[] = {
    {KERN_PROC_PROC | KERN_PROC_INC_THREAD, "kern.proc.proc_td"}, {KERN_PROC_ALL, "kern.proc.all"},
  };
  for(size_t k = 0; k < sizeof(nodes) / sizeof(*nodes); k++) {
    int mib[3] = {CTL_KERN, KERN_PROC, nodes[k].oid};
    *which = nodes[k].name;
    *len = 0;
    if(sysctl(mib, 3, NULL, len, NULL, 0) || !*len) continue;
    *len += *len / 4;
    char *buf = malloc(*len);
    if(buf && !sysctl(mib, 3, buf, len, NULL, 0)) return buf;
    free(buf);
  }
  return NULL;
}

int
plat_find_named_thread(const char *name, int except) {
  int found = -1;
  size_t len;
  const char *which;
  char *buf = thread_records(&len, &which);
  if(buf) {
    size_t need = offsetof(struct kinfo_proc, ki_tdname) + sizeof(((struct kinfo_proc *)0)->ki_tdname);
    for(size_t at = 0; found < 0 && at + need <= len;) {
      const struct kinfo_proc *p = (const struct kinfo_proc *)(buf + at);
      if(p->ki_structsize < (int)need) break;
      char td[sizeof(p->ki_tdname) + 1];
      memcpy(td, p->ki_tdname, sizeof(p->ki_tdname));
      td[sizeof(p->ki_tdname)] = 0;
      if(p->ki_pid != except && p->ki_stat != SZOMB && !strcmp(td, name)) found = p->ki_pid;
      at += (size_t)p->ki_structsize;
    }
  }
  free(buf);
  return found;
}

/* (ki_runtime: the processor time of all its threads, kept by the kernel as they run, in microseconds) */
typedef struct { const char *name; plat_proc_t *out; int max, n; } named_t;

static void
named_one(const struct kinfo_proc *p, const char *name, void *ctx) {
  named_t *f = ctx;
  if(f->n >= f->max || p->ki_stat == SZOMB || strcmp(name, f->name)) return;
  plat_proc_t *o = &f->out[f->n++];
  o->pid = p->ki_pid;
  o->start = (unsigned long long)p->ki_start.tv_sec * 1000000ull + (unsigned long long)p->ki_start.tv_usec + 1;
  o->cpu_us = (unsigned long long)p->ki_runtime;
}

int
plat_processes_named(const char *name, plat_proc_t *out, int max) {
  named_t f = {name, out, max, 0};
  return each_process(named_one, &f) ? -1 : f.n;
}

unsigned long long
plat_process_start(int pid) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
  static _Thread_local char buf[8192];
  size_t len = sizeof(buf);
  const struct kinfo_proc *p = (const struct kinfo_proc *)buf;
  size_t need = offsetof(struct kinfo_proc, ki_stat) + 1;
  if(pid <= 0 || sysctl(mib, 4, buf, &len, NULL, 0) || len < need || p->ki_structsize < (int)need ||
     p->ki_pid != pid || p->ki_stat == SZOMB) {
    return 0;
  }
  return (unsigned long long)p->ki_start.tv_sec * 1000000ull + (unsigned long long)p->ki_start.tv_usec + 1;
}


/* The mount table in a buffer of the caller's (getmntinfo keeps one
   buffer for the whole process, and the ShellUI guard reads the table
   while the main thread may). Free it; NULL with *n < 0 on failure. */
static struct statfs *
mount_table(int *n) {
  for(int tries = 0; tries < 4; tries++) {
    int want = getfsstat(NULL, 0, MNT_NOWAIT);
    if(want < 0) break;
    want += 32;   /* mounts may come meanwhile */
    struct statfs *sf = malloc(sizeof(*sf) * (size_t)want);
    if(!sf) break;
    *n = getfsstat(sf, (long)(sizeof(*sf) * (size_t)want), MNT_NOWAIT);
    if(*n >= 0 && *n < want) return sf;
    free(sf);
  }
  *n = -1;
  return NULL;
}


int
plat_our_top_mounts(const char *stage_prefix, const char *under, char (*out)[1024], int max, int *buried) {
  int n, count = 0;
  *buried = 0;
  struct statfs *sf = mount_table(&n);   /* in the order mounts were made: a later one at a point is over it */
  if(!sf) return -1;
  for(int i = 0; i < n && count < max; i++) {
    if(strcmp(sf[i].f_fstypename, "nullfs") || strncmp(sf[i].f_mntfromname, stage_prefix, strlen(stage_prefix)) ||
       strncmp(sf[i].f_mntonname, under, strlen(under))) {
      continue;
    }
    int top = 1;   /* nothing but ours over it */
    for(int k = i + 1; k < n && top; k++) {
      top = strcmp(sf[k].f_mntonname, sf[i].f_mntonname) ||
            (!strcmp(sf[k].f_fstypename, "nullfs") && !strncmp(sf[k].f_mntfromname, stage_prefix, strlen(stage_prefix)));
    }
    if(top) snprintf(out[count++], 1024, "%s", sf[i].f_mntonname);
    else (*buried)++;
  }
  free(sf);
  return count;
}

int
plat_our_mounts(const char *stage_prefix, const char *under, char (*out)[1024], int max) {
  int n, count = 0;
  struct statfs *sf = mount_table(&n);
  if(!sf) return -1;
  for(int i = 0; i < n && count < max; i++) {
    if(strcmp(sf[i].f_fstypename, "nullfs") || strncmp(sf[i].f_mntfromname, stage_prefix, strlen(stage_prefix)) ||
       strncmp(sf[i].f_mntonname, under, strlen(under))) {
      continue;
    }
    snprintf(out[count++], 1024, "%s", sf[i].f_mntonname);
  }
  free(sf);
  return count;
}


void
plat_dump_threads(FILE *out) {
  size_t len;
  const char *which;
  char *buf = thread_records(&len, &which);
  if(!buf) {
    fprintf(out, "# no thread list (%s: %s)\n", which, strerror(errno));
  } else {
    fprintf(out, "# from %s\n", which);
    size_t need = offsetof(struct kinfo_proc, ki_comm) + COMMLEN + 1;
    int records = 0;
    for(size_t at = 0; at + need <= len; records++) {
      const struct kinfo_proc *p = (const struct kinfo_proc *)(buf + at);
      if(p->ki_structsize < (int)need) break;
      char td[TDNAMLEN + 2], comm[COMMLEN + 2];
      snprintf(td, sizeof(td), "%.*s", TDNAMLEN + 1, p->ki_tdname);
      snprintf(comm, sizeof(comm), "%.*s", COMMLEN + 1, p->ki_comm);
      /* ki_tid and ki_numthreads come after fields whose size changed
         between FreeBSD versions: shown as read, for comparison. */
      size_t tail = offsetof(struct kinfo_proc, ki_tid) + sizeof(p->ki_tid);
      if(p->ki_structsize >= (int)tail && at + tail <= len) {
        fprintf(out, "%d %d %s / %s (stat %d, threads %d)\n", p->ki_pid, p->ki_tid, td, comm, p->ki_stat, p->ki_numthreads);
      } else {
        fprintf(out, "%d - %s / %s (stat %d)\n", p->ki_pid, td, comm, p->ki_stat);
      }
      at += (size_t)p->ki_structsize;
    }
    fprintf(out, "# %d record(s), %zu bytes, record size %d (this build's %zu)\n", records, len,
            len >= sizeof(int) ? *(const int *)buf : 0, sizeof(struct kinfo_proc));
  }
  free(buf);
}


static void
dump_one(const struct kinfo_proc *p, const char *name, void *ctx) {
  unsigned long long ms = (unsigned long long)p->ki_runtime / 1000;
  fprintf(ctx, "%d %s cpu=%llu.%03llus\n", p->ki_pid, name, ms / 1000, ms % 1000);
}

void
plat_dump_processes(FILE *out) {
  each_process(dump_one, out);
}


/* One kqueue for the main thread's waits: mounts (EVFILT_FS) and the exit
   of one watched process (EVFILT_PROC). */
int
plat_wait_events(int pid, int ms) {
  static int kq = -1, watched = -1;
  if(kq < 0) {
    struct kevent ev;
    kq = kqueue();
    EV_SET(&ev, 0, EVFILT_FS, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if(kq >= 0 && kevent(kq, &ev, 1, NULL, 0, NULL)) {
      close(kq);
      kq = -1;
    }
  }
  if(kq < 0) {
    usleep(ms * 1000);
    return 0;
  }
  if(pid != watched) {
    struct kevent ev;
    if(watched > 0) {
      EV_SET(&ev, watched, EVFILT_PROC, EV_DELETE, 0, 0, NULL);
      kevent(kq, &ev, 1, NULL, 0, NULL);
    }
    watched = -1;
    EV_SET(&ev, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    if(pid > 0 && !kevent(kq, &ev, 1, NULL, 0, NULL)) watched = pid;
  }
  struct timespec wait = {ms / 1000, (ms % 1000) * 1000000L};
  struct kevent got[16];
  int k = kevent(kq, NULL, 0, got, 16, &wait), events = 0;
  for(int i = 0; i < k; i++) {
    if(got[i].filter == EVFILT_FS) events |= PLAT_EV_MOUNTS;
    if(got[i].filter == EVFILT_PROC && (got[i].fflags & NOTE_EXIT)) {
      events |= PLAT_EV_EXIT;
      watched = -1;
    }
  }
  return events;
}


/* Each calling thread has a kqueue of its own. */
int
plat_wait_exit(int pid, int ms) {
  static _Thread_local int kq = -1, watched = -1;
  struct kevent ev;
  if(kq < 0) kq = kqueue();
  if(kq >= 0 && pid != watched) {
    EV_SET(&ev, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    if(!kevent(kq, &ev, 1, NULL, 0, NULL)) watched = pid;
    else if(errno == ESRCH) return 1;
  }
  if(kq < 0 || pid != watched) {
    /* No event to wait for (no kqueue, or the process can't be watched): wait a little all the same, then look.
       Never straight back: the caller asks again at once, which would spin. Never long: an exit is acted on at
       once (shellui_guard). One that has exited counts as gone. */
    usleep((ms < 50 ? ms : 50) * 1000);
    return !plat_process_start(pid);
  }
  struct timespec wait = {ms / 1000, (ms % 1000) * 1000000L};
  int k = kevent(kq, NULL, 0, &ev, 1, &wait);
  if(k > 0 && ev.filter == EVFILT_PROC && (ev.fflags & NOTE_EXIT)) {
    watched = -1;
    return 1;
  }
  return 0;
}


int
plat_unmount_once(const char *on) {
  return unmount(on, 0);
}


int
plat_list_views(plat_view_t *out, int max) {
  int n, count = 0;
  struct statfs *sf = mount_table(&n);

  for(int i = 0; i < n && count < max; i++) {
    if(strcmp(sf[i].f_fstypename, "nullfs") && strcmp(sf[i].f_fstypename, "unionfs")) continue;
    if(sf[i].f_mntfromname[0] != '/' || !strcmp(sf[i].f_mntfromname, sf[i].f_mntonname)) continue;
    if(!strncmp(sf[i].f_mntfromname, STAGE_DIR "/", sizeof(STAGE_DIR))) continue;
    snprintf(out[count].from, sizeof(out[count].from), "%s", sf[i].f_mntfromname);
    snprintf(out[count].on, sizeof(out[count].on), "%s", sf[i].f_mntonname);
    count++;
  }
  free(sf);
  return count;
}


void
plat_set_name(const char *name) {
  long tid;
  if(!thr_self(&tid)) thr_set_name(tid, name);
}


int
plat_mount_count(void) {
  return getfsstat(NULL, 0, MNT_NOWAIT);
}


void
plat_dump_mounts(FILE *out) {
  int n;
  struct statfs *sf = mount_table(&n);
  for(int i = 0; i < n; i++) {
    fprintf(out, "%-10s %s -> %s flags=0x%llx\n", sf[i].f_fstypename, sf[i].f_mntfromname,
            sf[i].f_mntonname, (unsigned long long)sf[i].f_flags);
  }
  free(sf);
}


uint64_t
plat_free_bytes(const char *path) {
  struct statfs sf;

  /* A figure that can't be right (no size, more free than there is, a storage under 2 GB) is taken as unknown:
     the storage check then refuses nothing, rather than refusing everything on a figure it can't trust. */
  if(statfs(path, &sf) || sf.f_bsize <= 0 || sf.f_blocks <= 0 || sf.f_bavail > (int64_t)sf.f_blocks) return UINT64_MAX;
  if((uint64_t)sf.f_blocks * (uint64_t)sf.f_bsize < (2ull << 30)) return UINT64_MAX;
  return sf.f_bavail > 0 ? (uint64_t)sf.f_bavail * (uint64_t)sf.f_bsize : 0;
}


int
plat_is_our_mount(const char *on, const char *stage_prefix) {
  struct statfs sf;

  if(statfs(on, &sf)) return 0;
  return !strcmp(sf.f_mntonname, on) &&
         !strcmp(sf.f_fstypename, "nullfs") &&
         !strncmp(sf.f_mntfromname, stage_prefix, strlen(stage_prefix));
}


/* Mirrors etaHEN's Toolbox card installer: single-title registration when the
   export exists, otherwise the registration scan (e.g. 13.60). */
int
plat_register_tile(const char *title_id) {
  static const char lib[] = "libSceAppInstUtil.sprx";
  int (*initialize)(void) = lookup(lib, "sceAppInstUtilInitialize");
  int (*install)(const char *, const char *, void *) = dlsym(RTLD_DEFAULT, "sceAppInstUtilAppInstallTitleDir");
  if(!install) install = lookup(lib, "sceAppInstUtilAppInstallTitleDir");
  int (*install_all)(void *) = install ? NULL : lookup(lib, "sceAppInstUtilAppInstallAll");

  if(!initialize || (!install && !install_all)) {
    log_err("tile: AppInstUtil is not available");
    return -1;
  }

  uint64_t saved = kernel_get_ucred_authid(getpid());
  if(!saved || kernel_set_ucred_authid(getpid(), 0x4801000000000013ull)) {
    log_err("tile: cannot switch authid");
    return -1;
  }

  log_msg("tile: registering with %s", install ? "single-title install" : "the registration scan");
  int rc = initialize();
  if(!rc) rc = install ? install(title_id, "/user/app/", NULL) : install_all(NULL);

  if(kernel_set_ucred_authid(getpid(), saved)) log_err("tile: could not restore authid");
  if(rc) log_err("tile: registration returned 0x%x", rc);
  return rc ? -1 : 0;
}


void
plat_crash_regs(void *ucontext, uint64_t *rip, uint64_t *rsp, uint64_t *rbp) {
  ucontext_t *uc = ucontext;
  *rip = uc ? (uint64_t)uc->uc_mcontext.mc_rip : 0;
  *rsp = uc ? (uint64_t)uc->uc_mcontext.mc_rsp : 0;
  *rbp = uc ? (uint64_t)uc->uc_mcontext.mc_rbp : 0;
}


void
plat_socket_nosigpipe(int fd) {
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
}
