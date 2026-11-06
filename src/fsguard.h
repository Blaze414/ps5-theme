/* fsguard.c: every write the plugin makes goes through these. A write is only
   allowed inside BASE_DIR or TILE_DIR, and is made in the folder that was
   checked: opened from its root one folder at a time, never through a link,
   the last part never followed. */
#ifndef PS5_THEME_FSGUARD_H
#define PS5_THEME_FSGUARD_H

#include <stddef.h>
#include <stdint.h>

struct stat;

int  fs_guard_init(void);
/* Which file it is (device, inode) and its size and time. Theme, stock and stage files are only ever replaced, never
   changed in place, so the same identity is the same content. */
typedef struct fs_id { unsigned long long dev, ino, size, mtime_s, mtime_ns; } fs_id_t;
int  fs_id_of(const struct stat *st, fs_id_t *id);   /* 0 for a regular file */
int  fs_id_same(const fs_id_t *a, const fs_id_t *b);

int  fs_read_within(const char *path, const char *root);
int  fs_open_read(const char *path, const char *root);   /* a plain file inside root, reached as a write is */
int  fs_open_dir(const char *path);                       /* a folder of the plugin's, for openat later */
int  fs_open_write(const char *path);
int  fs_open_append(const char *path, int quiet);

/* Free space kept for the console on the storage the plugin writes to. */
#define FS_RESERVE (1ull << 30)
#define STORAGE_FULL "the console's storage is nearly full: free some space, then try again"
int  fs_room_for(uint64_t need);
/* How many writes the plugin has made: what was worked out from its folders holds while this stays the same. */
unsigned fs_generation(void);

int  fs_mkdirs(const char *path);
int  fs_rename(const char *from, const char *to);
int  fs_unlink(const char *path);
int  fs_rmdir(const char *path);
int  fs_symlink(const char *target, const char *path);
int  fs_link(const char *existing, const char *path);
int  fs_copy_file(const char *existing, const char *path);
int  fs_copy_tree(const char *src, const char *dst);
int  fs_link_tree(const char *src, const char *dst);
int  fs_rm_rf_under(const char *root, const char *path);
int  fs_write_file(const char *path, const void *data, size_t len);
int  fs_append_file(const char *path, const void *data, size_t len);
int  fs_is_dir(const char *path);
uint64_t fs_tree_bytes(const char *path);   /* the files under it, never through a link */
/* Replace an installed copy of ps5-theme (etaHEN's or Payload Manager's folders), checked first. */
int  fs_replace_payload(const char *path, const void *data, size_t len, int (*check)(const char *tmp));

uint32_t crc32_update(uint32_t crc, const void *buf, size_t len);
int  fs_crc32_file(const char *path, uint32_t *crc, uint64_t *size);

#endif
