#!/usr/bin/env python3
"""Wrap an ELF in etaHEN's .plugin container.

Format (etaHEN port_plugin.h / etaHEN-Plugins make_plugin.py):
  "etaHEN_PLUGIN\\0" + <4 letters + 5 digits> + "\\0" + <x.xx> + "\\0" + ELF
"""
import re
import sys


def main():
    if len(sys.argv) != 5:
        sys.exit("usage: make_plugin.py <in.elf> <out.plugin> <TITLE00001> <x.xx>")
    src, dst, title_id, version = sys.argv[1:]
    if not re.fullmatch(r"[A-Za-z]{4}[0-9]{5}", title_id):
        sys.exit("title ID must be four letters and five digits")
    if not re.fullmatch(r"[0-9]\.[0-9]{2}", version):
        sys.exit("version must look like 1.00")

    with open(src, "rb") as f:
        elf = f.read()
    if elf[:4] != b"\x7fELF" or elf[4] != 2 or elf[5] != 1:
        sys.exit(f"{src} is not a 64-bit little-endian ELF")

    header = b"etaHEN_PLUGIN\0" + title_id.encode() + b"\0" + version.encode() + b"\0"
    assert len(header) == 29
    with open(dst, "wb") as f:
        f.write(header + elf)
    print(f"wrote {dst} ({title_id} v{version}, {len(header) + len(elf)} bytes)")


if __name__ == "__main__":
    main()
