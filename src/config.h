/* What the plugin is, and where it keeps things on the console. */
#ifndef PS5_THEME_CONFIG_H
#define PS5_THEME_CONFIG_H

#define PLUGIN_VERSION "1.91"
#define TITLE_ID       "THEM00001"
#define WEB_PORT       8095

/* The plugin's own folder: with TILE_DIR, the only place it writes (fsguard.c). */
#define BASE_DIR      "/data/ps5-theme"
#define THEMES_DIR    BASE_DIR "/themes"
#define STAGE_DIR     BASE_DIR "/stage"
#define DUMP_DIR      BASE_DIR "/dump"
#define STOCK_DIR     BASE_DIR "/stock"
#define SNAP_DIR      BASE_DIR "/snapshots"
#define UPDATE_DIR    BASE_DIR "/update"      /* drop a newer ps5-theme.elf or .plugin here */
#define LOG_DIR       BASE_DIR "/logs"
#define INCOMING_DIR  BASE_DIR "/incoming"    /* uploads being received (upload.c) */
#define LOG_FILE      LOG_DIR "/log.txt"
#define ACTIVE_FILE   BASE_DIR "/active"
#define MOUNTS_FILE   BASE_DIR "/mounts"
#define LIVE_FILE     BASE_DIR "/live"        /* the applied theme's targets and stages */
#define TOAST_ICON    BASE_DIR "/icon.png"    /* beside its notifications, as etaHEN's own (written at start) */
#define INSTANCE_LOCK BASE_DIR "/instance.lock"
#define PID_FILE      BASE_DIR "/pid"         /* "<pid> <start>" of the copy holding the lock */
#define TILE_DIR      "/user/app/" TITLE_ID

/* Installed games' and apps' metadata, one folder per title: their icons and
   other pictures may be themed (only pictures: the rest is the system's).
   Most PS5 games' records point instead at the sce_sys folder of the game's
   installation, /user/app/<title>/sce_sys, the only part of APP_DIR that
   may be themed. */
#define APPMETA_DIR     "/user/appmeta"
#define APP_DIR         "/user/app"
#define PATCH_DIR       "/user/patch"
#define DOWNLOAD_DIR    "/user/download"   /* the console's downloads, one folder per title, written as they come */
/* The pictures the console downloads from PlayStation Network: a title's
   (one folder per title) and its game's (one folder per concept, which a
   game's editions share). The app database points some games' tiles at
   these instead (Death Stranding's at conceptmeta/234585 on 13.42). */
#define CATALOG_DIR         "/user/catalog_downloader"
#define CATALOG_APPMETA_DIR CATALOG_DIR "/appmeta"
#define CONCEPTMETA_DIR     CATALOG_DIR "/conceptmeta"
#define FONT_DIR        "/preinst/common/font"    /* the console's text fonts (SST and others) */
#define SYS_ASSET_DIR   "/system/vsh_asset"       /* the Appearance themes' textures, Welcome backgrounds */

#endif
