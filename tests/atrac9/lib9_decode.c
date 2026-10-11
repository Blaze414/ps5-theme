/* Decode an .at9 with LibAtrac9 into interleaved 16-bit PCM (test reference). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libatrac9.h"

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int
main(int argc, char **argv) {
  if(argc != 3) return 2;
  FILE *f = fopen(argv[1], "rb");
  if(!f) return 1;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(len);
  if(!b || fread(b, 1, len, f) != (size_t)len) return 1;
  fclose(f);

  uint8_t cfg[4] = {0};
  long data = 0, dlen = 0;
  for(long p = 12; p + 8 <= len;) {
    uint32_t sz = le32(b + p + 4);
    if(!memcmp(b + p, "fmt ", 4)) memcpy(cfg, b + p + 8 + 44, 4);
    if(!memcmp(b + p, "data", 4)) { data = p + 8; dlen = sz; }
    p += 8 + sz + (sz & 1);
  }
  void *h = Atrac9GetHandle();
  if(Atrac9InitDecoder(h, cfg)) { fprintf(stderr, "init failed\n"); return 1; }
  Atrac9CodecInfo info;
  Atrac9GetCodecInfo(h, &info);
  FILE *o = fopen(argv[2], "wb");
  short pcm[256 * 8];
  for(long s = 0; s + info.superframeSize <= dlen; s += info.superframeSize) {
    long pos = data + s;
    for(int fr = 0; fr < info.framesInSuperframe; fr++) {
      int used = 0, rc = Atrac9Decode(h, b + pos, pcm, &used);
      if(rc) { fprintf(stderr, "decode error %d in superframe %ld\n", rc, s / info.superframeSize); return 1; }
      pos += used;
      fwrite(pcm, 2, info.frameSamples * info.channels, o);
    }
  }
  fclose(o);
  free(b);
  Atrac9ReleaseHandle(h);
  return 0;
}
