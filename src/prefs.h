/* prefs.c: settings that aren't a theme's. "starts": the notifications when the plugin starts, stops or updates;
   "tips": what to do to see an applied theme's changes (rest mode, music); "progress": how far a long apply has got.
   Failures are always said. */
#ifndef PS5_THEME_PREFS_H
#define PS5_THEME_PREFS_H

#define PREF_NAMES "starts", "tips", "progress"

int         pref_on(const char *name);
int         pref_set(const char *name, int on);
const char *pref_name(int i);   /* the i-th setting's name, NULL past the last */

#endif
