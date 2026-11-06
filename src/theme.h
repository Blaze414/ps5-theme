/* theme.c: what a theme is on the console: its name and theme.ini, its home music settings, and the system files
   it may change. */
#ifndef PS5_THEME_THEME_H
#define PS5_THEME_THEME_H

#include <stddef.h>

/* Theme music. The console plays its own home music, HOME_MUSIC (ATRAC9),
   which a theme replaces like any other file; the browser encodes it. The
   theme also keeps the source as 48 kHz 16-bit stereo PCM, so the music can
   be encoded again at another loudness or for a console whose home music has
   a different format. */
#define HOME_MUSIC       "/system_ex/vsh_asset/bgm_home.at9"
#define MUSIC_FILE       "music.wav"       /* track 1's source; added tracks are music2.wav, music3.wav, … */
#define MUSIC_MAX_BYTES  (128ull << 20)
#define MUSIC_RATE       48000
#define MUSIC_MAX_TRACKS 8                 /* a playlist's songs; the console caps playback at ~2× its own track */
#define MUSIC_MAX_MS     3600000           /* bounds for the selected portion */
#define BLEND_MAX_MS     8000
#define GAIN_MIN         (-12)             /* dB relative to the system music's loudness */
#define GAIN_MAX         12

/* theme.ini music settings: loudness, the music part and the loop part of
   music.wav (milliseconds, end 0 = to the end of the music; loop -1 = none),
   the order they play in and how the joints are blended. Orders:
     music        the music part repeats
     loop-first   the loop part once, then the music part repeats
     alternate    the loop part, then the music part, over and over
     music-first  the music part once, then the loop part repeats

   A playlist (tracks >= 2) plays several source songs in sequence, each its
   own part [start,end], joined by `between`; at the end it loops back to the
   first (end_mode "loop-all") or repeats the last track's loop part
   ("loop-last", using loop_start/loop_end on the last slot). The browser
   assembles all of it into the one bgm_home.at9; the plugin only stores the
   sources (music.wav, music2.wav, …) and these settings. Per-track parts are
   kept per slot (track_start_ms[slot-1]); `seq` is the play order of slots. */
typedef struct {
  int gain;                              /* dB */
  int start_ms, end_ms;                  /* slot 1's part; also the single song when tracks == 1 */
  int play_from_ms;                      /* "music" order: where playback starts, -1 = the start */
  int loop_start_ms, loop_end_ms;        /* single song's loop part, or the last track's for "loop-last" */
  char order[12];
  char loop[12];                         /* joint at the loop point: "crossfade", "fade" or "cut" */
  int blend_ms;                          /* length of the crossfade or fades */
  int tracks;                            /* number of playlist tracks, 1 .. MUSIC_MAX_TRACKS */
  int seq[MUSIC_MAX_TRACKS];             /* play order, as slot numbers (1-based) */
  int track_start_ms[MUSIC_MAX_TRACKS];  /* each slot's part, indexed by slot - 1 (slot 1 mirrors start_ms) */
  int track_end_ms[MUSIC_MAX_TRACKS];
  char between[12];                      /* joint between songs: "crossfade", "fade" or "cut" */
  char end_mode[12];                     /* "loop-all" or "loop-last" */
} music_settings_t;
void read_music_settings(const char *id, music_settings_t *m);
int  write_theme_ini(const char *id, const char *name, const char *fw, const music_settings_t *music);
int  music_loop_valid(const char *loop);
int  music_order_valid(const char *order);
int  music_end_mode_valid(const char *mode);
void music_source_file(int slot, char *out, size_t size);   /* "music.wav" or "music<slot>.wav" */

int  theme_name_valid(const char *name);
void theme_title(const char *id, char *out, size_t size);  /* its name from theme.ini, else its ID */
int  read_ini_value(const char *path, const char *key, char *out, size_t size);   /* 0 if found */
void fw_string(char *out, size_t size);
int  fw_matches(const char *list, const char *fw);

/* The system files a theme may change: in a themable folder (target_allowed), and for asset_path_valid also an
   existing regular file whose real location is allowed too. */
int  target_allowed(const char *path);
int  asset_path_valid(const char *path);
const char *theme_folder(size_t i);   /* the i-th themable folder, ending in '/'; NULL after the last */
/* Kinds of themable folder. The home screen's own views only (home_view_only): titles' pictures and the fonts. */
int  title_target(const char *dir);
int  font_target(const char *dir);
int  artwork_target(const char *dir);
int  home_view_only(const char *dir);
void asset_listing_path(char *out, size_t size);
/* Where the system version of `asset` can be read. While a theme is applied the system path shows the theme's
   version, so every apply keeps a copy of each file it replaces under STOCK_DIR/<firmware>/. */
void stock_path(const char *asset, char *out, size_t size);

#endif
