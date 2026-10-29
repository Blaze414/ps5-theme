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


/* ---- A small picture of a console texture, for the manager's previews ----

   The preview draws the console's own artwork (its home backgrounds, the Appearance themes' pieces) where a theme
   leaves it, so what it shows is what the console shows. The browser can't read them: they are zstd-compressed BC7
   using all eight of its modes. They are decoded here, read-only, to one pixel per 4x4 block (each the block's
   average, weighted by how opaque its pixels are): a 4K background becomes 960x540. BC7 as Microsoft specifies it;
   the tables are the specification's, checked against Pillow's decoder on the console's own files. */

#define PREVIEW_MAX_PIXELS (1u << 20)   /* a quarter-size picture of up to 4096x4096 */

static const uint16_t bc7_part2[64] = {
  0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec, 0xfe80, 0xe800, 0xffe8, 0xff00,
  0xfff0, 0xf000, 0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce, 0x088c, 0x3110, 0x6666, 0x366c,
  0x17e8, 0x0ff0, 0x718e, 0x399c, 0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696, 0xa55a, 0x73ce, 0x13c8,
  0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660, 0x0272, 0x04e4, 0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6, 0x639c,
  0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22,
};
static const uint32_t bc7_part3[64] = {
  0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050, 0x5555a0a0, 0x5a5a5050, 0xaa550000,
  0xaa555500, 0xaaaa5500, 0x90909090, 0x94949494, 0xa4a4a4a4, 0xa9a59450, 0x2a0a4250, 0xa5945040, 0x0a425054,
  0xa5a5a500, 0x55a0a0a0, 0xa8a85454, 0x6a6a4040, 0xa4a45000, 0x1a1a0500, 0x0050a4a4, 0xaaa59090, 0x14696914,
  0x69691400, 0xa08585a0, 0xaa821414, 0x50a4a450, 0x6a5a0200, 0xa9a58000, 0x5090a0a8, 0xa8a09050, 0x24242424,
  0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50, 0x500aa550, 0xaaaa4444, 0x66660000, 0xa5a0a5a0, 0x50a050a0,
  0x69286928, 0x44aaaa44, 0x66666600, 0xaa444444, 0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580,
  0xaa141414, 0x96960000, 0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000, 0x40804080, 0xa9a8a9a8, 0xaaaaaa44,
  0x2a4a5254,
};
static const uint8_t bc7_anchor2[64] = {
  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
  15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
};
static const uint8_t bc7_anchor3a[64] = {
  3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
  8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
};
static const uint8_t bc7_anchor3b[64] = {
  15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
  15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
};
/* Per mode: subsets, partition bits, rotation bits, index-selection bits, colour bits, alpha bits, a p-bit per
   endpoint, a p-bit per subset, index bits, second index bits. */
