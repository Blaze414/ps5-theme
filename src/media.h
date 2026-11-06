/* The file formats a theme may hold, read and checked without trusting them: pictures (verify.c, png.c,
   inflate.c, svg.c), the interface's resource files (rco.c), fonts (font.c), ATRAC9 sound (atrac9.c), textures
   (texture.c, zstd.c). Each check is 0 if the candidate may replace the console's own file, else -1 and a plain
   reason in `why`. */
#ifndef PS5_THEME_MEDIA_H
#define PS5_THEME_MEDIA_H

#include <stddef.h>
#include <stdint.h>

/* verify.c: a picture's format and size. */
enum { IMG_NONE = 0, IMG_PNG, IMG_JPEG, IMG_WEBP };
typedef struct image_info {
  int      format;
  uint32_t width, height;
} image_info_t;
int         image_format_for_path(const char *path);
const char *image_format_name(int format);
int         image_probe(const char *path, image_info_t *out);
int         image_probe_mem(const uint8_t *data, size_t len, image_info_t *out);

/* svg.c: the subset of SVG the console's own pictures use. */
int         svg_check_mem(const uint8_t *data, size_t len, char *why, size_t why_len);

/* inflate.c, png.c: a picture's pixels as RGBA (8-bit RGB or RGBA, not interlaced), and pixels of the stock's
   size written back as a PNG of exactly the stock's kind (its chunks, its zlib header). At most 2048 pixels a
   side. */
long        inflate_raw(const uint8_t *src, size_t len, size_t *used, uint8_t *dst, size_t cap, size_t window);
uint8_t    *png_decode_rgba(const uint8_t *png, size_t len, uint32_t *w, uint32_t *h);
uint8_t    *png_encode_like(const uint8_t *stock, size_t stock_len, const uint8_t *rgba, size_t *out_len);
int         png_check_against(const uint8_t *stock, size_t stock_len, const uint8_t *cand, size_t cand_len,
                              char *why, size_t why_len);

/* rco.c: ShellUI's resource files; only their pictures, sounds, colours and shapes may change. */
int         rco_validate(const char *candidate, const char *stock, char *why, size_t len);
/* One of the console's interface icons (an iconid_* picture) from the interface file at `path`: 0 with *out
   (malloc'd, the caller frees) and *len, or -1 when there's no such icon. */
int         rco_icon(const char *path, const char *id, uint8_t **out, size_t *len);

/* font.c: a whole TrueType or OpenType font with the characters text needs. */
int         font_check_mem(const uint8_t *buf, size_t n, char *why, size_t len);

/* atrac9.c: ATRAC9 (.at9) format and bitstream. */
int         at9_validate(const char *candidate, const char *stock, char *why, size_t len);

/* texture.c: DDS textures (.dds, .dds.zst). */
typedef struct texture_info {
  uint32_t width, height, mips, array, dxgi;
  char     fourcc[5];
  uint32_t header_len;
  uint64_t data_len;
  int      known;            /* data size checked against the format */
} texture_info_t;
uint8_t    *texture_load(const char *path, size_t *len, int *compressed);
int         texture_parse(const uint8_t *d, size_t len, texture_info_t *t);
/* The texture's format and DDS header; *zstd (if given) says whether the file holds zstd-compressed data, found by
   its content (the console keeps some compressed textures under a plain .dds name). */
int         texture_info(const char *path, texture_info_t *t, uint8_t header[148], int *zstd);
int         texture_validate(const char *candidate, const char *stock, char *why, size_t len);
/* The frame header of a system texture's zstd file (up to 16 bytes), for the page to copy exactly; 0 if the file
   isn't zstd-compressed. */
size_t      texture_frame_head(const char *path, uint8_t out[16]);
const char *texture_format_name(const texture_info_t *t);
/* A quarter-size picture of a texture (one RGBA pixel per 4x4 block, its opacity-weighted average), for the
   manager's previews: BC7, BC1 and 8-bit RGBA/BGRA. 0 with *rgba malloc'd (the caller frees). */
int         texture_preview(const char *path, uint8_t **rgba, uint32_t *w, uint32_t *h);
int         texture_preview_mem(const uint8_t *d, size_t len, uint8_t **rgba, uint32_t *w, uint32_t *h);
/* An opaque picture of the stock texture's size written as that texture (its header, its format, every mip):
   BC7 or BC1, uncompressed. NULL for anything else. */
uint8_t    *texture_encode_like(const uint8_t *stock, size_t stock_len, const uint8_t *rgba, size_t *out_len);

/* zstd.c: Zstandard decompression, to read and verify .dds.zst textures. */
int64_t     zstd_content_size(const uint8_t *src, size_t len);
int64_t     zstd_decompress(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);
uint64_t    xxh64(const void *data, size_t len);

#endif
