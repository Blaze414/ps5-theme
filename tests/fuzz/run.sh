#!/usr/bin/env bash
# Fuzz the plugin's parsers (zstd, DDS, ATRAC9, WAV, image headers, JSON, SVG,
# interface resource files, fonts)
# with libFuzzer, AddressSanitizer and UBSan.
#   tests/fuzz/run.sh                 every target for 60 s
#   FUZZ_SECONDS=600 tests/fuzz/run.sh
#   FUZZ_TARGETS="at9 wav" tests/fuzz/run.sh
# Inputs that crash or hang end up in tests/fuzz/out/.
set -euo pipefail
cd "$(dirname "$0")/../.."
docker build --platform linux/amd64 -f tests/fuzz/Dockerfile -t ps5-theme-fuzz .
mkdir -p tests/fuzz/out
exec docker run --rm --platform linux/amd64 -e FUZZ_SECONDS="${FUZZ_SECONDS:-60}" \
  -e FUZZ_TARGETS="${FUZZ_TARGETS:-zstd texture at9 wav image json svg png rco rco-data font}" \
  -v "$PWD/tests/fuzz/out:/out" ps5-theme-fuzz
