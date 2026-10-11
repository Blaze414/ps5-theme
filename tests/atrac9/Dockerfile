# ATRAC9 codec cross-check: web/atrac9.js against two independent decoders,
# LibAtrac9 (pinned commit) and FFmpeg. See run.sh.
FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates git gcc libc6-dev make ffmpeg nodejs python3 python3-numpy \
 && rm -rf /var/lib/apt/lists/*
RUN git clone https://github.com/Thealexbarney/LibAtrac9.git /opt/LibAtrac9 \
 && git -C /opt/LibAtrac9 checkout --quiet efca2e3af35562a09a9bb6deed90e45b4b824dc4
WORKDIR /src
COPY src/ src/
COPY web/ web/
COPY tests/atrac9/ tests/atrac9/
COPY tests/host/at9_fixture.js tests/host/at9_fixture.js
RUN gcc -O2 -I/opt/LibAtrac9/C/src -o /usr/local/bin/lib9-decode tests/atrac9/lib9_decode.c /opt/LibAtrac9/C/src/*.c -lm \
 && gcc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc \
        -o /usr/local/bin/at9-validate tests/atrac9/validate.c src/atrac9.c -lpthread
CMD ["sh", "tests/atrac9/check.sh"]
