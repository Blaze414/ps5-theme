/* store.c: themes' folders as the manager changes them: where each part of a theme is kept, the copy that Discard
   puts back, and the lock that keeps a Discard's swap of folders out of sight. */
#ifndef PS5_THEME_STORE_H
#define PS5_THEME_STORE_H

#include <limits.h>
#include <stddef.h>

int  theme_exists(const char *id);
int  theme_file_path(const char *id, const char *asset, char *out, size_t size);   /* files/<system path>; -1: too long */
void music_path(const char *id, int slot, char *out, size_t size);
int  preview_path(const char *id, const char *kind, char *out, size_t size);    /* -1: no such kind */
void choices_path(const char *id, char *out, size_t size);
int  source_path(const char *id, const char *asset, char *out, size_t size);

/* A theme with unsaved changes has a saved copy in SNAP_DIR: kept before the first change, dropped by Save, put
   back by Discard. */
int  theme_unsaved(const char *id);
int  keep_snapshot(const char *id);
void drop_snapshot(const char *id);
int  restore_snapshot(const char *id);                     /* Discard; 0 when the saved copy is back */
void drop_snapshot_part(const char *id, const char *part);   /* e.g. "sources", from the saved copy too */
void snapshots_recover(void);   /* a Discard cut off between its two steps, put right (at start) */

/* The paths a walk of a theme's folders builds at each level, taken from the heap: a web worker has 512 KB of
   stack, and three paths a level on it, 32 levels down, came near that (and past it where a path may be 4 KB long). */
typedef struct { char dir[PATH_MAX], child_rel[PATH_MAX], child[PATH_MAX], kept[PATH_MAX]; } walk_paths_t;

/* Discard's swap of folders: a reader waits it out (swap_wait), or holds it off for a short read. */
void swap_wait(void);
void swap_read_begin(void);
void swap_read_end(void);

#endif
