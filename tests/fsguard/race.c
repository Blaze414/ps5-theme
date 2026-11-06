/* The write guard against a folder swapped for a link while it writes (time of check to time of use).

   One thread keeps replacing a folder inside the plugin's folder with a link to a folder outside it, and putting it
   back, as fast as it can. Others write, append, rename, link, copy, make folders and delete through that folder
   with the guard's functions, the whole time. Afterwards nothing may have been made, changed or deleted outside:
   the folder outside holds exactly the one file put there first, unchanged.

   Built against src/fsguard.c (tests/fsguard/run.sh); built against the guard before 1.88, which checked a path
   and then used it, the folder outside gets files. */

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "fsguard.h"
#include "log.h"
#include "platform.h"

#define AREA    BASE_DIR "/race"
#define SWAPPED AREA "/dir"
#define AWAY    AREA "/dir.away"
#define OUTSIDE "/tmp/ps5-theme-outside"
#define KEEP    OUTSIDE "/keep.txt"

/* What the guard needs from the rest of the plugin. */
void log_err(const char *fmt, ...) { (void)fmt; }
void log_msg(const char *fmt, ...) { (void)fmt; }
uint64_t plat_free_bytes(const char *path) { (void)path; return UINT64_MAX; }
int plat_mounted_under(const char *path) { (void)path; return 0; }

static atomic_int stop;
static atomic_long swaps, ops;

static void *
swapper(void *arg) {
  (void)arg;
  while(!atomic_load(&stop)) {
    if(rename(SWAPPED, AWAY)) continue;
    if(!symlink(OUTSIDE, SWAPPED)) {
      for(volatile int i = 0; i < 200; i++) {}
      unlink(SWAPPED);
    }
    rename(AWAY, SWAPPED);
    atomic_fetch_add(&swaps, 1);
  }
  return NULL;
}

static void *
writer(void *arg) {
  long n = (long)(intptr_t)arg;
  char a[256], b[256], c[256], d[256];
  for(long i = 0; !atomic_load(&stop); i++) {
    snprintf(a, sizeof(a), SWAPPED "/w%ld-%ld", n, i % 8);
    snprintf(b, sizeof(b), SWAPPED "/r%ld-%ld", n, i % 8);
    snprintf(c, sizeof(c), SWAPPED "/sub%ld/deeper", n);
    snprintf(d, sizeof(d), SWAPPED "/sub%ld", n);
    switch(i % 9) {
    case 0: fs_write_file(a, "x", 1); break;
    case 1: fs_append_file(a, "y", 1); break;
    case 2: fs_rename(a, b); break;
    case 3: fs_link(b, a); break;
    case 4: fs_copy_file(b, a); break;
    case 5: fs_mkdirs(c); break;
    case 6: fs_rm_rf_under(BASE_DIR, d); break;
    case 7: fs_unlink(b); fs_unlink(SWAPPED "/keep.txt"); break;
    case 8: { int fd = fs_open_write(a); if(fd >= 0) close(fd); fs_symlink("/", b); fs_unlink(b); break; }
    }
    atomic_fetch_add(&ops, 1);
  }
  return NULL;
}

/* Everything in OUTSIDE but keep.txt, and keep.txt's state. */
static int
outside_changed(void) {
  DIR *d = opendir(OUTSIDE);
  if(!d) return printf("the folder outside is gone\n"), 1;
  struct dirent *e;
  int bad = 0;
  while((e = readdir(d))) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, "keep.txt")) continue;
    printf("made outside: %s\n", e->d_name);
    bad = 1;
  }
  closedir(d);
  char buf[16] = {0};
  FILE *f = fopen(KEEP, "r");
  if(!f || !fgets(buf, sizeof(buf), f) || strcmp(buf, "keep")) {
    printf("changed or deleted outside: keep.txt\n");
    bad = 1;
  }
  if(f) fclose(f);
  return bad;
}

int
main(int argc, char **argv) {
  int seconds = argc > 1 ? atoi(argv[1]) : 10;
  if(system("rm -rf " AREA " " OUTSIDE " && mkdir -p " BASE_DIR) || mkdir(OUTSIDE, 0755) || fs_mkdirs(SWAPPED)) {
    printf("could not set up\n");
    return 2;
  }
  FILE *f = fopen(KEEP, "w");
  if(!f) return 2;
  fputs("keep", f);
  fclose(f);
  fs_write_file(SWAPPED "/keep.txt", "inside", 6);

  pthread_t s, w[4];
  pthread_create(&s, NULL, swapper, NULL);
  for(long i = 0; i < 4; i++) pthread_create(&w[i], NULL, writer, (void *)(intptr_t)i);
  sleep((unsigned)seconds);
  atomic_store(&stop, 1);
  pthread_join(s, NULL);
  for(int i = 0; i < 4; i++) pthread_join(w[i], NULL);

  int bad = outside_changed();
  printf("%ld swaps, %ld guarded operations: %s\n", atomic_load(&swaps), atomic_load(&ops),
         bad ? "the guard was sent outside its folder" : "nothing outside the folder was touched");
  return bad;
}
