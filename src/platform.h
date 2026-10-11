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

/* Keep writes to a closed peer from raising SIGPIPE. */
void     plat_socket_nosigpipe(int fd);

#endif
