/* Uploads: a request's body is received into a file of its own, outside every theme, with its length and the
   page's checksum checked as it comes; it is then checked against the console's own file (format, exact size) and
   moved into its theme under the theme lock. */

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <sys/param.h>

#include "api.h"
#include "config.h"
#include "fsguard.h"
#include "http.h"
#include "status.h"
#include "store.h"

/* The temporary file an upload is received into: its own for each request, so two uploads of the same file at
   once never write into each other. It ends in ".part", which listings and checks skip. */
static atomic_uint upload_seq;

void
part_name(char *out, size_t size, const char *dst) {
  /* Received in a folder of its own, outside every theme: a theme's folder can be moved aside meanwhile (Discard),
     copied (Duplicate, the snapshot Discard goes back to) or deleted, and an upload under way must neither go with
     it nor fail for it. Moved into the theme, under its lock, once it has passed its checks (place_upload). */
  const char *base = strrchr(dst, '/');
  snprintf(out, size, "%s/%u-%s.part", INCOMING_DIR, atomic_fetch_add(&upload_seq, 1), base ? base + 1 : dst);
}

/* Stream the request body into `part` (no lock held while the network is
   slow), checking it against the browser's checksum. 0 on success; on failure
   the error has been sent and `part` removed. */
int
receive_upload(request_t *req, const char *part, uint32_t *crc_out, char crc_hex[9]) {
  char want_crc[16];
  if(fs_room_for((uint64_t)req->content_length)) {
    respond_error(req->fd, 507, STORAGE_FULL);
    return -1;
  }
  int out = fs_mkdirs(INCOMING_DIR) ? -1 : fs_open_write(part);
  if(out < 0) {
    respond_error(req->fd, 500, "could not open file");
    return -1;
  }

  long remaining = req->content_length;
  size_t first = MIN(req->body_len, (size_t)remaining);
  uint32_t crc = crc32_update(0, req->body, first);
  int ok = send_all(out, req->body, first) == 0;
  remaining -= first;
  while(ok && remaining > 0) {
    ssize_t n = read(req->fd, req->io, MIN((size_t)IO_CHUNK, (size_t)remaining));
    if(n < 0 && errno == EINTR) continue;
    if(n <= 0) { ok = 0; break; }
    crc = crc32_update(crc, req->io, n);
    ok = send_all(out, req->io, n) == 0;
    remaining -= n;
  }
  if(fsync(out)) ok = 0;
  if(close(out)) ok = 0;
  if(!ok) {
    fs_unlink(part);
    respond_error(req->fd, 400, "upload was interrupted");
    return -1;
  }

  /* Verification 1: the bytes are what the browser sent. */
  snprintf(crc_hex, 9, "%08x", crc);
  get_param(req->query, "crc", want_crc, sizeof(want_crc));
  if(want_crc[0] && strcasecmp(want_crc, crc_hex)) {
    fs_unlink(part);
    respond_error(req->fd, 422, "upload was corrupted in transfer (checksum mismatch)");
    return -1;
  }
  *crc_out = crc;
  return 0;
}

/* `keep`: the upload is a change Discard can undo (not a picture the
   library made by itself). `length` is what the file must measure: what was
   received, or its length after the check wrote it again (a texture's frame). */
int
place_upload(request_t *req, const char *id, const char *part, const char *dst, uint32_t crc,
             uint64_t *size, int keep, uint64_t length) {
  uint32_t stored = 0;
  /* The destination's folders are made only here, under the themes lock: a folder of a theme made outside it could
     be made while Discard has moved the theme aside, and Discard couldn't then put either copy back (1.77's test of
     every combination found it, with the theme's files left aside and the theme empty). */
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s", dst);
  if(strrchr(dir, '/')) *strrchr(dir, '/') = 0;
  themes_lock();
  int moved = theme_exists(id) && (!keep || !keep_snapshot(id)) && !fs_mkdirs(dir) && !fs_rename(part, dst);
  int placed = moved && !fs_crc32_file(dst, &stored, size) && stored == crc && *size == length;
  if(moved && !placed) fs_unlink(dst);   /* it didn't read back as sent: never leave it in the theme */
  themes_unlock();
  if(!placed) {
    fs_unlink(part);
    respond_error(req->fd, 500, "could not store the file");
    return -1;
  }
  return 0;
}
