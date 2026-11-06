/* mounts.c: the record of the overlays this plugin mounted (MOUNTS_FILE). Main thread. */
#ifndef PS5_THEME_MOUNTS_H
#define PS5_THEME_MOUNTS_H

#include <limits.h>

#define MAX_RECORDS 2048   /* overlays recorded at most */

typedef struct {
  char on[PATH_MAX];
  char src[256];
  char real[512];
} mount_rec_t;

int read_mount_records(mount_rec_t *recs, int max);
int write_mount_records(const mount_rec_t *recs, int n);
int record_mount(const char *on, const char *src, const char *real);   /* one line added */

#endif
