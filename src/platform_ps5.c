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
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/mount.h>
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


/* An app may still be reading from the overlay: wait for it rather than
   pulling the files away, and force only as a last resort. */
int
plat_unmount(const char *on) {
  for(int i = 0; i < 15; i++) {
    if(!unmount(on, 0)) return 0;
    if(errno != EBUSY) break;
    usleep(200 * 1000);
  }
  if(errno == EBUSY) log_err("unmount %s: still busy after 3 s, forcing", on);
  return unmount(on, MNT_FORCE);
}


int
plat_list_views(plat_view_t *out, int max) {
  struct statfs *sf = NULL;
  int n = getmntinfo(&sf, MNT_NOWAIT), count = 0;

  for(int i = 0; i < n && count < max; i++) {
    if(strcmp(sf[i].f_fstypename, "nullfs") && strcmp(sf[i].f_fstypename, "unionfs")) continue;
    if(sf[i].f_mntfromname[0] != '/' || !strcmp(sf[i].f_mntfromname, sf[i].f_mntonname)) continue;
    if(!strncmp(sf[i].f_mntfromname, STAGE_DIR "/", sizeof(STAGE_DIR))) continue;
    snprintf(out[count].from, sizeof(out[count].from), "%s", sf[i].f_mntfromname);
    snprintf(out[count].on, sizeof(out[count].on), "%s", sf[i].f_mntonname);
    count++;
  }
  return count;
}


int
plat_mount_count(void) {
  return getfsstat(NULL, 0, MNT_NOWAIT);
}


void
plat_dump_mounts(FILE *out) {
  struct statfs *sf = NULL;
  int n = getmntinfo(&sf, MNT_NOWAIT);
  for(int i = 0; i < n; i++) {
    fprintf(out, "%-10s %s -> %s flags=0x%llx\n", sf[i].f_fstypename, sf[i].f_mntfromname,
            sf[i].f_mntonname, (unsigned long long)sf[i].f_flags);
  }
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
