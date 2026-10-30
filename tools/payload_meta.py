#!/usr/bin/env python3
"""The plugin's description, in one place, for the ELF and for PS5 Payload Manager.

  payload_meta.py json <TITLE00001> <x.xx>             the JSON the ELF carries (.ps5meta.json), on stdout
  payload_meta.py check <ps5-theme.elf> <TITLE00001> <x.xx>
                                                      check the built ELF carries it, and the logo, as it should
  payload_meta.py repo <ps5-theme.elf> <TITLE00001> <x.xx> <outdir> [--date YYYY-MM-DD]
                                                      PS5 Payload Manager's repository files for a release:
                                                      repository.json (the payload, its version, checksum and
                                                      download address) and icons.json (the logo)

The ELF carries two sections the console never loads (they take no memory): `.ps5meta.json`, UTF-8 JSON (schema 1:
id, name, version, title_id, author, description, homepage, license, icon), and `.ps5meta.icon`, the logo as a PNG.
Any tool can read them without running the payload, for example:
  llvm-objcopy -O binary --only-section=.ps5meta.json ps5-theme.elf meta.json
"""
import base64
import datetime
import hashlib
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LOGO = os.path.join(ROOT, "assets", "icon0.png")            # the Themes tile's icon, 512 x 512
LOGO_SMALL = os.path.join(ROOT, "assets", "icon-128.png")   # the same, 128 x 128 with rounded corners, for lists

ABOUT = {
    "id": "ps5-theme",
    "name": "PS5 Themes",
    "author": "Blaze414",
    "description": ("Custom home-screen themes: backgrounds, icons, sounds, fonts and home music, as read-only "
                    "overlays that never write the system. Manage them from the Themes tile, or from a phone or "
                    "computer at http://<console>:8095."),
    "homepage": "https://github.com/Blaze414/ps5-theme",
    "license": "GPL-3.0-or-later",
    "category": "Customization",
}
RELEASES = "https://github.com/Blaze414/ps5-theme/releases/download"


def about(title_id, version):
    if not re.fullmatch(r"[A-Z]{4}[0-9]{5}", title_id):
        sys.exit("title ID must be four capital letters and five digits")
    if not re.fullmatch(r"[0-9]\.[0-9]{2}", version):
        sys.exit("version must look like 1.00")
    return {"schema": 1, "id": ABOUT["id"], "name": ABOUT["name"], "version": version, "title_id": title_id,
            "author": ABOUT["author"], "description": ABOUT["description"], "homepage": ABOUT["homepage"],
            "license": ABOUT["license"], "icon": ".ps5meta.icon"}


def sections(path):
    """The ELF's sections by name: {name: bytes}."""
    with open(path, "rb") as f:
        elf = f.read()
    if elf[:4] != b"\x7fELF" or elf[4] != 2 or elf[5] != 1:
        sys.exit(f"{path} is not a 64-bit little-endian ELF")
    shoff, = struct.unpack_from("<Q", elf, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x3A)
    if not shoff or shentsize < 64 or shoff + shnum * shentsize > len(elf) or shstrndx >= shnum:
        sys.exit(f"{path} has no section table")
    heads = [struct.unpack_from("<IIQQQQIIQQ", elf, shoff + i * shentsize) for i in range(shnum)]
    _, _, _, _, stroff, strsize, *_ = heads[shstrndx]
    names = elf[stroff:stroff + strsize]
    out = {}
    for name, kind, flags, _, off, size, *_ in heads:
        label = names[name:names.index(b"\0", name)].decode()
        out[label] = (flags, b"" if kind == 8 else elf[off:off + size])   # (SHT_NOBITS has no bytes in the file)
    return out


def check(path, title_id, version):
    found = sections(path)
    for name in (".ps5meta.json", ".ps5meta.icon"):
        if name not in found:
            sys.exit(f"{path}: no {name} section")
        if found[name][0] & 2:   # SHF_ALLOC
            sys.exit(f"{path}: {name} would be loaded into memory")
    meta = json.loads(found[".ps5meta.json"][1].decode("utf-8"))
    want = about(title_id, version)
    if meta != want:
        sys.exit(f"{path}: .ps5meta.json is {meta}, not {want}")
    with open(LOGO, "rb") as f:
        if found[".ps5meta.icon"][1] != f.read():
            sys.exit(f"{path}: .ps5meta.icon isn't the logo")
    print(f"{path}: carries its description ({meta['name']} {meta['version']}, {meta['title_id']}) and the logo")


def repo(path, title_id, version, outdir, date):
    """Payload Manager's repository files: shown with the logo, name, version, author and description, and offered
    as an update when a newer one is published. The payload's address is this version's release download."""
    with open(path, "rb") as f:
        elf = f.read()
    check(path, title_id, version)
    with open(LOGO_SMALL, "rb") as f:
        icon = "data:image/png;base64," + base64.b64encode(f.read()).decode()
    if len(icon) > 12288:
        sys.exit("the logo is too large for Payload Manager (12,288 characters at most)")
    entry = {
        "id": ABOUT["id"],
        "name": ABOUT["name"],
        "filename": "ps5-theme.elf",
        "url": f"{RELEASES}/v{version}/ps5-theme.elf",
        "description": ABOUT["description"],
        "version": f"v{version}",
        "last_update": date,
        "category": ABOUT["category"],
        "checksum": hashlib.sha256(elf).hexdigest(),
        "author": ABOUT["author"],
        "icon_key": ABOUT["id"],
        "details_url": "https://raw.githubusercontent.com/Blaze414/ps5-theme/main/README.md",
        "source": ABOUT["homepage"],
    }
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "repository.json"), "w") as f:
        json.dump({"name": "PS5 Themes", "icons_url": "icons.json", "payloads": [entry]}, f, indent=2)
        f.write("\n")
    with open(os.path.join(outdir, "icons.json"), "w") as f:
        json.dump({"icons": {ABOUT["id"]: icon}}, f, indent=2)
        f.write("\n")
    print(f"wrote {outdir}/repository.json and icons.json for v{version} ({entry['checksum'][:12]}…)")


def main():
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == "json":
        sys.stdout.write(json.dumps(about(a[1], a[2]), ensure_ascii=False, indent=2) + "\n")
    elif len(a) == 4 and a[0] == "check":
        check(a[1], a[2], a[3])
    elif len(a) in (5, 7) and a[0] == "repo" and (len(a) == 5 or a[5] == "--date"):
        repo(a[1], a[2], a[3], a[4], a[6] if len(a) == 7 else datetime.date.today().isoformat())
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