static const uint8_t bc7_modes[8][10] = {
  {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
  {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
  {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};
static const uint8_t bc7_w2[4] = {0, 21, 43, 64}, bc7_w3[8] = {0, 9, 18, 27, 37, 46, 55, 64},
                     bc7_w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

static unsigned
bits_at(const uint8_t *b, unsigned *pos, unsigned n) {
  unsigned v = 0;
  for(unsigned i = 0; i < n && *pos < 128; i++, (*pos)++) v |= (unsigned)((b[*pos >> 3] >> (*pos & 7)) & 1) << i;
  return v;
}

static uint8_t
bc7_mix(unsigned e0, unsigned e1, unsigned bits, unsigned idx) {
  unsigned w = bits == 2 ? bc7_w2[idx & 3] : bits == 3 ? bc7_w3[idx & 7] : bc7_w4[idx & 15];
  return (uint8_t)(((64 - w) * e0 + w * e1 + 32) >> 6);
}

/* One 16-byte BC7 block to 16 RGBA pixels. A reserved mode decodes to clear black, as the specification says. */
static void
bc7_block(const uint8_t *b, uint8_t px[64]) {
  unsigned mode = 0, pos = 0;
  while(mode < 8 && !((b[0] >> mode) & 1)) mode++;
  if(mode == 8) { memset(px, 0, 64); return; }
  const uint8_t *m = bc7_modes[mode];
  unsigned ns = m[0], cb = m[4], ab = m[5], ib = m[8], ib2 = m[9];
  pos = mode + 1;
  unsigned part = bits_at(b, &pos, m[1]), rot = bits_at(b, &pos, m[2]), sel = bits_at(b, &pos, m[3]);
  unsigned ep[6][4];   /* endpoints (two per subset), RGBA */
  for(unsigned c = 0; c < 3; c++) for(unsigned e = 0; e < ns * 2; e++) ep[e][c] = bits_at(b, &pos, cb);
  for(unsigned e = 0; e < ns * 2; e++) ep[e][3] = ab ? bits_at(b, &pos, ab) : 255;
  unsigned cbits = cb, abits = ab;
  if(m[6] || m[7]) {
    unsigned pb[6];
    for(unsigned i = 0; i < (m[6] ? ns * 2 : ns); i++) pb[i] = bits_at(b, &pos, 1);
    for(unsigned e = 0; e < ns * 2; e++) {
      unsigned p = m[6] ? pb[e] : pb[e >> 1];
      for(unsigned c = 0; c < 3; c++) ep[e][c] = ep[e][c] << 1 | p;
      if(ab) ep[e][3] = ep[e][3] << 1 | p;
    }
    cbits++;
    if(ab) abits++;
  }
  for(unsigned e = 0; e < ns * 2; e++) {
    for(unsigned c = 0; c < 3; c++) ep[e][c] = (ep[e][c] << (8 - cbits)) | (ep[e][c] >> (2 * cbits - 8));
    if(ab) ep[e][3] = (ep[e][3] << (8 - abits)) | (ep[e][3] >> (2 * abits - 8));
  }
  unsigned subset[16], a1 = 0, a2 = 0;
  for(unsigned i = 0; i < 16; i++) {
    subset[i] = ns == 1 ? 0 : ns == 2 ? (bc7_part2[part] >> i) & 1 : (bc7_part3[part] >> (2 * i)) & 3;
  }
  if(ns == 2) a1 = bc7_anchor2[part];
  if(ns == 3) { a1 = bc7_anchor3a[part]; a2 = bc7_anchor3b[part]; }
  unsigned idx[16], idx2[16];
  for(unsigned i = 0; i < 16; i++) {
    int anchor = i == 0 || (ns > 1 && i == a1) || (ns > 2 && i == a2);
    idx[i] = bits_at(b, &pos, anchor ? ib - 1 : ib);
  }
  for(unsigned i = 0; i < 16; i++) idx2[i] = ib2 ? bits_at(b, &pos, i == 0 ? ib2 - 1 : ib2) : 0;
  for(unsigned i = 0; i < 16; i++) {
    const unsigned *e0 = ep[subset[i] * 2], *e1 = ep[subset[i] * 2 + 1];
    unsigned ci = idx[i], cbn = ib, ai = idx[i], abn = ib;
    if(ib2) {
      if(sel) { ci = idx2[i]; cbn = ib2; ai = idx[i]; abn = ib; }
      else    { ci = idx[i]; cbn = ib; ai = idx2[i]; abn = ib2; }
    }
    uint8_t *p = px + i * 4;
    for(unsigned c = 0; c < 3; c++) p[c] = bc7_mix(e0[c], e1[c], cbn, ci);
    p[3] = ab ? bc7_mix(e0[3], e1[3], abn, ai) : 255;
    if(rot) { uint8_t t = p[3]; p[3] = p[rot - 1]; p[rot - 1] = t; }
  }
}

/* One 8-byte BC1 block to 16 RGBA pixels. */
static void
bc1_block(const uint8_t *b, uint8_t px[64]) {
  unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8, pal[4][4];
  for(int k = 0; k < 2; k++) {
    unsigned c = k ? c1 : c0;
    pal[k][0] = ((c >> 11) & 31) * 255 / 31; pal[k][1] = ((c >> 5) & 63) * 255 / 63; pal[k][2] = (c & 31) * 255 / 31; pal[k][3] = 255;
  }
  for(int c = 0; c < 3; c++) {
    if(c0 > c1) { pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3; pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3; }
    else        { pal[2][c] = (pal[0][c] + pal[1][c]) / 2; pal[3][c] = 0; }
  }
  pal[2][3] = 255;
  pal[3][3] = c0 > c1 ? 255 : 0;
  uint32_t bits = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
  for(int i = 0; i < 16; i++) for(int c = 0; c < 4; c++) px[i * 4 + c] = (uint8_t)pal[(bits >> (2 * i)) & 3][c];
}

int
texture_preview_mem(const uint8_t *d, size_t len, uint8_t **rgba, uint32_t *ow, uint32_t *oh) {
  texture_info_t t;
  *rgba = NULL;
  if(texture_parse(d, len, &t) || !t.known) return -1;
  int bc7 = t.dxgi == 98 || t.dxgi == 99, bc1 = t.dxgi == 71 || t.dxgi == 72 || !strcmp(t.fourcc, "DXT1");
  int pb = pixel_bytes(t.dxgi), bgra = t.dxgi == 87 || t.dxgi == 88 || t.dxgi == 91 || t.dxgi == 93;
  if(!bc7 && !bc1 && pb != 4) return -1;
  uint32_t bw = (t.width + 3) / 4, bh = (t.height + 3) / 4;
  if((uint64_t)bw * bh > PREVIEW_MAX_PIXELS) return -1;
  uint8_t *out = malloc((size_t)bw * bh * 4);
  if(!out) return -1;
  const uint8_t *data = d + t.header_len;   /* the first mip of the first layer */
  uint8_t px[64];
  for(uint32_t by = 0; by < bh; by++) {
    for(uint32_t bx = 0; bx < bw; bx++) {
      if(bc7) bc7_block(data + ((size_t)by * bw + bx) * 16, px);
      else if(bc1) bc1_block(data + ((size_t)by * bw + bx) * 8, px);
      else {
        for(int i = 0; i < 16; i++) {
          uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
          if(x >= t.width) x = t.width - 1;
          if(y >= t.height) y = t.height - 1;
          const uint8_t *s = data + ((size_t)y * t.width + x) * 4;
          px[i * 4] = s[bgra ? 2 : 0]; px[i * 4 + 1] = s[1]; px[i * 4 + 2] = s[bgra ? 0 : 2];
          px[i * 4 + 3] = t.dxgi == 87 || t.dxgi == 91 || pb != 4 ? 255 : s[3];
        }
      }
      /* the block's average, its colour weighted by how opaque each pixel is, and only the pixels inside the picture */
      uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
      for(int i = 0; i < 16; i++) {
        if(bx * 4 + (i & 3) >= t.width || by * 4 + (i >> 2) >= t.height) continue;
        const uint8_t *p = px + i * 4;
        r += p[0] * p[3]; g += p[1] * p[3]; b += p[2] * p[3]; a += p[3]; n++;
      }
      uint8_t *o = out + ((size_t)by * bw + bx) * 4;
      o[0] = a ? (uint8_t)(r / a) : 0; o[1] = a ? (uint8_t)(g / a) : 0; o[2] = a ? (uint8_t)(b / a) : 0;
      o[3] = n ? (uint8_t)(a / n) : 0;
    }
  }
  *rgba = out;
  *ow = bw;
  *oh = bh;
  return 0;
}

int
texture_preview(const char *path, uint8_t **rgba, uint32_t *w, uint32_t *h) {
  size_t len;
  int compressed;
  uint8_t *d = texture_load(path, &len, &compressed);
  if(!d) return -1;
  int rc = texture_preview_mem(d, len, rgba, w, h);
  free(d);
  return rc;
}


/* ---- Writing a texture like the console's, for icons the plugin makes by itself ----

   The same encoders as the manager's page (an opaque BC7 mode-6 block, a BC1 block), so an icon made here is what
   the page would have made: the endpoints from the block's principal colour line, each pixel the nearest palette
   colour. Every mip level of the console's texture is written, each a 2x2 average of the one above. */

/* No maths library on the console: the two needed here, rounding as the page's Math.round does (halves up). */
static double dabs(double v) { return v < 0 ? -v : v; }
static long round_half_up(double v) { long r = (long)(v + 0.5); if((double)r > v + 0.5) r--; return r; }

static void
block_line(const uint8_t px[64], double lo[3], double hi[3]) {
  double m[3] = {0, 0, 0}, cov[6] = {0, 0, 0, 0, 0, 0}, a[3] = {0.58, 0.58, 0.58};
  for(int i = 0; i < 16; i++) for(int c = 0; c < 3; c++) m[c] += px[i * 4 + c] / 16.0;
  for(int i = 0; i < 16; i++) {
    double r = px[i * 4] - m[0], g = px[i * 4 + 1] - m[1], b = px[i * 4 + 2] - m[2];
    cov[0] += r * r; cov[1] += r * g; cov[2] += r * b; cov[3] += g * g; cov[4] += g * b; cov[5] += b * b;
  }
  for(int k = 0; k < 8; k++) {
    double nx = cov[0] * a[0] + cov[1] * a[1] + cov[2] * a[2], ny = cov[1] * a[0] + cov[3] * a[1] + cov[4] * a[2];
    double nz = cov[2] * a[0] + cov[4] * a[1] + cov[5] * a[2];
    double mx = dabs(nx) > dabs(ny) ? dabs(nx) : dabs(ny);
    if(dabs(nz) > mx) mx = dabs(nz);
    if(mx < 1e-9) break;
    a[0] = nx / mx; a[1] = ny / mx; a[2] = nz / mx;
  }
  double len2 = a[0] * a[0] + a[1] * a[1] + a[2] * a[2], tmin = 0, tmax = 0;
  if(len2 > 1e-12) {
    tmin = 1e30; tmax = -1e30;
    for(int i = 0; i < 16; i++) {
      double t = ((px[i * 4] - m[0]) * a[0] + (px[i * 4 + 1] - m[1]) * a[1] + (px[i * 4 + 2] - m[2]) * a[2]) / len2;
      if(t < tmin) tmin = t;
      if(t > tmax) tmax = t;
    }
  }
  for(int c = 0; c < 3; c++) { lo[c] = m[c] + tmin * a[c]; hi[c] = m[c] + tmax * a[c]; }
}

static int clampi(long v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : (int)v; }

static void
put_bits(uint8_t *out, unsigned *bit, unsigned v, unsigned n) {
  for(unsigned i = 0; i < n; i++, (*bit)++) if((v >> i) & 1) out[*bit >> 3] |= (uint8_t)(1 << (*bit & 7));
}

/* One opaque 4x4 block as BC7 mode 6 (7-bit endpoints, p-bit 1 so alpha stays 255, 4-bit indices). */
static void
bc7_encode_block(const uint8_t px[64], uint8_t out[16]) {
  double lo[3], hi[3];
  block_line(px, lo, hi);
  int e0[3], e1[3], pal[16][3], idx[16];
  for(int c = 0; c < 3; c++) {
    e0[c] = clampi(round_half_up((lo[c] - 1) / 2), 0, 127);
    e1[c] = clampi(round_half_up((hi[c] - 1) / 2), 0, 127);
  }
  for(int i = 0; i < 16; i++) for(int c = 0; c < 3; c++) pal[i][c] = ((64 - bc7_w4[i]) * ((e0[c] << 1) | 1) + bc7_w4[i] * ((e1[c] << 1) | 1) + 32) >> 6;
  for(int p = 0; p < 16; p++) {
    int best = 0;
    long bestErr = -1;
    for(int i = 0; i < 16; i++) {
      long dr = px[p * 4] - pal[i][0], dg = px[p * 4 + 1] - pal[i][1], db = px[p * 4 + 2] - pal[i][2];
      long e = dr * dr + dg * dg + db * db;
      if(bestErr < 0 || e < bestErr) { bestErr = e; best = i; }
    }
    idx[p] = best;
  }
  if(idx[0] >= 8) {   /* the anchor index must fit in 3 bits */
    for(int c = 0; c < 3; c++) { int t = e0[c]; e0[c] = e1[c]; e1[c] = t; }
    for(int p = 0; p < 16; p++) idx[p] = 15 - idx[p];
  }
  unsigned bit = 0;
  memset(out, 0, 16);
  put_bits(out, &bit, 64, 7);
  for(int c = 0; c < 3; c++) { put_bits(out, &bit, (unsigned)e0[c], 7); put_bits(out, &bit, (unsigned)e1[c], 7); }
  put_bits(out, &bit, 127, 7); put_bits(out, &bit, 127, 7);
  put_bits(out, &bit, 1, 1); put_bits(out, &bit, 1, 1);
  put_bits(out, &bit, (unsigned)idx[0], 3);
  for(int p = 1; p < 16; p++) put_bits(out, &bit, (unsigned)idx[p], 4);
}

/* One opaque 4x4 block as BC1: endpoints larger first, so the block has four opaque colours. */
static void
bc1_encode_block(const uint8_t px[64], uint8_t out[8]) {
  double lo[3], hi[3];
  block_line(px, lo, hi);
  #define Q(v, n) clampi(round_half_up((v) * (n) / 255.0), 0, (n))
  unsigned c0 = (unsigned)(Q(hi[0], 31) << 11 | Q(hi[1], 63) << 5 | Q(hi[2], 31));
  unsigned c1 = (unsigned)(Q(lo[0], 31) << 11 | Q(lo[1], 63) << 5 | Q(lo[2], 31));
  #undef Q
  if(c0 < c1) { unsigned t = c0; c0 = c1; c1 = t; }
  uint32_t bits = 0;
  if(c0 != c1) {
    int pal[4][3];
    unsigned cs[2] = {c0, c1};
    for(int k = 0; k < 2; k++) {
      unsigned c = cs[k];
      pal[k][0] = (int)((c >> 11) << 3 | c >> 13); pal[k][1] = (int)(((c >> 5) & 63) << 2 | ((c >> 9) & 3)); pal[k][2] = (int)((c & 31) << 3 | ((c >> 2) & 7));
    }
    for(int c = 0; c < 3; c++) { pal[2][c] = (int)round_half_up((2.0 * pal[0][c] + pal[1][c]) / 3); pal[3][c] = (int)round_half_up((pal[0][c] + 2.0 * pal[1][c]) / 3); }
    for(int p = 0; p < 16; p++) {
      int best = 0;
      long bestErr = -1;
      for(int i = 0; i < 4; i++) {
        long dr = px[p * 4] - pal[i][0], dg = px[p * 4 + 1] - pal[i][1], db = px[p * 4 + 2] - pal[i][2];
        long e = dr * dr + dg * dg + db * db;
        if(bestErr < 0 || e < bestErr) { bestErr = e; best = i; }
      }
      bits |= (uint32_t)best << (2 * p);
    }
  }
  out[0] = (uint8_t)c0; out[1] = (uint8_t)(c0 >> 8); out[2] = (uint8_t)c1; out[3] = (uint8_t)(c1 >> 8);
  for(int i = 0; i < 4; i++) out[4 + i] = (uint8_t)(bits >> (8 * i));
}

uint8_t *
texture_encode_like(const uint8_t *stock, size_t sl, const uint8_t *rgba, size_t *outlen) {
  texture_info_t t;
  *outlen = 0;
  if(texture_parse(stock, sl, &t) || !t.known || t.array != 1) return NULL;
  int bc7 = t.dxgi == 98 || t.dxgi == 99, bc1 = t.dxgi == 71 || t.dxgi == 72 || !strcmp(t.fourcc, "DXT1");
  if(!bc7 && !bc1) return NULL;
  size_t total = t.header_len + (size_t)t.data_len;
  uint8_t *out = malloc(total), *level = malloc((size_t)t.width * t.height * 4);
  if(!out || !level) { free(out); free(level); return NULL; }
  memcpy(out, stock, t.header_len);
  memcpy(level, rgba, (size_t)t.width * t.height * 4);
  size_t pos = t.header_len, bb = bc7 ? 16 : 8;
  uint32_t w = t.width, h = t.height;
  for(uint32_t m = 0; m < t.mips; m++) {
    uint8_t px[64];
    for(uint32_t by = 0; by < (h + 3) / 4; by++) {
      for(uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
        for(int i = 0; i < 16; i++) {
          uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
          if(x >= w) x = w - 1;
          if(y >= h) y = h - 1;
          memcpy(px + i * 4, level + ((size_t)y * w + x) * 4, 4);
        }
        if(bc7) bc7_encode_block(px, out + pos); else bc1_encode_block(px, out + pos);
        pos += bb;
      }
    }
    if(m + 1 < t.mips) {   /* the next level: each pixel the average of up to four */
      uint32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
      for(uint32_t y = 0; y < nh; y++) for(uint32_t x = 0; x < nw; x++) for(int c = 0; c < 4; c++) {
        uint32_t x0 = x * 2 < w ? x * 2 : w - 1, y0 = y * 2 < h ? y * 2 : h - 1;
        uint32_t x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
        level[((size_t)y * nw + x) * 4 + c] = (uint8_t)((level[((size_t)y0 * w + x0) * 4 + c] + level[((size_t)y0 * w + x1) * 4 + c] +
                                                         level[((size_t)y1 * w + x0) * 4 + c] + level[((size_t)y1 * w + x1) * 4 + c] + 2) / 4);
      }
      w = nw; h = nh;
    }
  }
  free(level);
  if(pos != total) { free(out); return NULL; }
  *outlen = total;
  return out;
}
