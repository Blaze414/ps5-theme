/* libFuzzer harness for the plugin's own parsers: everything that reads a
   file a client uploaded. FUZZ_TARGET picks one:

     zstd     zstd_content_size + zstd_decompress (1 MB output cap)
     texture  texture_parse, then texture_validate against a stock .dds.zst
     at9      at9_validate against a stock ATRAC9 file
     wav      wav_probe, and the policy's audio check against a stock WAV
     image    image_probe (PNG, JPEG, WebP headers)
     json     the policy's JSON check against a stock JSON
     svg      the policy's SVG check
     rco      rco_validate against a stock resource file (interface pictures)
     png      png_check_against: a PNG against the console's own (a palette
              picture), which inflates and unfilters it
     font     font_check_mem on the bytes, and the policy's font check against a
              stock font (a replacement font for the console's text)
     rco-data the same, keeping the stock file's structure: the input gives
              the pointers into the file table (8 bytes each, in order) and
              then the file table itself, so the per-picture checks get
              exercised

   Stock files come from FUZZ_STOCK_DIR (see run.sh). Path-based checks get
   the input through a file in /dev/shm. Any crash, sanitizer report or leak
   is a finding. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "../../src/policy.c"   /* for the static checks */

static const char *target, *stock_dir;
static char cand[64];

/* rco-data: the stock resource file and where its file pointers are. */
static uint8_t *rco_stock;
static size_t rco_stock_len, rco_table_off, rco_slots[4096];
static int rco_nslots;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void
rco_prepare(const char *path) {
  FILE *f = fopen(path, "rb");
  if(!f) abort();
  fseek(f, 0, SEEK_END);
  rco_stock_len = ftell(f);
  fseek(f, 0, SEEK_SET);
  rco_stock = malloc(rco_stock_len);
  if(fread(rco_stock, 1, rco_stock_len, f) != rco_stock_len) abort();
  fclose(f);
  size_t tree = rd32(rco_stock + 8), tsize = rd32(rco_stock + 12);
  rco_table_off = rd32(rco_stock + 8 + 8 * 8);
  for(size_t at = 0; at < tsize;) {
    uint32_t nattr = rd32(rco_stock + tree + at + 4);
    for(uint32_t i = 0; i < nattr; i++) {
      size_t a = tree + at + 28 + 16 * i;
      if(rd32(rco_stock + a + 4) == 8 && rco_nslots < 4096) rco_slots[rco_nslots++] = a + 8;
    }
    at += 28 + 16 * (size_t)nattr;
  }
}

int
LLVMFuzzerInitialize(int *argc, char ***argv) {
  target = getenv("FUZZ_TARGET");
  stock_dir = getenv("FUZZ_STOCK_DIR");
  if(!target || !stock_dir) {
    fprintf(stderr, "set FUZZ_TARGET and FUZZ_STOCK_DIR\n");
    exit(2);
  }
  snprintf(cand, sizeof(cand), "/dev/shm/fuzz-%d", (int)getpid());
  if(!strcmp(target, "rco-data")) {
    char p[512];
    snprintf(p, sizeof(p), "%s/stock.rco", stock_dir);
    rco_prepare(p);
  }
  return 0;
}

static void
put(const uint8_t *d, size_t n) {
  int fd = open(cand, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(fd < 0 || (n && write(fd, d, n) != (ssize_t)n)) abort();
  close(fd);
}

static const char *
stock(const char *name) {
  static char p[512];
  snprintf(p, sizeof(p), "%s/%s", stock_dir, name);
  return p;
}

int
LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
  char why[256];

  if(!strcmp(target, "zstd")) {
    zstd_content_size(d, n);
    uint8_t *out = malloc(1 << 20);
    zstd_decompress(d, n, out, 1 << 20);
    free(out);
  } else if(!strcmp(target, "texture")) {
    texture_info_t t;
    texture_parse(d, n, &t);
    put(d, n);
    texture_validate(cand, stock("stock.dds.zst"), why, sizeof(why));
  } else if(!strcmp(target, "at9")) {
    put(d, n);
    at9_validate(cand, stock("stock.at9"), why, sizeof(why));
  } else if(!strcmp(target, "wav")) {
    wav_info_t w;
    put(d, n);
    wav_probe(cand, &w);
    check_audio(cand, stock("stock.wav"), why, sizeof(why));
    music_wav_ok(cand, &w, why, sizeof(why));
  } else if(!strcmp(target, "image")) {
    image_info_t i;
    put(d, n);
    image_probe(cand, &i);
  } else if(!strcmp(target, "json")) {
    put(d, n);
    check_json(cand, stock("stock.json"), why, sizeof(why));
  } else if(!strcmp(target, "svg")) {
    put(d, n);
    check_svg(cand, why, sizeof(why));
  } else if(!strcmp(target, "png")) {
    static uint8_t *stock_png;
    static size_t stock_png_len;
    if(!stock_png) {
      FILE *f = fopen(stock("stock-palette.png"), "rb");
      if(!f) abort();
      fseek(f, 0, SEEK_END);
      stock_png_len = ftell(f);
      fseek(f, 0, SEEK_SET);
      stock_png = malloc(stock_png_len);
      if(fread(stock_png, 1, stock_png_len, f) != stock_png_len) abort();
      fclose(f);
    }
    png_check_against(stock_png, stock_png_len, d, n, why, sizeof(why));
  } else if(!strcmp(target, "rco")) {
    put(d, n);
    rco_validate(cand, stock("stock.rco"), why, sizeof(why));
  } else if(!strcmp(target, "font")) {
    font_check_mem(d, n, why, sizeof(why));
    put(d, n);
    check_font(cand, stock("stock.ttf"), why, sizeof(why));
  } else if(!strcmp(target, "rco-data")) {
    size_t ptrs = (size_t)rco_nslots * 8;
    if(n < ptrs) return 0;
    uint8_t *b = malloc(rco_table_off + n - ptrs + 1);
    memcpy(b, rco_stock, rco_table_off);
    for(int i = 0; i < rco_nslots; i++) memcpy(b + rco_slots[i], d + 8 * i, 8);
    memcpy(b + rco_table_off, d + ptrs, n - ptrs);
    uint32_t size = (uint32_t)(n - ptrs);
    memcpy(b + 12 + 8 * 8, &size, 4);
    put(b, rco_table_off + n - ptrs);
    free(b);
    rco_validate(cand, stock("stock.rco"), why, sizeof(why));
  }
  return 0;
}
