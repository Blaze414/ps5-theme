#!/usr/bin/env bash
# Cross-check the ATRAC9 encoder and decoder (web/atrac9.js) and the plugin's
# validator (src/atrac9.c):
#   tests/atrac9/run.sh                 synthetic signals
#   tests/atrac9/run.sh path/to.at9     also a console file, such as bgm_home.at9
#                                       (read locally, never copied into the image)
set -euo pipefail
cd "$(dirname "$0")/../.."
docker build --platform linux/amd64 -f tests/atrac9/Dockerfile -t ps5-theme-atrac9-test .
if [ $# -gt 0 ]; then
  exec docker run --rm --platform linux/amd64 -v "$(cd "$(dirname "$1")" && pwd)/$(basename "$1"):/console.at9:ro" ps5-theme-atrac9-test
fi
exec docker run --rm --platform linux/amd64 ps5-theme-atrac9-test
