/* The settings the manager changes that aren't about a theme (BASE_DIR/settings): what the console says, and when.
   One line per setting, "name=0" or "name=1"; a setting not in the file is on. Any thread, under its own lock. */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "prefs.h"
#include "config.h"
#include "fsguard.h"

#define PREFS_FILE BASE_DIR "/settings"

static const char *const names[] = { PREF_NAMES };
#define NPREFS (sizeof(names) / sizeof(*names))
static int values[NPREFS];
static int loaded;
static pthread_mutex_t prefs_lock = PTHREAD_MUTEX_INITIALIZER;

static int
index_of(const char *name) {
  for(size_t i = 0; i < NPREFS; i++) if(name && !strcmp(name, names[i])) return (int)i;
  return -1;
}

/* Called with prefs_lock. */
static void
load(void) {
  if(loaded) return;
  loaded = 1;
  for(size_t i = 0; i < NPREFS; i++) values[i] = 1;
  FILE *f = fopen(PREFS_FILE, "r");
  if(!f) return;
  char line[64];
  while(fgets(line, sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if(!eq) continue;
    *eq = 0;
    int i = index_of(line);
    if(i >= 0) values[i] = eq[1] != '0';
  }
  fclose(f);
}

int
pref_on(const char *name) {
  int i = index_of(name);
  if(i < 0) return 1;
  pthread_mutex_lock(&prefs_lock);
  load();
  int v = values[i];
  pthread_mutex_unlock(&prefs_lock);
  return v;
}

int
pref_set(const char *name, int on) {
  int i = index_of(name);
  if(i < 0) return -1;
  char buf[256];
  size_t n = 0;
  pthread_mutex_lock(&prefs_lock);
  load();
  int was = values[i];
  values[i] = !!on;
  for(size_t k = 0; k < NPREFS; k++) n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s=%d\n", names[k], values[k]);
  int rc = fs_write_file(PREFS_FILE, buf, n);
  if(rc) values[i] = was;
  pthread_mutex_unlock(&prefs_lock);
  return rc;
}

const char *
pref_name(int i) {
  return i >= 0 && (size_t)i < NPREFS ? names[i] : NULL;
}
