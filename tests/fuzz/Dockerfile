# Fuzzing the plugin's parsers with libFuzzer (see run.sh).
FROM ubuntu:24.04

RUN apt-get update \
 && apt-get install -y --no-install-recommends clang libclang-rt-18-dev libc6-dev python3 zstd nodejs \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY src/ src/
COPY web/ web/
COPY tests/host/at9_fixture.js tests/host/at9_fixture.js
COPY tests/fuzz/ tests/fuzz/
RUN clang -std=gnu11 -D_GNU_SOURCE -g -O1 -fno-omit-frame-pointer \
      -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -Isrc -Wno-unused-function \
      -o /fuzz tests/fuzz/fuzz.c src/verify.c src/texture.c src/zstd.c src/atrac9.c src/fsguard.c src/log.c \
      tests/fuzz/stubs.c -lpthread -lm \
 && python3 tests/fuzz/seeds.py /stock /corpus

CMD ["sh", "tests/fuzz/fuzz.sh"]
