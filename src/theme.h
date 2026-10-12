/* Shared declarations for ps5-theme. */
#ifndef PS5_THEME_H
#define PS5_THEME_H

#include <stddef.h>
#include <stdint.h>

#define PLUGIN_VERSION "1.30"
#define TITLE_ID       "THEM00001"
#define WEB_PORT       8095

#define BASE_DIR     "/data/ps5-theme"
#define THEMES_DIR   BASE_DIR "/themes"
#define STAGE_DIR    BASE_DIR "/stage"
#define DUMP_DIR     BASE_DIR "/dump"
#define STOCK_DIR    BASE_DIR "/stock"
#define SNAP_DIR     BASE_DIR "/snapshots"
#define UPDATE_DIR   BASE_DIR "/update"     /* drop a newer ps5-theme.elf or .plugin here */
#define LOG_DIR      BASE_DIR "/logs"
#define LOG_FILE     LOG_DIR "/log.txt"
#define ACTIVE_FILE  BASE_DIR "/active"
#define MOUNTS_FILE  BASE_DIR "/mounts"
#define TILE_DIR     "/user/app/" TITLE_ID


/* log.c */
void log_init(void);
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));


/* fsguard.c: every write the plugin makes goes through these. A write is only
   allowed when the real (symlink-resolved) location is inside BASE_DIR or
   TILE_DIR, and the final path component is never followed. */
int  fs_guard_init(void);
int  fs_write_allowed(const char *path);
int  fs_read_within(const char *path, const char *root);
int  fs_open_write(const char *path);
int  fs_mkdirs(const char *path);
int  fs_rename(const char *from, const char *to);
int  fs_unlink(const char *path);
int  fs_rmdir(const char *path);
int  fs_symlink(const char *target, const char *path);
int  fs_link(const char *existing, const char *path);
int  fs_rm_rf_under(const char *root, const char *path);
int  fs_write_file(const char *path, const void *data, size_t len);
int  fs_is_dir(const char *path);
uint32_t crc32_update(uint32_t crc, const void *buf, size_t len);
int  fs_crc32_file(const char *path, uint32_t *crc, uint64_t *size);
int  fs_copy_tree(const char *src, const char *dst);
int  fs_link_tree(const char *src, const char *dst);


/* verify.c: image format and size checks used before a file enters a theme. */
enum { IMG_NONE = 0, IMG_PNG, IMG_JPEG, IMG_WEBP };
typedef struct image_info {
  int      format;
  uint32_t width, height;
} image_info_t;

int         image_format_for_path(const char *path);
const char *image_format_name(int format);
int         image_probe(const char *path, image_info_t *out);


/* policy.c: which system files a theme may replace and how a candidate is
   validated against the stock file. Code and unknown types are protected. */
enum { KIND_BLOCKED = 0, KIND_IMAGE, KIND_SVG, KIND_AUDIO, KIND_FONT, KIND_JSON, KIND_VIDEO, KIND_TEXTURE };
typedef struct wav_info {
  uint32_t format, channels, rate, bits;
  uint8_t  subformat[16];
  uint64_t data_offset, data_size;
} wav_info_t;

int         asset_kind(const char *path);
const char *asset_kind_name(int kind);
uint64_t    asset_size_limit(int kind);
int         asset_validate(const char *candidate, const char *stock, char *why, size_t len);
int         asset_checks_remembered(void);   /* results reused since the last call */
int         wav_probe(const char *path, wav_info_t *out);


/* Theme music. The console plays its own home music, /system_ex/vsh_asset/
   bgm_home.at9 (ATRAC9), which a theme replaces like any other file; the
   browser encodes it. The theme also keeps the source as 48 kHz 16-bit
   stereo PCM, so the music can be encoded again at another loudness or for a
   console whose home music has a different format. */
#define MUSIC_FILE      "music.wav"
#define MUSIC_MAX_BYTES (128ull << 20)
#define MUSIC_RATE      48000
#define HOME_MUSIC      "/system_ex/vsh_asset/bgm_home.at9"
#define GAIN_MIN        (-12)            /* dB relative to the system music's loudness */
#define GAIN_MAX        12
#define MUSIC_MAX_MS    3600000          /* bounds for the selected portion */
#define BLEND_MAX_MS    8000

