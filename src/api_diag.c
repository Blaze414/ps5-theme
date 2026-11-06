/* For finding out why something went wrong: the log, crash reports, the mount table, and which files and databases
   name a title's pictures. Read-only. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <sys/stat.h>

#include "api.h"
#include "config.h"
#include "fsguard.h"
#include "http.h"
#include "log.h"
#include "platform.h"

void
api_log(request_t *req) {
  int in = open(LOG_FILE, O_RDONLY);
  ssize_t n = 0;

  if(in >= 0) {
    off_t size = lseek(in, 0, SEEK_END);
    lseek(in, size > (off_t)IO_CHUNK ? size - (off_t)IO_CHUNK : 0, SEEK_SET);
    n = read(in, req->io, IO_CHUNK);
    close(in);
  }
  respond(req->fd, 200, "text/plain; charset=utf-8", req->io, n > 0 ? n : 0);
}

static int
crash_name_valid(const char *name) {
  size_t n = strlen(name);
  if(n < 11 || n > 40 || strncmp(name, "crash-", 6) || strcmp(name + n - 4, ".txt")) return 0;
  for(size_t i = 6; i < n - 4; i++) if(name[i] < '0' || name[i] > '9') return 0;
  return 1;
}

void
api_crashes(request_t *req) {
  strbuf_t sb = {0};
  int first = 1;
  DIR *d = opendir(LOG_DIR);

  SB_LIT(&sb, "{\"crashes\":[");
  if(d) {
    struct dirent *e;
    while((e = readdir(d))) {
      if(!crash_name_valid(e->d_name)) continue;
      if(!first) SB_LIT(&sb, ",");
      first = 0;
      sb_json_str(&sb, e->d_name);
    }
    closedir(d);
  }
  SB_LIT(&sb, "]}");
  respond_json(req->fd, 200, &sb);
}

void
api_crash(request_t *req) {
  char name[64], path[PATH_MAX];

  get_param(req->query, "name", name, sizeof(name));
  if(!crash_name_valid(name)) return respond_error(req->fd, 404, "no such report");
  snprintf(path, sizeof(path), "%s/%s", LOG_DIR, name);
  respond_file(req, path);
}

/* The live mount table, for diagnosing what the system shows where (read-only). */
void
api_mounts(request_t *req) {
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  const char *path = DUMP_DIR "/mounts-now.txt", *tmp = DUMP_DIR "/mounts-now.txt.tmp";
  pthread_mutex_lock(&lock);
  int fd = fs_open_write(tmp);
  FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
  if(f) {
    plat_dump_mounts(f);
    fclose(f);
  } else if(fd >= 0) {
    close(fd);
  }
  int ok = f && !fs_rename(tmp, path);
  if(ok) respond_file(req, path);
  pthread_mutex_unlock(&lock);
  if(!ok) respond_error(req->fd, 500, "could not read the mount table");
}

/* Which of a title's picture and sound files, the console's and the theme's
   copies of them, any process opens or reads in the next few minutes
   (read-only). For finding where the home screen gets a title's icon: start
   it, then rest mode and wake the console. */
#define WATCH_FILES 96

typedef struct {
  FILE *f;
  char (*paths)[PATH_MAX];
  int last_i, repeat;
  char last_what[8];
  long last_ms;
} watch_out_t;

static void
watch_seen(int i, const char *what, long ms, void *ctx) {
  watch_out_t *w = ctx;
  if(i == w->last_i && !strcmp(what, w->last_what) && ms - w->last_ms < 1000) {
    w->repeat++;
    return;
  }
  if(w->repeat) fprintf(w->f, "           (%d more)\n", w->repeat);
  w->repeat = 0;
  fprintf(w->f, "+%3ld.%03ld s  %-5s %s\n", ms / 1000, ms % 1000, what, w->paths[i]);
  w->last_i = i;
  snprintf(w->last_what, sizeof(w->last_what), "%s", what);
  w->last_ms = ms;
}

