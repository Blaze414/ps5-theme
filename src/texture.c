/* DDS textures (.dds and zstd-compressed .dds.zst), as used by ShellUI.

   A replacement is accepted only if it decodes to a DDS whose header is
   byte-identical to the stock texture's (same format, size, mip levels, array
   size) and whose total length matches. For the common formats the data size
   implied by the header is checked as well. A compressed one must also be
   the console's own kind of zstd file: one frame that is the whole file,
   starting with exactly the system file's frame header (descriptor, window
   and content size), and a BC7 texture's blocks must each use a valid mode.
   A texture that is right except for its frame header (the pages before 1.56
   wrote one form for every texture) is not refused: its file is written
   again, in place, with the system file's own header (and checksum), the
   picture data untouched. */

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


/* The length of a zstd frame header: magic, descriptor, window descriptor
   (unless single-segment), dictionary id and content size. 0 if it isn't
   one or is cut short. */
static size_t
frame_header_len(const uint8_t *p, size_t n) {
  if(n < 6 || !is_zstd(p, n) || (p[4] & 8)) return 0;
  int single = (p[4] >> 5) & 1, fcs = p[4] >> 6, dict = p[4] & 3;
  size_t len = 5 + (single ? 0 : 1) + (dict == 3 ? 4 : (size_t)dict) + (fcs == 0 ? (size_t)single : fcs == 1 ? 2u : fcs == 2 ? 4u : 8u);
  return len <= n ? len : 0;
}


/* Where the zstd frame at the start of the file ends (walking its block
   headers), or 0 if it is damaged or cut short. */
static size_t
frame_end(const uint8_t *p, size_t n) {
  size_t o = frame_header_len(p, n);
  if(!o) return 0;
  int last;
  do {
    if(o + 3 > n) return 0;
    uint32_t h = p[o] | p[o + 1] << 8 | (uint32_t)p[o + 2] << 16;
    int type = (h >> 1) & 3;
    last = h & 1;
    if(type == 3) return 0;                       /* a reserved block type */
    o += 3 + (type == 1 ? 1 : (size_t)(h >> 3));  /* an RLE block holds one byte */
    if(o > n) return 0;
  } while(!last);
  if(p[4] & 4) o += 4;                            /* the content checksum */
  return o <= n ? o : 0;
}


/* A texture file's zstd frame, for comparing with the system file's: its
   header bytes, and whether the frame is the whole file. */
typedef struct {
  uint8_t head[16];
  size_t  head_len;
  int     whole;
} frame_t;

static void
frame_of(const uint8_t *file, size_t flen, frame_t *f) {
  memset(f, 0, sizeof(*f));
  f->head_len = frame_header_len(file, flen);
  if(f->head_len > sizeof(f->head)) f->head_len = 0;
  if(f->head_len) memcpy(f->head, file, f->head_len);
  f->whole = frame_end(file, flen) == flen;
}


/* The decoded bytes as one zstd frame headed exactly like `like`'s (the
   system file's header bytes, copied: the content size is the same), in
   stored blocks no larger than its window allows, and with the content
   checksum when its descriptor asks for one; written over `path`. 0 on
   success. A frame header with a dictionary is left alone. */
static int
reframe_like(const char *path, const uint8_t *data, size_t n, const frame_t *like) {
  if(!like->head_len || (like->head[4] & 3) || !n) return -1;
  uint8_t desc = like->head[4];
  size_t window = n;                                   /* single segment: the window is the content */
  if(!((desc >> 5) & 1)) {
    unsigned wb = like->head[5];
    size_t base = (size_t)1 << (10 + (wb >> 3));
    window = base + (base >> 3) * (wb & 7);
  }
  size_t block = window < 131072 ? window : 131072, blocks = (n + block - 1) / block;
  size_t total = like->head_len + blocks * 3 + n + ((desc & 4) ? 4 : 0);
  uint8_t *out = malloc(total);
  if(!out) return -1;
  memcpy(out, like->head, like->head_len);
  size_t o = like->head_len;
  for(size_t b = 0; b < blocks; b++) {
    size_t size = n - b * block < block ? n - b * block : block;
    uint32_t h = (uint32_t)(size << 3) | (b == blocks - 1);
    out[o] = h & 255;
    out[o + 1] = (h >> 8) & 255;
    out[o + 2] = (h >> 16) & 255;
    memcpy(out + o + 3, data + b * block, size);
    o += 3 + size;
  }
  if(desc & 4) {
    uint32_t sum = (uint32_t)xxh64(data, n);
    for(int i = 0; i < 4; i++) out[o + i] = (sum >> (8 * i)) & 255;
  }
  int rc = fs_write_file(path, out, total);
  free(out);
  return rc;
}


