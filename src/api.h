/* The manager's API: one handler for each route (routes.c lists them), grouped by what they serve. Every handler
   answers on req->fd before it returns. */
#ifndef PS5_THEME_API_H
#define PS5_THEME_API_H

#include <stddef.h>
#include <stdint.h>

#include "http.h"

/* api_themes.c: the library, and a theme as a whole. */
void api_status(request_t *req);
void api_theme_files(request_t *req);
void api_create_theme(request_t *req);
void api_delete_theme(request_t *req);
void api_duplicate_theme(request_t *req);
void api_theme_settings(request_t *req);
void api_save_theme(request_t *req);
void api_discard_theme(request_t *req);
void api_apply(request_t *req);

/* api_files.c: a theme's files, and the pictures its textures were made from (sources). */
void api_put_file(request_t *req);
void api_get_file(request_t *req);
void api_check_file(request_t *req);
void api_delete_file(request_t *req);
void api_put_source(request_t *req);
void api_get_source(request_t *req);
void api_sources(request_t *req);
void api_delete_sources(request_t *req);
int  source_kind_ok(const char *asset);   /* a system file whose picture is kept */

/* api_parts.c: a theme's music sources, preview pictures and the editor's choices. */
void api_put_music(request_t *req);
void api_get_music(request_t *req);
void api_delete_music(request_t *req);
void api_put_preview(request_t *req);
void api_get_preview(request_t *req);
void api_delete_preview(request_t *req);
void api_get_choices(request_t *req);
void api_put_choices(request_t *req);
/* The preview picture the library shows for a theme: its path and time, and which kind it is; NULL if none. */
const char *shown_preview(const char *id, char *path, size_t size, long long *mtime);

/* api_style.c: a theme's icon style, for games and apps installed later. */
void api_put_icon_style(request_t *req);
void api_post_icon_style(request_t *req);
void api_get_icon_style(request_t *req);
int  style_title_ok(const char *t);   /* a title ID the style may name */

/* api_mix.c: Mix and match. */
void api_mix_themes(request_t *req);

/* api_console.c: the console's own files, read-only. */
void api_assets(request_t *req);
void api_app_db(request_t *req);
void api_get_asset(request_t *req);
void api_ui_icon(request_t *req);
void api_texture(request_t *req);
void api_texture_picture(request_t *req);
void api_audio(request_t *req);
/* Which titles show the pictures of which concept folder (from the console's app database). */
#define MAX_CONCEPTS 256
typedef struct { char concept[16], title[10]; } concept_t;
int  title_concepts(concept_t *out);

/* api_diag.c: the log, crash reports and what the plugin sees, for finding out why. */
void api_log(request_t *req);
void api_crashes(request_t *req);
void api_crash(request_t *req);
void api_mounts(request_t *req);
void api_watch(request_t *req);
void api_procs(request_t *req);
void api_title_where(request_t *req);

/* api_system.c: the plugin itself: who may open the manager, its settings, its storage, updating and stopping. */
void api_access(request_t *req);
void api_pair_start(request_t *req);
void api_pair_finish(request_t *req);
void api_set_access(request_t *req);
void api_forget_device(request_t *req);
void api_settings(request_t *req);
void api_set_setting(request_t *req);
void api_storage(request_t *req);
void api_storage_clean(request_t *req);
void api_handoff(request_t *req);
void api_stop(request_t *req);

/* upload.c: a request's body received into INCOMING_DIR, then placed into a theme (see there). */
void part_name(char *out, size_t size, const char *dst);
int  receive_upload(request_t *req, const char *part, uint32_t *crc_out, char crc_hex[9]);
int  place_upload(request_t *req, const char *id, const char *part, const char *dst, uint32_t crc,
                  uint64_t *size, int keep, uint64_t length);

#endif
