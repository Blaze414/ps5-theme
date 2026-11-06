/* Console-specific operations. platform_ps5.c implements them for the PS5;
   tests/host/platform_linux.c implements them for the end-to-end test rig. */
#ifndef PS5_THEME_PLATFORM_H
#define PS5_THEME_PLATFORM_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

/* Raise privileges and leave the sandbox. 0 on success. */
int      plat_init(void);

/* Firmware as 0xMMmm0000, e.g. 0x13420000 for 13.42. 0 if unknown. */
uint32_t plat_fw_version(void);

void     plat_notify(const char *msg);
/* A notification as etaHEN shows its own: `icon` (a PNG on the console) beside a first line and a second under it.
   Where the console can't show that, the plain one, the lines joined. */
void     plat_toast(const char *title, const char *detail, const char *icon);

/* `n` bytes from the kernel's random source, for keys. 0 on success, -1 when it can't be read: never anything weaker
   in its place. */
int      plat_random(void *buf, size_t n);

/* Kernel log line (visible with etaHEN's klog), for the earliest startup steps. */
void     plat_klog(const char *msg);

/* Read-only overlay of directory `from` onto directory `on`. */
int      plat_mount_ro(const char *from, const char *on);
int      plat_unmount(const char *on);

/* Longest mount point path the kernel accepts (FreeBSD MNAMELEN - 1). */
size_t   plat_mount_path_max(void);

/* Folders the system shows in a second place: nullfs mounts of `from` at
   `on` (for example an app sandbox's view of /system_ex). Mounts made by this
   plugin are not included. Returns the number filled in. */
typedef struct plat_view {
  char from[512];
  char on[512];
} plat_view_t;
int      plat_list_views(plat_view_t *out, int max);

/* Is anything mounted at `path`, or at a folder under it (from the mount table)? 1 yes, 0 no, -1 can't tell. */
int      plat_mounted_under(const char *path);

/* Number of mounts, cheaply (to notice when the table changes). */
int      plat_mount_count(void);

/* Write the whole mount table, one mount per line, for diagnostics. */
void     plat_dump_mounts(FILE *out);

/* Is `on` a mount point created by plat_mount_ro from below stage_prefix? */
int      plat_is_our_mount(const char *on, const char *stage_prefix);

/* Register the home-screen tile staged under /user/app/<title_id>. */
int      plat_register_tile(const char *title_id);

/* Registers from a signal handler's ucontext. Async-signal-safe. */
void     plat_crash_regs(void *ucontext, uint64_t *rip, uint64_t *rsp, uint64_t *rbp);

/* Name this process's main thread, as etaHEN's loader names a plugin. */
void     plat_set_name(const char *name);

/* Watch files for being opened, read and closed by any process, for up to
   `seconds`: seen(index into paths, "open"/"read"/"close", milliseconds
   since the start, ctx) for each. Files that can't be watched are skipped.
   -1 if watching isn't possible at all. For diagnosis. */
int      plat_watch_files(const char *const *paths, int n, int seconds,
                          void (*seen)(int index, const char *what, long ms, void *ctx), void *ctx);

/* Bytes free to an ordinary writer on the filesystem holding `path`, or UINT64_MAX when it can't be told. */
uint64_t plat_free_bytes(const char *path);

/* The process whose kernel command name is `name`, or -1. */
int      plat_find_process(const char *name);


/* The live process with a thread named `name` (see plat_set_name), other
   than `except`, or -1: how another program finds the running plugin, as
   every payload's process is named after the loader ("payload.elf"). */
int      plat_find_named_thread(const char *name, int except);

/* When process `pid` started (an opaque, nonzero count), or 0 if there is no
   such live process: with the pid, it tells a process from a later one
   that reused the pid. */
unsigned long long plat_process_start(int pid);

/* Mount points of this plugin's overlays (mounts of folders under
   `stage_prefix`) that start with `under`, oldest first, from the console's
   mount table. -1 where it can't be listed that way (emulated mounts). */
int      plat_our_mounts(const char *stage_prefix, const char *under, char (*out)[1024], int max);

/* The same, but only overlays with no mount but ours made over them at their point: unmounting that path, newest
   first, takes exactly ours off (the kernel unmounts the newest mount there), never a mount of the system's made
   over one of ours. *buried counts those of ours under such a mount, left alone. With `view`, each one's view: the
   place in the table of the newest mount not ours at its point or at a folder holding it (-1: none), which is the
   system's view it is in; the system takes a sandbox down newest view first. -1 where it can't be listed. */