/* The frame header of a system texture's file, for the page to copy; 0 if the
   file isn't zstd-compressed. */
size_t
texture_frame_head(const char *path, uint8_t out[16]) {
  uint8_t head[32];
  int fd = open(path, O_RDONLY);
  ssize_t n = fd < 0 ? -1 : read(fd, head, sizeof(head));
  if(fd >= 0) close(fd);
  if(n < 6) return 0;
  size_t len = frame_header_len(head, (size_t)n);
  if(len > 16) return 0;
  memcpy(out, head, len);
  return len;
}


/* A texture file's decoded DDS bytes (caller frees), at most `max_decoded`
   of them: a file declaring more is refused before anything is allocated
   for it (*too_big, if given, says so). zstd is recognised by its magic
   bytes, not the file name (uploads arrive as .part files). *compressed
   tells which it was. */
static uint8_t *
load_decoded(const char *path, size_t max_decoded, size_t *len, int *compressed, int *too_big, frame_t *frame) {
  size_t flen;
  if(too_big) *too_big = 0;
  uint8_t *file = read_all(path, MAX_DECODED, &flen);
  if(!file) return NULL;
  *compressed = is_zstd(file, flen);
  if(frame) frame_of(file, flen, frame);
  if(!*compressed) {
    if(flen > max_decoded) {
      if(too_big) *too_big = 1;
      free(file);
      return NULL;
    }
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
  if(size == 0 || (size > 0 && (uint64_t)size > max_decoded)) {
    if(too_big && size > 0) *too_big = 1;
    free(file);
    return NULL;
  }
  size_t cap = size > 0 ? (size_t)size : (16u << 20 < max_decoded ? 16u << 20 : max_decoded);
  for(;;) {
    uint8_t *out = malloc(cap);
    int64_t n = out ? zstd_decompress(file, flen, out, cap) : -1;
    if(n > 0 && (size < 0 || n == size)) {
      free(file);
      *len = (size_t)n;
      return out;
    }
    free(out);
    if(size > 0 || cap >= max_decoded) break;
    cap = cap * 2 < max_decoded ? cap * 2 : max_decoded;
  }
  free(file);
  return NULL;
}


uint8_t *
texture_load(const char *path, size_t *len, int *compressed) {
  return load_decoded(path, MAX_DECODED, len, compressed, NULL, NULL);
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
texture_info(const char *path, texture_info_t *t, uint8_t header[148], int *zstd) {
  size_t len;
  int compressed;
  uint8_t *d = texture_load(path, &len, &compressed);
  if(!d) return -1;
  if(zstd) *zstd = compressed;
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

  frame_t sf = {0}, cf = {0};
  size_t sfl;
  uint8_t *sraw = read_all(stock, MAX_DECODED, &sfl);
  if(sraw) frame_of(sraw, sfl, &sf);
  free(sraw);
  uint8_t *s = texture_load(stock, &sl, &sz);
  if(!s || texture_parse(s, sl, &st)) {
    free(s);
    snprintf(why, len, "cannot read the system texture to compare against");
    return -1;
  }
  /* A replacement must decode to exactly the system texture's length, so
     no more than that is ever unpacked. */
  int too_big;
  uint8_t *c = load_decoded(cand, sl, &cl, &cz, &too_big, &cf);
  if(!c) {
    free(s);
    if(too_big) snprintf(why, len, "texture is larger than the system texture (%zu bytes unpacked)", sl);
    else snprintf(why, len, sz ? "not a valid zstd-compressed texture" : "not a readable texture");
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
  } else {
    /* BC7's first byte holds the block's mode as the position of its lowest
       set bit: zero is a reserved mode, which the console would draw as black. */
    if(st.dxgi == 98 || st.dxgi == 99) {
      for(size_t i = st.header_len, n = 0; i + 16 <= cl && !bad; i += 16, n++) {
        if(!c[i]) {
          snprintf(why, len, "block %zu of the texture uses a reserved BC7 mode", n);
          bad = 1;
        }
      }
    }
    /* A compressed texture is one frame headed like the system file's. Right in every
       other way but its header, it is written again with the system file's. */
    if(!bad && sz && !cf.whole) {
      snprintf(why, len, "the texture's zstd frame must be the whole file, with nothing after it");
      bad = 1;
    }
    if(!bad && sz && (cf.head_len != sf.head_len || memcmp(cf.head, sf.head, sf.head_len)) && reframe_like(cand, c, cl, &sf)) {
      snprintf(why, len, "the texture's zstd frame must start exactly like the system file's (same flags, window and size field)");
      bad = 1;
    }
  }
  free(s);
  free(c);
  return bad ? -1 : 0;
}
