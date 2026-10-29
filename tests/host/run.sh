#!/usr/bin/env bash
# Build and run the end-to-end tests.
#   tests/host/run.sh            full suite in a privileged container (real mounts)
#   tests/host/run.sh emulated   suite inside a docker build step (mounts emulated)
#   tests/host/run.sh serve      plugin on http://localhost:18095 with a fake
#                                console, for trying the UI in a browser
#   tests/host/run.sh tsan       the full suite against a ThreadSanitizer build,
#                                which stops at the first data race between threads
set -euo pipefail
cd "$(dirname "$0")/../.."

case "${1:-}" in
  emulated)
    exec docker build --platform linux/amd64 -f tests/host/Dockerfile --target test-emulated \
      --build-arg CACHE_BUST="$(date +%s)" --progress=plain . ;;
  tsan)
    # ThreadSanitizer needs the machine's own architecture (it can't run under x86-64 emulation on an ARM Mac)
    arch=$(uname -m); case "$arch" in x86_64) arch=amd64 ;; aarch64|arm64) arch=arm64 ;; esac
    docker build --platform "linux/$arch" -f tests/host/Dockerfile --target full -t ps5-theme-host-test-tsan .
    exec docker run --rm --privileged --platform "linux/$arch" -e PS5THEME_TEST_BINARY=build-host/ps5-theme-tsan \
      -e TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 report_signal_unsafe=0" ps5-theme-host-test-tsan \
      sh -c 'make -f tests/host/Makefile build-host/ps5-theme-tsan >/dev/null && python3 -u tests/host/test_e2e.py' ;;
  serve)
    docker build --platform linux/amd64 -f tests/host/Dockerfile --target full -t ps5-theme-host-test .
    exec docker run --rm --privileged --platform linux/amd64 -p 127.0.0.1:18095:8095 \
      ps5-theme-host-test python3 -u tests/host/test_e2e.py --serve ;;
  *)
    docker build --platform linux/amd64 -f tests/host/Dockerfile --target full -t ps5-theme-host-test .
    exec docker run --rm --privileged --platform linux/amd64 ps5-theme-host-test ;;
esac
