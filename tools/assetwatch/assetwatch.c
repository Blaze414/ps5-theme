/* ps5-assetwatch: which files does the home screen load, and when? A
   one-shot, read-only diagnostic for finding where a texture or picture on
   screen comes from. For two minutes it watches the home screen (SceShellUI)
   and the web renderer (SceNKWebProcess): a couple of times a second it asks
   the system which files each has mapped into its memory, and writes down
   every file that appears or goes away, with the time in milliseconds, to
   /data/ps5-theme/dump/assetwatch.txt. The files already loaded when it
   starts are listed first as the baseline.

   Start it from a payload loader (port 9021) or PS5 Payload Manager. When
   the "watching" notification appears, do the thing you want to look at:
   press the PS button for the Control Center, open a menu, switch tabs.
   Wait for the "done" notification, then read the file over FTP. Code
   libraries (.sprx, .so) are left out.

   It changes nothing and reads no file's contents. It sees whole files, not
   the pictures inside them, and only files that are mapped at the moment of
   a sample. A sample of a big process takes the system some milliseconds,
   so the pace adapts: never faster than twice a second, and slower when a
   sample is slow. */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include <ps5/kernel.h>

#ifndef KERN_PROC_VMMAP
#define KERN_PROC_VMMAP 32
#endif

#define OUT_DIR       "/data/ps5-theme/dump"
#define OUT_FILE      OUT_DIR "/assetwatch.txt"
#define WATCH_SECONDS 120
#define MIN_STEP_MS   500
#define MAX_FILES     3072
#define PATH_LEN      256

static const char *const watched[] = {"SceShellUI", "SceNKWebProcess"};
#define NWATCHED ((int)(sizeof(watched) / sizeof(*watched)))

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


static long
now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}


/* The set of files one process had mapped at one sample. */
typedef struct {
  int n;
  uint64_t hash[MAX_FILES];
  char path[MAX_FILES][PATH_LEN];
} fileset_t;

static uint64_t
fnv(const char *s) {
  uint64_t h = 1469598103934665603ull;
  for(; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
  return h;
}

static int
has(const fileset_t *set, uint64_t h, const char *p) {
  for(int i = 0; i < set->n; i++) if(set->hash[i] == h && !strcmp(set->path[i], p)) return 1;
  return 0;
}

/* Code libraries are not what is being looked for. */
static int
wanted(const char *p, size_t n) {
  static const char *const skip[] = {".sprx", ".prx", ".elf", ".so", ".dll"};
  const char *slash = strrchr(p, '/');
  if(!slash || !strchr(slash, '.')) return 0;              /* a file, with an extension */
  if(!strncmp(p, "/dev/", 5)) return 0;
  for(size_t i = 0; i < sizeof(skip) / sizeof(*skip); i++) {
    size_t e = strlen(skip[i]);
    if(n >= e && !strcasecmp(p + n - e, skip[i])) return 0;
  }
  return 1;
}

/* The paths in a buffer the system filled: runs of printable characters
   starting with a slash (the layout of its records is not assumed). */
static void
collect(const char *buf, size_t len, fileset_t *out) {
  out->n = 0;
  for(size_t i = 0; i < len;) {
    if(buf[i] != '/') {
      i++;
      continue;
    }
    size_t j = i;
    while(j < len && (unsigned char)buf[j] >= 0x20 && (unsigned char)buf[j] < 0x7f) j++;
    size_t n = j - i;
    if(n >= 6 && n < PATH_LEN) {
      char p[PATH_LEN];
      memcpy(p, buf + i, n);
      p[n] = 0;
      uint64_t h = fnv(p);
      if(wanted(p, n) && !has(out, h, p) && out->n < MAX_FILES) {
        out->hash[out->n] = h;
        memcpy(out->path[out->n], p, n + 1);
        out->n++;
      }
    }
    i = j + 1;
  }
}


/* The kernel's process list: records start with their own size. 0 if the
   process was found, with its pid. */
static int
find_process(const char *name) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t len = 0;
  if(sysctl(mib, 4, NULL, &len, NULL, 0) || !len) return -1;
  len += len / 4;
  char *buf = malloc(len);
  if(!buf) return -1;
  int pid = -1;
  if(!sysctl(mib, 4, buf, &len, NULL, 0)) {
    size_t need = offsetof(struct kinfo_proc, ki_comm) + COMMLEN + 1;
    for(size_t at = 0; at + need <= len && pid < 0;) {
      const struct kinfo_proc *p = (const struct kinfo_proc *)(buf + at);
      if(p->ki_structsize < (int)need) break;
      char comm[COMMLEN + 1];
      memcpy(comm, p->ki_comm, COMMLEN);
      comm[COMMLEN] = 0;
      if(!strcmp(comm, name)) pid = p->ki_pid;
      at += (size_t)p->ki_structsize;
    }
  }
  free(buf);
  return pid;
}

