/* The record of the overlays this plugin has mounted (MOUNTS_FILE), so that after a crash, a hand-over or a restart
   of the plugin it knows which overlays are its own, and the guard and the stop tool know what to take out.

   One line per overlay, oldest first: "<mount point>\t<source>\t<system folder it shows>" (copies before 1.39 wrote
   the mount point alone). A mount point can be listed more than once: an overlay still in use when it was due to come
   down stays, and the next one goes on top of it (see revert_mounts in apply.c).

   A line is added as each overlay goes on, without waiting for the disk: the record matters only while the console
   runs (a restart takes every overlay with it), and writing it whole and waiting at each of 217 overlays took seconds
   on the console, while the home screen showed its own look. A crash leaves what was written to the kernel; a power
   loss can leave a last line cut short, which is never taken for a record, and the record is written whole again
   before the next line goes on (1.79).

   Main thread only (the guard and the crash handler read the file, never write it). */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mounts.h"
#include "config.h"
#include "fsguard.h"


static int records_torn;     /* the last read found a last line cut short */
static int nrecorded = -1;   /* lines in MOUNTS_FILE, -1 until read */

int
read_mount_records(mount_rec_t *recs, int max) {
  char line[PATH_MAX + 256 + 512 + 8];
  int n = 0;
  FILE *f = fopen(MOUNTS_FILE, "r");
  if(!f) return 0;
  records_torn = 0;
  while(n < max && fgets(line, sizeof(line), f)) {
    if(!strchr(line, '\n')) { records_torn = 1; break; }   /* a last line cut short: not a record */
    line[strcspn(line, "\r\n")] = 0;
    if(!line[0]) continue;
    char *src = strchr(line, '\t'), *real = src ? strchr(src + 1, '\t') : NULL;
    if(src) *src++ = 0;
    if(real) *real++ = 0;
    snprintf(recs[n].on, sizeof(recs[n].on), "%s", line);
    snprintf(recs[n].src, sizeof(recs[n].src), "%s", src ? src : "");
    snprintf(recs[n].real, sizeof(recs[n].real), "%s", real ? real : "");
    n++;
  }
  fclose(f);
  return n;
}

int
write_mount_records(const mount_rec_t *recs, int n) {
  nrecorded = -1;
  if(!n) return fs_unlink(MOUNTS_FILE) && errno != ENOENT ? -1 : 0;
  size_t cap = (size_t)n * (sizeof(*recs) + 3), len = 0;
  char *buf = malloc(cap);
  if(!buf) return -1;
  for(int i = 0; i < n; i++) len += snprintf(buf + len, cap - len, "%s\t%s\t%s\n", recs[i].on, recs[i].src, recs[i].real);
  int ret = fs_write_file(MOUNTS_FILE, buf, len);
  free(buf);
  if(!ret) nrecorded = n;
  return ret;
}

/* How many records the file holds, and whether its last line was cut short (as read_mount_records counts them). */
static int
count_records(int *torn) {
  char line[PATH_MAX + 256 + 512 + 8];
  int n = 0;
  *torn = 0;
  FILE *f = fopen(MOUNTS_FILE, "r");
  if(!f) return 0;
  while(n < MAX_RECORDS && fgets(line, sizeof(line), f)) {
    if(!strchr(line, '\n')) { *torn = 1; break; }
    n += line[0] != '\n' && line[0] != '\r';
  }
  fclose(f);
  return n;
}

int
record_mount(const char *on, const char *src, const char *real) {
  if(nrecorded < 0) {
    int torn, n = count_records(&torn);
    if(torn) {   /* (written as the power went): the record is written whole again first, so no line runs on from it */
      mount_rec_t *all = malloc(sizeof(*all) * MAX_RECORDS);
      if(!all) return -1;
      n = read_mount_records(all, MAX_RECORDS);
      int bad = write_mount_records(all, n);
      free(all);
      if(bad) return -1;
    }
    nrecorded = n;
  }
  if(nrecorded >= MAX_RECORDS) return -1;
  char line[PATH_MAX + 256 + 512 + 8];
  int len = snprintf(line, sizeof(line), "%s\t%s\t%s\n", on, src, real);
  if(len < 0 || len >= (int)sizeof(line) || fs_append_file(MOUNTS_FILE, line, (size_t)len)) {
    nrecorded = -1;
    return -1;
  }
  nrecorded++;
  return 0;
}