static int
watch_add_folder(char (*paths)[PATH_MAX], int n, const char *dir) {
  DIR *d = opendir(dir);
  struct dirent *e;
  while(d && n < WATCH_FILES && (e = readdir(d))) {
    const char *dot = strrchr(e->d_name, '.');
    struct stat st;
    if(e->d_name[0] == '.' || !dot || (strcasecmp(dot, ".png") && strcasecmp(dot, ".dds") && strcasecmp(dot, ".at9"))) continue;
    if(snprintf(paths[n], PATH_MAX, "%s/%s", dir, e->d_name) >= PATH_MAX || stat(paths[n], &st) || !S_ISREG(st.st_mode)) continue;
    n++;
  }
  if(d) closedir(d);
  return n;
}

void
api_watch(request_t *req) {
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  char id[16], secs[16];
  get_param(req->query, "id", id, sizeof(id));
  get_param(req->query, "seconds", secs, sizeof(secs));
  int ok = strlen(id) == 9, seconds = atoi(secs);
  for(int i = 0; ok && i < 9; i++) ok = i < 4 ? id[i] >= 'A' && id[i] <= 'Z' : id[i] >= '0' && id[i] <= '9';
  if(!ok) return respond_error(req->fd, 400, "id must be a title ID such as PPSA01234");
  if(seconds <= 0) seconds = 120;
  if(seconds > 300) seconds = 300;
  if(pthread_mutex_trylock(&lock)) return respond_error(req->fd, 409, "a watch is already running");

  static char paths[WATCH_FILES][PATH_MAX];
  char folders[8][PATH_MAX];
  int n = 0, nf = 3;
  snprintf(folders[0], PATH_MAX, "%s/%s", APPMETA_DIR, id);
  snprintf(folders[1], PATH_MAX, "%s/%s/sce_sys", APP_DIR, id);
  snprintf(folders[2], PATH_MAX, "%s/%s", CATALOG_APPMETA_DIR, id);
  concept_t *concepts = malloc(sizeof(*concepts) * MAX_CONCEPTS);
  int nc = concepts ? title_concepts(concepts) : 0;
  for(int k = 0; k < nc && nf < 8; k++) {
    if(!strcmp(concepts[k].title, id)) snprintf(folders[nf++], PATH_MAX, "%s/%s", CONCEPTMETA_DIR, concepts[k].concept);
  }
  free(concepts);
  for(int k = 0; k < nf; k++) n = watch_add_folder(paths, n, folders[k]);
  /* The theme's staged copies of those folders, as mounted. */
  FILE *live = fopen(LIVE_FILE, "r");
  char line[PATH_MAX * 2 + 64];
  while(live && fgets(line, sizeof(line), live)) {
    line[strcspn(line, "\r\n")] = 0;
    char *state = strchr(line, '\t'), *dir = state ? strchr(state + 1, '\t') : NULL, *real = dir ? strchr(dir + 1, '\t') : NULL;
    if(strncmp(line, "stage", 5) || !real) continue;
    *dir++ = 0;
    *real++ = 0;
    char *end = strchr(real, '\t');
    if(end) *end = 0;
    for(int k = 0; k < nf; k++) if(!strcmp(real, folders[k])) n = watch_add_folder(paths, n, dir);
  }
  if(live) fclose(live);

  const char *path = DUMP_DIR "/watch.txt", *tmp = DUMP_DIR "/watch.txt.tmp";
  int fd = fs_open_write(tmp);
  FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
  if(!f) {
    if(fd >= 0) close(fd);
    pthread_mutex_unlock(&lock);
    return respond_error(req->fd, 500, "could not write the report");
  }
  fprintf(f, "# watching %d file(s) of %s for %d s\n", n, id, seconds);
  for(int i = 0; i < n; i++) fprintf(f, "#   %s\n", paths[i]);
  fflush(f);
  log_msg("watching %d picture file(s) of %s for %d s", n, id, seconds);
  watch_out_t w = {f, paths, -1, 0, "", 0};
  const char *list[WATCH_FILES];
  for(int i = 0; i < n; i++) list[i] = paths[i];
  if(plat_watch_files(list, n, seconds, watch_seen, &w)) fprintf(f, "# files can't be watched here\n");
  if(w.repeat) fprintf(f, "           (%d more)\n", w.repeat);
  fprintf(f, "# done\n");
  fclose(f);
  ok = !fs_rename(tmp, path);
  pthread_mutex_unlock(&lock);
  if(ok) respond_file(req, path);
  else respond_error(req->fd, 500, "could not write the report");
}

