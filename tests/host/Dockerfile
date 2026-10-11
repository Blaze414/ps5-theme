# End-to-end test rig: the real plugin code on Linux with a fake console tree.
#
# Full run (real read-only bind mounts, needs a privileged container):
#   tests/host/run.sh
# Unprivileged run inside a build step (mounts emulated, see platform_linux.c):
#   tests/host/run.sh emulated
FROM ubuntu:24.04 AS base

RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc libc6-dev make python3 zstd nodejs \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY src/ src/
COPY web/ web/
COPY assets/ assets/
COPY tests/host/ tests/host/
RUN make -f tests/host/Makefile

CMD ["python3", "-u", "tests/host/test_e2e.py"]


FROM base AS test-emulated
ARG CACHE_BUST=0
RUN EMULATE_MOUNTS=1 python3 -u tests/host/test_e2e.py


FROM base AS full
