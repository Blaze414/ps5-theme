/* Decompress argv[1] with the plugin's zstd decoder and compare with argv[2]. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int64_t zstd_decompress(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);
int64_t zstd_content_size(const uint8_t *src, size_t len);

static uint8_t *
slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if(!f) { perror(path); exit(2); }
  fseek(f, 0, SEEK_END);
  *len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(*len + 1);
  if(fread(b, 1, *len, f) != *len) exit(2);
  fclose(f);
  return b;
}

int
main(int argc, char **argv) {
  size_t clen, olen;
  uint8_t *c = slurp(argv[1], &clen), *o = slurp(argv[2], &olen);
  uint8_t *out = malloc(olen + 64);
  int64_t n = zstd_decompress(c, clen, out, olen + 64);
  if(n != (int64_t)olen || memcmp(out, o, olen)) {
    printf("FAIL %s (got %lld, want %zu)\n", argv[1], (long long)n, olen);
    fflush(stdout);
    return 1;
  }
  if(argc > 3) {  /* corruption must be rejected, never crash */
    for(size_t i = 0; i < 300; i++) {
      size_t at = (size_t)rand() % clen;
      c[at] ^= (uint8_t)(1 + rand() % 255);
      zstd_decompress(c, clen, out, olen + 64);
      zstd_decompress(c, at, out, olen + 64);  /* truncated */
      c[at] ^= 0;
    }
  }
  printf("ok   %s (%zu -> %zu)\n", argv[1], clen, olen);
  fflush(stdout);
  free(c);
  free(o);
  free(out);
  return 0;
}