int      plat_our_top_mounts(const char *stage_prefix, const char *under, char (*out)[1024], int max, int *buried,
                             int *view);

/* Mounts at or under `under` other than this plugin's overlays (mounts of folders under `stage_prefix`): their
   points, at most `max` of them, oldest first; *ours counts this plugin's there. The number there is (which may be
   more than `max`), or -1 where it can't be listed. */
int      plat_others_under(const char *stage_prefix, const char *under, char (*out)[1024], int max, int *ours);

/* Every thread: "pid tid name / process name" lines, for diagnosis. */
void     plat_dump_threads(FILE *out);

/* Every process, one "pid name cpu=<seconds>s" line each (the processor time it has used), for diagnosis. */
void     plat_dump_processes(FILE *out);

/* Wait up to `ms` for a mount or unmount anywhere (PLAT_EV_MOUNTS), or for
   process `pid` to exit (PLAT_EV_EXIT, when pid > 0). 0 on timeout; where
   events aren't available it just waits. */
#define PLAT_EV_MOUNTS 1
#define PLAT_EV_EXIT   2
int      plat_wait_events(int pid, int ms);

/* Wait up to `ms` for process `pid` to exit: 1 once it has (or if there is
   no such process), 0 on timeout. For one watched process per thread. */
int      plat_wait_exit(int pid, int ms);

/* 1 once process `pid` has begun to exit (the kernel's "working on exiting"; where that can't be told, once it is a
   zombie), or is gone; 0 while it runs. Ahead of plat_wait_exit by as long as a big process takes to wind down. */
int      plat_process_exiting(int pid);

/* One unmount attempt, without waiting for the folder to be free. */
int      plat_unmount_once(const char *on);

/* Keep writes to a closed peer from raising SIGPIPE. */
void     plat_socket_nosigpipe(int fd);

/* ---- System calls the SDK's libc gets wrong ----

   ps5-payload-sdk's libc makes these calls itself and returns what the kernel hands back without looking at it: on a
   failure that is the error number, as if it were the result, and errno stays as it was. openat of a missing folder
   gave 2, which reads as an open file: 1.81 to 1.84 walked into it, and a new theme's folder was never made (see
   docs/challenges.md). On the PS5 each of them goes through platform_ps5.c instead, where a failure is -1 with
   errno, as libkernel's own calls and POSIX have it. tools/check_syscalls.py fails a build in which anything else
   calls one of the SDK's. */
#ifdef __PROSPERO__
#include <sys/types.h>
struct stat;
struct statfs;
struct iovec;
int     ps5_openat(int dir, const char *path, int flags, ...);
int     ps5_mkdirat(int dir, const char *path, mode_t mode);
int     ps5_fstatat(int dir, const char *path, struct stat *st, int flags);
int     ps5_renameat(int from_dir, const char *from, int to_dir, const char *to);
int     ps5_unlinkat(int dir, const char *path, int flags);
int     ps5_linkat(int from_dir, const char *from, int to_dir, const char *to, int flags);
int     ps5_symlinkat(const char *target, int dir, const char *path);
ssize_t ps5_readlink(const char *path, char *buf, size_t size);
int     ps5_statfs(const char *path, struct statfs *sf);
int     ps5_getfsstat(struct statfs *buf, long size, int mode);
int     ps5_nmount(struct iovec *iov, unsigned int n, int flags);
int     ps5_unmount(const char *dir, int flags);
int     ps5_thr_self(long *id);
int     ps5_thr_set_name(long id, const char *name);
#define openat(...)              ps5_openat(__VA_ARGS__)
#define mkdirat(d, p, m)         ps5_mkdirat(d, p, m)
#define fstatat(d, p, s, f)      ps5_fstatat(d, p, s, f)
#define renameat(a, b, c, d)     ps5_renameat(a, b, c, d)
#define unlinkat(d, p, f)        ps5_unlinkat(d, p, f)
#define linkat(a, b, c, d, f)    ps5_linkat(a, b, c, d, f)
#define symlinkat(t, d, p)       ps5_symlinkat(t, d, p)
#define readlink(p, b, n)        ps5_readlink(p, b, n)
#define statfs(p, s)             ps5_statfs(p, s)
#define getfsstat(b, n, m)       ps5_getfsstat(b, n, m)
#define nmount(v, n, f)          ps5_nmount(v, n, f)
#define unmount(d, f)            ps5_unmount(d, f)
#define thr_self(i)              ps5_thr_self(i)
#define thr_set_name(i, n)       ps5_thr_set_name(i, n)
#endif

#endif