/* Every process's pid and name (read-only), to find the ones to watch. */
void
api_procs(request_t *req) {
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  const char *path = DUMP_DIR "/procs.txt", *tmp = DUMP_DIR "/procs.txt.tmp";
  pthread_mutex_lock(&lock);
  int fd = fs_open_write(tmp);
  FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
  if(f) {
    plat_dump_processes(f);
    fclose(f);
  } else if(fd >= 0) {
    close(fd);
  }
  int ok = f && !fs_rename(tmp, path);
  if(ok) respond_file(req, path);
  pthread_mutex_unlock(&lock);
  if(!ok) respond_error(req->fd, 500, "could not list the processes");
}

/* Where the system keeps a title's pictures, for finding what the home
   screen draws (read-only, names and sizes only): the files under the
   folders ShellUI can see whose path names the title, and the strings in
   the system's databases that name it. On 13.42, PS4 games' tiles followed
   /user/appmeta/<title>/icon0.png and PS5 games' didn't. */

#define WHERE_LINES   3000
#define WHERE_VISITS  200000
#define WHERE_STRINGS 400
#define WHERE_RUN     8192    /* longer runs are cut; around the title ID is shown */

typedef struct {
  FILE *f;
  const char *id;
  int lines, visits, strings;
  uint32_t seen[WHERE_STRINGS];
  uint8_t buf[1 << 16];
} where_t;

static void
where_walk(where_t *w, const char *dir, int depth, int named) {
  DIR *d = opendir(dir);
  if(!d) {
    if(!named) fprintf(w->f, "# %s: %s\n", dir, strerror(errno));
    return;
  }
  struct dirent *e;
  while((e = readdir(d)) && w->lines < WHERE_LINES && w->visits < WHERE_VISITS) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    w->visits++;
    char path[PATH_MAX];
    struct stat st;
    if(snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path) || lstat(path, &st)) continue;
    int here = named || strstr(e->d_name, w->id);
    if(S_ISDIR(st.st_mode)) {
      /* A folder named after the title is listed two levels down. */
      int next = here && !named ? 2 : depth - 1;
      if(next >= 0) where_walk(w, path, next, here);
      else if(here) { fprintf(w->f, "%s/ (not listed)\n", path); w->lines++; }
    } else if(here) {
      fprintf(w->f, "%s %lld %lld%s\n", path, (long long)st.st_size, (long long)st.st_mtime, S_ISLNK(st.st_mode) ? " symlink" : "");
      w->lines++;
    }
  }
  closedir(d);
}

/* A printable run from a database: kept if it names the title, once, as
   up to 300 characters either side of the first mention. */
static void
where_run(where_t *w, const char *path, char *run, size_t n) {
  if(n < strlen(w->id) || w->strings >= WHERE_STRINGS) return;
  run[n] = 0;
  char *at = strstr(run, w->id);
  if(!at) return;
  uint32_t h = crc32_update(0, run, n);
  for(int k = 0; k < w->strings; k++) if(w->seen[k] == h) return;
  w->seen[w->strings++] = h;
  char *from = at - run > 300 ? at - 300 : run;
  int len = (int)(run + n - from) > 600 + (int)strlen(w->id) ? 600 + (int)strlen(w->id) : (int)(run + n - from);
  fprintf(w->f, "%s: %s%.*s%s\n", path, from > run ? "..." : "", len, from, from + len < run + n ? "..." : "");
}

