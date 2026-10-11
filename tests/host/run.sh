#!/usr/bin/env bash
# Build and run the end-to-end tests.
#   tests/host/run.sh            full suite in a privileged container (real mounts)
#   tests/host/run.sh emulated   suite inside a docker build step (mounts emulated)
#   tests/host/run.sh serve      plugin on http://localhost:18095 with a fake
#                                console, for trying the UI in a browser
set -euo pipefail
cd "$(dirname "$0")/../.."

case "${1:-}" in
  emulated)
    exec docker build --platform linux/amd64 -f tests/host/Dockerfile --target test-emulated \
      --build-arg CACHE_BUST="$(date +%s)" --progress=plain . ;;
  serve)
    docker build --platform linux/amd64 -f tests/host/Dockerfile --target full -t ps5-theme-host-test .
    exec docker run --rm --privileged --platform linux/amd64 -p 127.0.0.1:18095:8095 \
      ps5-theme-host-test python3 -u tests/host/test_e2e.py --serve ;;
  *)
    docker build --platform linux/amd64 -f tests/host/Dockerfile --target full -t ps5-theme-host-test .
    exec docker run --rm --privileged --platform linux/amd64 ps5-theme-host-test ;;
esac
