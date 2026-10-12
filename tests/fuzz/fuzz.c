/* libFuzzer harness for the plugin's own parsers: everything that reads a
   file a client uploaded. FUZZ_TARGET picks one:

     zstd     zstd_content_size + zstd_decompress (1 MB output cap)
     texture  texture_parse, then texture_validate against a stock .dds.zst
     at9      at9_validate against a stock ATRAC9 file
     wav      wav_probe, and the policy's audio check against a stock WAV
     image    image_probe (PNG, JPEG, WebP headers)
     json     the policy's JSON check against a stock JSON
     svg      the policy's SVG check

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

int
LLVMFuzzerInitialize(int *argc, char ***argv) {
  target = getenv("FUZZ_TARGET");
  stock_dir = getenv("FUZZ_STOCK_DIR");
  if(!target || !stock_dir) {
    fprintf(stderr, "set FUZZ_TARGET and FUZZ_STOCK_DIR\n");
    exit(2);
  }
  snprintf(cand, sizeof(cand), "/dev/shm/fuzz-%d", (int)getpid());
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
  }
  return 0;
}
