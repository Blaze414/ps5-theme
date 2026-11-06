#!/usr/bin/env bash
# The write guard against a folder swapped for a link while it writes (tests/fsguard/race.c), in the host test
# image, for SECONDS seconds (default 20):
#   tests/fsguard/run.sh
#   SECONDS_TO_RACE=60 tests/fsguard/run.sh
# With GUARD=<file>, it is built against that guard instead (e.g. the one before 1.88, to see the race it had).
set -euo pipefail
cd "$(dirname "$0")/../.."
docker build -q --platform linux/amd64 -f tests/host/Dockerfile --target full -t ps5-theme-host-test . >/dev/null
guard=${GUARD:-src/fsguard.c}
exec docker run --rm --platform linux/amd64 -v "$PWD:/w:ro" -e GUARD="$guard" -e SECONDS_TO_RACE="${SECONDS_TO_RACE:-20}" \
  ps5-theme-host-test sh -c 'cp -r /w /tmp/w && cd /tmp/w &&
    cc -std=gnu11 -D_GNU_SOURCE -O1 -g -Isrc -Wall -Wextra -Wno-unused-parameter -o /tmp/race tests/fsguard/race.c "$GUARD" -lpthread &&
    /tmp/race "$SECONDS_TO_RACE"'
