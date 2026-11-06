/* Every route of the manager's API, and who may ask for it.

   Who may ask anything at all is decided first, by access.c: devices on the console's own network only, by default
   only the ones allowed with a code the console shows, and never a page that reached the console through an
   internet name. Every request that changes something must also carry "X-PS5-Theme: 1". Browsers cannot add a
   custom header cross-origin without a CORS preflight (which is never answered), so other web pages cannot drive
   this API. */

#include <signal.h>
#include <string.h>

#include "access.h"
#include "api.h"
#include "http.h"
#include "log.h"
#include "store.h"

/* A refusal by access_gate: for the page's own requests, JSON saying what to do ("pair": a code is needed;
   "console": it opens only on the console); for anything else, the reason as text. */
static void
respond_access(request_t *req, int status, const char *why) {
  if(strncmp(req->path, "/api/", 5)) {
    respond(req->fd, status, "text/plain; charset=utf-8", why, strlen(why));
    return;
  }
  strbuf_t sb = {0};
  SB_LIT(&sb, "{\"error\":");
  sb_json_str(&sb, why);
  sb_printf(&sb, ",\"access\":\"%s\"}", status == 401 ? "pair" : req->who.mode == ACCESS_CONSOLE ? "console" : "refused");
  respond_json(req->fd, status, &sb);
}

static void page(request_t *req)    { respond_page_asset(req, 0, "text/html; charset=utf-8"); }
static void encoder(request_t *req) { respond_page_asset(req, 1, "text/javascript; charset=utf-8"); }
static void logo(request_t *req)    { respond_page_asset(req, 2, "image/png"); }

#ifdef HOST_TEST
static void
debug_crash(request_t *req) {
  log_msg("debug: crashing on request");
  raise(SIGSEGV);
}
#endif

/* A short read of a theme's contents (its files, choices, icon style, sources): the whole of it before or after a
   Discard, never half of each (see store.c). Only routes that answer a little JSON: a large file sent to a slow
   client would hold a Discard up (respond_file looks again instead). None of them takes themes_lock. */
#define THEME_READ 1

typedef struct {
  const char *method, *path;
  void      (*fn)(request_t *req);
  int         open;    /* what a device not allowed yet may ask for (access_gate) */
  int         flags;
} route_t;

