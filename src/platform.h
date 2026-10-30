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
   over one of ours. *buried counts those of ours under such a mount, left alone. -1 where it can't be listed. */
int      plat_our_top_mounts(const char *stage_prefix, const char *under, char (*out)[1024], int max, int *buried);

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

/* One unmount attempt, without waiting for the folder to be free. */
int      plat_unmount_once(const char *on);

/* Keep writes to a closed peer from raising SIGPIPE. */
void     plat_socket_nosigpipe(int fd);

#endif
