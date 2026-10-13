/* ps5-fontscan: which font files do the console's processes have open or
   mapped? A one-shot, read-only diagnostic for the font test: it asks the
   system, process by process, for the files each one has open and the files
   mapped into its memory, and writes down those that look like fonts (a path
   with "font" in it, or ending .otf, .ttf or .ttc), with the path as that
   process sees it, to /data/ps5-theme/dump/fontscan.txt. It changes nothing
   and reads no file's contents.

   Start it from a payload loader (port 9021) or PS5 Payload Manager, after
   the home screen is up with the theme applied (after rest mode), wait for
   the "done" notification, then read the file over FTP. */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include <ps5/kernel.h>

#ifndef KERN_PROC_VMMAP
#define KERN_PROC_VMMAP 32
#endif
#ifndef KERN_PROC_FILEDESC
#define KERN_PROC_FILEDESC 33
#endif

#define OUT_DIR   "/data/ps5-theme/dump"
#define OUT_FILE  OUT_DIR "/fontscan.txt"
#define MAX_PATHS 64
#define MAX_PROCS 4096

typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void
note(const char *msg) {
  notify_request_t req;
  memset(&req, 0, sizeof(req));
  strncpy(req.message, msg, sizeof(req.message) - 1);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}


/* Root, full capabilities and the real filesystem, as the theme plugin. */
static int
raise_privileges(void) {
  uint8_t caps[16];
  pid_t pid = getpid();
  memset(caps, 0xff, sizeof(caps));
  if(kernel_set_ucred_authid(pid, 0x4800000000010003l) || kernel_set_ucred_caps(pid, caps) ||
     kernel_set_ucred_uid(pid, 0) || kernel_set_ucred_ruid(pid, 0)) return -1;
  intptr_t root = kernel_get_root_vnode();
  if(!root || kernel_set_proc_rootdir(pid, root) || kernel_set_proc_jaildir(pid, root)) return -1;
  return 0;
}


static int
fontish(const char *s, size_t n) {
  static const char *const ext[] = {".otf", ".ttf", ".ttc"};
  for(size_t i = 0; i + 4 <= n; i++) {
    if(!strncasecmp(s + i, "font", 4)) return 1;
  }
  for(size_t e = 0; e < sizeof(ext) / sizeof(*ext); e++) {
    if(n >= 4 && !strncasecmp(s + n - 4, ext[e], 4)) return 1;
  }
  return 0;
}

/* The paths in a buffer the system filled (the layout of its records is not
   assumed: runs of printable characters starting with a slash are read). Each
   font-like one not seen yet goes into out[]. */
static int
paths_in(const char *buf, size_t len, char (*out)[256], int count) {
  for(size_t i = 0; i < len;) {
    if(buf[i] != '/') {
      i++;
      continue;
    }
    size_t j = i;
    while(j < len && (unsigned char)buf[j] >= 0x20 && (unsigned char)buf[j] < 0x7f) j++;
    size_t n = j - i;
    if(n >= 6 && n < 256 && fontish(buf + i, n)) {
      int seen = 0;
      for(int k = 0; k < count && !seen; k++) seen = !strncmp(out[k], buf + i, n) && !out[k][n];
      if(!seen && count < MAX_PATHS) {
        memcpy(out[count], buf + i, n);
        out[count][n] = 0;
        count++;
      }
    }
    i = j + 1;
  }
  return count;
}

/* Ask for one process's table (KERN_PROC_VMMAP or KERN_PROC_FILEDESC);
   0 on success with the bytes in *buf (free it), else the errno. */
static int
table(int what, int pid, char **buf, size_t *len) {
  int mib[4] = {CTL_KERN, KERN_PROC, what, pid};
  *buf = NULL;
  *len = 0;
  if(sysctl(mib, 4, NULL, len, NULL, 0)) return errno;
  *len += *len / 4 + 4096;   /* it may grow meanwhile */
  *buf = malloc(*len);
  if(!*buf) return ENOMEM;
  if(sysctl(mib, 4, *buf, len, NULL, 0)) {
    int e = errno;
    free(*buf);
    *buf = NULL;
    return e;
  }
  return 0;
}


int
main(void) {
  if(raise_privileges()) {
    note("ps5-fontscan: could not get the rights it needs");
    return 1;
  }
  mkdir("/data/ps5-theme", 0755);
  mkdir(OUT_DIR, 0755);
  FILE *out = fopen(OUT_FILE, "w");
  if(!out) {
    note("ps5-fontscan: could not write to " OUT_DIR);
    return 1;
  }

  /* The process list: each record starts with its own size. */
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t len = 0;
  char *procs = NULL;
  if(!sysctl(mib, 4, NULL, &len, NULL, 0) && len) {
    len += len / 4;
    procs = malloc(len);
    if(procs && sysctl(mib, 4, procs, &len, NULL, 0)) {
      free(procs);
      procs = NULL;
    }
  }
  fprintf(out, "ps5-fontscan: font files open or mapped, per process\n");
  if(!procs) {
    fprintf(out, "could not list the processes: %s\n", strerror(errno));
    fclose(out);
    note("ps5-fontscan: could not list the processes");
    return 1;
  }

  size_t need = offsetof(struct kinfo_proc, ki_comm) + COMMLEN + 1;
  int listed = 0, with_fonts = 0, failed_map = 0, failed_fd = 0, last_errno = 0;
  static char found[MAX_PATHS][256];
  for(size_t at = 0; at + need <= len && listed < MAX_PROCS;) {
    const struct kinfo_proc *p = (const struct kinfo_proc *)(procs + at);
    if(p->ki_structsize < (int)need) break;
    at += (size_t)p->ki_structsize;
    char name[COMMLEN + 1];
    memcpy(name, p->ki_comm, COMMLEN);
    name[COMMLEN] = 0;
    listed++;

    int count = 0;
    char *buf;
    size_t blen;
    int e = table(KERN_PROC_VMMAP, p->ki_pid, &buf, &blen);
    if(e) { failed_map++; last_errno = e; }
    else { count = paths_in(buf, blen, found, count); free(buf); }
    e = table(KERN_PROC_FILEDESC, p->ki_pid, &buf, &blen);
    if(e) { failed_fd++; last_errno = e; }
    else { count = paths_in(buf, blen, found, count); free(buf); }

    if(count) {
      with_fonts++;
      fprintf(out, "\n%d %s\n", p->ki_pid, name);
      for(int k = 0; k < count; k++) fprintf(out, "  %s\n", found[k]);
    }
  }
  free(procs);
  fprintf(out, "\n%d processes looked at, %d with font files; mapped-file tables refused for %d, open-file tables for %d "
               "(last error: %s)\n", listed, with_fonts, failed_map, failed_fd, last_errno ? strerror(last_errno) : "none");
  fclose(out);

  char msg[160];
  snprintf(msg, sizeof(msg), "ps5-fontscan done: %d of %d processes hold font files (" OUT_FILE ")", with_fonts, listed);
  note(msg);
  return 0;
}