/* One process's mapped files; 0 on success. */
static int
sample(int pid, fileset_t *out) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, pid};
  size_t len = 0;
  if(sysctl(mib, 4, NULL, &len, NULL, 0)) return errno;
  len += len / 4 + 4096;
  char *buf = malloc(len);
  if(!buf) return ENOMEM;
  if(sysctl(mib, 4, buf, &len, NULL, 0)) {
    int e = errno;
    free(buf);
    return e;
  }
  collect(buf, len, out);
  free(buf);
  return 0;
}


static fileset_t cur, prev[NWATCHED];
static int prev_pid[NWATCHED];

int
main(void) {
  if(raise_privileges()) {
    note("ps5-assetwatch: could not get the rights it needs");
    return 1;
  }
  mkdir("/data/ps5-theme", 0755);
  mkdir(OUT_DIR, 0755);
  FILE *out = fopen(OUT_FILE, "w");
  if(!out) {
    note("ps5-assetwatch: could not write to " OUT_DIR);
    return 1;
  }
  setvbuf(out, NULL, _IOLBF, 0);   /* readable while it runs */
  fprintf(out, "ps5-assetwatch: files mapped by the home screen and the web renderer, for %d s\n"
               "+ appeared, - gone; the time is in milliseconds from the start\n", WATCH_SECONDS);

  char msg[160];
  snprintf(msg, sizeof(msg), "ps5-assetwatch: watching for %d s. Do what you want to look at now (PS button, menus, tabs)", WATCH_SECONDS);
  note(msg);

  long start = now_ms(), last_beat = 0, step = MIN_STEP_MS, appeared = 0, gone = 0;
  int samples = 0;
  while(now_ms() - start < WATCH_SECONDS * 1000L) {
    long t0 = now_ms();
    for(int w = 0; w < NWATCHED; w++) {
      int pid = find_process(watched[w]);
      long t = now_ms() - start;
      if(pid < 0) {
        if(prev_pid[w] > 0) fprintf(out, "%ld %s: process %d is gone\n", t, watched[w], prev_pid[w]);
        prev_pid[w] = 0;
        prev[w].n = 0;
        continue;
      }
      int e = sample(pid, &cur);
      if(e) {
        fprintf(out, "%ld %s (%d): could not read its mapped files: %s\n", t, watched[w], pid, strerror(e));
        continue;
      }
      if(pid != prev_pid[w]) {
        fprintf(out, "%ld %s: %s %d, %d files mapped%s\n", t, watched[w], prev_pid[w] ? "new process" : "found, pid", pid, cur.n,
                prev_pid[w] ? " (a restart: its files follow as appeared)" : " (the baseline follows)");
        if(prev_pid[w]) prev[w].n = 0;
      }
      int baseline = prev_pid[w] == 0;
      for(int i = 0; i < cur.n; i++) {
        if(!has(&prev[w], cur.hash[i], cur.path[i])) {
          fprintf(out, "%ld %s %s %s\n", t, watched[w], baseline ? "=" : "+", cur.path[i]);
          if(!baseline) appeared++;
        }
      }
      for(int i = 0; i < prev[w].n; i++) {
        if(!has(&cur, prev[w].hash[i], prev[w].path[i])) {
          fprintf(out, "%ld %s - %s\n", t, watched[w], prev[w].path[i]);
          gone++;
        }
      }
      memcpy(&prev[w], &cur, sizeof(cur) - sizeof(cur.path) + (size_t)cur.n * PATH_LEN);   /* the used part */
      prev_pid[w] = pid;
    }
    samples++;
    long took = now_ms() - t0;
    step = took * 5 > MIN_STEP_MS ? took * 5 : MIN_STEP_MS;   /* slower when a sample is slow */
    if(now_ms() - start - last_beat >= 10000) {
      last_beat = now_ms() - start;
      fprintf(out, "%ld ... still watching (%d samples, last took %ld ms)\n", last_beat, samples, took);
    }
    struct timespec ts = {step / 1000, (step % 1000) * 1000000L};
    nanosleep(&ts, NULL);
  }
  fprintf(out, "\nDone: %d samples, %ld files appeared, %ld went away.\n", samples, appeared, gone);
  fclose(out);

  snprintf(msg, sizeof(msg), "ps5-assetwatch done: %ld files appeared (" OUT_FILE ")", appeared);
  note(msg);
  return 0;
}