/* theme.ini music settings: loudness, the music part and the loop part of
   music.wav (milliseconds, end 0 = to the end of the music; loop -1 = none),
   the order they play in and how the joints are blended. Orders:
     music        the music part repeats
     loop-first   the loop part once, then the music part repeats
     alternate    the loop part, then the music part, over and over
     music-first  the music part once, then the loop part repeats */
typedef struct {
  int gain;                              /* dB */
  int start_ms, end_ms;
  int play_from_ms;                      /* "music" order: where playback starts, -1 = the start */
  int loop_start_ms, loop_end_ms;
  char order[12];
  char loop[12];                         /* "crossfade", "fade" or "cut" */
  int blend_ms;                          /* length of the crossfade or fades */
} music_settings_t;
int   music_wav_ok(const char *path, wav_info_t *info, char *why, size_t len);


/* atrac9.c: ATRAC9 (.at9) format and bitstream checks. */
int   at9_validate(const char *candidate, const char *stock, char *why, size_t len);


/* texture.c: DDS textures (.dds, .dds.zst). */
typedef struct texture_info {
  uint32_t width, height, mips, array, dxgi;
  char     fourcc[5];
  uint32_t header_len;
  uint64_t data_len;
  int      known;            /* data size checked against the format */
} texture_info_t;

uint8_t    *texture_load(const char *path, size_t *len, int *compressed);
int         texture_parse(const uint8_t *d, size_t len, texture_info_t *t);
int         texture_info(const char *path, texture_info_t *t, uint8_t header[148]);
int         texture_validate(const char *candidate, const char *stock, char *why, size_t len);
const char *texture_format_name(const texture_info_t *t);


/* zstd.c: Zstandard decompression (to read and verify .dds.zst textures). */
int64_t  zstd_content_size(const uint8_t *src, size_t len);
int64_t  zstd_decompress(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);
uint64_t xxh64(const void *data, size_t len);


/* crash.c */
#define WEB_WORKERS 4
enum { CRUMB_MAIN = 0, CRUMB_LISTEN, CRUMB_TILE, CRUMB_WEB, CRUMB_COUNT = CRUMB_WEB + WEB_WORKERS };
void crash_install(void);
void crash_thread_init(int slot);
void crash_thread_exit(int slot);
void crumb(int slot, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void crash_report_previous(void);


/* main.c */
void fw_string(char *out, size_t size);
int  read_ini_value(const char *path, const char *key, char *out, size_t size);
int  target_allowed(const char *path);
int  theme_name_valid(const char *name);
int  fw_matches(const char *list, const char *fw);
void asset_listing_path(char *out, size_t size);
void read_active(char *out, size_t size);
int  write_active(const char *name);
int  applied_theme(char *out, size_t size);
int  applying_theme(char *out, size_t size);
int  apply_progress(char *text, size_t size);   /* percent, or -1 when idle */
void last_result(char *out, size_t size);
void themes_lock(void);
void themes_unlock(void);
void read_music_settings(const char *id, music_settings_t *m);
int  music_loop_valid(const char *loop);
int  music_order_valid(const char *order);
/* Where the system version of `asset` can be read. While a theme is applied
   the system path shows the theme's version, so every apply keeps a copy of
   each file it replaces under STOCK_DIR/<firmware>/. */
void stock_path(const char *asset, char *out, size_t size);
int  write_theme_ini(const char *id, const char *name, const char *fw, const music_settings_t *music);


/* update.c: one running copy, and handing over to a newer one. */
int   version_value(const char *v);        /* "1.23" -> 123, or -1 */
int   my_version(void);
int   instance_lock(int wait_ms);          /* 0 once this copy holds the lock */
int   running_version(void);               /* of the copy answering locally, or -1 */
int   ask_handoff(void);
void  request_handoff(int pid);            /* pid of the newer copy, or 0 if it didn't say */
int   handoff_requested(void);
int   handoff_due(void);                   /* asked, and the newer copy is still there */
int   update_check(void);                  /* 1 if a newer copy was started */


/* web.c, tile.c */
void *web_server_thread(void *arg);
void  lan_address(char *out, size_t size);
int   tile_install(void);

#endif