static const route_t routes[] = {
  /* The page, and asking for a code: open to a device not allowed yet, so it can say what to do. */
  { "GET",    "/",                      page,                ACCESS_OPEN_PAGE, 0 },
  { "GET",    "/index.html",            page,                ACCESS_OPEN_PAGE, 0 },
  { "GET",    "/atrac9.js",             encoder,             ACCESS_OPEN_PAGE, 0 },
  { "GET",    "/icon.png",              logo,                ACCESS_OPEN_PAGE, 0 },
  { "GET",    "/api/access",            api_access,          ACCESS_OPEN_PAGE, 0 },
  { "POST",   "/api/pair/start",        api_pair_start,      ACCESS_OPEN_PAIR, 0 },
  { "POST",   "/api/pair/finish",       api_pair_finish,     ACCESS_OPEN_PAIR, 0 },

  /* The plugin. */
  { "GET",    "/api/status",            api_status,          ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/settings",          api_settings,        ACCESS_OPEN_NOT, 0 },
  { "POST",   "/api/settings",          api_set_setting,     ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/storage",           api_storage,         ACCESS_OPEN_NOT, 0 },
  { "POST",   "/api/storage/clean",     api_storage_clean,   ACCESS_OPEN_NOT, 0 },
  { "POST",   "/api/access",            api_set_access,      ACCESS_OPEN_NOT, 0 },
  { "DELETE", "/api/access/devices",    api_forget_device,   ACCESS_OPEN_NOT, 0 },
  { "POST",   "/api/apply",             api_apply,           ACCESS_OPEN_NOT, 0 },
  { "POST",   "/api/handoff",           api_handoff,         ACCESS_OPEN_NOT, 0 },
  { "POST",   "/api/stop",              api_stop,            ACCESS_OPEN_NOT, 0 },

  /* Themes. */
  { "POST",   "/api/themes",            api_create_theme,    ACCESS_OPEN_NOT, 0 },
  { "DELETE", "/api/themes",            api_delete_theme,    ACCESS_OPEN_NOT, 0 },
  { "POST",   "/api/themes/duplicate",  api_duplicate_theme, ACCESS_OPEN_NOT, 0 },
  { "POST",   "/api/themes/mix",        api_mix_themes,      ACCESS_OPEN_NOT, 0 },
  { "POST",   "/api/theme/settings",    api_theme_settings,  ACCESS_OPEN_NOT, 0 },
  { "POST",   "/api/theme/save",        api_save_theme,      ACCESS_OPEN_NOT, 0 },
  { "POST",   "/api/theme/discard",     api_discard_theme,   ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/theme/files",       api_theme_files,     ACCESS_OPEN_NOT, THEME_READ },
  { "GET",    "/api/theme/file",        api_get_file,        ACCESS_OPEN_NOT, 0 },
  { "PUT",    "/api/theme/file",        api_put_file,        ACCESS_OPEN_NOT, 0 },
  { "DELETE", "/api/theme/file",        api_delete_file,     ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/theme/check",       api_check_file,      ACCESS_OPEN_NOT, THEME_READ },
  { "GET",    "/api/theme/source",      api_get_source,      ACCESS_OPEN_NOT, 0 },
  { "PUT",    "/api/theme/source",      api_put_source,      ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/theme/sources",     api_sources,         ACCESS_OPEN_NOT, THEME_READ },
  { "DELETE", "/api/theme/sources",     api_delete_sources,  ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/theme/music",       api_get_music,       ACCESS_OPEN_NOT, 0 },
  { "PUT",    "/api/theme/music",       api_put_music,       ACCESS_OPEN_NOT, 0 },
  { "DELETE", "/api/theme/music",       api_delete_music,    ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/theme/preview",     api_get_preview,     ACCESS_OPEN_NOT, 0 },
  { "PUT",    "/api/theme/preview",     api_put_preview,     ACCESS_OPEN_NOT, 0 },
  { "DELETE", "/api/theme/preview",     api_delete_preview,  ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/theme/choices",     api_get_choices,     ACCESS_OPEN_NOT, THEME_READ },
  { "PUT",    "/api/theme/choices",     api_put_choices,     ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/theme/icon-style",  api_get_icon_style,  ACCESS_OPEN_NOT, THEME_READ },
  { "PUT",    "/api/theme/icon-style",  api_put_icon_style,  ACCESS_OPEN_NOT, 0 },
  { "POST",   "/api/theme/icon-style",  api_post_icon_style, ACCESS_OPEN_NOT, 0 },

  /* The console's own files, read-only. */
  { "GET",    "/api/assets",            api_assets,          ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/app-db",            api_app_db,          ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/asset",             api_get_asset,       ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/asset/picture",     api_texture_picture, ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/asset/texture",     api_texture,         ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/asset/audio",       api_audio,           ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/ui-icon",           api_ui_icon,         ACCESS_OPEN_NOT, 0 },

  /* Finding out why. */
  { "GET",    "/api/log",               api_log,             ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/crashes",           api_crashes,         ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/crash",             api_crash,           ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/mounts",            api_mounts,          ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/title/where",       api_title_where,     ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/diag/watch",        api_watch,           ACCESS_OPEN_NOT, 0 },
  { "GET",    "/api/diag/procs",        api_procs,           ACCESS_OPEN_NOT, 0 },
#ifdef TEST_HOOKS
  { "POST",   "/api/debug/break-listener", http_break_listener, ACCESS_OPEN_NOT, 0 },
  { "POST",   "/api/debug/deafen",      http_deafen,         ACCESS_OPEN_NOT, 0 },
#endif
#ifdef HOST_TEST
  { "POST",   "/api/debug/crash",       debug_crash,         ACCESS_OPEN_NOT, 0 },
#endif
};

static int
changes(const char *method) {
  return !strcmp(method, "POST") || !strcmp(method, "PUT") || !strcmp(method, "DELETE");
}

void
route(request_t *req) {
  const route_t *r = NULL;
  for(size_t i = 0; i < sizeof(routes) / sizeof(*routes) && !r; i++) {
    if(!strcmp(routes[i].method, req->method) && !strcmp(routes[i].path, req->path)) r = &routes[i];
  }
  /* Who is asking, before anything else, for every request. */
  char why[160];
  int st = access_gate(req->fd, req->has_host ? req->host : NULL, req->cookie, r ? r->open : ACCESS_OPEN_NOT,
                       &req->who, why, sizeof(why));
  if(st) {
    respond_access(req, st, why);
  } else if(changes(req->method) && !req->has_token) {
    respond_error(req->fd, 403, "missing X-PS5-Theme header");
  } else if(!r) {
    respond_error(req->fd, 404, "not found");
  } else if(r->flags & THEME_READ) {
    swap_read_begin();
    r->fn(req);
    swap_read_end();
  } else {
    r->fn(req);
  }
}
