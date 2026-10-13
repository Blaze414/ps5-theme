/* Image header parsing, so a file only enters a theme when it really is the
   format the system asset is (ShellUI would otherwise fail to draw it). */

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "theme.h"


#define PROBE_BYTES (256 * 1024)


int
image_format_for_path(const char *path) {
  const char *ext = strrchr(path, '.');
  if(!ext || strchr(ext, '/')) return IMG_NONE;
  if(!strcasecmp(ext, ".png")) return IMG_PNG;
  if(!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return IMG_JPEG;
  if(!strcasecmp(ext, ".webp")) return IMG_WEBP;
  return IMG_NONE;
}


const char *
image_format_name(int format) {
  switch(format) {
  case IMG_PNG:  return "PNG";
  case IMG_JPEG: return "JPEG";
  case IMG_WEBP: return "WebP";
  default:       return "unknown";
  }
}


static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8; }
static uint32_t le24(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16; }
static uint32_t le32(const uint8_t *p) { return le24(p) | (uint32_t)p[3] << 24; }


static int
probe_png(const uint8_t *b, size_t n, image_info_t *out) {
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  if(n < 33 || memcmp(b, sig, 8) || memcmp(b + 12, "IHDR", 4) || be32(b + 8) != 13) return -1;
  out->format = IMG_PNG;
  out->width = be32(b + 16);
  out->height = be32(b + 20);
  return 0;
}


static int
probe_jpeg(const uint8_t *b, size_t n, image_info_t *out) {
  if(n < 4 || b[0] != 0xff || b[1] != 0xd8) return -1;
  size_t i = 2;
  while(i + 9 < n) {
    if(b[i] != 0xff) return -1;
    uint8_t marker = b[i + 1];
    if(marker == 0xff) { i++; continue; }
    if(marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) { i += 2; continue; }
    uint32_t len = be16(b + i + 2);
    if(len < 2) return -1;
    /* SOF0..SOF15 except DHT (C4), JPG (C8) and DAC (CC). */
    if(marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
      out->format = IMG_JPEG;
      out->height = be16(b + i + 5);
      out->width = be16(b + i + 7);
      return 0;
    }
    if(marker == 0xda || marker == 0xd9) return -1;  /* scan or end before a frame header */
    i += 2 + len;
  }
  return -1;
}


static int
probe_webp(const uint8_t *b, size_t n, image_info_t *out) {
  if(n < 30 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WEBP", 4)) return -1;
  const uint8_t *c = b + 12;
  if(!memcmp(c, "VP8 ", 4) && c[11] == 0x9d && c[12] == 0x01 && c[13] == 0x2a) {
    out->width = le16(c + 14) & 0x3fff;
    out->height = le16(c + 16) & 0x3fff;
  } else if(!memcmp(c, "VP8L", 4) && c[8] == 0x2f) {
    uint32_t bits = le32(c + 9);
    out->width = (bits & 0x3fff) + 1;
    out->height = ((bits >> 14) & 0x3fff) + 1;
  } else if(!memcmp(c, "VP8X", 4)) {
    out->width = le24(c + 12) + 1;
    out->height = le24(c + 15) + 1;
  } else {
    return -1;
  }
  out->format = IMG_WEBP;
  return 0;
}


/* 0 and fills `out` when `data` starts with a well-formed PNG, JPEG or WebP
   header. */
int
image_probe_mem(const uint8_t *data, size_t len, image_info_t *out) {
  memset(out, 0, sizeof(*out));
  if(!probe_png(data, len, out) || !probe_jpeg(data, len, out) || !probe_webp(data, len, out)) {
    return out->width && out->height ? 0 : -1;
  }
  return -1;
}


/* The same for a file. */
int
image_probe(const char *path, image_info_t *out) {
  size_t n = 0;
  int ret = -1;

  memset(out, 0, sizeof(*out));
  int fd = open(path, O_RDONLY);
  if(fd < 0) return -1;
  uint8_t *buf = malloc(PROBE_BYTES);
  if(!buf) { close(fd); return -1; }
  while(n < PROBE_BYTES) {
    ssize_t r = read(fd, buf + n, PROBE_BYTES - n);
    if(r <= 0) break;
    n += r;
  }
  close(fd);

  ret = image_probe_mem(buf, n, out);
  free(buf);
  return ret;
}