static void
where_strings(where_t *w, const char *path) {
  int fd = open(path, O_RDONLY);
  if(fd < 0) { fprintf(w->f, "# %s: %s\n", path, strerror(errno)); return; }
  char run[WHERE_RUN + 1];
  size_t n = 0;
  ssize_t r;
  while(w->strings < WHERE_STRINGS && (r = read(fd, w->buf, sizeof(w->buf))) > 0) {
    for(ssize_t i = 0; i < r; i++) {
      if(w->buf[i] >= 0x20 && w->buf[i] < 0x7f) {
        if(n < WHERE_RUN) run[n++] = (char)w->buf[i];
        continue;
      }
      where_run(w, path, run, n);
      n = 0;
    }
  }
  where_run(w, path, run, n);
  close(fd);
}

static void
where_databases(where_t *w, const char *dir, int depth) {
  DIR *d = opendir(dir);
  if(!d) { fprintf(w->f, "# %s: %s\n", dir, strerror(errno)); return; }
  struct dirent *e;
  while((e = readdir(d)) && w->strings < WHERE_STRINGS) {
    char path[PATH_MAX];
    struct stat st;
    if(e->d_name[0] == '.' || snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path) || lstat(path, &st)) continue;
    if(S_ISDIR(st.st_mode) && depth > 0) where_databases(w, path, depth - 1);
    else if(S_ISREG(st.st_mode) && st.st_size <= (128 << 20) && (strstr(e->d_name, ".db") || strstr(e->d_name, ".json"))) where_strings(w, path);
  }
  closedir(d);
}

void
api_title_where(request_t *req) {
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  static const char *roots[] = {
    "/system_data/priv/appmeta", "/system_data/priv/home", "/system_data/priv/cache", "/system_data/priv/game",
    "/system_data/priv/mms", "/system_data/nobackup/temp", "/user/appmeta", "/user/temp", "/user/av_contents",
    "/user/bgft", "/user/app", "/user/patch", "/user/catalog_downloader", "/user/download", "/user/home",
  };
  char id[16];
  get_param(req->query, "id", id, sizeof(id));
  int ok = strlen(id) == 9;
  for(int i = 0; ok && i < 9; i++) ok = i < 4 ? id[i] >= 'A' && id[i] <= 'Z' : id[i] >= '0' && id[i] <= '9';
  if(!ok) return respond_error(req->fd, 400, "id must be a title ID such as PPSA01234");

  const char *path = DUMP_DIR "/where.txt", *tmp = DUMP_DIR "/where.txt.tmp";
  pthread_mutex_lock(&lock);
  int fd = fs_open_write(tmp);
  FILE *f = fd < 0 ? NULL : fdopen(fd, "w");
  if(f) {
    where_t *w = calloc(1, sizeof(*w));
    if(w) {
      w->f = f;
      w->id = id;
      fprintf(f, "# files whose path names %s\n", id);
      for(size_t i = 0; i < sizeof(roots) / sizeof(*roots); i++) where_walk(w, roots[i], 4, 0);
      if(w->lines >= WHERE_LINES || w->visits >= WHERE_VISITS) fprintf(f, "# stopped after %d file(s), %d visited\n", w->lines, w->visits);
      fprintf(f, "# strings naming %s in the system's databases\n", id);
      where_databases(w, "/system_data/priv/mms", 1);
      where_databases(w, "/system_data/priv/home", 2);
      free(w);
    }
    fclose(f);
  } else if(fd >= 0) {
    close(fd);
  }
  ok = f && !fs_rename(tmp, path);
  if(ok) respond_file(req, path);
  pthread_mutex_unlock(&lock);
  if(!ok) respond_error(req->fd, 500, "could not write the listing");
}
