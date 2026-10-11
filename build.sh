#!/usr/bin/env bash
# Build ps5-theme.plugin with Docker. Output: build/ps5-theme.plugin (and .elf).
set -euo pipefail
cd "$(dirname "$0")"
docker build --platform linux/amd64 --target artifact --output type=local,dest=build .
ls -l build
