#!/usr/bin/env python3
"""Compare web/atrac9.js against LibAtrac9 and FFmpeg (see check.sh).

FFmpeg's ATRAC9 decoder outputs the opposite polarity to LibAtrac9 (an
inaudible sign convention), so its samples are negated before comparing."""
import json
import sys

import numpy as np

out = sys.argv[1]
m = json.load(open(f"{out}/manifest.json"))
failed = 0


def load(name, kind, channels):
    return np.fromfile(f"{out}/{name}.{kind}.raw", dtype="<i2").reshape(-1, channels).astype(np.int64)


def check(cond, msg):
    global failed
    if not cond:
        failed += 1
        print(f"FAIL  {msg}")
    return cond


def decoders_agree(name, channels):
    js, lib, ff = (load(name, k, channels) for k in ("js", "lib9", "ff"))
    d_lib, d_ff = np.abs(js - lib).max(), np.abs(js + ff).max()
    check(len(js) == len(lib) == len(ff), f"{name}: decoders return different lengths")
    check(d_lib <= 1, f"{name}: differs from LibAtrac9 by {d_lib}")
    check(d_ff <= 1, f"{name}: differs from FFmpeg by {d_ff}")
    valid = open(f"{out}/{name}.valid.txt").read().strip()
    check(valid == "ok", f"{name}: plugin validator says {valid!r}")
    return lib, d_lib, d_ff, valid


if m["console"]:
    c = m["console"]
    _, d_lib, d_ff, valid = decoders_agree("console", c["channels"])
    print(f"console file  decode {c['ms']} ms, loudness {c['loudness']:.1f} dBFS, max diff LibAtrac9 {d_lib} / FFmpeg {d_ff}, validator {valid}")

print(f"{'signal':10} {'SNR':>8} {'worst':>8} {'encode':>8}  max diff vs LibAtrac9 / FFmpeg")
for s in m["signals"]:
    name, ch = s["name"], s["channels"]
    lib, d_lib, d_ff, _ = decoders_agree(name, ch)
    inp = np.fromfile(f"{out}/{name}.f32", dtype="<f4").reshape(-1, 2).astype(np.float64)
    ref = np.clip(inp, -1, 32767 / 32768)
    dec = lib[s["delay"]:s["delay"] + s["samples"], :2] / 32768.0
    noise = ((dec - ref) ** 2).sum()
    snr = 10 * np.log10((ref ** 2).sum() / noise) if noise and (ref ** 2).sum() else float("inf")
    rest = np.abs(lib[:, 2:]).max() if ch > 2 else 0
    check(rest == 0, f"{name}: silent channels decoded to sound ({rest})")
    if s["minSnr"] is not None:
        check(snr >= s["minSnr"], f"{name}: SNR {snr:.1f} dB below {s['minSnr']} dB")
    else:
        check(np.abs(dec - ref).max() <= 2 / 32768 or name == "short", f"{name}: output differs from the input")
    print(f"{name:10} {snr:7.1f}dB {s['worst']:7.1f}dB {s['ms']:6d}ms  {d_lib} / {d_ff}")

print("\nall checks passed" if not failed else f"\n{failed} check(s) failed")
sys.exit(1 if failed else 0)
