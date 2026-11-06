/* running.c: finding the running copy and taking overlays out; shared with ps5-theme-stop. */
#ifndef PS5_THEME_RUNNING_H
#define PS5_THEME_RUNNING_H

#include <stddef.h>

int running_copy(int self);
/* One request to the copy answering on this console's web port (`out`: its reply); 0 on a 200 reply. */
int local_request(const char *method, const char *path, char *out, size_t size);
int title_mount_point(const char *on);
/* Overlays of ours under `under` that `match`, newest view first: how many came out (*stuck: how many stayed). */
int release_overlays(const char *under, int (*match)(const char *on), char (*recorded)[1024], int nrecorded, int *stuck);
/* The same from the mount table only, looking for process `pid` to be gone meanwhile (*gone: when, ms; 0: not seen). */
int release_watching(const char *under, int (*match)(const char *on), int pid, long long *gone, int *stuck);

#endif
