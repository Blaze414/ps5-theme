/* policy.c: which system files a theme may replace, and how a candidate is checked against the console's own.
   Code and unknown types are never replaced. */
#ifndef PS5_THEME_POLICY_H
#define PS5_THEME_POLICY_H

#include <stddef.h>
#include <stdint.h>

#include "fsguard.h"

enum { KIND_BLOCKED = 0, KIND_IMAGE, KIND_SVG, KIND_AUDIO, KIND_FONT, KIND_JSON, KIND_VIDEO, KIND_TEXTURE, KIND_RCO };

int         asset_kind(const char *path);
const char *asset_kind_name(int kind);
uint64_t    asset_size_limit(int kind);
int         title_folder(const char *dir);
int         app_path_ok(const char *path);
int         asset_validate(const char *candidate, const char *stock, char *why, size_t len);
/* The same, and the identity of the very file that passed: what is copied later is checked against it. */
int         asset_validate_id(const char *candidate, const char *stock, char *why, size_t len, fs_id_t *passed);
int         asset_checks_remembered(void);   /* results reused since the last call */

typedef struct wav_info {
  uint32_t format, channels, rate, bits;
  uint8_t  subformat[16];
  uint64_t data_offset, data_size;
} wav_info_t;
int         wav_probe(const char *path, wav_info_t *out);
/* A theme's music source must be 48 kHz 16-bit stereo PCM: 1 if it is, else 0 and why. */
int         music_wav_ok(const char *path, wav_info_t *info, char *why, size_t len);

#endif
