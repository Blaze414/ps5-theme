/* DDS textures (.dds and zstd-compressed .dds.zst), as used by ShellUI.

   A replacement is accepted only if it decodes to a DDS whose header is
   byte-identical to the stock texture's (same format, size, mip levels, array
   size) and whose total length matches. For the common formats the data size
   implied by the header is checked as well. */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#include "theme.h"


#define MAX_COMPRESSED  (128ull << 20)
#define MAX_DECODED     (256ull << 20)


static uint32_t
le32(const uint8_t *p) {
  return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}


static uint8_t *
read_all(const char *path, size_t max, size_t *len) {
  struct stat st;
  int fd = open(path, O_RDONLY);
  if(fd < 0) return NULL;
  if(fstat(fd, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size > max || !st.st_size) {
    close(fd);
    return NULL;
  }
  uint8_t *b = malloc((size_t)st.st_size);
  size_t got = 0;
  while(b && got < (size_t)st.st_size) {
    ssize_t n = read(fd, b + got, (size_t)st.st_size - got);
    if(n <= 0) break;
    got += n;
  }
  close(fd);
  if(!b || got != (size_t)st.st_size) {
    free(b);
    return NULL;
  }
  *len = got;
  return b;
}


static int
is_zstd(const uint8_t *p, size_t len) {
  return len >= 4 && p[0] == 0x28 && p[1] == 0xb5 && p[2] == 0x2f && p[3] == 0xfd;
}


/* Load a texture file and return its decoded DDS bytes (caller frees).
   zstd is recognised by its magic bytes, not the file name (uploads arrive
   as .part files). *compressed tells which it was. */
uint8_t *
texture_load(const char *path, size_t *len, int *compressed) {
  size_t flen;
  uint8_t *file = read_all(path, MAX_DECODED, &flen);
  if(!file) return NULL;
  *compressed = is_zstd(file, flen);
  if(!*compressed) {
    *len = flen;
    return file;
  }
  if(flen > MAX_COMPRESSED) {
    free(file);
    return NULL;
  }
  /* The content size is optional in zstd frames (the console's own files
     declare it, streamed ones may not): without it, grow the buffer. */
  int64_t size = zstd_content_size(file, flen);
  if(size == 0 || (size > 0 && (uint64_t)size > MAX_DECODED)) {
    free(file);
    return NULL;
  }
  size_t cap = size > 0 ? (size_t)size : 16u << 20;
  for(;;) {
    uint8_t *out = malloc(cap);
    int64_t n = out ? zstd_decompress(file, flen, out, cap) : -1;
    if(n > 0 && (size < 0 || n == size)) {
      free(file);
      *len = (size_t)n;
      return out;
    }
    free(out);
    if(size > 0 || cap >= MAX_DECODED) break;
    cap *= 2;
  }
  free(file);
  return NULL;
}


static int
block_bytes(uint32_t dxgi, const char *fourcc) {
  switch(dxgi) {
  case 71: case 72: case 80: case 81: return 8;                     /* BC1, BC4 */
  case 74: case 75: case 77: case 78: case 83: case 84:            /* BC2, BC3, BC5 */
  case 95: case 96: case 98: case 99: return 16;                    /* BC6H, BC7 */
  }
  if(!strcmp(fourcc, "DXT1")) return 8;
  if(!strcmp(fourcc, "DXT3") || !strcmp(fourcc, "DXT5")) return 16;
  return 0;
}


static int
pixel_bytes(uint32_t dxgi) {
  switch(dxgi) {
  case 28: case 29: case 87: case 88: case 91: case 93: return 4;   /* RGBA8 / BGRA8 variants */
  }
  return 0;
}


const char *
texture_format_name(const texture_info_t *t) {
  switch(t->dxgi) {
  case 71: return "BC1";       case 72: return "BC1 sRGB";
  case 74: return "BC2";       case 75: return "BC2 sRGB";
  case 77: return "BC3";       case 78: return "BC3 sRGB";
  case 80: return "BC4";       case 83: return "BC5";
  case 95: return "BC6H";      case 96: return "BC6H signed";
  case 98: return "BC7";       case 99: return "BC7 sRGB";
  case 28: return "RGBA8";     case 29: return "RGBA8 sRGB";
  case 87: return "BGRA8";     case 91: return "BGRA8 sRGB";
  }
  return t->fourcc[0] ? t->fourcc : "unknown";
}


/* Parse a DDS header. 0 on success. */
int
texture_parse(const uint8_t *d, size_t len, texture_info_t *t) {
  memset(t, 0, sizeof(*t));
  if(len < 128 || memcmp(d, "DDS ", 4) || le32(d + 4) != 124 || le32(d + 76) != 32) return -1;
  t->height = le32(d + 12);
  t->width = le32(d + 16);
  t->mips = le32(d + 28) ? le32(d + 28) : 1;
  t->array = 1;
  uint32_t pf_flags = le32(d + 80);
  if(pf_flags & 4) memcpy(t->fourcc, d + 84, 4);
  t->header_len = 128;
  if(!strcmp(t->fourcc, "DX10")) {
    if(len < 148) return -1;
    t->dxgi = le32(d + 128);
    t->array = le32(d + 140) ? le32(d + 140) : 1;
    t->header_len = 148;
  }
  if(!t->width || !t->height || t->width > 16384 || t->height > 16384 || t->mips > 16 || t->array > 64) return -1;
  t->data_len = len - t->header_len;

  /* Expected payload for formats we know, summed over mips and array layers. */
  int bb = block_bytes(t->dxgi, t->fourcc), pb = pixel_bytes(t->dxgi);
  if(bb || pb) {
    uint64_t want = 0;
    for(uint32_t m = 0; m < t->mips; m++) {
      uint64_t w = t->width >> m ? t->width >> m : 1, h = t->height >> m ? t->height >> m : 1;
      want += bb ? ((w + 3) / 4) * ((h + 3) / 4) * bb : w * h * pb;
    }
    want *= t->array;
    if(want != t->data_len) return -1;
    t->known = 1;
  }
  return 0;
}


int
texture_info(const char *path, texture_info_t *t, uint8_t header[148]) {
  size_t len;
  int compressed;
  uint8_t *d = texture_load(path, &len, &compressed);
  if(!d) return -1;
  int rc = texture_parse(d, len, t);
  if(!rc && header) memcpy(header, d, t->header_len);
  free(d);
  return rc;
}


/* Policy check for KIND_TEXTURE. */
int
texture_validate(const char *cand, const char *stock, char *why, size_t len) {
  size_t sl, cl;
  int sz, cz;
  texture_info_t st, ct;

  uint8_t *s = texture_load(stock, &sl, &sz);
  if(!s || texture_parse(s, sl, &st)) {
    free(s);
    snprintf(why, len, "cannot read the system texture to compare against");
    return -1;
  }
  uint8_t *c = texture_load(cand, &cl, &cz);
  if(!c) {
    free(s);
    snprintf(why, len, sz ? "not a valid zstd-compressed texture" : "not a readable texture");
    return -1;
  }
  int bad = texture_parse(c, cl, &ct);
  if(cz != sz) {
    snprintf(why, len, sz ? "texture must be zstd-compressed like the system file" : "texture must not be compressed, like the system file");
    bad = 1;
  } else if(bad) {
    snprintf(why, len, "not a valid DDS texture");
  } else if(ct.header_len != st.header_len || memcmp(c, s, st.header_len)) {
    snprintf(why, len, "texture must be %ux%u %s with %u mip level(s), exactly like the system file (got %ux%u %s, %u)",
             st.width, st.height, texture_format_name(&st), st.mips,
             ct.width, ct.height, texture_format_name(&ct), ct.mips);
    bad = 1;
  } else if(cl != sl) {
    snprintf(why, len, "texture data is %zu bytes, expected %zu", cl, sl);
    bad = 1;
  }
  free(s);
  free(c);
  return bad ? -1 : 0;
}
