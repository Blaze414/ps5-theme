#!/bin/sh
# Runs inside the image built by run.sh.
set -eu
OUT=/tmp/at9
if [ -f /console.at9 ]; then node tests/atrac9/codec_test.js "$OUT" /console.at9; else node tests/atrac9/codec_test.js "$OUT"; fi
for f in "$OUT"/*.at9; do
  case "$f" in */template.at9) [ -f /console.at9 ] || continue ;; esac
  lib9-decode "$f" "${f%.at9}.lib9.raw"
  ffmpeg -hide_banner -v error -y -i "$f" -f s16le "${f%.at9}.ff.raw"
  at9-validate "$f" "$OUT/template.at9" > "${f%.at9}.valid.txt" || true
done
if [ -f /console.at9 ]; then
  cp "$OUT/template.lib9.raw" "$OUT/console.lib9.raw"
  cp "$OUT/template.ff.raw" "$OUT/console.ff.raw"
  cp "$OUT/template.valid.txt" "$OUT/console.valid.txt"
fi
python3 tests/atrac9/compare.py "$OUT"
