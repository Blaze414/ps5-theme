#!/usr/bin/env bash
# Checks src/zstd.c against the reference zstd tool (run inside the test image).
set -euo pipefail
cd "$(dirname "$0")"
cc -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I../../src -o /tmp/zt zstd_test.c ../../src/zstd.c
work=$(mktemp -d)
python3 - "$work" <<'PY'
import os, random, sys
w = sys.argv[1]; r = random.Random(5)
words = [bytes(r.choice(b"abcdefghijklmnopqrstuvwxyz") for _ in range(r.randint(2, 9))) for _ in range(300)]
open(f"{w}/text", "wb").write(b" ".join(r.choice(words) for _ in range(400000)))
open(f"{w}/random", "wb").write(r.randbytes(700000))
open(f"{w}/zeros", "wb").write(bytes(3000000))
open(f"{w}/mixed", "wb").write(b"".join((r.randbytes(r.randint(1, 3000)) if r.random() < .3 else bytes([r.randint(0, 255)]) * r.randint(1, 5000)) for _ in range(3000)))
open(f"{w}/tiny", "wb").write(b"hello")
open(f"{w}/empty", "wb").write(b"")
grad = bytearray()
for y in range(1024):
    for x in range(1024):
        grad += bytes(((x * 255 // 1023), (y * 255 // 1023), 128, 255))
open(f"{w}/gradient", "wb").write(grad)
PY
cp /tmp/zt "$work/binary"
fail=0
for f in text random zeros mixed tiny empty gradient binary; do
  for lvl in 1 3 9 19; do
    zstd -q -f -$lvl "$work/$f" -o "$work/$f.$lvl.zst"
    /tmp/zt "$work/$f.$lvl.zst" "$work/$f" ${CORRUPT:+x} || fail=1
  done
  zstd -q -f --ultra -22 --long=27 "$work/$f" -o "$work/$f.u.zst"; /tmp/zt "$work/$f.u.zst" "$work/$f" || fail=1
  zstd -q -f -3 --no-check "$work/$f" -o "$work/$f.nc.zst"; /tmp/zt "$work/$f.nc.zst" "$work/$f" || fail=1
done
cat "$work/text.3.zst" "$work/zeros.1.zst" > "$work/multi.zst"; cat "$work/text" "$work/zeros" > "$work/multi"
/tmp/zt "$work/multi.zst" "$work/multi" || fail=1
CORRUPT=1 /tmp/zt "$work/mixed.19.zst" "$work/mixed" x || fail=1
if [ -f real.zst ]; then zstd -q -d -f real.zst -o "$work/real"; /tmp/zt real.zst "$work/real" || fail=1; fi
exit $fail
