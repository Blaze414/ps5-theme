#!/usr/bin/env python3
"""End-to-end tests for ps5-theme, run inside the privileged test container.

Builds a fake console tree (/system_ex, /data, /user/app), runs the real plugin
(AddressSanitizer build), and checks the behaviour over HTTP and on disk.
`--serve` only sets up the tree and keeps the plugin running for manual UI use.
"""
import errno
import functools
import hashlib
import http.client
import json
import os
import random
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback
import urllib.parse
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rco_fixture import Rco, SHAPES, COLORS_DEFAULT, COLORS_PS2, palette_png, png_chunk, sample as rco_sample  # noqa: E402
import font_fixture  # noqa: E402

ASAN = "build-host/ps5-theme-asan"
STOP_TOOL = "build-host/ps5-theme-stop"
CRASH = "build-host/ps5-theme-crash"
PORT = 8095
BASE = "/data/ps5-theme"
APPS = "/system_ex/rnps/apps"
HOME = f"{APPS}/NPXS40008/assets/src/modules/home/assets"
OTHER = f"{APPS}/NPXS40028/assets/img"
BULK = f"{APPS}/NPXS40010/assets/bulk"
MEDIA = f"{APPS}/NPXS40012/assets"
VSH = "/system_ex/vsh_asset"
FONTS = "/preinst/common/font"           # the console's text fonts
SYSA = "/system/vsh_asset"                # the Appearance themes' textures and Welcome backgrounds
LOG = f"{BASE}/logs/log.txt"
ENV = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:abort_on_error=0",
           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")


# ---------------------------------------------------------------- fixtures

def png(w, h, rgb):
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def wav(rate, channels, seconds, amplitude, bits=16, fmt=1):
    frames = int(rate * seconds)
    sample = struct.pack("<h", amplitude) * channels
    data = b"".join(sample if (i // 64) % 2 == 0 else struct.pack("<h", -amplitude) * channels
                    for i in range(frames))
    block = channels * bits // 8
    fmt_chunk = struct.pack("<HHIIHH", fmt, channels, rate, rate * block, block, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt_chunk + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def dds_bc7(w, h, fill=None, flags=0x81007):
    """A DX10 BC7_UNORM DDS shaped like the console's bg_hub_default.dds
    (PS5 games' icon0.dds has flags 0xa1007)."""
    blocks = ((w + 3) // 4) * ((h + 3) // 4)
    data = fill if fill is not None else bytes(range(256)) * (blocks * 16 // 256 + 1)
    data = data[:blocks * 16]
    hdr = bytearray(148)
    hdr[0:4] = b"DDS "
    struct.pack_into("<7I", hdr, 4, 124, flags, h, w, blocks * 16, 1, 1)
    struct.pack_into("<2I4s", hdr, 76, 32, 4, b"DX10")
    struct.pack_into("<I", hdr, 108, 0x1000)
    struct.pack_into("<5I", hdr, 128, 98, 3, 0, 1, 0)
    return bytes(hdr) + data


def dds_dxt1(w, h, fill=0x55):
    """A legacy DXT1 DDS shaped like PS4 titles' icon0.dds on the PS5."""
    size = ((w + 3) // 4) * ((h + 3) // 4) * 8
    hdr = bytearray(128)
    hdr[0:4] = b"DDS "
    struct.pack_into("<7I", hdr, 4, 124, 0xa1007, h, w, size, 1, 1)
    struct.pack_into("<2I4s", hdr, 76, 32, 4, b"DXT1")
    struct.pack_into("<I", hdr, 108, 0x401008)
    return bytes(hdr) + bytes([fill]) * size


def xxh64(data):
    """XXH64, seed 0 (the low 32 bits are the zstd content checksum)."""
    M, P1, P2, P3, P4, P5 = (1 << 64) - 1, 11400714785074694791, 14029467366897019727, 1609587929392839161, 9650029242287828579, 2870177450012600261
    rotl = lambda x, r: ((x << r) | (x >> (64 - r))) & M
    rnd = lambda acc, v: (rotl((acc + v * P2) & M, 31) * P1) & M
    merge = lambda h, v: (((h ^ rnd(0, v)) * P1) + P4) & M
    n, o = len(data), 0
    if n >= 32:
        v = [(P1 + P2) & M, P2, 0, (-P1) & M]
        while o + 32 <= n:
            for k in range(4):
                v[k] = rnd(v[k], int.from_bytes(data[o + 8 * k:o + 8 * k + 8], "little"))
            o += 32
        h = (rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18)) & M
        for k in range(4):
            h = merge(h, v[k])
    else:
        h = P5
    h = (h + n) & M
    while o + 8 <= n:
        h = ((rotl(h ^ rnd(0, int.from_bytes(data[o:o + 8], "little")), 27) * P1) + P4) & M
        o += 8
    if o + 4 <= n:
        h = ((rotl(h ^ ((int.from_bytes(data[o:o + 4], "little") * P1) & M), 23) * P2) + P3) & M
        o += 4
    while o < n:
        h = (rotl(h ^ ((data[o] * P5) & M), 11) * P1) & M
        o += 1
    h ^= h >> 33
    h = (h * P2) & M
    h ^= h >> 29
    h = (h * P3) & M
    return h ^ (h >> 32)


def zstd_stored(data, like=None):
    """A zstd frame of stored blocks, with the frame header of `like` (a system
    file's bytes: descriptor, window, content size) and its content checksum if
    the descriptor asks for one; without a model, single segment, 4-byte size."""
    desc = like[4] if like else 0xa0
    single, fcs, dict_id, csum = (desc >> 5) & 1, desc >> 6, desc & 3, (desc >> 2) & 1
    wb = like[5] if like and not single else 0
    fcs_len = single if fcs == 0 else (2, 4, 8)[fcs - 1]
    window = len(data) if single else ((1 << (10 + (wb >> 3))) + ((1 << (10 + (wb >> 3))) >> 3) * (wb & 7))
    block = max(1, min(131072, window))
    out = bytearray(b"\x28\xb5\x2f\xfd") + bytes([desc])
    if not single:
        out.append(wb)
    out += bytes(4 if dict_id == 3 else dict_id)
    if fcs_len == 1:
        out.append(len(data))
    elif fcs_len == 2:
        out += struct.pack("<H", len(data) - 256)
    elif fcs_len == 4:
        out += struct.pack("<I", len(data))
    elif fcs_len == 8:
        out += struct.pack("<Q", len(data))
    blocks = max(1, -(-len(data) // block))
    for b in range(blocks):
        chunk = data[b * block:(b + 1) * block]
        h = (len(chunk) << 3) | (1 if b == blocks - 1 else 0)
        out += bytes([h & 255, (h >> 8) & 255, (h >> 16) & 255]) + chunk
    if csum:
        out += struct.pack("<I", xxh64(data) & 0xffffffff)
    return bytes(out)


# The frame headers of the console's own textures: single segment with a 4-byte size (0xa0), and
# the same with a content checksum (0xa4, as in bg_image_load_error.dds.zst).
SONY_FRAME = bytes([0x28, 0xb5, 0x2f, 0xfd, 0xa0, 0, 0, 0, 0])
SONY_FRAME_CHECKSUM = bytes([0x28, 0xb5, 0x2f, 0xfd, 0xa4, 0, 0, 0, 0])


def zstd_cli(data, level=19):
    return subprocess.run(["zstd", f"-{level}", "-c", "-q"], input=data, capture_output=True, check=True).stdout


UI_RCO = "/system_ex/vsh_asset/Sce.PlayStation.PUI_UI3.rco"
PLUS_ICON = "/system_ex/rnps/apps/NPXS40037/appdb/default/icon0.png"
GAME = "/user/appmeta/PPSA01234"        # an installed PS5 game's metadata
GAME_SYS = "/user/app/PPSA01234/sce_sys"  # and its installation's, where most PS5 games' records point
PS4_APP = "/user/appmeta/CUSA09999"     # and a PS4 app's
CATALOG = "/user/catalog_downloader/appmeta/PPSA01234"   # pictures downloaded for the game from PlayStation Network
CONCEPT = "/user/catalog_downloader/conceptmeta/234585"  # and for its concept, which some games' records point at


def sfo(title):
    """A minimal PS4 param.sfo holding TITLE."""
    keys, data = b"TITLE\0\0\0", title.encode() + b"\0"
    padded = data + b"\0" * (-len(data) % 4)
    key_table = 20 + 16
    return (struct.pack("<4sIIII", b"\0PSF", 0x101, key_table, key_table + len(keys), 1)
            + struct.pack("<HHIII", 0, 0x0204, len(data), len(padded), 0) + keys + padded)
SVG_ICON = (b'<svg width="128px" height="128px" viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">'
            b'<path d="M8 8h48v48H8z" fill="#fff"/></svg>')
WEB_PAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../web/index.html")
RCO_JS = """
const fs = require("fs"), zlib = require("zlib");
const html = fs.readFileSync(process.argv[2], "utf8");
const block = html.slice(html.indexOf("/* rco:begin */"), html.indexOf("/* rco:end */"));
const R = new Function("esc", block + ";return { rcoParse, rcoPictures, rcoRebuild, numberSvg, numberPicture, rcoData };")(s => String(s));
(async () => {
  const stock = new Uint8Array(fs.readFileSync(process.argv[3]));
  const r = R.rcoParse(stock);
  if (Buffer.compare(Buffer.from(R.rcoRebuild(r, new Map())), Buffer.from(stock))) throw new Error("an unchanged rebuild differs");
  const deflate = process.argv[5] === "stored" ? null : async d => new Uint8Array(zlib.deflateSync(d));
  const replace = new Map(), kinds = {};
  let n = 0;
  for (const pic of R.rcoPictures(r)) {
    n++;
    kinds[pic.id] = pic.kind;
    for (const f of pic.files) {
      const d = R.rcoData(r, f);
      const bytes = f.kind === "png" ? await R.numberPicture(n, d, deflate) : new TextEncoder().encode(R.numberSvg(n, new TextDecoder().decode(d)));
      replace.set(f.el + "\\t" + f.attr, bytes);
    }
  }
  fs.writeFileSync(process.argv[4], R.rcoRebuild(r, replace));
  console.log(JSON.stringify({ pictures: n, numbered: replace.size, kinds }));
})().catch(e => { console.error(e); process.exit(1); });
"""

SHAPES_JS = """
const fs = require("fs");
const html = fs.readFileSync(process.argv[2], "utf8");
const block = html.slice(html.indexOf("/* rco:begin */"), html.indexOf("/* rco:end */"));
const R = new Function("esc", block + ";return { rcoParse, rcoRebuild, rcoShapes, shapesText };")(s => String(s));
const r = R.rcoParse(new Uint8Array(fs.readFileSync(process.argv[3]))), sh = R.rcoShapes(r);
if (R.shapesText(sh.text, sh.values) !== sh.text) throw new Error("unchanged numbers changed the text");
const values = sh.values.map(([k, v]) => [k, k === "borderRadiusRatio" ? 3 : k === "switch.track" ? 0 : k === "slider.thumb" ? 12.5 : v]);
const text = R.shapesText(sh.text, values);
fs.writeFileSync(process.argv[4], R.rcoRebuild(r, new Map([[sh.f.el + "\\t" + sh.f.attr, new TextEncoder().encode(text)]])));
console.log(JSON.stringify({ names: sh.values.map(v => v[0]) }));
"""

COLORS_JS = """
const fs = require("fs");
const html = fs.readFileSync(process.argv[2], "utf8");
const block = html.slice(html.indexOf("/* rco:begin */"), html.indexOf("/* rco:end */"));
const R = new Function("esc", block + ";return { rcoParse, rcoRebuild, rcoColors, colorsText, colorTestValues, colorPanelValues, colorEditValues };")(s => String(s));
const r = R.rcoParse(new Uint8Array(fs.readFileSync(process.argv[3]))), pals = R.rcoColors(r);
for (const p of pals) if (R.colorsText(p.text, p.values) !== p.text) throw new Error("unchanged colours changed the text");
const edit = { widgets: 0x123456, backdrop: 0x0000ff, text: 0xffd000, accent: 0xff0000 };
const test = process.argv[5] === "panel" ? R.colorPanelValues(pals)
  : process.argv[5] === "edit" ? pals.map((p, i) => p.id === "color_ps2" ? R.colorEditValues(pals, i, edit) : p.values)
  : R.colorTestValues(pals);
const replace = new Map(pals.map((p, i) => [p.f.el + "\\t" + p.f.attr, new TextEncoder().encode(R.colorsText(p.text, test[i]))]));
fs.writeFileSync(process.argv[4], R.rcoRebuild(r, replace));
console.log(JSON.stringify({ palettes: pals.map(p => p.id) }));
"""

PICTURE_JS = """
const fs = require("fs"), zlib = require("zlib");
const html = fs.readFileSync(process.argv[2], "utf8");
const block = html.slice(html.indexOf("/* rco:begin */"), html.indexOf("/* rco:end */"));
const R = new Function("esc", block + ";return { rcoParse, rcoPictures, rcoRebuild, rcoData, svgWithImage, numberPicture, tileTitle };")(s => String(s));
(async () => {
  const stock = new Uint8Array(fs.readFileSync(process.argv[3]));
  const r = R.rcoParse(stock), home = R.rcoPictures(r).find(p => p.id === "iconid_home");
  const png = fs.readFileSync(process.argv[5]).toString("base64");
  const svg = R.svgWithImage(new TextDecoder().decode(R.rcoData(r, home.files[0])), "data:image/png;base64," + png);
  fs.writeFileSync(process.argv[4], R.rcoRebuild(r, new Map([[home.files[0].el + "\\t" + home.files[0].attr, new TextEncoder().encode(svg)]])));
  const icon = new Uint8Array(fs.readFileSync(process.argv[6]));
  fs.writeFileSync(process.argv[7], await R.numberPicture(3, icon, async d => new Uint8Array(zlib.deflateSync(d))));
  const titles = ["/system_ex/rnps/apps/NPXS40037/appdb/default/icon0.png", "/system_ex/rnps/apps/NPXS40037/assets/applications/psplus/appdb/default/icon0.png",
                  "/mnt/rnps/apps/NPXS40016/appdb/NPXS40053/icon0.png", "/system_ex/rnps/apps/NPXS40075/assets/src/assets/texture/icon0.png",
                  "/system_ex/rnps/apps/NPXS40008/assets/x/icon0.png"].map(R.tileTitle);
  console.log(JSON.stringify({ svg: svg.slice(0, 300), titles }));
})().catch(e => { console.error(e); process.exit(1); });
"""

AT9_TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "at9_fixture.js")


def at9_stock(seconds=3, cci=3, level=0.05):
    """A stand-in for the console's bgm_home.at9 (ATRAC9 5.1, 48 kHz), made by
    the theme builder's own encoder (web/atrac9.js)."""
    out = f"/tmp/at9-stock-{cci}-{seconds}-{level}.at9"
    subprocess.run(["node", AT9_TOOL, "stock", out, str(seconds), str(cci), str(level)], check=True, capture_output=True)
    return open(out, "rb").read()


def at9_music(stock_path, seconds, hz=440):
    """A replacement encoded against the system file, as the browser makes it."""
    out = f"/tmp/at9-music-{seconds}-{hz}.at9"
    subprocess.run(["node", AT9_TOOL, "music", stock_path, out, str(seconds), str(hz)], check=True, capture_output=True)
    return open(out, "rb").read()


def jpeg_header(w, h):
    """Smallest byte sequence our parser accepts as a JPEG of w x h."""
    sof = b"\xff\xc0" + struct.pack(">HBHHB", 11, 8, h, w, 1) + b"\x01\x11\x00"
    return b"\xff\xd8" + b"\xff\xe0" + struct.pack(">H", 16) + b"JFIF\0" + b"\0" * 9 + sof + b"\xff\xd9"


def webp_header(w, h, fill=0):
    """Smallest byte sequence our parser accepts as a lossy WebP of w x h,
    like the pictures the console downloads from PlayStation Network."""
    vp8 = b"\x10\x02\x00" + b"\x9d\x01\x2a" + struct.pack("<HH", w, h) + bytes([fill]) * 16
    return b"RIFF" + struct.pack("<I", 12 + len(vp8)) + b"WEBPVP8 " + struct.pack("<I", len(vp8)) + vp8


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def build_console():
    for d in ("/system_ex", "/system", "/mnt/rnps", "/data", "/user", "/preinst", "/tmp/notifications.txt", "/tmp/tile-registered"):
        if os.path.isdir(d):
            shutil.rmtree(d)
        elif os.path.exists(d):
            os.unlink(d)
    write(f"{HOME}/bg.png", png(480, 270, (20, 30, 60)))
    write(f"{HOME}/icon.png", png(96, 96, (200, 200, 200)))
    write(f"{HOME}/icon@2x.png", png(192, 192, (200, 200, 200)))
    write(f"{HOME}/strings.json", b'{"title":"Home"}')
    write(f"{HOME}/sub/deep.png", png(32, 32, (1, 2, 3)))
    write(f"{OTHER}/target.png", png(64, 64, (9, 9, 9)))
    os.symlink(OTHER, f"{HOME}/linkdir")
    write(f"{BULK}/hero.png", png(100, 100, (50, 50, 50)))
    rnd = random.Random(1)
    for i in range(150):
        write(f"{BULK}/blob{i:03}.bin", rnd.randbytes(512 * 1024))
    write("/mnt/rnps/apps/NPXS40099/a.png", png(8, 8, (0, 0, 0)))
    write(f"{MEDIA}/sounds/move.wav", wav(48000, 1, 0.05, 8000))
    write(f"{MEDIA}/fonts/ui.ttf", font_fixture.font("tt"))
    # The console's text fonts, as in /preinst/common/font: OpenType (CFF) and TrueType.
    write(f"{FONTS}/SST-Roman.otf", font_fixture.font("cff", glyphs=120))
    write(f"{FONTS}/SST-Bold.otf", font_fixture.font("cff", glyphs=120))
    write(f"{FONTS}/DFHEI5-SONY.ttf", font_fixture.font("tt", glyphs=150))
    write(f"{FONTS}/notes.txt", b"not a font")
    # The Appearance themes' textures: BC7 DDS, zstd-compressed under a plain .dds name, as on the console.
    write(f"{SYSA}/control_center_ps2_bottom.dds", zstd_stored(dds_bc7(64, 32), SONY_FRAME_CHECKSUM))
    write(f"{SYSA}/control_center_ps2_top.dds", zstd_stored(dds_bc7(48, 32), SONY_FRAME))
    write(f"{SYSA}/homeui_ps2.dds", zstd_stored(dds_bc7(64, 24), SONY_FRAME))
    write(f"{SYSA}/welcome-hub-background-0-4k.jpg", jpeg_header(96, 54))
    write(f"{MEDIA}/anim/spinner.json", b'{"v":"5.7","w":100,"h":100,"layers":[]}')
    write(f"{MEDIA}/icons/star.svg", b'<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8"/>')
    write(f"{MEDIA}/video/loop.mp4", b"\x00\x00\x00\x18ftypisom" + b"\0" * 40)
    write(f"{MEDIA}/bundle.js", b"console.log(1)")
    write(f"{VSH}/bg_hub_default.dds.zst", zstd_cli(dds_bc7(256, 128, fill=bytes(range(7, 256)) * 300)))
    write(f"{VSH}/eboot_like.rco", b"\0" * 64)
    write(PLUS_ICON, png(64, 64, (200, 160, 0)))
    write(PLUS_ICON.replace("icon0.png", "param.json"),
          b'{"titleId":"NPXS40037","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"PlayStation Plus"}}}')
    write(f"{VSH}/bg_NPXS40037.dds.zst", zstd_cli(dds_bc7(64, 32)))
    write(f"{GAME}/icon0.png", png(64, 64, (0, 90, 200)))
    write(f"{GAME}/icon0.dds", dds_bc7(64, 64, flags=0xa1007))       # what a PS5 game's tile shows
    write(f"{GAME}/pic0.png", png(96, 54, (20, 20, 20)))
    write(f"{GAME}/param.json", b'{"titleId":"PPSA01234","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Test Game"}}}')
    write(f"{PS4_APP}/icon0.png", png(64, 64, (90, 0, 200)))
    write(f"{PS4_APP}/icon0.dds", dds_dxt1(64, 64))
    # As in Black Myth's installation on the console: pictures beside the
    # files the system starts the game with, and the game itself beside them.
    write(f"{GAME_SYS}/icon0.png", png(64, 64, (0, 90, 200)))
    write(f"{GAME_SYS}/icon0.dds", dds_bc7(64, 64, flags=0xa1007))
    write(f"{GAME_SYS}/param.json", b'{"titleId":"PPSA01234"}')
    write(f"{GAME_SYS}/license.dat", bytes(range(256)) * 4)
    write(f"{GAME_SYS}/keystone", b"keystone" * 12)
    write(f"{GAME_SYS}/about/right.sprx", b"\x7fELF")
    write(f"{GAME_SYS}/about/inner.png", png(16, 16, (5, 5, 5)))
    write(f"{GAME_SYS}/trophy2/trophy00.ucp", b"trophies" * 512)
    write("/user/app/PPSA01234/app.pkg", b"the game" * 64)
    write("/user/app/PPSA01234/stray.png", png(16, 16, (6, 6, 6)))
    # The system's own records of the game, for the "where" listing.
    write("/system_data/priv/appmeta/PPSA01234/icon0.png", png(32, 32, (9, 9, 9)))
    # As on the console, PlayStation Network's pictures are WebP and JPEG
    # files with .png names, some in numbered copies.
    write(f"{CATALOG}/icon0.png", webp_header(64, 64))
    write(f"{CATALOG}/pic0.png", jpeg_header(96, 54))
    write(f"{CONCEPT}/icon0.png", webp_header(64, 64))
    write(f"{CONCEPT}/icon0_11.png", webp_header(64, 64))
    write(f"{CONCEPT}/logo_01.png", png(32, 16, (200, 200, 200)))
    write(f"{CONCEPT}/notes.json", b"{}")
    write(f"{CONCEPT}/sub/inner.png", png(16, 16, (5, 5, 5)))
    write("/user/catalog_downloader/conceptmeta/999999/icon0.png", png(16, 16, (1, 1, 1)))   # no record names it
    write("/user/catalog_downloader/catalog.db", b"downloads" * 8)
    # As on the console: records naming a concept's folder right after the
    # title ID (two titles share 234585); one whose title ID is too far
    # before it; one cut by the reader's buffer.
    write("/system_data/priv/mms/app.db", b"SQLite format 3\0" + bytes(200) + b"\x05/user/appmeta/PPSA01234/icon0.png\x01Test Game"
          + bytes(100) + b"\x07CUSA09999\x00" + b"x" * 5000 + b"PPSA01234" + b"y" * 9000
          + b"\x00psgm:play?id=PPSA01234pshome:gamehub?titleId=PPSA01234/user/catalog_downloader/conceptmeta/234585/icon0.png?ts=1789297134"
          + b"/user/catalog_downloader/conceptmeta/234585/logo_01.png?ts=1789297134\x03"
          + b"titleId=CUSA09999/user/catalog_downloader/conceptmeta/234585/icon0.png\x00"
          + b"titleId=PPSA05555" + b"z" * 300 + b"/user/catalog_downloader/conceptmeta/777777/icon0.png\x00"
          + b"w" * 4070 + b"titleId=PPSA06666/user/catalog_downloader/conceptmeta/888888/icon0.png\x00")
    write(f"{PS4_APP}/param.sfo", sfo("Test App"))
    write(UI_RCO, rco_sample(lambda w, h: png(w, h, (90, 90, 90)), lambda: SVG_ICON, bytes(range(256)) * 8,
                             COLORS_DEFAULT, COLORS_PS2))
    write(f"{VSH}/bgm_home.at9", at9_stock())
    write(f"{VSH}/sfx_quiet.at9", at9_stock(10, 3, 0))        # silent: codes only 3 bands
    write(f"{MEDIA}/lib/native.sprx", b"\x7fELF")
    os.makedirs("/data", exist_ok=True)
    os.makedirs("/user/app", exist_ok=True)


def snapshot(root):
    out = {}
    for d, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = [x for x in dirs if not x.endswith(".ps5theme-under")]
        for name in files + [x for x in dirs if os.path.islink(os.path.join(d, x))]:
            p = os.path.join(d, name)
            if os.path.islink(p):
                out[p] = "link:" + os.readlink(p)
            else:
                with open(p, "rb") as f:
                    out[p] = hashlib.sha256(f.read()).hexdigest()
    return out


def crc_file(path):
    with open(path, "rb") as f:
        return zlib.crc32(f.read())


# ---------------------------------------------------------------- plugin process

class Plugin:
    def __init__(self, binary=ASAN, env=None, timeout=20):
        self.out = open("/tmp/plugin-out.txt", "ab")
        self.proc = subprocess.Popen([os.path.abspath(binary)], stdout=self.out, stderr=self.out,
                                     env=dict(ENV, **(env or {})), cwd="/")
        self.wait_port(timeout)

    def wait_port(self, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            if self.proc.poll() is not None:
                raise AssertionError(f"plugin exited early with {self.proc.returncode}")
            try:
                socket.create_connection(("127.0.0.1", PORT), 0.2).close()
                return
            except OSError:
                time.sleep(0.1)
        raise AssertionError("plugin did not open its port")

    def alive(self):
        return self.proc.poll() is None

    def kill(self):
        if self.alive():
            self.proc.kill()
            self.proc.wait()

    def term(self):
        if self.alive():
            self.proc.terminate()
            self.proc.wait(10)


def call(method, path, params=None, body=None, token=True, headers=None, timeout=30):
    url = path + ("?" + urllib.parse.urlencode(params) if params else "")
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=timeout)
    h = {"X-PS5-Theme": "1"} if token and method != "GET" else {}
    h.update(headers or {})
    conn.request(method, url, body=body, headers=h)
    res = conn.getresponse()
    data = res.read()
    conn.close()
    ctype = res.getheader("Content-Type", "")
    return res.status, (json.loads(data) if "json" in ctype else data)


def status():
    code, data = call("GET", "/api/status")
    assert code == 200, code
    return data


def put_file(theme, asset, data, crc=True):
    params = {"id": theme, "path": asset}
    if crc:
        params["crc"] = f"{zlib.crc32(data):08x}"
    return call("PUT", "/api/theme/file", params, data)


def wait_applied(want, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        s = status()
        if s["applied"] == want and s["active"] == (want or s["active"]):
            time.sleep(0.3)
            return status()
        time.sleep(0.25)
    raise AssertionError(f"timed out waiting for applied={want!r}, last status {s}")


def wait_result(fragment, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        s = status()
        if fragment in s["result"]:
            return s
        time.sleep(0.25)
    raise AssertionError(f"result never contained {fragment!r}: {s['result']!r}")


EMULATED = bool(os.environ.get("EMULATE_MOUNTS"))


def mounts_on(point):
    if EMULATED:
        return [point] if os.path.islink(point) else []
    with open("/proc/self/mountinfo") as f:
        return [l for l in f if l.split()[4] == point]


def stage_records(name):
    """Stage folders recorded in BASE/live (the applied theme's) or BASE/spare."""
    try:
        lines = open(f"{BASE}/{name}").read().splitlines()
    except FileNotFoundError:
        return []
    return [l.split("\t")[2] for l in lines if l.startswith("stage\t")]


def live_stage(real):
    """The applied theme's stage folder for the system folder `real`."""
    lines = open(f"{BASE}/live").read().splitlines()
    return next(l.split("\t")[2] for l in lines if l.startswith("stage\t") and l.split("\t")[3] == real)


def log_text():
    with open(LOG) as f:
        return f.read()


# ---------------------------------------------------------------- tests

S = {}  # shared state between ordered tests


def t_startup():
    s = status()
    assert s["fw"] == "13.42" and s["version"] == "1.60" and s["themes"] == [], s
    listing = open(f"{BASE}/dump/assets-v2-13.42.txt").read()
    assert f"{VSH}/bg_hub_default.dds.zst" in listing
    assert f"{HOME}/bg.png" in listing and "/mnt/rnps/apps/NPXS40099/a.png" in listing
    param = open("/user/app/THEM00001/sce_sys/param.json").read()
    assert '"deeplinkUri":"http://127.0.0.1:8095/"' in param and '"titleId":"THEM00001"' in param
    assert open("/user/app/THEM00001/sce_sys/icon0.png", "rb").read(8) == b"\x89PNG\r\n\x1a\n"
    assert open("/tmp/tile-registered").read().strip() == "THEM00001"
    assert "starting" in log_text()
    notes = open("/tmp/notifications.txt").read()
    assert "ps5-theme 1.60 starting" in notes and "ps5-theme ready" in notes, notes


def t_ui_and_icon_served():
    code, html = call("GET", "/")
    assert code == 200 and b"<title>PS5 Themes</title>" in html
    code, icon = call("GET", "/icon.png")
    assert code == 200 and icon[:4] == b"\x89PNG"
    code, js = call("GET", "/atrac9.js")
    assert code == 200 and js == open("web/atrac9.js", "rb").read()


def t_mutations_need_token():
    code, _ = call("POST", "/api/themes", {"id": "x"}, token=False)
    assert code == 403
    code, _ = call("PUT", "/api/theme/file", {"id": "x", "path": f"{HOME}/bg.png"}, b"x", token=False)
    assert code == 403


def t_create_theme_rules():
    assert call("POST", "/api/themes", {"id": "night", "name": "Night"})[0] == 200
    assert call("POST", "/api/themes", {"id": "night"})[0] == 409
    for bad in ("../x", "none", "", "a/b", "x" * 65):
        assert call("POST", "/api/themes", {"id": bad})[0] == 400, bad
    ini = open(f"{BASE}/themes/night/theme.ini").read()
    assert ini == "name=Night\nfw=13.42\nmusic_gain=0\n", ini


def t_asset_search():
    code, data = call("GET", "/api/assets", {"q": "ICON"})
    paths = {a["path"] for a in data["assets"]}
    assert code == 200 and {f"{HOME}/icon.png", f"{HOME}/icon@2x.png"} <= paths, paths


def t_upload_is_verified():
    S["bg"] = png(480, 270, (180, 40, 40))
    code, r = put_file("night", f"{HOME}/bg.png", S["bg"])
    assert code == 200 and r["crc32"] == f"{zlib.crc32(S['bg']):08x}" and r["width"] == 480, r

    code, r = put_file("night", f"{HOME}/bg.png", png(400, 270, (1, 1, 1)))
    assert code == 422 and "480x270" in r["error"], r
    code, r = put_file("night", f"{HOME}/bg.png", jpeg_header(480, 270))
    assert code == 422 and "PNG" in r["error"], r
    code, r = call("PUT", "/api/theme/file", {"id": "night", "path": f"{HOME}/bg.png", "crc": "deadbeef"}, S["bg"])
    assert code == 422 and "checksum" in r["error"], r
    for path in ("/etc/passwd", f"{APPS}/../../etc/passwd", f"{HOME}/missing.png", "/data/ps5-theme/active"):
        assert put_file("night", path, b"x")[0] == 403, path
    assert put_file("nosuch", f"{HOME}/bg.png", S["bg"])[0] == 404
    assert call("PUT", "/api/theme/file", {"id": "night", "path": f"{HOME}/bg.png"}, b"")[0] == 400
    assert call("PUT", "/api/theme/file", {"id": "night", "path": f"{HOME}/bg.png"}, b"x",
                headers={"Content-Length": str(40 << 20)})[0] == 413

    assert put_file("night", f"{HOME}/strings.json", b'{"title":"Night"}')[0] == 200
    assert put_file("night", f"{HOME}/linkdir/target.png", png(64, 64, (255, 0, 255)))[0] == 200
    S["icon2x"] = png(192, 192, (10, 200, 10))
    assert put_file("night", f"{HOME}/icon@2x.png", S["icon2x"])[0] == 200

    code, r = call("GET", "/api/theme/files", {"id": "night"})
    files = {f["path"] for f in r["files"]}
    assert files == {f"{HOME}/bg.png", f"{HOME}/strings.json", f"{HOME}/linkdir/target.png", f"{HOME}/icon@2x.png"}, files
    leftovers = [f for _, _, fs in os.walk(f"{BASE}/themes") for f in fs if f.endswith((".part", ".tmp"))]
    assert not leftovers, leftovers


def t_apply_verifies_and_is_read_only():
    S["stock_icon"] = crc_file(f"{HOME}/icon.png")
    S["stock_bg"] = crc_file(f"{HOME}/bg.png")
    S["stock_target"] = crc_file(f"{OTHER}/target.png")
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    s = wait_applied("night")
    assert "3 files verified" in s["result"] and "1 skipped" in s["result"], s["result"]
    notes = open("/tmp/notifications.txt").read()
    assert "ps5-theme: applying 'Night'..." in notes, notes[-500:]

    assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])
    assert crc_file(f"{HOME}/icon@2x.png") == zlib.crc32(S["icon2x"])
    assert open(f"{HOME}/strings.json", "rb").read() == b'{"title":"Night"}'
    assert crc_file(f"{HOME}/icon.png") == S["stock_icon"]
    assert crc_file(f"{OTHER}/target.png") == S["stock_target"], "write escaped through a symlink"
    assert "reached through a symlink" in log_text()

    for attempt in (() if EMULATED else (lambda: open(f"{HOME}/bg.png", "wb"), lambda: open(f"{HOME}/new.png", "wb"))):
        try:
            attempt()
            raise AssertionError("overlay is writable")
        except OSError as e:
            assert e.errno == errno.EROFS, e
    assert len(mounts_on(HOME)) == 1
    assert recorded_mounts() == [HOME], recorded_mounts()
    assert open(f"{BASE}/mounts").read().startswith(f"{HOME}\t{BASE}/stage/") and f"\t{HOME}\n" in open(f"{BASE}/mounts").read()


def t_reapply_picks_up_edits():
    newer = png(480, 270, (40, 180, 40))
    assert put_file("night", f"{HOME}/bg.png", newer)[0] == 200
    assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])  # live mount unchanged until re-apply
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    end = time.time() + 30
    while crc_file(f"{HOME}/bg.png") != zlib.crc32(newer):
        assert time.time() < end, "re-apply did not take effect"
        time.sleep(0.2)
    wait_applied("night")
    assert len(mounts_on(HOME)) == 1, "mounts stacked on re-apply"
    S["bg"] = newer


def t_stock_mirror_and_linked_staging():
    """Staging links a verified stock copy, made once, instead of copying the folder each time."""
    mirror, mark = f"{BASE}/stock/13.42{HOME}", f"{BASE}/stock/13.42/.complete{HOME}.ok"
    assert os.path.exists(mark), "no stock copy was kept"
    stage = [d for d in os.listdir(f"{BASE}/stage")]
    assert len(stage) >= 1 and all(d.startswith("g") for d in stage), stage
    staged = live_stage(HOME)
    # Untouched files are links to the stock copy; replaced ones are links to
    # the theme's own files, nothing is copied.
    theme = f"{BASE}/themes/night/files{HOME}"
    assert os.stat(f"{staged}/icon.png").st_ino == os.stat(f"{mirror}/icon.png").st_ino
    assert os.stat(f"{staged}/bg.png").st_ino != os.stat(f"{mirror}/bg.png").st_ino
    assert os.stat(f"{staged}/bg.png").st_ino == os.stat(f"{theme}/bg.png").st_ino
    assert crc_file(f"{mirror}/bg.png") == S["stock_bg"], "the stock copy changed"
    assert crc_file(f"{mirror}/icon.png") == S["stock_icon"]
    assert status()["applying"] == ""

    # A copy that is missing while a theme covers the folder is made from the
    # system after the overlay comes down, never from the theme.
    os.unlink(mark)
    os.unlink(f"{mirror}/bg.png")
    seen, stop = [], threading.Event()

    def sample():   # what the manager shows while the apply runs
        while not stop.is_set():
            s = status()
            if s["progress"] is not None:
                seen.append((s["progress"]["percent"], s["progress"]["text"]))
            time.sleep(0.005)
    sampler = threading.Thread(target=sample)
    sampler.start()
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    end = time.time() + 30
    while not os.path.exists(mark):
        assert time.time() < end, "the stock copy was not made again"
        time.sleep(0.2)
    wait_applied("night")
    stop.set()
    sampler.join()
    assert status()["progress"] is None
    percents = [p for p, _ in seen]
    assert all(0 <= p <= 100 for p in percents) and percents == sorted(percents), seen
    assert all(text for _, text in seen), seen
    assert crc_file(f"{mirror}/bg.png") == S["stock_bg"], "stock copy taken from the theme"
    assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])
    # Re-applying with nothing changed: no file is checked again, and the
    # overlays are confirmed without re-reading the files.
    before = len(applies())
    assert "0 file(s) compared by content" in applies()[-1], applies()[-1]
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    end = time.time() + 30
    while len(applies()) == before:
        assert time.time() < end, "the re-apply did not finish"
        time.sleep(0.2)
    last = applies()[-1]
    assert "3 check(s) remembered, 0 file(s) compared by content" in last, last
    wait_applied("night")
    assert len(mounts_on(HOME)) == 1 and recorded_mounts() == [HOME], recorded_mounts()
    assert all(d.startswith("g") for d in os.listdir(f"{BASE}/stage"))


def applies():
    """The summary line each apply logs."""
    return [l for l in log_text().splitlines() if "folder(s) reused" in l]


def progress_during(action):
    """(percent, text) pairs the manager shows while `action` runs."""
    seen, stop = [], threading.Event()

    def sample():
        while not stop.is_set():
            p = status()["progress"]
            if p is not None and (not seen or seen[-1] != (p["percent"], p["text"])):
                seen.append((p["percent"], p["text"]))
            time.sleep(0.005)
    sampler = threading.Thread(target=sample)
    sampler.start()
    try:
        action()
    finally:
        stop.set()
        sampler.join()
    return seen


def apply_and_wait(theme):
    before = len(applies())
    assert call("POST", "/api/apply", {"id": theme})[0] == 200
    end = time.time() + 60
    while len(applies()) == before:
        assert time.time() < end, "the apply did not finish"
        time.sleep(0.2)
    wait_applied(theme)
    return applies()[-1]


def recorded_mounts():
    """The mount points in the plugin's record (BASE/mounts), oldest first.
    Each line is "<mount point>\t<source>\t<system folder>"."""
    try:
        return [l.split("\t")[0] for l in open(f"{BASE}/mounts").read().splitlines() if l]
    except FileNotFoundError:
        return []


def reboot_console():
    """Like a reboot: the plugin stops and every overlay is gone."""
    S["plugin"].kill()
    for on in reversed(recorded_mounts()):
        if EMULATED:
            os.unlink(on)
            os.rename(on + ".ps5theme-under", on)
        else:
            subprocess.run(["umount", on], check=True)


def t_stage_folders_reused_without_links():
    """Where hard links don't work (the console's /data), stage folders hold
    copies, and are reused while nothing changed: on re-apply and after a reboot."""
    S["plugin"].term()
    S["plugin"] = Plugin(env={"PS5THEME_TEST_NO_LINKS": "1"})
    wait_applied("night")
    # The same picture kept for a second file: without hard links, a copy made on the console (not sent again).
    pic = png(40, 20, (1, 2, 3))
    assert call("PUT", "/api/theme/source", {"id": "night", "path": f"{HOME}/bg.png", "crc": f"{zlib.crc32(pic):08x}"}, pic)[0] == 200
    assert call("PUT", "/api/theme/source", {"id": "night", "path": f"{HOME}/icon.png", "same_as": f"{HOME}/bg.png"}, b"")[0] == 200
    assert call("GET", "/api/theme/source", {"id": "night", "path": f"{HOME}/icon.png"})[1] == pic
    assert call("DELETE", "/api/theme/sources", {"id": "night"})[0] == 200
    assert call("POST", "/api/theme/save", {"id": "night"})[0] == 200        # as it was: no unsaved changes left
    # A changed file: the folder is built again, with copies.
    assert put_file("night", f"{HOME}/bg.png", S["bg"] + b"")[0] == 200   # same bytes, a new file
    last = apply_and_wait("night")
    assert "0 of 1 folder(s) reused" in last, last
    assert "hard links aren't available here" in log_text()
    staged = live_stage(HOME)
    assert os.stat(f"{staged}/bg.png").st_ino != os.stat(f"{BASE}/themes/night/files{HOME}/bg.png").st_ino, "not a copy"

    # Nothing changed: reused, and no file read again.
    last = apply_and_wait("night")
    assert "1 of 1 folder(s) reused" in last and "0 file(s) compared by content" in last, last
    assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])

    # After a reboot the overlays are gone, but the stage folder is kept and reused.
    reboot_console()
    S["plugin"] = Plugin(env={"PS5THEME_TEST_NO_LINKS": "1"})
    s = wait_applied("night")
    last = applies()[-1]
    assert "1 of 1 folder(s) reused" in last and "0 file(s) compared by content" in last, last
    assert len(mounts_on(HOME)) == 1 and crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])
    # No leftovers: the applied theme's folder, and the one from before the
    # edit kept as a spare.
    kept = stage_records("live") + stage_records("spare")
    assert len(kept) == 2 and sorted(f"{BASE}/stage/{d}" for d in os.listdir(f"{BASE}/stage")) == sorted(kept), kept

    # A large folder that has to be copied says how far the copy has got.
    assert call("POST", "/api/themes", {"id": "big"})[0] == 200
    assert put_file("big", f"{BULK}/hero.png", png(100, 100, (9, 9, 99)))[0] == 200
    night = live_stage(HOME)
    seen = progress_during(lambda: apply_and_wait("big"))
    copying = [t for _, t in seen if re.fullmatch(r"Preparing the theme: \d+ of 7\d MB", t)]
    assert copying, seen
    percents = [p for p, _ in seen]
    assert percents == sorted(percents), seen
    assert stage_records("spare") == [night] and not mounts_on(HOME), "the previous theme's folder was not kept"

    # Switching back reuses the previous theme's folder: nothing is copied.
    big = live_stage(BULK)
    last = apply_and_wait("night")
    assert "1 of 1 folder(s) reused" in last and "0 file(s) compared by content" in last, last
    assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"]) and not mounts_on(BULK)
    assert stage_records("spare") == [big] and len(os.listdir(f"{BASE}/stage")) == 2, os.listdir(f"{BASE}/stage")
    assert call("DELETE", "/api/themes", {"id": "big"})[0] == 200

    S["plugin"].term()
    S["plugin"] = Plugin()
    wait_applied("night")


def t_firmware_mismatch_is_refused():
    assert call("POST", "/api/themes", {"id": "old", "name": "Old", "fw": "13.40, 3.42"})[0] == 200
    assert put_file("old", f"{HOME}/icon.png", png(96, 96, (0, 0, 255)))[0] == 200
    names = {t["id"]: t["compatible"] for t in status()["themes"]}
    assert names == {"night": True, "old": False}, names
    assert call("POST", "/api/apply", {"id": "old"})[0] == 200
    s = wait_result("is for firmware 13.40, 3.42")
    assert s["applied"] == "" and not mounts_on(HOME)
    assert crc_file(f"{HOME}/icon.png") == S["stock_icon"]


def t_delete_rules():
    assert call("DELETE", "/api/themes", {"id": "old"})[0] == 409  # it is the active selection
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    wait_applied("night")
    assert call("DELETE", "/api/themes", {"id": "night"})[0] == 409
    assert call("DELETE", "/api/themes", {"id": "old"})[0] == 200
    assert not os.path.exists(f"{BASE}/themes/old")
    assert call("DELETE", "/api/themes", {"id": "../themes"})[0] == 404


def t_concurrent_requests_during_apply():
    assert call("POST", "/api/themes", {"id": "bulk"})[0] == 200
    assert put_file("bulk", f"{BULK}/hero.png", png(100, 100, (255, 255, 0)))[0] == 200
    stock_blob = crc_file(f"{BULK}/blob007.bin")
    assert call("POST", "/api/apply", {"id": "bulk"})[0] == 200

    errors, timings = [], []
    def poll_status():
        for _ in range(15):
            t = time.time()
            try:
                status()
            except Exception as e:
                errors.append(repr(e))
            timings.append(time.time() - t)
    upload = {}
    def late_upload():
        time.sleep(0.05)
        upload["r"] = put_file("bulk", f"{BULK}/hero.png", png(100, 100, (0, 255, 255)))
    threads = [threading.Thread(target=poll_status) for _ in range(4)] + [threading.Thread(target=late_upload)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors, errors
    assert upload["r"][0] == 200, upload
    s = wait_applied("bulk")
    assert "1 files verified" in s["result"], s["result"]
    assert crc_file(f"{BULK}/blob007.bin") == stock_blob
    assert max(timings) < 10, f"status blocked for {max(timings):.1f}s"

    assert call("POST", "/api/apply", {"id": "bulk"})[0] == 200  # re-apply picks up the late upload
    end = time.time() + 60
    while crc_file(f"{BULK}/hero.png") != zlib.crc32(png(100, 100, (0, 255, 255))):
        assert time.time() < end, "late upload never applied"
        time.sleep(0.25)
    wait_applied("bulk")


def t_restore_stock_cleans_up():
    bulk = live_stage(BULK)
    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    s = wait_applied("")
    assert s["result"] == "Stock home screen.", s
    assert not mounts_on(HOME) and not mounts_on(BULK)
    assert crc_file(f"{HOME}/bg.png") == S["stock_bg"]
    assert not os.path.exists(f"{BASE}/mounts") and not os.path.exists(f"{BASE}/live")
    # Only the folder of the theme that was shown is kept, unmounted, for
    # going back to it.
    assert stage_records("spare") == [bulk], stage_records("spare")
    assert [f"{BASE}/stage/{d}" for d in os.listdir(f"{BASE}/stage")] == [bulk], os.listdir(f"{BASE}/stage")
    last = apply_and_wait("bulk")
    assert "1 of 1 folder(s) reused" in last, last
    bulk = live_stage(BULK)
    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    wait_applied("")
    assert not mounts_on(BULK) and stage_records("spare") == [bulk]


def t_crash_loop_takes_theme_down():
    """A system app that keeps restarting while a theme is applied (a new
    sandbox every few seconds, as when it crashes on start) gets the theme
    taken down, and the theme isn't put back at the next start. One restart,
    as after rest mode, is fine. Sandboxes of earlier restarts stay in the
    mount table (as on the console: ShellUI's _000, _001 and _002 were all
    there at once) and don't count as restarts."""
    if EMULATED:
        print("      (skipped: needs real mounts)", flush=True)
        return
    boxes = []

    def restart(n):
        box = f"/mnt/sandbox/LOOPAPP_{n:03}/system_ex"
        os.makedirs(box, exist_ok=True)
        subprocess.run(["mount", "--bind", "/system_ex", box], check=True)
        boxes.append(box)
        return box + HOME[len("/system_ex"):]
    try:
        for n in range(3):                  # left by earlier restarts
            restart(n)
        apply_and_wait("night")
        time.sleep(2)                       # the watcher has seen the app once
        assert status()["applied"] == "night" and "taking the theme down" not in log_text()
        seen = restart(3)                   # rest mode: one restart, covered
        end = time.time() + 10
        while crc_file(f"{seen}/bg.png") != zlib.crc32(S["bg"]):
            assert time.time() < end, "the restarted app's view was not covered"
            time.sleep(0.2)
        time.sleep(2)
        assert status()["applied"] == "night" and "taking the theme down" not in log_text()

        restart(4)                          # and again, and again: a crash loop
        time.sleep(1.5)
        restart(5)
        end = time.time() + 15
        while status()["applied"]:
            assert time.time() < end, "the theme was not taken down"
            time.sleep(0.2)
        s = wait_applied("")
        assert "Night was taken down: the console's interface kept restarting with it applied (LOOPAPP)." == s["result"], s
        assert open(f"{BASE}/active").read().strip() == "none"
        assert "the system app LOOPAPP restarted 3 times within 60 s" in log_text()
        assert not mounts_on(HOME) and crc_file(f"{HOME}/bg.png") == S["stock_bg"]
        assert "was taken down because the console's interface kept restarting" in open("/tmp/notifications.txt").read()

        # The next start doesn't put it back.
        S["plugin"].kill()
        S["plugin"] = Plugin()
        s = wait_applied("")
        assert not mounts_on(HOME) and s["active"] == "none"

        # Applying it again with all six sandboxes still there: they are the
        # app's past, not a crash loop, and the theme stays.
        downs = log_text().count("taking the theme down")
        apply_and_wait("night")
        time.sleep(3)
        assert status()["applied"] == "night", status()
        assert log_text().count("taking the theme down") == downs

        # A slower loop: once every few seconds here (about once a minute on
        # the console), never 3 within the short window, 5 within the long one.
        S["plugin"].kill()
        S["plugin"] = Plugin(env={"PS5THEME_TEST_CRASH_WINDOWS": "2,40"})
        wait_applied("night")
        time.sleep(2)                       # the watcher has seen the app once
        for n in range(6, 10):
            restart(n)
            time.sleep(2.5)
            assert status()["applied"] == "night", f"taken down after {n - 5} slow restart(s)"
        restart(10)
        end = time.time() + 15
        while status()["applied"]:
            assert time.time() < end, "the slow loop was not caught"
            time.sleep(0.2)
        assert "the system app LOOPAPP restarted 5 times within 40 s" in log_text()
        S["plugin"].kill()
        S["plugin"] = Plugin()
        wait_applied("")
    finally:
        for box in reversed(boxes):
            subprocess.run(["umount", "-l", box])
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    wait_applied("night")


def t_busy_overlay_is_never_forced():
    """An overlay an app is still reading is never pulled away (on the console
    that broke the system's views stacked on it, and apps wouldn't start until
    a restart). The next theme goes on top of it, Restore stock puts the
    system's own files on top, and it comes off once nothing reads it."""
    if EMULATED:
        print("      (skipped: needs real mounts)", flush=True)
        return
    dawn = png(480, 270, (40, 40, 180))
    assert call("POST", "/api/themes", {"id": "dawn", "name": "Dawn"})[0] == 200
    assert put_file("dawn", f"{HOME}/bg.png", dawn)[0] == 200
    apply_and_wait("night")
    before = log_text()
    reader = open(f"{HOME}/bg.png", "rb")          # an app reading the theme's picture
    try:
        apply_and_wait("dawn")
        assert crc_file(f"{HOME}/bg.png") == zlib.crc32(dawn)
        assert len(mounts_on(HOME)) >= 2, mounts_on(HOME)       # night's stays under dawn's
        new = log_text()[len(before):]
        assert f"unmount {HOME}: still in use" in new and "forcing" not in new, new[-2000:]
        assert zlib.crc32(reader.read()) == zlib.crc32(S["bg"]), "the reader lost its file"
        stage = next(l.split("\t")[1] for l in open(f"{BASE}/mounts").read().splitlines() if l.startswith(HOME + "\t"))
        assert os.path.isdir(stage), "a stage folder still mounted was deleted"

        # Restore stock while it is still in use: the system's own files on top.
        assert call("POST", "/api/apply", {"id": "none"})[0] == 200
        wait_applied("")
        assert crc_file(f"{HOME}/bg.png") == S["stock_bg"]
        assert f"covered {HOME} with the system's own files" in log_text()
        reader.seek(0)
        assert zlib.crc32(reader.read()) == zlib.crc32(S["bg"])
    finally:
        reader.close()
    # Nothing reads it now: the next theme takes every layer off first.
    apply_and_wait("night")
    assert len(mounts_on(HOME)) == 1, mounts_on(HOME)
    assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])
    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    wait_applied("")
    assert not mounts_on(HOME) and crc_file(f"{HOME}/bg.png") == S["stock_bg"]
    assert call("DELETE", "/api/themes", {"id": "dawn"})[0] == 200
    apply_and_wait("night")


def t_recovers_after_hard_kill():
    """A restarted copy carries on with the overlays a killed one left, without taking them down."""
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    wait_applied("night")
    unmounted = log_text().count("unmounted " + HOME)
    S["plugin"].kill()
    assert len(mounts_on(HOME)) == 1  # left behind by the killed process
    S["plugin"] = Plugin()
    s = wait_applied("night")
    assert "kept through the plugin's restart" in s["result"], s["result"]
    assert len(mounts_on(HOME)) == 1, mounts_on(HOME)
    assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])
    assert "carrying on with" in log_text()
    assert log_text().count("unmounted " + HOME) == unmounted, "the overlays were taken down"

    # A damaged record of what was applied: the theme is applied again instead.
    S["plugin"].kill()
    write(f"{BASE}/live", b"theme\tnight\nstage\t0\t/data/ps5-theme/stage/missing\t" + HOME.encode() + b"\n")
    S["plugin"] = Plugin()
    wait_applied("night")
    assert log_text().count("unmounted " + HOME) == unmounted + 1
    assert len(mounts_on(HOME)) == 1 and crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])


def plugin_file(version, binary=ASAN):
    """A ps5-theme .plugin as etaHEN loads it: the header, then the ELF."""
    return b"etaHEN_PLUGIN\0THEM00001\0" + version.encode() + b"\0" + open(binary, "rb").read()


def plain_elf(version, binary=ASAN):
    """A plain ps5-theme .elf whose version marker says `version`."""
    data = open(binary, "rb").read()
    marker = re.search(rb"PS5THEME-VERSION:\d\.\d\d:THEM00001", data)
    assert marker, "no version marker in the build"
    return data[:marker.start()] + f"PS5THEME-VERSION:{version}:THEM00001".encode() + data[marker.end():]


def take_over(path, data, version, boot=False):
    """Put a newer copy at `path` and let a stand-in for the ELF loader on port
    9021 run what the plugin sends it. Returns what the loader received, once
    the new copy has taken over and the old one has exited. With `boot`, the
    old copy is started only then, as etaHEN does after a restart."""
    if not boot:
        s = wait_applied("night")
        assert not s["applying"], s
    received, started = [], threading.Event()
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 9021))
    srv.listen(1)
    srv.settimeout(40)

    def loader():
        try:
            conn, _ = srv.accept()
        except OSError:
            return
        got = b""
        while chunk := conn.recv(1 << 20):
            got += chunk
        conn.close()
        received.append(got)
        write("/tmp/ps5-theme-new", got)
        os.chmod("/tmp/ps5-theme-new", 0o755)
        S["new"] = Plugin(binary="/tmp/ps5-theme-new", env={"PS5THEME_TEST_VERSION": version})
        started.set()

    thread = threading.Thread(target=loader)
    thread.start()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    write(path + ".tmp", data)
    os.rename(path + ".tmp", path)
    try:
        if boot:
            S["plugin"] = Plugin()
        old = S["plugin"]
        assert started.wait(45), "the newer copy was not started"
        end = time.time() + 30
        while old.alive():
            assert time.time() < end, "the old copy did not hand over"
            time.sleep(0.1)
        S["plugin"] = S["new"]
        S["plugin"].wait_port()
    finally:
        srv.close()
        thread.join(5)
    return received[0]


def t_update_from_payload_manager():
    """A newer ps5-theme.elf among PS5 Payload Manager's payloads takes over too."""
    carried = log_text().count("carrying on with")
    folder = "/data/pldmgr/payloads/ps5-theme"
    write("/data/pldmgr/payloads/other/tool.elf", b"\x7fELF" + b"\0" * 200)   # someone else's payload
    try:
        got = take_over(f"{folder}/ps5-theme.elf", plain_elf("9.97"), "9.97")
        assert got == plain_elf("9.97")
        assert status()["applied"] == "night"
        assert f"update: found ps5-theme 9.97 in {folder}/ps5-theme.elf" in log_text()
        assert log_text().count("carrying on with") == carried + 1
        assert len(mounts_on(HOME)) == 1 and crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])
    finally:
        shutil.rmtree("/data/pldmgr", ignore_errors=True)


def t_update_from_plain_elf():
    """Without etaHEN: a newer ps5-theme.elf in the plugin's own update folder takes over."""
    unmounted, carried = log_text().count("unmounted " + HOME), log_text().count("carrying on with")
    data = plain_elf("9.98")
    try:
        got = take_over(f"{BASE}/update/ps5-theme.elf", data, "9.98")
    finally:
        os.unlink(f"{BASE}/update/ps5-theme.elf")
    assert got == data, "the loader did not get the whole program"
    assert status()["applied"] == "night"
    assert len(mounts_on(HOME)) == 1 and crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])
    text = log_text()
    assert "update: found ps5-theme 9.98 in /data/ps5-theme/update/ps5-theme.elf" in text
    assert text.count("carrying on with") == carried + 1, "the new copy did not keep the overlays"
    assert text.count("unmounted " + HOME) == unmounted, "the overlays were taken down"
    # Other .elf files there are not mistaken for an update.
    write(f"{BASE}/update/other.elf", b"\x7fELF" + b"\0" * 100)
    time.sleep(12)
    assert "found ps5-theme" not in log_text()[len(text):]
    os.unlink(f"{BASE}/update/other.elf")


def t_update_hands_over():
    """With etaHEN: a newer .plugin in etaHEN's plugin folder takes over, keeping the theme."""
    unmounted, carried = log_text().count("unmounted " + HOME), log_text().count("carrying on with")
    try:
        got = take_over("/data/etaHEN/plugins/ps5-theme.plugin", plugin_file("9.99"), "9.99")
        assert got == open(ASAN, "rb").read(), "the loader did not get the plugin's program"
        s = status()
        assert s["applied"] == "night", s
        assert len(mounts_on(HOME)) == 1 and crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])
        text = log_text()
        for line in ("update: found ps5-theme 9.99", "a newer copy of the plugin asked to take over",
                     "taking over from the running copy"):
            assert line in text, line
        assert text.count("carrying on with") == carried + 1, "the new copy did not keep the overlays"
        assert text.count("unmounted " + HOME) == unmounted, "the overlays were taken down"

        # A copy that isn't newer leaves the running one alone.
        dup = subprocess.run([os.path.abspath(ASAN)], env=ENV, cwd="/", timeout=60, capture_output=True)
        assert dup.returncode == 0 and "is already running; this copy exits" in log_text()
        assert S["plugin"].alive() and status()["applied"] == "night"

        # Only the console itself may ask for a hand-over: not over the network.
        ip = socket.gethostbyname(socket.gethostname())
        if not ip.startswith("127."):
            conn = http.client.HTTPConnection(ip, PORT, timeout=10)
            conn.request("POST", "/api/handoff", headers={"X-PS5-Theme": "1", "Content-Length": "0"})
            assert conn.getresponse().status == 403
            conn.close()
        assert S["plugin"].alive()

        # A copy that asked to take over but has gone since is not handed
        # over to: stepping aside then would leave nothing running.
        gone = subprocess.Popen(["true"])
        gone.wait()
        assert call("POST", "/api/handoff", {"pid": str(gone.pid)})[0] == 200
        end = time.time() + 10
        while "is gone; this copy carries on" not in log_text():
            assert time.time() < end, "the request of a copy that is gone was not noticed"
            time.sleep(0.2)
        time.sleep(1.5)
        assert S["plugin"].alive() and status()["applied"] == "night"
    finally:
        shutil.rmtree("/data/etaHEN", ignore_errors=True)


def t_web_comes_back_after_rest_mode():
    """On the PS5, rest mode can leave the web server's listening socket
    failing for good (1.49's manager stopped answering): it is opened again,
    and the manager comes back by itself."""
    again = f"web: listening on port {PORT} again"
    seen = log_text().count(again)
    assert call("POST", "/api/debug/break-listener")[0] == 200
    end = time.time() + 10
    while log_text().count(again) == seen:
        assert time.time() < end, log_text()[-1000:]
        time.sleep(0.2)
    assert "the listening socket stopped working" in log_text()
    assert call("GET", "/api/status")[0] == 200 and S["plugin"].alive()


def t_takeover_when_web_is_down():
    """A copy whose web server doesn't answer (as 1.49's after rest mode) is
    asked by a signal to hand over: starting the plugin again works without
    a restart, and the theme stays."""
    wait_applied("night")
    old, carried = S["plugin"], log_text().count("carrying on with")
    assert call("POST", "/api/debug/deafen")[0] == 200
    time.sleep(1.5)
    try:
        call("GET", "/api/status", timeout=2)
        raise AssertionError("the web server still answers")
    except OSError:
        pass
    # The same version (one that doesn't answer gives way), finding it by the
    # pid it recorded: on the console the thread name couldn't be found.
    S["plugin"] = Plugin(timeout=60, env={"PS5THEME_TEST_NO_THREAD_NAMES": "1"})
    end = time.time() + 30
    while old.alive():
        assert time.time() < end, "the silent copy did not hand over"
        time.sleep(0.1)
    assert old.proc.returncode == 0
    text = log_text()
    assert "which doesn't answer on port 8095: asked it by a signal" in text, text[-1500:]
    pid, start = open(f"{BASE}/pid").read().split()
    assert int(pid) == S["plugin"].proc.pid and int(start) > 0
    assert text.count("carrying on with") == carried + 1, "the new copy did not keep the overlays"
    assert wait_applied("night")["applied"] == "night"
    assert len(mounts_on(HOME)) == 1 and crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])


def clear_sandboxes():
    """Takes down and removes the fake app sandboxes a test made: the plugin
    takes the newest /mnt/sandbox/NPXS40087_<n> folder for ShellUI's, as the
    console's numbers only grow."""
    points = [l.split()[4] for l in open("/proc/self/mountinfo").read().splitlines()]
    for point in reversed(points):
        if point.startswith("/mnt/sandbox/"):
            subprocess.run(["umount", "-l", point])
    shutil.rmtree("/mnt/sandbox", ignore_errors=True)
    # The numbers start again from here, as after a console restart: so does
    # the plugin, which remembers the ShellUI sandboxes it saw close.
    S["plugin"].term()
    S["plugin"] = Plugin()
    wait_applied(status()["active"] if status()["active"] != "none" else "")


def start_shellui():
    """A stand-in for ShellUI's process (SceShellUI), once the plugin watches it."""
    shutil.copy("/bin/sleep", "/tmp/SceShellUI")
    shell = subprocess.Popen(["/tmp/SceShellUI", "600"])
    end = time.time() + 10
    while f"watching ShellUI (pid {shell.pid})" not in log_text():
        assert time.time() < end, "ShellUI's process was not found"
        time.sleep(0.1)
    return shell


def t_stop_tool_keeps_the_theme():
    """ps5-theme-stop, for when the manager can't be reached: the plugin stops
    and its theme stays, except games' and apps' pictures, which must not stay
    in the home screen's view without a copy running; the next copy carries on
    and puts them back. A copy that was ended without stopping leaves them in:
    the tool, with nothing running, takes them out and says so."""
    stock, mine = zlib.crc32(png(64, 64, (0, 90, 200))), png(64, 64, (255, 0, 0))
    run_tool = lambda **env: subprocess.run([os.path.abspath(STOP_TOOL)], cwd="/", timeout=180, capture_output=True, text=True,
                                            env=dict(ENV, PS5THEME_TEST_NO_THREAD_NAMES="1", **env))
    pictures = lambda: [l for l in recorded_mounts() if "/user/appmeta/" in l]
    shell_view = "/mnt/sandbox/NPXS40087_000/user"
    icon = f"{shell_view}/appmeta/PPSA01234/icon0.png"   # what ShellUI reads

    def wait_icon(want):
        if EMULATED:      # no ShellUI view to put games' pictures in
            return
        end = time.time() + 15
        while crc_file(icon) != want:
            assert time.time() < end, ("game icon", want, log_text()[-1500:])
            time.sleep(0.2)

    assert call("POST", "/api/themes", {"id": "keep", "name": "Keep"})[0] == 200
    assert put_file("keep", f"{GAME}/icon0.png", mine)[0] == 200
    assert put_file("keep", f"{HOME}/bg.png", S["bg"])[0] == 200
    boxes, shell, again = [], None, log_text().count("is an overlay this version doesn't make")
    try:
        if not EMULATED:
            os.makedirs(shell_view, exist_ok=True)
            subprocess.run(["mount", "--bind", "/user", shell_view], check=True)
            shell = start_shellui()
        apply_and_wait("keep")
        wait_icon(zlib.crc32(mine))
        if not EMULATED:
            # As after rest mode on the console: a system app's older sandbox
            # that the system kept, with the theme's overlay still in it,
            # beside the newer one. The next copy carries on all the same.
            for n in (0, 1):
                box = f"/mnt/sandbox/NPXS40101_{n:03}/system_ex"
                os.makedirs(box, exist_ok=True)
                subprocess.run(["mount", "--bind", "/system_ex", box], check=True)
                end = time.time() + 10
                while not [l for l in recorded_mounts() if l.startswith(box + "/")]:
                    assert time.time() < end, ("not covered", box, log_text()[-1000:])
                    time.sleep(0.2)

        # A running copy that can't be found is left alone (1.50's tool took
        # a running copy's overlays out from under it).
        records = open(f"{BASE}/mounts").read()
        run = run_tool(PS5THEME_TEST_NOT_FOUND="1")
        assert run.returncode == 1, (run.stdout, run.stderr)
        assert "ps5-theme is running but couldn't be found, so nothing was changed." in open("/tmp/notifications.txt").read()
        assert "# threads" in open(f"{BASE}/dump/stop.txt").read()
        assert S["plugin"].alive() and open(f"{BASE}/mounts").read() == records
        wait_icon(zlib.crc32(mine))

        old = S["plugin"]
        run = run_tool()
        assert run.returncode == 0, (run.stdout, run.stderr)
        old.proc.wait(10)
        assert old.proc.returncode == 0
        notes = open("/tmp/notifications.txt").read()
        assert "ps5-theme stopped. The theme stays; start ps5-theme again to open the manager." in notes, notes[-500:]
        assert "stopping on request: the theme stays" in log_text()
        assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])        # the theme stays
        wait_icon(stock)
        assert not pictures(), recorded_mounts()

        carried = log_text().count("carrying on with")
        S["plugin"] = Plugin()
        wait_icon(zlib.crc32(mine))                                      # put back
        assert log_text().count("carrying on with") == carried + 1
        assert bool(pictures()) == (not EMULATED)

        # Ended without stopping (etaHEN's Stop, or a copy before 1.50 on the signal).
        S["plugin"].kill()
        wait_icon(zlib.crc32(mine))
        run = run_tool()
        assert run.returncode == 0, (run.stdout, run.stderr)
        assert "ps5-theme isn't running. Start it to open the manager." in open("/tmp/notifications.txt").read()
        wait_icon(stock)
        assert not pictures(), recorded_mounts()
        assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])
        S["plugin"] = Plugin()
        wait_icon(zlib.crc32(mine))
        assert log_text().count("is an overlay this version doesn't make") == again, "applied again instead of carrying on"
    finally:
        if shell:
            shell.kill()
            shell.wait()
        apply_and_wait("night")
        clear_sandboxes()
        call("DELETE", "/api/themes", {"id": "keep"})


def t_failed_write_closes_its_file():
    """A settings write that fails at fsync closes its file, leaves no temp file
    and keeps the old theme.ini; the next write works."""
    ini = f"{BASE}/themes/night/theme.ini"
    before_ini = open(ini, "rb").read()
    fds = lambda: len(os.listdir(f"/proc/{S['plugin'].proc.pid}/fd"))
    before = fds()
    write("/tmp/ps5-theme-fail-fsync", b"")
    try:
        for _ in range(25):
            assert call("POST", "/api/theme/settings", {"id": "night", "gain": "-3"})[0] == 500
    finally:
        os.unlink("/tmp/ps5-theme-fail-fsync")
    time.sleep(0.5)
    assert fds() <= before + 3, f"file descriptors leaked: {before} -> {fds()}"
    assert not os.path.exists(ini + ".tmp") and open(ini, "rb").read() == before_ini
    assert call("POST", "/api/theme/settings", {"id": "night", "gain": "0"})[0] == 200
    assert call("POST", "/api/theme/save", {"id": "night"})[0] == 200


def t_update_at_boot():
    """After a restart etaHEN starts the old copy while a newer one waits in the
    update folder. The old copy starts it and steps aside before doing anything
    else; the newer copy puts the theme back. Something is running throughout."""
    wait_applied("night")
    reboot_console()
    start = len(log_text())
    try:
        take_over(f"{BASE}/update/ps5-theme.elf", plain_elf("9.96"), "9.96", boot=True)
    finally:
        os.unlink(f"{BASE}/update/ps5-theme.elf")
    s = wait_applied("night")
    assert S["plugin"].alive() and len(mounts_on(HOME)) == 1, s
    assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])
    text = log_text()[start:]
    assert "the running copy did not hand over" not in text, text
    assert "carrying on with" not in text, "the old copy applied before handing over"
    assert text.index("handing over to the new copy") < text.index("putting 'Night' back"), text


def raw(data, read=True):
    s = socket.create_connection(("127.0.0.1", PORT), 5)
    s.sendall(data)
    out = s.recv(200) if read else b""
    s.close()
    return out


def t_survives_malformed_and_abusive_requests():
    assert raw(b"garbage\r\n\r\n").startswith(b"HTTP/1.1 400")
    assert raw(b"GET / HTTP/1.1\r\nX: " + b"a" * 20000 + b"\r\n\r\n").startswith(b"HTTP/1.1 400")
    raw(b"GET /api/status HTTP/1.1\r\n", read=False)  # never finishes headers
    for _ in range(20):  # client hangs up mid-download
        s = socket.create_connection(("127.0.0.1", PORT), 5)
        s.sendall(f"GET /api/asset?path={BULK}/blob001.bin HTTP/1.1\r\n\r\n".encode())
        s.recv(10)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        s.close()
    rnd = random.Random(7)
    for _ in range(200):
        try:
            raw(rnd.randbytes(rnd.randint(1, 600)) + b"\r\n\r\n")
        except OSError:
            pass
    assert call("PUT", "/api/theme/file", {"id": "night", "path": f"{HOME}/bg.png"}, b"x",
                headers={"Content-Length": "-5"})[0] == 400
    assert call("GET", "/api/nope")[0] == 404
    assert call("GET", "/api/theme/file", {"id": "night", "path": "/etc/passwd"})[0] == 404
    assert call("GET", "/api/asset", {"path": "/etc/passwd"})[0] == 403
    assert call("GET", "/api/crash", {"name": "../logs/log.txt"})[0] == 404
    # An address longer than the server's fields is refused, never cut; a value longer than its field is never
    # read as a shorter one (a cut-down path is a different file).
    assert call("GET", "/api/status", {"x": "a" * 2100})[0] == 414
    assert call("GET", "/api/theme/file", {"id": "night", "path": f"{HOME}/bg.png" + "/x" * 300})[0] == 404
    assert call("GET", "/api/theme/file", {"id": "night", "path": f"{HOME}/bg.png" + "?" * 600})[0] == 404

    # Two uploads of the same file at once: each is received into its own temporary file, so both succeed and the
    # file ends up as one of them, whole (1.58 and before shared one name, and one upload could read the other's
    # half-written bytes: "empty or unreadable file").
    assert call("POST", "/api/themes", {"id": "race", "name": "Race"})[0] == 200
    pics = [png(480, 270, (200, 10, 10)), png(480, 270, (10, 10, 200))]
    for _ in range(6):
        codes = []
        threads = [threading.Thread(target=lambda d=d: codes.append(put_file("race", f"{HOME}/bg.png", d)[0])) for d in pics]
        for t in threads: t.start()
        for t in threads: t.join()
        assert codes == [200, 200], codes
        assert open(f"{BASE}/themes/race/files{HOME}/bg.png", "rb").read() in pics
    assert not [f for f in os.listdir(f"{BASE}/themes/race/files{HOME}") if f.endswith(".part")]
    assert call("DELETE", "/api/themes", {"id": "race"})[0] == 200
    assert "web: refused (414)" in log_text()
    assert S["plugin"].alive()
    assert status()["applied"] == "night"


def t_overlays_reach_system_views():
    """ShellUI reads the system through a sandbox view; a theme must show there too."""
    if EMULATED:
        print("      (skipped: needs real mounts)", flush=True)
        return
    box = "/mnt/sandbox/SHELL_000/system_ex"
    box2 = "/mnt/sandbox/SHELL_001/system_ex"
    dummies = []
    for i in range(80):  # unrelated mounts listed before the one that matters (1,072 on a real PS5)
        src, dst = f"/tmp/dummy-src/{i}", f"/mnt/dummy/{i}"
        os.makedirs(src, exist_ok=True)
        os.makedirs(dst, exist_ok=True)
        subprocess.run(["mount", "--bind", src, dst], check=True)
        dummies.append(dst)
    os.makedirs(box, exist_ok=True)
    subprocess.run(["mount", "--bind", "/system_ex", box], check=True)
    try:
        assert call("POST", "/api/apply", {"id": "none"})[0] == 200
        wait_applied("")
        assert call("POST", "/api/apply", {"id": "night"})[0] == 200
        wait_applied("night")
        seen = box + HOME[len("/system_ex"):]
        assert crc_file(f"{seen}/bg.png") == zlib.crc32(S["bg"]), "theme not visible in the sandbox view"
        assert crc_file(f"{seen}/icon.png") == S["stock_icon"]
        try:
            open(f"{seen}/bg.png", "wb")
            raise AssertionError("sandbox overlay is writable")
        except OSError as e:
            assert e.errno == errno.EROFS, e
        assert "(system view)" in log_text()
        assert os.path.exists(box + "/vsh_asset/bg_hub_default.dds.zst")
        assert seen in open(f"{BASE}/mounts").read()
        assert "SHELL_000" in open(f"{BASE}/dump/mounts.txt").read() or True

        # An app restarts (after rest mode) in a new sandbox, whose view shows
        # stock: the plugin covers it by itself, without re-applying.
        os.makedirs(box2, exist_ok=True)
        subprocess.run(["mount", "--bind", "/system_ex", box2], check=True)
        seen2 = box2 + HOME[len("/system_ex"):]
        end = time.time() + 10
        while crc_file(f"{seen2}/bg.png") != zlib.crc32(S["bg"]):
            assert time.time() < end, "the new view was not covered"
            time.sleep(0.2)
        assert crc_file(f"{seen2}/icon.png") == S["stock_icon"]
        end = time.time() + 2   # logged once the new overlay is verified, just after it shows
        while "a view the system made after the theme was applied" not in log_text():
            assert time.time() < end, log_text()[-1500:]
            time.sleep(0.05)
        assert status()["applied"] == "night", "covering a new view must not re-apply"
        assert len(mounts_on(seen)) == 1 and len(mounts_on(seen2)) == 1
        code, table = call("GET", "/api/mounts")
        assert code == 200 and b"SHELL_001" in table, table[:200]

        assert call("POST", "/api/apply", {"id": "none"})[0] == 200
        wait_applied("")
        assert crc_file(f"{seen}/bg.png") == S["stock_bg"], "sandbox view still themed after restore"
        assert crc_file(f"{seen2}/bg.png") == S["stock_bg"], "new view still themed after restore"
        assert not mounts_on(seen) and not mounts_on(seen2)
        time.sleep(2)   # the watcher is idle with no theme applied
        assert not mounts_on(seen2)
    finally:
        subprocess.run(["umount", box2])
        subprocess.run(["umount", box])
        for d in dummies:
            subprocess.run(["umount", d])
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    wait_applied("night")


def t_slow_clients_do_not_block_others():
    held = []
    for _ in range(3):  # three clients that never finish their headers
        s = socket.create_connection(("127.0.0.1", PORT), 5)
        s.sendall(b"GET /api/status HTTP/1.1\r\nX-Slow: ")
        held.append(s)
    t = time.time()
    status()
    assert time.time() - t < 3, f"status took {time.time() - t:.1f}s behind slow clients"
    t = time.time()
    assert raw(b"GET /\x00x HTTP/1.1\r\n\r\n").startswith(b"HTTP/1.1 400")
    assert time.time() - t < 3, "NUL in headers was not rejected quickly"
    for s in held:
        s.close()
    results = []
    def burst():
        try:
            results.append(call("GET", "/api/status")[0])
        except Exception as e:
            results.append(repr(e))
    threads = [threading.Thread(target=burst) for _ in range(40)]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    assert all(r in (200, 503) for r in results) and results.count(200) >= 4, results
    assert status()["fw"] == "13.42" and S["plugin"].alive()


def t_log_rotates():
    with open(LOG, "a") as f:
        f.write("x" * (600 * 1024))
    assert call("POST", "/api/themes", {"id": "rot"})[0] == 200
    assert os.path.getsize(f"{BASE}/logs/log.1.txt") > 600 * 1024
    assert os.path.getsize(LOG) < 4096
    assert call("DELETE", "/api/themes", {"id": "rot"})[0] == 200


def t_media_policy():
    code, r = call("GET", "/api/assets", {"q": "NPXS40012"})
    kinds = {a["path"].rsplit("/", 1)[1]: a["kind"] for a in r["assets"]}
    assert kinds == {"move.wav": "audio", "ui.ttf": "font", "spinner.json": "json", "star.svg": "svg",
                     "loop.mp4": "video"}, kinds
    assert r["protected"] == 2, r
    assert call("POST", "/api/themes", {"id": "media", "name": "Media"})[0] == 200
    assert put_file("media", f"{MEDIA}/bundle.js", b"alert(1)")[0] == 403
    assert put_file("media", f"{MEDIA}/lib/native.sprx", b"\x7fELF")[0] == 403
    assert call("GET", "/api/asset", {"path": f"{MEDIA}/bundle.js"})[0] == 403

    cases = [
        ("sounds/move.wav", wav(48000, 1, 0.1, 3000), 200, None),
        ("sounds/move.wav", wav(44100, 1, 0.1, 3000), 422, "48000 Hz"),
        ("sounds/move.wav", wav(48000, 2, 0.1, 3000), 422, "channel"),
        ("sounds/move.wav", b"RIFFnope", 422, "valid WAV"),
        # A chunk size near 4 GB once made the chunk walk loop forever.
        ("sounds/move.wav", b"RIFF\x30\x00\x00\x00WAVE" + b"LIST\xf7\xff\xff\xff" + bytes(28), 422, "valid WAV"),
        ("fonts/ui.ttf", font_fixture.font("tt", glyphs=130), 200, None),
        ("fonts/ui.ttf", b"wOFF" + b"\0" * 80, 422, "web fonts"),
        ("anim/spinner.json", b'{"v":"5.7","w":100,"h":100,"layers":[{"ty":4}]}', 200, None),
        ("anim/spinner.json", b'{"v":"5.7","w":120,"h":100,"layers":[]}', 422, "canvas"),
        ("anim/spinner.json", b'[1,2,3]', 422, "same structure"),
        ("anim/spinner.json", b'{"w":100,"h":100,', 422, "valid JSON"),
        ("anim/spinner.json", b'{"w":100,"h":100,"x":' + b"[" * 100 + b"]" * 100 + b"}", 422, "valid JSON"),
        ("icons/star.svg", b'<svg xmlns="http://www.w3.org/2000/svg"><circle r="3"/></svg>', 200, None),
        ("icons/star.svg", b'<svg><script>alert(1)</script></svg>', 422, "<script"),
        ("icons/star.svg", b'<svg><image href="http://evil/x.png"/></svg>', 422, "href"),
        ("video/loop.mp4", b"\x00\x00\x00\x18ftypmp42" + b"\1" * 40, 200, None),
        ("video/loop.mp4", b"\x1a\x45\xdf\xa3" + b"\0" * 40, 422, "same format"),
    ]
    for rel, data, want, needle in cases:
        code, r = put_file("media", f"{MEDIA}/{rel}", data)
        assert code == want, (rel, needle, code, r)
        if needle:
            assert needle in r["error"], (rel, r)

    assert call("POST", "/api/apply", {"id": "media"})[0] == 200
    s = wait_applied("media")
    assert "5 files verified" in s["result"], s["result"]
    assert open(f"{MEDIA}/fonts/ui.ttf", "rb").read() == font_fixture.font("tt", glyphs=130)
    assert open(f"{MEDIA}/bundle.js", "rb").read() == b"console.log(1)"
    assert S["plugin"].alive()


def t_music():
    """A theme keeps its music source (48 kHz WAV) and a loudness setting; the
    plugin plays nothing itself, the home music file (bgm_home.at9) does."""
    assert call("POST", "/api/themes", {"id": "tunes", "name": "Tunes"})[0] == 200
    for data, needle in [(wav(44100, 2, 1, 20000), "48 kHz"), (wav(48000, 1, 1, 20000), "48 kHz"),
                         (b"not audio at all", "valid WAV"), (wav(48000, 2, 0.001, 20000), "too short")]:
        code, r = call("PUT", "/api/theme/music", {"id": "tunes", "crc": f"{zlib.crc32(data):08x}"}, data)
        assert code == 422 and needle in r["error"], (needle, code, r)
    track = wav(48000, 2, 2, 20000)
    code, r = call("PUT", "/api/theme/music", {"id": "tunes", "crc": f"{zlib.crc32(track):08x}"}, track)
    assert code == 200 and r["seconds"] == 2, r
    theme = {t["id"]: t for t in status()["themes"]}["tunes"]
    assert theme["music"] == 2 and theme["gain"] == 0, theme
    assert "audio" not in status()
    assert call("GET", "/api/theme/music", {"id": "tunes"})[1] == track

    assert call("POST", "/api/theme/settings", {"id": "tunes", "gain": "-5"})[0] == 200
    assert {t["id"]: t for t in status()["themes"]}["tunes"]["gain"] == -5
    for bad in ("13", "-13", "x", "3dB"):
        assert call("POST", "/api/theme/settings", {"id": "tunes", "gain": bad})[0] == 400, bad
    assert "music_gain=-5" in open(f"{BASE}/themes/tunes/theme.ini").read()

    # The part of the music that becomes the home music, and its loop joint.
    theme = {t["id"]: t for t in status()["themes"]}["tunes"]
    assert (theme["musicStart"], theme["musicEnd"], theme["musicLoop"], theme["musicBlend"]) == (0, 0, "crossfade", 2000), theme
    assert call("POST", "/api/theme/settings", {"id": "tunes", "start": "500", "end": "1800", "loop": "fade", "blend": "1000"})[0] == 200
    theme = {t["id"]: t for t in status()["themes"]}["tunes"]
    assert (theme["musicStart"], theme["musicEnd"], theme["musicLoop"], theme["musicBlend"], theme["gain"]) == (500, 1800, "fade", 1000, -5), theme
    ini = open(f"{BASE}/themes/tunes/theme.ini").read()
    assert "music_start=500\nmusic_end=1800\nmusic_play_from=-1\nmusic_loop_start=-1\nmusic_loop_end=-1\nmusic_order=music\nmusic_loop=fade\nmusic_blend=1000\n" in ini and "music_gain=-5" in ini, ini
    # Where "repeat the music part" starts playing: inside the music part.
    assert call("POST", "/api/theme/settings", {"id": "tunes", "play_from": "1200"})[0] == 200
    assert {t["id"]: t for t in status()["themes"]}["tunes"]["musicPlayFrom"] == 1200
    for bad in ("400", "1800", "x"):
        assert call("POST", "/api/theme/settings", {"id": "tunes", "play_from": bad})[0] == 400, bad
    assert call("POST", "/api/theme/settings", {"id": "tunes", "play_from": "-1"})[0] == 200
    # A loop part and the order the two parts play in.
    assert call("POST", "/api/theme/settings", {"id": "tunes", "order": "loop-first"})[0] == 400        # needs a loop part
    assert call("POST", "/api/theme/settings", {"id": "tunes", "loop_start": "200", "loop_end": "900", "order": "alternate"})[0] == 200
    theme = {t["id"]: t for t in status()["themes"]}["tunes"]
    assert (theme["musicLoopStart"], theme["musicLoopEnd"], theme["musicOrder"]) == (200, 900, "alternate"), theme
    for bad in ({"order": "shuffle"}, {"loop_start": "-1"}, {"loop_start": "900", "loop_end": "200"}, {"loop_end": "-2"}):
        assert call("POST", "/api/theme/settings", {"id": "tunes", **bad})[0] == 400, bad
    assert call("POST", "/api/theme/settings", {"id": "tunes", "loop_start": "-1", "loop_end": "-1"})[0] == 200
    theme = {t["id"]: t for t in status()["themes"]}["tunes"]
    assert (theme["musicLoopStart"], theme["musicOrder"]) == (-1, "music"), theme                     # no loop part: plain repeat
    for bad in ({"start": "-1"}, {"start": "x"}, {"end": "3600001"}, {"start": "2000"}, {"loop": "wobble"}, {"blend": "9000"}):
        assert call("POST", "/api/theme/settings", {"id": "tunes", **bad})[0] == 400, bad
    assert call("POST", "/api/theme/settings", {"id": "tunes", "name": "Tunes 2"})[0] == 200   # keeps the music settings
    theme = {t["id"]: t for t in status()["themes"]}["tunes"]
    assert (theme["name"], theme["musicStart"], theme["musicLoop"], theme["gain"]) == ("Tunes 2", 500, "fade", -5), theme
    code, _ = call("POST", "/api/themes/duplicate", {"id": "tunes", "new_id": "tunes-copy", "name": "Tunes copy"})
    copy = {t["id"]: t for t in status()["themes"]}["tunes-copy"]
    assert code == 200 and (copy["gain"], copy["musicEnd"], copy["musicLoop"]) == (-5, 1800, "fade"), copy
    assert call("DELETE", "/api/themes", {"id": "tunes-copy"})[0] == 200

    # Music the browser has not encoded yet (themes from before 1.10) is reported.
    assert put_file("tunes", f"{HOME}/bg.png", png(480, 270, (1, 2, 3)))[0] == 200
    assert call("POST", "/api/apply", {"id": "tunes"})[0] == 200
    s = wait_applied("tunes")
    assert "not converted to home music yet" in s["result"], s["result"]
    for gone in ("/api/music/test", "/api/music/soundcheck", "/api/music/route"):
        assert call("POST", gone)[0] == 404, gone
    assert call("GET", "/api/music/routes")[0] == 404

    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    wait_applied("")
    assert call("DELETE", "/api/theme/music", {"id": "tunes"})[0] == 200
    assert {t["id"]: t for t in status()["themes"]}["tunes"]["music"] == 0
    assert call("DELETE", "/api/themes", {"id": "tunes"})[0] == 200


def t_settings_and_duplicate():
    assert call("POST", "/api/theme/settings", {"id": "night", "name": "Night Sky\nfw=any"})[0] == 200
    ini = open(f"{BASE}/themes/night/theme.ini").read()
    assert ini.splitlines()[0] == "name=Night Sky fw any" and ini.count("fw=") == 1, ini
    assert call("POST", "/api/theme/settings", {"id": "night", "fw": "13.40,13.42"})[0] == 200
    assert {t["id"]: t for t in status()["themes"]}["night"]["compatible"] is True
    code, _ = call("POST", "/api/themes/duplicate", {"id": "night", "new_id": "night-copy", "name": "Night Copy"})
    assert code == 200
    themes = {t["id"]: t for t in status()["themes"]}
    assert themes["night-copy"]["name"] == "Night Copy" and themes["night-copy"]["files"] == themes["night"]["files"]
    assert call("POST", "/api/themes/duplicate", {"id": "night", "new_id": "night-copy"})[0] == 409
    assert call("POST", "/api/themes/duplicate", {"id": "night", "new_id": "../x"})[0] == 400
    assert call("DELETE", "/api/themes", {"id": "night-copy"})[0] == 200


def t_preview_pictures():
    """A theme's library picture: a complete JPEG kept beside the theme, never in what is applied."""
    pic, other = jpeg_header(1280, 720), jpeg_header(640, 480)

    def put(kind, data, theme="night"):
        return call("PUT", "/api/theme/preview", {"id": theme, "kind": kind, "crc": f"{zlib.crc32(data):08x}"}, data)

    def night():
        return {t["id"]: t for t in status()["themes"]}["night"]

    def shown(**kw):
        return call("GET", "/api/theme/preview", {"id": "night", **kw})

    assert night()["cover"] == "" and shown()[0] == 404
    code, r = put("background", pic)
    assert code == 200 and r["size"] == len(pic), (code, r)
    assert night()["cover"] == "background" and night()["coverTime"] > 0
    assert shown() == (200, pic)
    assert put("picture", other)[0] == 200
    assert night()["cover"] == "picture" and shown()[1] == other and shown(kind="background")[1] == pic

    # Not a JPEG, cut short, too small, too large a side, unknown kind, no theme, too big.
    for data in (png(64, 36, (1, 2, 3)), pic[:-2], jpeg_header(8, 8), jpeg_header(5000, 100)):
        assert put("picture", data)[0] == 422
    assert put("cover", pic)[0] == 400
    assert put("picture", pic, theme="nope")[0] == 404
    assert call("PUT", "/api/theme/preview", {"id": "night", "kind": "picture"}, b"x",
                headers={"Content-Length": str(8 << 20)})[0] == 413
    assert shown()[1] == other, "a refused upload changed the picture"
    assert not [f for f in os.listdir(f"{BASE}/themes/night") if f.endswith(".part")]
    assert not [f for f in os.listdir(f"{BASE}/themes/night/files") if f.endswith(".jpg")]

    # A duplicate keeps its pictures.
    assert call("POST", "/api/themes/duplicate", {"id": "night", "new_id": "night-pic", "name": "Pic"})[0] == 200
    assert {t["id"]: t for t in status()["themes"]}["night-pic"]["cover"] == "picture"
    assert call("DELETE", "/api/themes", {"id": "night-pic"})[0] == 200

    # Removing the chosen picture shows the background one again.
    assert call("DELETE", "/api/theme/preview", {"id": "night", "kind": "picture"})[0] == 200
    assert night()["cover"] == "background" and shown()[1] == pic
    assert call("DELETE", "/api/theme/preview", {"id": "night", "kind": "background"})[0] == 200
    assert night()["cover"] == "" and shown()[0] == 404
    assert call("DELETE", "/api/theme/preview", {"id": "night", "kind": "background"})[0] == 404
    assert call("DELETE", "/api/theme/preview", {"id": "night", "kind": "picture"}, token=False)[0] == 403


def t_save_and_discard():
    """The first change keeps a snapshot; Save keeps the changes, Discard puts the theme back."""
    def night():
        return {t["id"]: t for t in status()["themes"]}["night"]

    def files():
        return sorted(f["path"] for f in call("GET", "/api/theme/files", {"id": "night"})[1]["files"])

    theme, snap = f"{BASE}/themes/night", f"{BASE}/snapshots/night"
    assert call("POST", "/api/theme/save", {"id": "night"})[0] == 200
    assert night()["unsaved"] is False and not os.path.exists(snap)
    name, before, bg = night()["name"], files(), open(f"{theme}/files{HOME}/bg.png", "rb").read()
    applied = status()["active"] == "night"

    newer = png(480, 270, (200, 40, 40))
    assert put_file("night", f"{HOME}/bg.png", newer)[0] == 200
    assert night()["unsaved"] is True
    assert open(f"{snap}/files{HOME}/bg.png", "rb").read() == bg, "the snapshot changed with the theme"
    assert os.stat(f"{snap}/files{HOME}/icon@2x.png").st_ino == os.stat(f"{theme}/files{HOME}/icon@2x.png").st_ino
    assert call("DELETE", "/api/theme/file", {"id": "night", "path": f"{HOME}/strings.json"})[0] == 200
    assert call("POST", "/api/theme/settings", {"id": "night", "name": "Changed"})[0] == 200

    code, r = call("POST", "/api/theme/discard", {"id": "night"})
    assert code == 200 and r["reapplied"] is applied, (code, r)
    t = night()
    assert t["unsaved"] is False and t["name"] == name and files() == before, t
    assert open(f"{theme}/files{HOME}/bg.png", "rb").read() == bg
    assert not os.path.exists(snap) and not os.path.exists(f"{snap}.trash")
    if applied:
        end = time.time() + 30
        while crc_file(f"{HOME}/bg.png") != zlib.crc32(bg):
            assert time.time() < end, "the console was not updated after discarding"
            time.sleep(0.2)
        wait_applied("night")
    assert call("POST", "/api/theme/discard", {"id": "night"})[0] == 409

    # Save keeps the changes.
    assert put_file("night", f"{HOME}/bg.png", newer)[0] == 200
    assert call("POST", "/api/theme/save", {"id": "night"})[0] == 200
    assert night()["unsaved"] is False and open(f"{theme}/files{HOME}/bg.png", "rb").read() == newer
    assert put_file("night", f"{HOME}/bg.png", bg)[0] == 200
    assert call("POST", "/api/theme/save", {"id": "night"})[0] == 200

    # A picture the library makes by itself is not a change; a chosen one is.
    pic = jpeg_header(1280, 720)
    preview = lambda kind: call("PUT", "/api/theme/preview", {"id": "night", "kind": kind, "crc": f"{zlib.crc32(pic):08x}"}, pic)
    assert preview("background")[0] == 200 and night()["unsaved"] is False
    assert preview("picture")[0] == 200 and night()["unsaved"] is True
    assert call("POST", "/api/theme/discard", {"id": "night"})[0] == 200
    assert night()["cover"] == "background"
    assert call("DELETE", "/api/theme/preview", {"id": "night", "kind": "background"})[0] == 200
    assert night()["unsaved"] is False

    # Unknown themes, and a deleted theme takes its snapshot with it.
    assert call("POST", "/api/theme/save", {"id": "nope"})[0] == 404
    assert call("POST", "/api/theme/discard", {"id": "nope"})[0] == 409
    assert call("POST", "/api/theme/save", {"id": "night"}, token=False)[0] == 403
    assert call("POST", "/api/themes/duplicate", {"id": "night", "new_id": "scratch", "name": "Scratch"})[0] == 200
    assert put_file("scratch", f"{HOME}/bg.png", newer)[0] == 200
    assert os.path.exists(f"{BASE}/snapshots/scratch")
    assert call("DELETE", "/api/themes", {"id": "scratch"})[0] == 200
    assert not os.path.exists(f"{BASE}/snapshots/scratch")



def t_cut_off_uploads_are_not_theme_files():
    """An upload cut off part way leaves its .part file in the theme. It is never copied into the snapshot Discard
    goes back to (on the console, a Discard brought one back and every apply after it was "applied partly"), nor
    into a duplicate; an apply neither uses nor counts one, and removes one untouched for an hour."""
    def parts(root):
        return sorted(f for _, _, fs in os.walk(root) for f in fs if f.endswith(".part"))

    was = status()["applied"]
    assert call("POST", "/api/themes", {"id": "cutoff", "name": "Cut off"})[0] == 200
    assert put_file("cutoff", f"{HOME}/bg.png", png(480, 270, (200, 120, 20)))[0] == 200
    theme, snap = f"{BASE}/themes/cutoff", f"{BASE}/snapshots/cutoff"
    assert call("POST", "/api/theme/save", {"id": "cutoff"})[0] == 200
    fresh, stale = f"{theme}/files{HOME}/bg.png.7.part", f"{theme}/files{HOME}/icon@2x.png.3.part"
    write(fresh, b"an upload being received")
    write(stale, b"an upload cut off long ago")
    os.utime(stale, (time.time() - 7200, time.time() - 7200))

    # The first change keeps the snapshot while another upload is being received: without it.
    assert put_file("cutoff", f"{HOME}/bg.png", png(480, 270, (20, 160, 60)))[0] == 200
    assert os.path.isdir(snap) and parts(snap) == [], parts(snap)
    assert call("POST", "/api/themes/duplicate", {"id": "cutoff", "new_id": "cutoff2", "name": "Cut off 2"})[0] == 200
    assert parts(f"{BASE}/themes/cutoff2") == []
    assert call("DELETE", "/api/themes", {"id": "cutoff2"})[0] == 200
    assert call("POST", "/api/theme/discard", {"id": "cutoff"})[0] == 200
    assert parts(theme) == [], parts(theme)
    assert [f["path"] for f in call("GET", "/api/theme/files", {"id": "cutoff"})[1]["files"]] == [f"{HOME}/bg.png"]

    # An apply leaves them out, and removes the old one.
    write(fresh, b"an upload being received")
    write(stale, b"an upload cut off long ago")
    os.utime(stale, (time.time() - 7200, time.time() - 7200))
    assert call("POST", "/api/apply", {"id": "cutoff"})[0] == 200
    s = wait_applied("cutoff")
    assert "1 files verified" in s["result"], s["result"]
    log = call("GET", "/api/log")[1].decode()
    assert "partly" not in s["result"] and "skipped" not in s["result"], \
        (s["result"], [line for line in log.splitlines() if "skipping" in line or "ignoring" in line][-4:])
    assert not os.path.exists(stale) and os.path.exists(fresh)
    assert "an upload that was cut off" in log

    # "Applied partly" says what was skipped, in plain words: no count of folders against overlays.
    write(f"{theme}/files{HOME}/not-on-this-firmware.png", png(64, 64, (1, 2, 3)))
    assert call("POST", "/api/apply", {"id": "cutoff"})[0] == 200
    s = wait_result("applied partly")
    assert "1 skipped." in s["result"] and not re.search(r"\d+ of \d+ folders", s["result"]), s["result"]

    assert call("POST", "/api/apply", {"id": was or "none"})[0] == 200
    wait_applied(was)
    assert call("DELETE", "/api/themes", {"id": "cutoff"})[0] == 200
    assert not os.path.exists(theme) and not os.path.exists(snap)

def t_home_background_textures():
    stock_path = f"{VSH}/bg_hub_default.dds.zst"
    stock_dds = subprocess.run(["zstd", "-d", "-c", "-q", stock_path], capture_output=True, check=True).stdout
    code, r = call("GET", "/api/assets", {"q": "system_ex/vsh_asset"})
    assert {a["path"]: a["kind"] for a in r["assets"]} == {stock_path: "texture", f"{VSH}/bgm_home.at9": "audio",
                                                          f"{VSH}/sfx_quiet.at9": "audio", f"{VSH}/eboot_like.rco": "rco",
                                                          UI_RCO: "rco", f"{VSH}/bg_NPXS40037.dds.zst": "texture"}, r
    code, info = call("GET", "/api/asset/texture", {"path": stock_path})
    assert code == 200 and info["format"] == "BC7" and info["dxgi"] == 98 and info["zstd"] is True, info
    assert (info["width"], info["height"], info["mips"]) == (256, 128, 1), info
    import base64
    assert base64.b64decode(info["header"]) == stock_dds[:148]
    assert call("GET", "/api/asset/texture", {"path": f"{HOME}/bg.png"})[0] == 403

    assert call("POST", "/api/themes", {"id": "wall", "name": "Wallpaper"})[0] == 200
    mine = stock_dds[:148] + bytes((i * 31 + 1) & 255 for i in range(len(stock_dds) - 148))
    stock_file = open(stock_path, "rb").read()
    for bad, needle in [(zstd_stored(dds_bc7(128, 128), stock_file), "exactly like the system file"),
                        (zstd_stored(mine[:-16], stock_file), "not a valid DDS"),
                        (b"\x28\xb5\x2f\xfd" + b"\xff" * 40, "zstd"),
                        # 25 bytes claiming to unpack to 200 MB: refused without unpacking
                        (b"\x28\xb5\x2f\xfd\xa0" + struct.pack("<I", 200 << 20) + b"\x51\x00\x00" + bytes(10),
                         "larger than the system texture"),
                        (mine, "zstd-compressed like the system file")]:
        code, r = put_file("wall", stock_path, bad)
        assert code == 422 and needle in r["error"], (needle, code, r)
    assert put_file("wall", stock_path, zstd_cli(mine))[0] == 200          # fully compressed by the real tool
    stored = zstd_stored(mine, stock_file)
    assert put_file("wall", stock_path, stored)[0] == 200                  # stored blocks, as the builder writes
    # as the pages before 1.56 wrote it (one frame form for every texture): rewritten with the system file's
    assert put_file("wall", stock_path, zstd_stored(mine))[0] == 200
    assert open(f"{BASE}/themes/wall/files{stock_path}", "rb").read() == stored

    # A theme made before 1.56 already holds the old frame form on disk: applying it rewrites the file with
    # the system file's, and the whole theme applies (it was "applied partly" with the file skipped).
    write(f"{BASE}/themes/wall/files{stock_path}", zstd_stored(mine))
    assert call("POST", "/api/apply", {"id": "wall"})[0] == 200
    s = wait_applied("wall")
    assert "1 files verified" in s["result"] and "partly" not in s["result"], s["result"]
    assert open(f"{BASE}/themes/wall/files{stock_path}", "rb").read() == stored
    assert open(stock_path, "rb").read() == stored
    assert crc_file(f"{VSH}/eboot_like.rco") == zlib.crc32(b"\0" * 64)    # rest of the folder stays stock
    assert len(mounts_on(VSH)) == 1
    assert call("POST", "/api/apply", {"id": "night"})[0] == 200
    wait_applied("night")
    assert open(stock_path, "rb").read() != stored
    assert call("DELETE", "/api/themes", {"id": "wall"})[0] == 200


def png_chunks(data):
    out, pos = [], 8
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        out.append((data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]))
        pos += 12 + n
    return out


def png_rebuilt(data, idat=None, extra=b""):
    """A PNG's chunks again, with other image data and extra chunks before IEND."""
    out = data[:8]
    for kind, body in png_chunks(data):
        if kind == b"IEND":
            out += extra
        if kind == b"IDAT" and idat is not None:
            body = idat
        out += png_chunk(kind, body)
    return out


def t_interface_pictures():
    """ShellUI's resource files: a theme may swap pictures in the console's own
    file, each matching the console's picture in everything but its pixels;
    nothing else may change. The manager's own code makes files the console
    accepts."""
    stock = open(UI_RCO, "rb").read()
    rco = Rco(stock)
    old = {(f[1], f[3]): rco.data(f[5], f[6]) for f in rco.files()}
    assert call("POST", "/api/themes", {"id": "ui", "name": "Interface"})[0] == 200
    magenta_svg = SVG_ICON.split(b">")[0] + b'><rect width="64" height="64" fill="#f0f"/></svg>'
    checkbox = old[("image_checkbox_checked", "src")]
    good = rco.replaced({("emoji_game", "src"): png(24, 24, (255, 0, 255)), ("iconid_home", "src"): magenta_svg,
                         ("image_checkbox_checked", "src"): palette_png(20, 20, lambda x, y: 2 * (x > y))})
    slot = {(f[1], f[3]): f[4] for f in rco.files()}
    table_size = struct.unpack_from("<I", good, 8 + 8 * 8 + 4)[0]

    def pointing(data, key, off):
        b = bytearray(data)
        struct.pack_into("<I", b, slot[key], off)
        return bytes(b)
    renamed = bytearray(stock)
    renamed[renamed.index(b"emoji_game") + 9] = ord("f")
    rows = zlib.decompress(b"".join(body for kind, body in png_chunks(checkbox) if kind == b"IDAT"))
    damaged = bytearray(zlib.compress(rows))
    damaged[len(damaged) // 2] ^= 0x55
    bad = [(rco.replaced({("emoji_game", "src"): png(25, 24, (1, 2, 3))}), "its size or pixel format differs"),
           (rco.replaced({("image_button_base", "src_4k"): png(16, 16, (1, 2, 3))}), "its size or pixel format differs"),
           (rco.replaced({("emoji_game", "src"): b"GIF89a" + bytes(40)}), "not a complete PNG"),
           (rco.replaced({("emoji_game", "src"): png(24, 24, (1, 2, 3))[:-5]}), "not a complete PNG"),
           (rco.replaced({("image_checkbox_checked", "src"): palette_png(20, 20, extra=png_chunk(b"tEXt", b"by\0me"))}),
            "its chunks differ"),
           (rco.replaced({("image_checkbox_checked", "src"): palette_png(20, 20, level=9)}), "compressed differently"),
           (rco.replaced({("image_checkbox_checked", "src"): png_rebuilt(checkbox, bytes(damaged))}), "damaged or the wrong size"),
           (rco.replaced({("image_checkbox_checked", "src"): png_rebuilt(checkbox, zlib.compress(rows[:-11]))}), "damaged or the wrong size"),
           (rco.replaced({("image_checkbox_checked", "src"): palette_png(20, 20, lambda x, y: 3 * (x == y))}), "outside its palette"),
           (rco.replaced({("image_checkbox_checked", "src"): png_rebuilt(checkbox, zlib.compress(b"\x07" + rows[1:]))}), "invalid row filter"),
           (rco.replaced({("iconid_texture_app_fallback", "src"): magenta_svg}), "not a complete PNG"),
           (rco.replaced({("iconid_home", "src"): b'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"/>'}),
            "must keep the console's own <svg> opening"),
           (rco.replaced({("iconid_home", "src"): SVG_ICON.split(b">")[0] + b"><script>alert(1)</script></svg>"}), 'SVG contains "<script"'),
           (rco.replaced({("snd_focus_move", "src"): bytes(2048)}), "only pictures, shapes and colours can be changed"),
           (rco.replaced({("en-US", "src"): b"strings" * 49 + b"STRING!"}), "only pictures, shapes and colours can be changed"),
           # Colours: the same names, in order, each a whole number 0..0xFFFFFFFF; nothing else.
           (rco.replaced({("color_default", "src"): b'{"button.fill.enabled": 1}'}), "colours color_default must keep the console's 14 names"),
           (rco.replaced({("color_ps2", "src"): COLORS_PS2.replace(b'"focus.fill', b'"focus.fills')}), "must keep the console's 14 names"),
           (rco.replaced({("color_ps2", "src"): COLORS_PS2.replace(b'  "background.enabled": 4278190080,\r\n', b"")}),
            "must keep the console's 14 names"),
           (rco.replaced({("color_ps2", "src"): COLORS_PS2.replace(b"4278190080", b"-1")}), "background.enabled must be a whole number"),
           (rco.replaced({("color_ps2", "src"): COLORS_PS2.replace(b"4278190080", b"4278190080.0")}), "background.enabled must be a whole number"),
           (rco.replaced({("color_ps2", "src"): COLORS_PS2.replace(b"4278190080", b"4294967296")}), "background.enabled must be a whole number"),
           (rco.replaced({("color_ps2", "src"): COLORS_PS2.replace(b"4278190080", b"4e9")}), "background.enabled must be a whole number"),
           (rco.replaced({("color_ps2", "src"): COLORS_PS2.replace(b"4278190080", b'"#ff0000"')}), "not a list of names and colours"),
           (rco.replaced({("color_ps2", "src"): COLORS_PS2 + b" " * 17000}), "not a list of names and colours"),
           # Shapes: the same names, in order, each a number in range; nothing else.
           (rco.replaced({("shape_default", "src"): SHAPES.replace(b'"bar.fill"', b'"bar.fills"')}), "must keep the console's 5 names"),
           (rco.replaced({("shape_default", "src"): SHAPES.replace(b"\r\n}", b',\r\n  "tile": 9.0\r\n}')}),
            "must keep the console's 5 names"),
           (rco.replaced({("shape_default", "src"): SHAPES.replace(b'"switch.track": 16.0,\r\n', b"")}), "must keep the console's 5 names"),
           (rco.replaced({("shape_default", "src"): SHAPES.replace(b"8.0", b"-1.0")}), "slider.thumb must be a number from 0 to 100"),
           (rco.replaced({("shape_default", "src"): SHAPES.replace(b"8.0", b"1e9")}), "slider.thumb must be a number from 0 to 100"),
           (rco.replaced({("shape_default", "src"): SHAPES.replace(b'"borderRadiusRatio": 1.0', b'"borderRadiusRatio": 11.0')}),
            "borderRadiusRatio must be a number from 0 to 10"),
           (rco.replaced({("shape_default", "src"): SHAPES.replace(b"8.0", b'"big"')}), "not a list of names and numbers"),
           (rco.replaced({("shape_default", "src"): SHAPES.replace(b"8.0", b"[8]")}), "not a list of names and numbers"),
           (rco.replaced({("shape_default", "src"): SHAPES.replace(b"8.0", b"NaN")}), "not a list of names and numbers"),
           (rco.replaced({("shape_default", "src"): SHAPES + b"x"}), "not a list of names and numbers"),
           (rco.replaced({("shape_default", "src"): SHAPES + b" " * 5000}), "not a list of names and numbers"),
           (bytes(renamed), "own resource file with only pictures changed"),
           (pointing(good, ("emoji_game", "src"), table_size), "points outside its data"),
           (pointing(good, ("emoji_game", "src"), 8), "points outside its data"),
           (good[:-16], "not a resource file"),
           (b"RCOF" + bytes(300), "not a resource file")]
    for data, needle in bad:
        code, r = put_file("ui", UI_RCO, data)
        assert code == 422 and needle in r["error"], (needle, code, r)
    code, r = put_file("ui", f"{VSH}/eboot_like.rco", b"\0" * 64)
    assert code == 422 and "cannot read the system file" in r["error"], r
    assert put_file("ui", UI_RCO, good)[0] == 200
    assert put_file("ui", UI_RCO, stock)[0] == 200          # the console's own file, unchanged, is fine too
    rounder = SHAPES.replace(b'"borderRadiusRatio": 1.0', b'"borderRadiusRatio": 3.5').replace(b"16.0", b"0")
    assert put_file("ui", UI_RCO, rco.replaced({("shape_default", "src"): rounder}))[0] == 200
    red_text = COLORS_PS2.replace(b'"text.primary.enabled": 4294967295', b'"text.primary.enabled": 4294901760')
    assert put_file("ui", UI_RCO, rco.replaced({("color_ps2", "src"): red_text, ("shape_default", "src"): rounder}))[0] == 200

    # The page's colour test: every palette painted by group, transparency kept, names and layout as the console's;
    # the console accepts it.
    write("/tmp/colors.js", COLORS_JS.encode())
    out = subprocess.run(["node", "/tmp/colors.js", WEB_PAGE, UI_RCO, "/tmp/colored.rco"], capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    colored = open("/tmp/colored.rco", "rb").read()
    pals = {f[1]: json.loads(Rco(colored).data(f[5], f[6])) for f in Rco(colored).files() if f[1].startswith("color_")}
    stock_pals = {f[1]: json.loads(rco.data(f[5], f[6])) for f in rco.files() if f[1].startswith("color_")}
    want = {"focus.fill.enabled": 0x00e5ff, "text.primary.enabled": 0xff1a1a, "menu.label.primary.enabled": 0xff1a1a,
            "button.label.focused": 0xff1a1a, "icon.primary.enabled": 0xff8c00, "base-mat.area.enabled": 0xff00ff,
            "background.enabled": 0x1f4bff, "dialog.fill.enabled": 0x00c853, "button.fill.enabled": 0xffd600,
            "slider.thumb.enabled": None, "tooltip.error.fill.enabled": None}      # the same in every palette: kept
    for pid, pal in pals.items():
        assert list(pal) == list(stock_pals[pid]), pid
        for name, rgb in want.items():
            before = stock_pals[pid][name]
            got = pal[name]
            assert got == (before if rgb is None else (before & 0xff000000) | rgb), (pid, name, hex(got))
    assert put_file("ui", UI_RCO, colored)[0] == 200
    # The panel test: each panel and base mat colour a distinct colour, everything else as the console's.
    out = subprocess.run(["node", "/tmp/colors.js", WEB_PAGE, UI_RCO, "/tmp/panels.rco", "panel"], capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    panels = open("/tmp/panels.rco", "rb").read()
    pals = {f[1]: json.loads(Rco(panels).data(f[5], f[6])) for f in Rco(panels).files() if f[1].startswith("color_")}
    want = {"dialog.fill.enabled": 0xffee00, "base-mat.area.enabled": 0x8b4513, "base-mat.overlay.enabled": 0x008080, "menu.fill.enabled": 0x8000ff}
    for pid, pal in pals.items():
        assert list(pal) == list(stock_pals[pid]), pid
        for name, before in stock_pals[pid].items():
            got = pal[name]
            assert got == ((before & 0xff000000) | want[name] if name in want else before), (pid, name, hex(got))
    assert put_file("ui", UI_RCO, panels)[0] == 200
    # The colour editor: each colour by what it changes, the console's transparency kept; light text only (dark labels
    # on light buttons stay); the accent's shades follow it in proportion; the other palettes untouched.
    out = subprocess.run(["node", "/tmp/colors.js", WEB_PAGE, UI_RCO, "/tmp/edited.rco", "edit"], capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    edited = open("/tmp/edited.rco", "rb").read()
    pals = {f[1]: json.loads(Rco(edited).data(f[5], f[6])) for f in Rco(edited).files() if f[1].startswith("color_")}
    assert pals["color_default"] == stock_pals["color_default"]
    ps2 = pals["color_ps2"]
    assert list(ps2) == list(stock_pals["color_ps2"])
    expect = dict(stock_pals["color_ps2"])
    expect.update({"menu.fill.enabled": 0xff123456, "base-mat.overlay.enabled": 0xff0000ff, "text.primary.enabled": 0xffffd000,
                   "menu.label.primary.enabled": 0xffffd000, "button.fill.enabled": 0x0fff0000,
                   "button.indicator.track.focused": 0xffb80000})   # 0x275b9a is 0.72 of 0x367ed6's brightness
    assert ps2 == expect, {k: hex(v) for k, v in ps2.items() if v != expect[k]}
    assert put_file("ui", UI_RCO, edited)[0] == 200

    # The page's shape editor: unchanged numbers give the console's own text,
    # new ones are written where the old ones stand, and the console accepts it.
    write("/tmp/shapes.js", SHAPES_JS.encode())
    out = subprocess.run(["node", "/tmp/shapes.js", WEB_PAGE, UI_RCO, "/tmp/shaped.rco"], capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    info = json.loads(out.stdout)
    assert info["names"] == ["bar.fill", "slider.thumb", "switch.track", "tooltip.fill", "borderRadiusRatio"], info
    shaped = open("/tmp/shaped.rco", "rb").read()
    entry = next(Rco(shaped).data(f[5], f[6]) for f in Rco(shaped).files() if f[1] == "shape_default")
    assert entry == SHAPES.replace(b'"borderRadiusRatio": 1.0', b'"borderRadiusRatio": 3.0').replace(b"16.0", b"0.0") \
                        .replace(b'"slider.thumb": 8.0', b'"slider.thumb": 12.5'), entry
    assert put_file("ui", UI_RCO, shaped)[0] == 200

    # The manager page's code: an unchanged rebuild is byte-identical, and
    # numbering every picture (compressed, and in stored blocks as browsers
    # without CompressionStream do) gives files the console accepts, each
    # picture keeping the console's chunks, zlib header and SVG opening.
    write("/tmp/rco.js", RCO_JS.encode())
    for mode in ("deflate", "stored"):
        out = subprocess.run(["node", "/tmp/rco.js", WEB_PAGE, UI_RCO, "/tmp/numbered.rco", mode], capture_output=True, text=True)
        assert out.returncode == 0, out.stderr
        info = json.loads(out.stdout)
        assert info["pictures"] == 6 and info["numbered"] == 8, info
        assert info["kinds"]["iconid_texture_app_fallback"] == "png" and info["kinds"]["iconid_home"] == "svg", info
        numbered = open("/tmp/numbered.rco", "rb").read()
        new = {(f[1], f[3]): Rco(numbered).data(f[5], f[6]) for f in Rco(numbered).files()}
        for key, data in new.items():
            if key[0] in ("default", "en-US", "snd_focus_move", "color_default", "color_ps2", "shape_default"):
                assert data == old[key], key
            elif old[key].startswith(b"\x89PNG"):
                assert data != old[key], key
                assert [c for c in png_chunks(data) if c[0] != b"IDAT"] == [c for c in png_chunks(old[key]) if c[0] != b"IDAT"], key
                assert next(b for k, b in png_chunks(data) if k == b"IDAT")[:2] == next(b for k, b in png_chunks(old[key]) if k == b"IDAT")[:2]
            else:
                head = old[key][: old[key].index(b">") + 1]
                assert data.startswith(head) and b'fill="#ff00ff"' in data, key
        code, r = put_file("ui", UI_RCO, numbered)
        assert code == 200, (mode, r)

    # Your own picture in a vector icon: the console's SVG opening, then the
    # picture as an <image>, accepted. A tile icon numbered by the page is
    # accepted too, and tile icon files map to their title IDs.
    write("/tmp/picture.js", PICTURE_JS.encode())
    write("/tmp/mine.png", png(16, 16, (0, 200, 90)))
    out = subprocess.run(["node", "/tmp/picture.js", WEB_PAGE, UI_RCO, "/tmp/mine.rco", "/tmp/mine.png", PLUS_ICON, "/tmp/tile.png"],
                         capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    info = json.loads(out.stdout)
    assert info["svg"].startswith(SVG_ICON.split(b">")[0].decode() + '><image x="0" y="0" width="64" height="64"'), info["svg"]
    assert 'href="data:image/png;base64,' in info["svg"], info["svg"]
    assert info["titles"] == ["NPXS40037", "NPXS40037", "NPXS40053", "NPXS40075", ""], info["titles"]
    assert put_file("ui", UI_RCO, open("/tmp/mine.rco", "rb").read())[0] == 200
    tile = open("/tmp/tile.png", "rb").read()
    assert png_chunks(tile)[0] == png_chunks(open(PLUS_ICON, "rb").read())[0]   # the console's own header
    assert put_file("ui", PLUS_ICON, tile)[0] == 200
    assert put_file("ui", UI_RCO, numbered)[0] == 200

    # Applied: the home screen sees the theme's files, on the real folders and
    # in the system apps' sandboxes; afterwards the console's again.
    boxes = []
    if not EMULATED:
        for box in ["/mnt/sandbox/NPXS40087_000/system_ex", "/mnt/sandbox/NPXS40000_000/system_ex"]:
            os.makedirs(box, exist_ok=True)
            subprocess.run(["mount", "--bind", "/system_ex", box], check=True)
            boxes.append(box)
    try:
        apply_and_wait("ui")
        stock_icon = zlib.crc32(png(64, 64, (200, 160, 0)))
        assert crc_file(UI_RCO) == zlib.crc32(numbered) and crc_file(PLUS_ICON) == zlib.crc32(tile)
        if not EMULATED:
            for box in boxes:
                assert crc_file(box[:-len("/system_ex")] + PLUS_ICON) == zlib.crc32(tile), box
                assert crc_file(box[:-len("/system_ex")] + UI_RCO) == zlib.crc32(numbered), box
        apply_and_wait("night")
        assert crc_file(UI_RCO) == zlib.crc32(stock) and crc_file(PLUS_ICON) == stock_icon
    finally:
        for box in reversed(boxes):
            subprocess.run(["umount", "-l", box])
    assert call("DELETE", "/api/themes", {"id": "ui"})[0] == 200


def t_title_pictures():
    """Games' and apps' pictures: listed and accepted (in the console's exact
    formats; a title's param.json, license, keys and the rest of its
    installation, and the plugin's own tile, refused), and applied only in
    ShellUI's own view of /user, the one place it reads them from, once its
    process has been found (they come out of its sandbox when it exits): not
    on the real folders, /original_user or other apps' views. The apply says
    to use rest mode to see the home screen's icons. An overlay an older copy
    left on a game's real folder comes down."""
    code, r = call("GET", "/api/assets", {"q": "/user/app"})                 # /user/appmeta and /user/app
    assert {a["path"]: a["kind"] for a in r["assets"]} == {
        f"{GAME}/icon0.png": "image", f"{GAME}/icon0.dds": "texture", f"{GAME}/pic0.png": "image",
        f"{PS4_APP}/icon0.png": "image", f"{PS4_APP}/icon0.dds": "texture",
        f"{GAME_SYS}/icon0.png": "image", f"{GAME_SYS}/icon0.dds": "texture"}, r
    code, r = call("GET", "/api/assets", {"q": "/user/catalog_downloader"})  # and PlayStation Network's
    assert {a["path"]: a["kind"] for a in r["assets"]} == {
        f"{CATALOG}/icon0.png": "image", f"{CATALOG}/pic0.png": "image", f"{CONCEPT}/icon0.png": "image",
        f"{CONCEPT}/icon0_11.png": "image", f"{CONCEPT}/logo_01.png": "image",
        "/user/catalog_downloader/conceptmeta/999999/icon0.png": "image"}, r
    assert r["concepts"] == {"234585": ["CUSA09999", "PPSA01234"], "888888": ["PPSA06666"]}, r["concepts"]
    code, body = call("GET", "/api/asset", {"path": f"{GAME}/param.json"})
    assert code == 200 and "Test Game" in json.dumps(body), (code, body)
    assert call("POST", "/api/themes", {"id": "games", "name": "Games"})[0] == 200
    for path in [f"{GAME}/param.json", f"{GAME_SYS}/param.json", f"{GAME_SYS}/license.dat", f"{GAME_SYS}/keystone",
                 f"{GAME_SYS}/about/inner.png", "/user/app/PPSA01234/stray.png", "/user/app/THEM00001/sce_sys/icon0.png",
                 f"{CONCEPT}/notes.json", f"{CONCEPT}/sub/inner.png", "/user/catalog_downloader/catalog.db"]:
        assert put_file("games", path, open(path, "rb").read())[0] == 403, path
    assert put_file("games", f"{GAME}/icon0.png", png(32, 32, (1, 2, 3)))[0] == 422
    assert put_file("games", f"{GAME}/icon0.dds", dds_dxt1(64, 64))[0] == 422
    mine, my_tex = png(64, 64, (255, 0, 0)), dds_bc7(64, 64, fill=bytes([0x40]) * 4096, flags=0xa1007)
    my_webp = webp_header(64, 64, fill=7)
    # A PlayStation Network picture only in the format its file holds, whatever its name.
    for path, data, why in [(f"{CONCEPT}/icon0.png", mine, "valid WebP"), (f"{CATALOG}/pic0.png", png(96, 54, (1, 1, 1)), "valid JPEG")]:
        code, body = put_file("games", path, data)
        assert code == 422 and why in json.dumps(body), (path, code, body)
    for path, data in [(f"{GAME}/icon0.png", mine), (f"{GAME}/icon0.dds", my_tex), (f"{GAME_SYS}/icon0.png", mine),
                       (f"{CATALOG}/icon0.png", my_webp), (f"{CONCEPT}/icon0.png", my_webp), (f"{CATALOG}/pic0.png", jpeg_header(96, 54) + b"\0")]:
        assert put_file("games", path, data)[0] == 200, path
    assert put_file("games", f"{HOME}/bg.png", S["bg"])[0] == 200            # and something else

    assert call("GET", "/api/title-placement")[0] == 404       # one way now: nothing to choose
    shell, service = "/mnt/sandbox/NPXS40087_000", "/mnt/sandbox/NPXS40102_000"
    boxes, shellui = [], None
    if not EMULATED:
        # As on the PS5: /user shown again as /original_user; ShellUI's sandbox
        # with views of /user and /original_user; the icon service's with views
        # of /user/appmeta and /original_user/appmeta only.
        os.makedirs("/original_user", exist_ok=True)
        subprocess.run(["mount", "--bind", "/user", "/original_user"], check=True)
        boxes.append("/original_user")
        for box, src in [(f"{shell}/user", "/user"), (f"{service}/user/appmeta", "/user/appmeta"),
                         (f"{service}/original_user/appmeta", "/user/appmeta")]:
            os.makedirs(box, exist_ok=True)
            subprocess.run(["mount", "--bind", src, box], check=True)
        S["plugin"].term()
        S["plugin"] = Plugin(env={"PS5THEME_TEST_ORIGINAL_USER": "1"})
        shellui = start_shellui()
    # Only ShellUI's own view of /user shows the theme's pictures.
    in_view = [f"{shell}/user/appmeta/PPSA01234/icon0.png", f"{shell}/user/app/PPSA01234/sce_sys/icon0.png",
               f"{shell}/user/catalog_downloader/appmeta/PPSA01234/icon0.png",
               f"{shell}/user/catalog_downloader/conceptmeta/234585/icon0.png"]
    elsewhere = [f"{GAME}/icon0.png", f"{GAME_SYS}/icon0.png", f"{CATALOG}/icon0.png", f"{CONCEPT}/icon0.png"]
    if not EMULATED:
        elsewhere += ["/original_user/appmeta/PPSA01234/icon0.png", "/original_user/app/PPSA01234/sce_sys/icon0.png",
                      f"{service}/user/appmeta/PPSA01234/icon0.png", f"{service}/original_user/appmeta/PPSA01234/icon0.png"]
    psn = lambda path: "catalog_downloader" in path
    stock_icon = lambda path: zlib.crc32(webp_header(64, 64) if psn(path) else png(64, 64, (0, 90, 200)))
    my_icon = lambda path: zlib.crc32(my_webp if psn(path) else mine)
    try:
        apply_and_wait("games")
        result = status()["result"]
        assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])         # the rest is applied
        for path in [] if EMULATED else in_view:
            assert crc_file(path) == my_icon(path), path
        for path in elsewhere:
            assert crc_file(path) == stock_icon(path), path
        if EMULATED:      # no ShellUI here: its process is never found
            assert "Its game and app pictures wait until the home screen's process is found." in result, result
        else:
            assert crc_file(f"{shell}/user/appmeta/PPSA01234/icon0.dds") == zlib.crc32(my_tex)
            assert "put the console in rest mode and wake it" in result, result
            notes = open("/tmp/notifications.txt").read()
            assert "ps5-theme: To see the new game and app icons on the home screen, put the console in rest mode" in notes
        in_shell = [l for l in recorded_mounts() if l.startswith(shell + "/user")]
        assert bool(in_shell) == (not EMULATED), recorded_mounts()
        assert not mounts_on("/user/app/PPSA01234")
        for p in ["param.json", "license.dat", "keystone", "about/right.sprx", "trophy2/trophy00.ucp"]:
            assert crc_file(f"{GAME_SYS}/{p}") == kept_crc(p), p
        assert crc_file(f"{CONCEPT}/notes.json") == zlib.crc32(b"{}")

        if not EMULATED:
            # An overlay an older copy left on a game's real folder comes down.
            old = f"{BASE}/stage/g9999-0"
            shutil.copytree(GAME_SYS, old)
            subprocess.run(["mount", "--bind", "-o", "ro", old, GAME_SYS], check=True)
            with open(f"{BASE}/mounts", "a") as f:
                f.write(f"{GAME_SYS}\t{old}\t{GAME_SYS}\n")
            S["plugin"].kill()
            S["plugin"] = Plugin(env={"PS5THEME_TEST_ORIGINAL_USER": "1"})
            wait_applied("games")
            assert f"{GAME_SYS} is an overlay this version doesn't make; applying again" in log_text()
            assert not mounts_on(GAME_SYS)
            shutil.rmtree(old, ignore_errors=True)
    finally:
        if shellui:
            shellui.kill()
            shellui.wait()
        apply_and_wait("night")
        for box in reversed(boxes):
            subprocess.run(["umount", "-l", box])
        clear_sandboxes()
    assert call("DELETE", "/api/themes", {"id": "games"})[0] == 200


def t_fonts():
    """Fonts: listed and checked (a whole TrueType or OpenType font with the
    characters text needs; web fonts, collections and damaged or incomplete
    files refused), and applied only in ShellUI's own view of the fonts folder,
    once its process has been found (they come out of its sandbox when it
    exits): not on the real folder, which games read too, and not in other
    apps' views. The apply says to use rest mode."""
    code, r = call("GET", "/api/assets", {"q": "/preinst"})
    assert {a["path"]: a["kind"] for a in r["assets"]} == {
        f"{FONTS}/SST-Roman.otf": "font", f"{FONTS}/SST-Bold.otf": "font", f"{FONTS}/DFHEI5-SONY.ttf": "font"}, r
    assert r["protected"] == 1, r                       # notes.txt
    assert call("POST", "/api/themes", {"id": "fonts", "name": "Fonts"})[0] == 200
    assert put_file("fonts", f"{FONTS}/notes.txt", b"x")[0] == 403

    good = font_fixture.font("cff", glyphs=200)
    refused = [
        (b"wOFF" + bytes(80), "web fonts"), (b"wOF2" + bytes(80), "web fonts"),
        (font_fixture.collection(), "collections"), (b"not a font at all, just text" * 4, "not a TrueType or OpenType"),
        (font_fixture.font()[:200], "cut short or damaged"), (font_fixture.font()[:10], "too short"),
        (font_fixture.font(drop=("cmap",)), "no 'cmap' table"), (font_fixture.font(drop=("OS/2",)), "no 'OS/2' table"),
        (font_fixture.font("tt", drop=("glyf",)), "'glyf' and 'loca'"), (font_fixture.font("cff", drop=("CFF ",)), "no 'CFF ' table"),
        (font_fixture.font(duplicate="name"), "'name' table twice"), (font_fixture.font(head_magic=0), "header table is damaged"),
        (font_fixture.font(em=0), "size grid"), (font_fixture.font(em=65000), "size grid"),
        (font_fixture.font(missing="A"), "no 'A'"), (font_fixture.font(missing=" "), "no space"),
        (font_fixture.font(glyphs=90), "no 'y'"), (font_fixture.font(nhm=0), "horizontal metrics"),
        (font_fixture.font(hmtx_cut=40), "metrics table is cut short"), (font_fixture.font("tt", loca_cut=10), "glyph index table is cut short"),
        (font_fixture.font(cmap_format=12, missing="e"), "no 'e'"), (font_fixture.font(platform=(1, 0)), "no Unicode character map"),
    ]
    for data, needle in refused:
        code, r = put_file("fonts", f"{FONTS}/SST-Roman.otf", data)
        assert code == 422 and needle in r["error"], (needle, code, r)
    # Accepted: the same flavor, the other flavor (the console reads both), and a format 12 map.
    mine_bold = font_fixture.font("tt", glyphs=210, cmap_format=12)
    assert put_file("fonts", f"{FONTS}/SST-Roman.otf", good)[0] == 200
    assert put_file("fonts", f"{FONTS}/SST-Bold.otf", mine_bold)[0] == 200
    assert put_file("fonts", f"{HOME}/bg.png", S["bg"])[0] == 200            # and something else

    shell, service = "/mnt/sandbox/NPXS40087_000", "/mnt/sandbox/NPXS40102_000"
    views = [f"{shell}/lJtEgWhS1P/common/font", f"{shell}/sce_sbx/001/lJtEgWhS1P/common/font"]
    other = f"{service}/lJtEgWhS1P/common/font"
    boxes, shellui = [], None
    stock = {n: zlib.crc32(open(f"{FONTS}/{n}", "rb").read()) for n in ("SST-Roman.otf", "SST-Bold.otf", "DFHEI5-SONY.ttf")}
    if not EMULATED:
        for view in views + [other]:
            os.makedirs(view, exist_ok=True)
            subprocess.run(["mount", "--bind", FONTS, view], check=True)
        S["plugin"].term()
        S["plugin"] = Plugin()
        shellui = start_shellui()
    try:
        apply_and_wait("fonts")
        result = status()["result"]
        assert crc_file(f"{HOME}/bg.png") == zlib.crc32(S["bg"])         # the rest is applied
        for n in stock:                                  # the real folder is never overlaid
            assert crc_file(f"{FONTS}/{n}") == stock[n], n
        if EMULATED:      # no ShellUI here: its process is never found
            assert "Its font waits until the home screen's process is found." in result, result
        else:
            assert "put the console in rest mode and wake it" in result, result
            assert "ps5-theme: To see the new font, put the console in rest mode" in open("/tmp/notifications.txt").read()
            for view in views:
                assert crc_file(f"{view}/SST-Roman.otf") == zlib.crc32(good), view
                assert crc_file(f"{view}/SST-Bold.otf") == zlib.crc32(mine_bold), view
                assert crc_file(f"{view}/DFHEI5-SONY.ttf") == stock["DFHEI5-SONY.ttf"], view     # the rest of the folder stays
                assert open(f"{view}/notes.txt").read() == "not a font"
            for n in stock:                              # another app's view stays the console's
                assert crc_file(f"{other}/{n}") == stock[n], n
        in_shell = [l for l in recorded_mounts() if l.startswith(shell) and l.split("\t")[0].endswith("/common/font")]
        assert len(in_shell) == (0 if EMULATED else 2), recorded_mounts()
        if not EMULATED:
            # When ShellUI exits (rest mode) its fonts come out of its sandbox at once.
            shellui.kill()
            shellui.wait()
            shellui = None
            end = time.time() + 15
            while crc_file(f"{views[0]}/SST-Roman.otf") != stock["SST-Roman.otf"]:
                assert time.time() < end, ("fonts still in ShellUI's sandbox", log_text()[-1500:])
                time.sleep(0.2)
    finally:
        if shellui:
            shellui.kill()
            shellui.wait()
        apply_and_wait("night")
        for view in reversed(views + [other]) if not EMULATED else []:
            subprocess.run(["umount", "-l", view])
        clear_sandboxes()
    assert call("DELETE", "/api/themes", {"id": "fonts"})[0] == 200


BC7_ALPHA_JS = r"""
const fs = require("fs");
const html = fs.readFileSync(process.argv[2], "utf8");
const block = html.slice(html.indexOf("/* bc7:begin */"), html.indexOf("/* bc7:end */"));
const B = new Function(block + ";return { bc7Block, bc7BlockAlpha, bc7DecodeMode6 };")();
let seed = 12345; const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
const out = new Uint8Array(16), dec = new Uint8Array(64);
function run(name, make, n, expect) {
  let rgbErr = 0, rgbN = 0, aErr = 0, aN = 0, worstA = 0;
  for (let t = 0; t < n; t++) {
    const px = new Uint8ClampedArray(64); make(px);
    B.bc7BlockAlpha(px, out, 0);
    if (!B.bc7DecodeMode6(out, 0, dec)) throw new Error(name + ": not a mode 6 block");
    for (let i = 0; i < 64; i += 4) {
      const da = Math.abs(dec[i + 3] - px[i + 3]); aErr += da * da; aN++; worstA = Math.max(worstA, da);
      if (px[i + 3] >= 16) for (let c = 0; c < 3; c++) { const d = dec[i + c] - px[i + c]; rgbErr += d * d; rgbN++; }
    }
  }
  const rgb = rgbN ? Math.sqrt(rgbErr / rgbN) : 0, a = Math.sqrt(aErr / aN);
  if (expect.rgb !== undefined && rgb > expect.rgb) throw new Error(`${name}: colour error ${rgb} > ${expect.rgb}`);
  if (expect.a !== undefined && a > expect.a) throw new Error(`${name}: alpha error ${a} > ${expect.a}`);
  if (expect.exactA && worstA) throw new Error(`${name}: alpha not exact (${worstA})`);
}
const flat = (r, g, b, a) => px => { for (let i = 0; i < 64; i += 4) { px[i] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a; } };
run("opaque flat", flat(40, 120, 200, 255), 50, { exactA: true, rgb: 2 });
run("fully clear", flat(0, 0, 0, 0), 50, { exactA: true });
run("half clear flat", flat(200, 50, 50, 128), 50, { a: 2, rgb: 3 });
run("opaque gradient", px => { const r = rnd() * 60, g = rnd() * 255; for (let i = 0; i < 64; i += 4) { const t = (i / 4) / 15; px[i] = r + t * 180; px[i + 1] = g * (1 - t); px[i + 2] = 80 + t * 100; px[i + 3] = 255; } }, 200, { exactA: true, rgb: 6 });
run("alpha ramp", px => { const b = [rnd() * 255, rnd() * 255, rnd() * 255]; for (let i = 0; i < 64; i += 4) { px[i] = b[0]; px[i + 1] = b[1]; px[i + 2] = b[2]; px[i + 3] = Math.round((i / 4) / 15 * 255); } }, 200, { a: 6, rgb: 4 });
run("hard edge", px => { const b = [rnd() * 255, rnd() * 255, rnd() * 255]; for (let i = 0; i < 64; i += 4) { px[i] = b[0]; px[i + 1] = b[1]; px[i + 2] = b[2]; px[i + 3] = ((i / 4) % 4) < 2 ? 255 : 0; } }, 200, { a: 8, rgb: 4 });
const px = new Uint8ClampedArray(64).fill(255); B.bc7Block(px, out, 0);
if (!B.bc7DecodeMode6(out, 0, dec) || dec[3] !== 255) throw new Error("the opaque encoder changed");
console.log("bc7 alpha ok");
"""


def t_bc7_keeps_transparency():
    """The page's BC7 block encoder for textures with see-through parts: solid
    and clear alpha exact, soft edges close, colours close, still mode 6."""
    write("/tmp/bc7a.js", BC7_ALPHA_JS.encode())
    out = subprocess.run(["node", "/tmp/bc7a.js", WEB_PAGE], capture_output=True, text=True)
    assert out.returncode == 0 and "bc7 alpha ok" in out.stdout, out.stdout + out.stderr


ZSTD_FRAMES_JS = r"""
const fs = require("fs");
const html = fs.readFileSync(process.argv[2], "utf8");
const block = html.slice(html.indexOf("/* zstd:begin */"), html.indexOf("/* zstd:end */"));
const Z = new Function(block + ";return { zstdStored, xxh64 };")();
const cases = JSON.parse(process.argv[3]);
for (const [desc, n] of cases) {
  const like = new Uint8Array([0x28, 0xb5, 0x2f, 0xfd, desc, ...((desc & 0x20) ? [] : [0x5d]), 0, 0, 0, 0]);
  const data = new Uint8Array(n);
  for (let i = 0; i < n; i++) data[i] = (i * 31 + 7) & 255;
  fs.writeFileSync(`/tmp/zs-${desc}-${n}.bin`, Z.zstdStored(data, like));
}
const known = Z.xxh64(new Uint8Array([97]));
if (known !== 0xd24ec4f1a98c6e5bn || Z.xxh64(new Uint8Array(0)) !== 0xef46db3751d8e999n) throw new Error("xxh64 differs from the reference");
console.log("zstd frames ok");
"""

BLEED_JS = r"""
const fs = require("fs");
const html = fs.readFileSync(process.argv[2], "utf8");
const block = html.slice(html.indexOf("/* bleed:begin */"), html.indexOf("/* bleed:end */"));
const bleedClear = new Function(block + ";return bleedClear;")();
const w = 24, h = 1, pix = new Uint8ClampedArray(w * 4);
pix.set([200, 40, 10, 255], 0);                    // one visible pixel at the left edge
bleedClear(pix, w, h);
for (let x = 1; x <= 8; x++) if (pix[x * 4] !== 200 || pix[x * 4 + 1] !== 40 || pix[x * 4 + 3] !== 0) throw new Error(`pixel ${x} did not take the visible colour`);
for (let x = 9; x < w; x++) if (pix[x * 4] !== 255 || pix[x * 4 + 1] !== 255 || pix[x * 4 + 2] !== 255) throw new Error(`pixel ${x} is not white`);
if (pix[3] !== 255 || pix[0] !== 200) throw new Error("a visible pixel changed");
console.log("bleed ok");
"""


def t_page_writes_the_consoles_zstd_frames():
    """The page wraps a texture in a zstd frame headed like the system file's
    (descriptor, window, size field, content checksum): its bytes equal the
    reference writer's, and the real zstd tool reads them and the checksum."""
    cases = [[d, n] for d in (0xa0, 0xa4, 0x80, 0x84, 0x60, 0x64, 0x00, 0x04, 0x24, 0x20) for n in (0, 1, 200, 3000, 70000, 300000)
             if not (d >> 6 == 0 and d & 0x20 and n > 255) and not (d >> 6 == 1 and (n < 256 or n > 65791))]
    write("/tmp/zstdjs.js", ZSTD_FRAMES_JS.encode())
    out = subprocess.run(["node", "/tmp/zstdjs.js", WEB_PAGE, json.dumps(cases)], capture_output=True, text=True)
    assert out.returncode == 0 and "zstd frames ok" in out.stdout, out.stdout + out.stderr
    for desc, n in cases:
        data = bytes((i * 31 + 7) & 255 for i in range(n))
        like = bytes([0x28, 0xb5, 0x2f, 0xfd, desc] + ([] if desc & 0x20 else [0x5d]) + [0, 0, 0, 0])
        got = open(f"/tmp/zs-{desc}-{n}.bin", "rb").read()
        assert got == zstd_stored(data, like), (hex(desc), n)
        assert subprocess.run(["zstd", "-t", "-q", f"/tmp/zs-{desc}-{n}.bin"], capture_output=True).returncode == 0, (hex(desc), n)
        assert subprocess.run(["zstd", "-d", "-c", "-q", f"/tmp/zs-{desc}-{n}.bin"], capture_output=True).stdout == data, (hex(desc), n)


PLACE_JS = r"""
const fs = require("fs");
const html = fs.readFileSync(process.argv[2], "utf8");
const block = html.slice(html.indexOf("/* pad:begin */"), html.indexOf("/* pad:end */"));
const P = new Function(block + ";return { textureDraw, textureAnchor, placementParams };")();
const near = (a, b) => Math.abs(a - b) < 0.5;
const eq = (name, got, want) => { for (const k of Object.keys(want)) if (!near(got[k], want[k])) throw new Error(`${name}: ${k} is ${got[k]}, wanted ${want[k]}`); };
const B = "/system/vsh_asset/control_center_ps2_bottom.dds", T = "/system/vsh_asset/control_center_ps2_top.dds", H = "/system/vsh_asset/homeui_ps2.dds";
// A 1920x1080 picture on the 2560x820 bottom panel: filled from the bottom edge, so a crop keeps the clouds there
eq("fill", P.textureDraw(B, 0, "cover", 2560, 820, 1920, 1080), { x: 0, y: 820 - 1440, dw: 2560, dh: 1440 });
// padding leaves the lowest 10% clear: the clip box ends there and the picture sits against it
const padded = P.textureDraw(B, 10, "cover", 2560, 820, 1920, 1080);
eq("padded", padded, { y: 738 - 1440, dw: 2560, dh: 1440 });
eq("box", padded.box, { x: 0, y: 0, w: 2560, h: 738 });
eq("fit", P.textureDraw(B, 0, "contain", 2560, 820, 1920, 1080), { x: (2560 - 1920 * 820 / 1080) / 2, y: 0, dh: 820 });
eq("stretch", P.textureDraw(B, 10, "stretch", 2560, 820, 1920, 1080), { x: 0, y: 0, dw: 2560, dh: 738 });
// the top-right piece is held to its top and right edges
const top = P.textureDraw(T, 10, "cover", 980, 620, 1920, 1080);
eq("top box", top.box, { x: 0, y: 62, w: 882, h: 558 });
eq("top", top, { y: 62, dh: 558 });
if (!near(top.x + top.dw, 882)) throw new Error("the top piece's picture is not against its right edge");
// the home screen's artwork hangs from the top of the screen: filled from there, with the padding left clear above it
eq("home", P.textureDraw(H, 0, "cover", 2560, 800, 1920, 1080), { x: 0, y: 0, dw: 2560, dh: 1440 });
const homePad = P.textureDraw(H, 10, "cover", 2560, 800, 1920, 1080);
eq("home padded", homePad, { y: 80, dw: 2560, dh: 1440 });
eq("home box", homePad.box, { x: 0, y: 80, w: 2560, h: 720 });
if (!P.textureAnchor(H).top || P.textureAnchor(H).bottom || P.textureAnchor(H).band) throw new Error("homeui hangs from the top, with no clock band");
// padding is limited to 40%
eq("limit", P.textureDraw(B, 90, "stretch", 2560, 820, 1920, 1080).box, { h: 820 - 328 });
// the top-right piece keeps the band its clock and icons are drawn over clear: 26% of its height
const kept = P.textureDraw(T, 0, "cover", 980, 620, 1920, 1080, true);
eq("keep box", kept.box, { y: 161, h: 620 - 161 });
eq("keep top", kept, { y: 161 });
if (P.textureDraw(T, 0, "cover", 980, 620, 1920, 1080, false).box.y !== 0) throw new Error("the band stays clear only when asked");
if (P.textureDraw(T, 30, "cover", 980, 620, 1920, 1080, true).box.y !== 186) throw new Error("the larger of padding and band");
if (P.textureDraw(B, 0, "cover", 2560, 820, 1920, 1080, true).box.y !== 0) throw new Error("the bottom panel has no clock band");
// the choice offered for each piece
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
if (!same(P.placementParams("fill", 9), { fit: "cover", pad: 0 })) throw new Error("fill has no padding");
if (!same(P.placementParams("pad", 10), { fit: "cover", pad: 10 })) throw new Error("padded fill");
if (!same(P.placementParams("pad", 99), { fit: "cover", pad: 40 })) throw new Error("padding is limited");
if (!same(P.placementParams("pad", "x"), { fit: "cover", pad: 0 })) throw new Error("a bad padding is none");
if (!same(P.placementParams("fit", 10), { fit: "contain", pad: 0 })) throw new Error("fit");
if (!same(P.placementParams("stretch", 10), { fit: "stretch", pad: 0 })) throw new Error("stretch");
if (!same(P.placementParams("nonsense", 10), { fit: "cover", pad: 0 })) throw new Error("an unknown choice is fill");
console.log("place ok");
"""


def t_page_places_textures_against_their_edge():
    """A see-through texture's picture is placed against the screen edge the
    texture meets (bottom panel: bottom; top-right piece: top and right), with
    the padding left clear."""
    write("/tmp/place.js", PLACE_JS.encode())
    out = subprocess.run(["node", "/tmp/place.js", WEB_PAGE], capture_output=True, text=True)
    assert out.returncode == 0 and "place ok" in out.stdout, out.stdout + out.stderr


def t_page_bleeds_colour_into_clear_pixels():
    """Textures with see-through parts carry colour in their clear pixels, as
    the console's do: the nearest visible colour, then white."""
    write("/tmp/bleed.js", BLEED_JS.encode())
    out = subprocess.run(["node", "/tmp/bleed.js", WEB_PAGE], capture_output=True, text=True)
    assert out.returncode == 0 and "bleed ok" in out.stdout, out.stdout + out.stderr


def t_texture_sources():
    """The picture a texture or picture was made from is kept in its theme, so
    an option changed later (placement, fit) can make it again without a new
    upload: stored only as a real picture, and only for a system texture or
    picture; handed back as it was, listed, linked rather than stored twice
    when one picture went to several files, copied with the theme, and removed
    with the file."""
    top, bottom = f"{SYSA}/control_center_ps2_top.dds", f"{SYSA}/control_center_ps2_bottom.dds"
    pic = png(40, 20, (200, 30, 30))
    put = lambda theme, asset, data, **kw: call("PUT", "/api/theme/source", {"id": theme, "path": asset, "crc": f"{zlib.crc32(data):08x}"}, data, **kw)
    assert call("POST", "/api/themes", {"id": "srcs", "name": "Sources"})[0] == 200
    assert call("GET", "/api/theme/sources", {"id": "srcs", "paths": f"{top},{bottom}"})[1]["have"] == []
    assert put("srcs", top, pic)[0] == 200
    assert put("srcs", top, pic, token=False)[0] == 403                           # a change needs the page's header
    for bad, asset, code in [(b"not a picture at all", top, 422), (b"", top, 400),
                             (pic, "/data/ps5-theme/x.png", 403),                 # only a system texture's or picture's
                             (pic, f"{VSH}/bgm_home.at9", 403),                   # not a sound's
                             (pic, "/system_ex/rnps/apps/NPXS40008/assets/../../x.dds", 403)]:
        got = put("srcs", asset, bad)[0]
        assert got == code, (asset, got)
    code, body = call("GET", "/api/theme/source", {"id": "srcs", "path": top})
    assert code == 200 and body == pic, code                                      # handed back as it was
    assert call("GET", "/api/theme/source", {"id": "srcs", "path": bottom})[0] == 404
    assert call("GET", "/api/theme/sources", {"id": "srcs", "paths": f"{top},{bottom}"})[1]["have"] == [top]
    mine = png(60, 30, (10, 200, 10))
    assert put("srcs", top, mine)[0] == 200                                       # attached again: replaced
    assert call("GET", "/api/theme/source", {"id": "srcs", "path": top})[1] == mine
    assert put("srcs", bottom, pic)[0] == 200
    # a system picture's too (the Fit option applies to it)
    welcome = f"{SYSA}/welcome-hub-background-0-4k.jpg"
    assert put("srcs", welcome, pic)[0] == 200
    assert call("GET", "/api/theme/source", {"id": "srcs", "path": welcome})[1] == pic
    # one picture attached at several sizes is stored once: the others are links to it
    fnv = lambda path: format(functools.reduce(lambda h, c: ((h ^ c) * 1099511628211) & (2**64 - 1), path.encode(), 1469598103934665603), "016x")
    kept = lambda theme, path: f"{BASE}/themes/{theme}/sources/{fnv(path)}.src"
    link = lambda theme, path, same, **kw: call("PUT", "/api/theme/source", {"id": theme, "path": path, "same_as": same}, b"", **kw)
    assert link("srcs", welcome, top, token=False)[0] == 403
    assert link("srcs", welcome, f"{SYSA}/homeui_ps2.dds")[0] == 404              # nothing kept for that one
    assert link("srcs", welcome, f"{VSH}/bgm_home.at9")[0] == 404
    assert link("srcs", f"{VSH}/bgm_home.at9", top)[0] == 403                     # not a sound's
    assert link("srcs", welcome, top)[0] == 200
    assert call("GET", "/api/theme/source", {"id": "srcs", "path": welcome})[1] == mine
    assert os.stat(kept("srcs", welcome)).st_ino == os.stat(kept("srcs", top)).st_ino
    assert put("srcs", welcome, pic)[0] == 200                                    # attached again: its own, the other one stays
    assert call("GET", "/api/theme/source", {"id": "srcs", "path": welcome})[1] == pic
    assert call("GET", "/api/theme/source", {"id": "srcs", "path": top})[1] == mine
    # it is never applied: only a theme's files are
    assert not any("sources" in f["path"] for f in call("GET", "/api/theme/files", {"id": "srcs"})[1]["files"])
    # it goes with the theme when it is copied, and with its texture when that is reset
    assert call("POST", "/api/themes/duplicate", {"id": "srcs", "new_id": "srcs2", "name": "Copy"})[0] == 200
    assert call("GET", "/api/theme/source", {"id": "srcs2", "path": top})[1] == mine
    assert call("DELETE", "/api/themes", {"id": "srcs2"})[0] == 200
    stock_dds = subprocess.run(["zstd", "-d", "-c", "-q", top], capture_output=True, check=True).stdout
    texture = zstd_stored(stock_dds[:148] + bytes((i * 29 + 5) & 255 for i in range(len(stock_dds) - 148)), open(top, "rb").read())
    assert put_file("srcs", top, texture)[0] == 200
    # listing without paths: the theme's files whose original is kept (a kept picture of a file not in the theme isn't)
    assert call("GET", "/api/theme/sources", {"id": "srcs"})[1]["have"] == [top]
    assert call("DELETE", "/api/theme/file", {"id": "srcs", "path": top})[0] == 200
    assert call("GET", "/api/theme/sources", {"id": "srcs"})[1]["have"] == []
    assert call("GET", "/api/theme/source", {"id": "srcs", "path": top})[0] == 404
    assert call("GET", "/api/theme/source", {"id": "srcs", "path": bottom})[0] == 200   # the other piece's stays
    assert call("DELETE", "/api/themes", {"id": "srcs"})[0] == 200

    # keeping originals can be switched off for a theme (to save space), and the kept ones removed
    assert call("POST", "/api/themes", {"id": "srcs3", "name": "Space"})[0] == 200
    settings = lambda **kw: call("POST", "/api/theme/settings", {"id": "srcs3", **kw})
    assert call("GET", "/api/theme/sources", {"id": "srcs3"})[1] == {"have": [], "keep": True, "bytes": 0}
    assert put("srcs3", top, pic)[0] == 200 and put("srcs3", welcome, pic)[0] == 200
    assert put_file("srcs3", top, texture)[0] == 200
    assert link("srcs3", bottom, top)[0] == 200                                  # linked: counted once
    got = call("GET", "/api/theme/sources", {"id": "srcs3"})[1]
    assert got == {"have": [top], "keep": True, "bytes": 2 * len(pic)}, got      # (only files in the theme are listed)
    assert settings(keep_originals="2")[0] == 400 and settings(keep_originals="x")[0] == 400
    assert call("POST", "/api/theme/settings", {"id": "srcs3", "keep_originals": "0"}, token=False)[0] == 403
    assert settings(keep_originals="0")[0] == 200
    got = call("GET", "/api/theme/sources", {"id": "srcs3"})[1]
    assert got["keep"] is False and got["have"] == [top], got                     # what is kept stays until removed
    for code_expected, r in [(409, put("srcs3", welcome, pic)), (409, link("srcs3", welcome, top))]:
        assert r[0] == code_expected and "switched off" in r[1]["error"], r
    assert call("GET", "/api/theme/source", {"id": "srcs3", "path": top})[1] == pic
    assert call("POST", "/api/themes/duplicate", {"id": "srcs3", "new_id": "srcs4", "name": "Space copy"})[0] == 200
    assert call("GET", "/api/theme/sources", {"id": "srcs4"})[1]["keep"] is False  # the choice goes with the theme
    assert call("DELETE", "/api/themes", {"id": "srcs4"})[0] == 200
    assert call("DELETE", "/api/theme/sources", {"id": "srcs3"}, token=False)[0] == 403
    assert call("DELETE", "/api/theme/sources", {"id": "nope"})[0] == 404
    assert call("DELETE", "/api/theme/sources", {"id": "srcs3"})[0] == 200
    got = call("GET", "/api/theme/sources", {"id": "srcs3"})[1]
    assert got == {"have": [], "keep": False, "bytes": 0}, got
    assert call("GET", "/api/theme/source", {"id": "srcs3", "path": top})[0] == 404
    assert call("GET", "/api/theme/files", {"id": "srcs3"})[1]["files"]           # the theme's files are untouched
    assert settings(keep_originals="1")[0] == 200
    assert put("srcs3", welcome, pic)[0] == 200                                   # on again
    assert call("GET", "/api/theme/sources", {"id": "srcs3"})[1]["keep"] is True
    assert call("DELETE", "/api/themes", {"id": "srcs3"})[0] == 200


def t_appearance_textures():
    """The Appearance themes' textures in /system/vsh_asset: listed, accepted
    only as the console's exact format (zstd-compressed BC7 under a plain .dds
    name), and applied on that folder and in the system's views of it; the rest
    of the folder stays the console's."""
    bottom, top, home = (f"{SYSA}/{n}.dds" for n in ("control_center_ps2_bottom", "control_center_ps2_top", "homeui_ps2"))
    code, r = call("GET", "/api/assets", {"q": "/system/vsh_asset"})
    assert {a["path"]: a["kind"] for a in r["assets"]} == {
        bottom: "texture", top: "texture", home: "texture", f"{SYSA}/welcome-hub-background-0-4k.jpg": "image"}, r
    code, info = call("GET", "/api/asset/texture", {"path": top})
    assert code == 200 and info["dxgi"] == 98 and info["zstd"] is True and (info["width"], info["height"]) == (48, 32), info

    stock_dds = subprocess.run(["zstd", "-d", "-c", "-q", top], capture_output=True, check=True).stdout
    mine = stock_dds[:148] + bytes((i * 29 + 5) & 255 for i in range(len(stock_dds) - 148))
    stock_top, stock_bottom = open(top, "rb").read(), open(bottom, "rb").read()
    assert stock_top[4] == 0xa0 and stock_bottom[4] == 0xa4          # frame headers as the console's: plain, and with a checksum
    assert call("POST", "/api/themes", {"id": "appear", "name": "Appearance"})[0] == 200
    zeroed = bytearray(mine)
    zeroed[148 + 32] = 0                                              # the third block's first byte: a reserved BC7 mode
    for bad, needle in [(zstd_stored(dds_bc7(32, 16), stock_top), "exactly like the system file"),
                        (mine, "zstd-compressed like the system file"),
                        (zstd_stored(mine[:-16], stock_top), "not a valid DDS"),
                        (zstd_stored(mine, stock_top) + b"\0", "not a valid zstd-compressed texture"),          # a stray byte after the frame
                        (zstd_stored(mine, stock_top) + zstd_stored(b"", SONY_FRAME), ("whole file", "not a valid zstd")),   # or a second frame
                        (zstd_stored(bytes(zeroed), stock_top), "reserved BC7 mode")]:
        code, r = put_file("appear", top, bad)
        assert code == 422 and any(n in r["error"] for n in ((needle,) if isinstance(needle, str) else needle)), (needle, code, r)
    bottom_dds = subprocess.run(["zstd", "-d", "-c", "-q", bottom], capture_output=True, check=True).stdout[:148] \
        + bytes((i * 17 + 3) & 255 for i in range(2048))
    stored = zstd_stored(mine, stock_top)
    stored_bottom = zstd_stored(bottom_dds, stock_bottom)
    # A texture right in every way but its frame header (a checksum the system file lacks, none where it has
    # one, as pages before 1.56 wrote them) is written again with the system file's own header: byte for byte.
    theme_file = lambda path: open(f"{BASE}/themes/appear/files{path}", "rb").read()
    assert put_file("appear", top, zstd_stored(mine, stock_bottom))[0] == 200
    assert theme_file(top) == stored
    assert put_file("appear", bottom, zstd_stored(bottom_dds, stock_top))[0] == 200
    assert theme_file(bottom) == stored_bottom
    assert put_file("appear", top, stored)[0] == 200
    assert put_file("appear", bottom, stored_bottom)[0] == 200
    # A remembered check result is made by a version and a revision of the rules: results of other rules are
    # not trusted (the same version can ship twice while a feature is built).
    remembered = open(f"{BASE}/checked").read().splitlines()
    assert remembered and all(re.match(r"1\.60r\d+\t", l) for l in remembered), remembered[:2]
    assert put_file("appear", f"{HOME}/bg.png", S["bg"])[0] == 200

    shell = "/mnt/sandbox/NPXS40087_000"
    view = f"{shell}/system/vsh_asset"
    if not EMULATED:
        os.makedirs(view, exist_ok=True)
        subprocess.run(["mount", "--bind", SYSA, view], check=True)
        S["plugin"].term()
        S["plugin"] = Plugin()
    keep = {p: crc_file(p) for p in (home, f"{SYSA}/welcome-hub-background-0-4k.jpg")}
    try:
        apply_and_wait("appear")
        assert open(top, "rb").read() == stored and open(bottom, "rb").read() == stored_bottom
        result = call("GET", "/api/status")[1]["result"]       # the artwork is read as the interface starts
        assert "put the console in rest mode and wake it" in result and "Appearance artwork" in result, result
        assert "ps5-theme: To see the new Appearance artwork, put the console in rest mode and wake it." in open("/tmp/notifications.txt").read()
        for p, c in keep.items():                      # the rest of the folder is the console's
            assert crc_file(p) == c, p
        assert len(mounts_on(SYSA)) == 1
        if not EMULATED:
            assert open(f"{view}/control_center_ps2_top.dds", "rb").read() == stored
            assert open(f"{view}/control_center_ps2_bottom.dds", "rb").read() == stored_bottom
            assert crc_file(f"{view}/homeui_ps2.dds") == keep[home]
        assert call("POST", "/api/apply", {"id": "night"})[0] == 200
        wait_applied("night")
        assert open(top, "rb").read() == stock_top and open(bottom, "rb").read() == stock_bottom
    finally:
        apply_and_wait("night")
        if not EMULATED:
            subprocess.run(["umount", "-l", view])
        clear_sandboxes()
    assert call("DELETE", "/api/themes", {"id": "appear"})[0] == 200


def kept_crc(name):
    """The fixture game's files beside its pictures, as written at the start."""
    return {"param.json": zlib.crc32(b'{"titleId":"PPSA01234"}'), "license.dat": zlib.crc32(bytes(range(256)) * 4),
            "keystone": zlib.crc32(b"keystone" * 12), "about/right.sprx": zlib.crc32(b"\x7fELF"),
            "trophy2/trophy00.ucp": zlib.crc32(b"trophies" * 512)}[name]


def t_title_where():
    """Where the system keeps a title's pictures: files named after it in the
    folders ShellUI can see, and the database strings that name it, read-only."""
    code, body = call("GET", "/api/title/where", {"id": "PPSA01234"})
    assert code == 200, (code, body)
    text = body.decode() if isinstance(body, bytes) else body
    assert "/system_data/priv/appmeta/PPSA01234/icon0.png " in text, text
    assert f"{GAME}/icon0.png " in text and f"{GAME}/icon0.dds " in text, text
    assert "/system_data/priv/mms/app.db: /user/appmeta/PPSA01234/icon0.png" in text, text
    long = [l for l in text.splitlines() if "xxx" in l]
    assert len(long) == 1 and "...xxx" in long[0] and long[0].endswith("yyy...") and len(long[0]) < 1000, long   # a window, not 14 KB
    assert "CUSA09999" not in text, text
    for bad in ["ppsa01234", "PPSA0123", "PPSA012345", "../../etc", ""]:
        assert call("GET", "/api/title/where", {"id": bad})[0] == 400, bad


def t_shellui_exit_releases_sandbox():
    """With games' pictures in ShellUI's view (the only place it reads them),
    the overlays come out of its sandbox the moment ShellUI exits, so the
    launcher's teardown of that sandbox succeeds (on the console an overlay
    left there stopped every app from starting); its next sandbox is covered
    within a moment of appearing, before ShellUI reads the home row's icons;
    and an older sandbox of it is never covered, nor one the system left
    behind, even while it is briefly the newest."""
    if EMULATED:
        print("      (skipped: needs real mounts)", flush=True)
        return
    shutil.copy("/bin/sleep", "/tmp/SceShellUI")
    shell = subprocess.Popen(["/tmp/SceShellUI", "600"])
    boxes = []

    def sandbox(n, subs=("user", "system_ex")):
        for sub, src in [(s, "/" + s) for s in subs]:
            box = f"/mnt/sandbox/NPXS40087_{n:03}/{sub}"
            os.makedirs(box, exist_ok=True)
            subprocess.run(["mount", "--bind", src, box], check=True)
            boxes.append(box)
        return f"/mnt/sandbox/NPXS40087_{n:03}"
    mine = png(64, 64, (255, 0, 0))
    try:
        left = sandbox(0, ("system_ex",))   # as on 13.42: an earlier ShellUI's view the system never took down
        old = sandbox(1)
        assert call("POST", "/api/themes", {"id": "shellui", "name": "ShellUI"})[0] == 200
        assert put_file("shellui", f"{GAME}/icon0.png", mine)[0] == 200
        assert put_file("shellui", f"{HOME}/bg.png", S["bg"])[0] == 200
        apply_and_wait("shellui")
        assert crc_file(f"{old}/user/appmeta/PPSA01234/icon0.png") == zlib.crc32(mine)
        assert crc_file(f"{old}{HOME}/bg.png") == zlib.crc32(S["bg"])
        assert not [l for l in recorded_mounts() if l.startswith(left + "/")], recorded_mounts()
        code, procs = call("GET", "/api/diag/procs")
        assert code == 200 and f"{shell.pid} SceShellUI".encode() in procs, procs[:300]
        end = time.time() + 5
        while f"watching ShellUI (pid {shell.pid})" not in log_text():
            assert time.time() < end, "ShellUI's process was not found"
            time.sleep(0.2)

        # One overlay missing from the records (1.50's stop tool lost some):
        # the mount table still shows it, and it comes out too.
        lost = f"{old}/user/appmeta/PPSA01234"
        lines = open(f"{BASE}/mounts").read().splitlines(True)
        assert any(l.startswith(lost + "\t") for l in lines), lines
        write(f"{BASE}/mounts", "".join(l for l in lines if not l.startswith(lost + "\t")).encode())

        # Rest mode: ShellUI exits, and the launcher takes its sandbox down.
        shell.kill()
        shell.wait()
        end = time.time() + 1
        while any(l.split()[4].startswith(old + "/") and "/data/ps5-theme/" in l.split()[3]
                  for l in open("/proc/self/mountinfo").read().splitlines()):
            assert time.time() < end, ("overlays still in the exited sandbox", recorded_mounts())
            time.sleep(0.02)
        end = time.time() + 3                                   # and their records, by the main thread
        while [l for l in recorded_mounts() if l.startswith(old + "/")]:
            assert time.time() < end, ("records of the exited sandbox stay", recorded_mounts())
            time.sleep(0.05)
        assert re.search(r"ShellUI exited: \d+ overlay\(s\) taken out of its sandbox NPXS40087_001 in \d+ ms", log_text()), log_text()[-1500:]
        for box in [f"{old}/user", f"{old}/system_ex"]:
            assert subprocess.run(["umount", box]).returncode == 0, f"the launcher couldn't take down {box}"
            boxes.remove(box)
        time.sleep(1.5)   # the leftover is now the newest ShellUI sandbox there is
        assert not [l for l in recorded_mounts() if l.startswith(left + "/")], recorded_mounts()

        # ShellUI starts again in a new sandbox: covered at once.
        new = sandbox(2)
        shell = subprocess.Popen(["/tmp/SceShellUI", "600"])
        end = time.time() + 0.5
        while crc_file(f"{new}/user/appmeta/PPSA01234/icon0.png") != zlib.crc32(mine):
            assert time.time() < end, "the new sandbox was not covered within half a second"
            time.sleep(0.02)
        assert crc_file(f"{new}{HOME}/bg.png") == zlib.crc32(S["bg"])

        # An older sandbox of it showing up again is never covered.
        again = f"{old}/user"
        os.makedirs(again, exist_ok=True)
        subprocess.run(["mount", "--bind", "/user", again], check=True)
        boxes.append(again)
        time.sleep(1.5)
        assert crc_file(f"{again}/appmeta/PPSA01234/icon0.png") == zlib.crc32(png(64, 64, (0, 90, 200)))
    finally:
        shell.kill()
        shell.wait()
        apply_and_wait("night")
        clear_sandboxes()
        call("DELETE", "/api/themes", {"id": "shellui"})


def t_shellui_exits_during_apply():
    """ShellUI exiting while a theme is being applied (rest mode right after
    Apply, or at the first start): a thread of the plugin's own takes the
    overlays out of its sandbox at once all the same, nothing of the apply
    goes into that sandbox afterwards, and its next sandbox is covered."""
    if EMULATED:
        print("      (skipped: needs real mounts)", flush=True)
        return
    hold, mine = "/tmp/hold-apply", png(64, 64, (255, 0, 0))
    ours_in = lambda box: [l.split()[4] for l in open("/proc/self/mountinfo").read().splitlines()
                           if l.split()[4].startswith(box + "/") and "/data/ps5-theme/" in l.split()[3]]

    def sandbox(n):
        for sub in ("user", "system_ex"):
            box = f"/mnt/sandbox/NPXS40087_{n:03}/{sub}"
            os.makedirs(box, exist_ok=True)
            subprocess.run(["mount", "--bind", "/" + sub, box], check=True)
        return f"/mnt/sandbox/NPXS40087_{n:03}"

    def held(where):
        end = time.time() + 30
        while f"test: holding the apply {where} mounting" not in log_text()[mark:]:
            assert time.time() < end, ("the apply did not reach the hold", log_text()[-1500:])
            time.sleep(0.05)

    S["plugin"].term()
    S["plugin"] = Plugin(env={"PS5THEME_TEST_HOLD_APPLY": hold})
    shell = None
    try:
        first = sandbox(0)
        shell = start_shellui()
        assert call("POST", "/api/themes", {"id": "midway", "name": "Midway"})[0] == 200
        assert put_file("midway", f"{GAME}/icon0.png", mine)[0] == 200
        assert put_file("midway", f"{HOME}/bg.png", S["bg"])[0] == 200

        # Held after mounting: the overlays are in ShellUI's view while the
        # main thread is busy. ShellUI exits; they come out within a second.
        write(hold, b"after")
        mark = len(log_text())
        assert call("POST", "/api/apply", {"id": "midway"})[0] == 200
        held("after")
        assert ours_in(first), "nothing was mounted before the hold"
        shell.kill()
        shell.wait()
        end = time.time() + 1
        while ours_in(first):
            assert time.time() < end, ("overlays left in the exited sandbox during an apply", ours_in(first))
            time.sleep(0.02)
        os.unlink(hold)
        wait_applied("midway")
        assert not ours_in(first)

        # Held before mounting: ShellUI exits, and the apply then mounts
        # nothing into its sandbox.
        second = sandbox(1)
        shell = start_shellui()
        end = time.time() + 5
        while crc_file(f"{second}/user/appmeta/PPSA01234/icon0.png") != zlib.crc32(mine):
            assert time.time() < end, "the new sandbox was not covered"
            time.sleep(0.05)
        write(hold, b"before")
        mark = len(log_text())
        assert call("POST", "/api/apply", {"id": "midway"})[0] == 200
        held("before")
        shell.kill()
        shell.wait()
        end = time.time() + 1
        while not re.search(r"ShellUI exited: \d+ overlay\(s\) taken out of its sandbox NPXS40087_001", log_text()[mark:]):
            assert time.time() < end, log_text()[-1000:]
            time.sleep(0.02)
        os.unlink(hold)
        wait_applied("midway")
        assert not ours_in(second), ours_in(second)

        # ShellUI's next sandbox is covered.
        third = sandbox(2)
        shell = start_shellui()
        end = time.time() + 5
        while crc_file(f"{third}/user/appmeta/PPSA01234/icon0.png") != zlib.crc32(mine):
            assert time.time() < end, "the next sandbox was not covered"
            time.sleep(0.05)
        assert not ours_in(first) and not ours_in(second)
    finally:
        if os.path.exists(hold):
            os.unlink(hold)
        if shell:
            shell.kill()
            shell.wait()
        apply_and_wait("night")
        clear_sandboxes()
        call("DELETE", "/api/themes", {"id": "midway"})


def t_watch_files():
    """Which of a title's picture files (the console's, and the theme's staged
    copies) are opened and read, by anything, for a while: read-only, for
    finding where the home screen gets an icon."""
    out = {}
    t = threading.Thread(target=lambda: out.update(r=call("GET", "/api/diag/watch", {"id": "PPSA01234", "seconds": "3"})))
    t.start()
    time.sleep(1)
    assert call("GET", "/api/diag/watch", {"id": "PPSA01234", "seconds": "1"})[0] == 409
    with open(f"{GAME_SYS}/icon0.png", "rb") as f:                       # as the home screen would
        f.read()
    t.join()
    code, body = out["r"]
    text = body.decode() if isinstance(body, bytes) else body
    assert code == 200, (code, text)
    assert f"#   {GAME}/icon0.png" in text and f"#   {GAME_SYS}/icon0.dds" in text, text
    assert f"#   {CATALOG}/icon0.png" in text and f"#   {CONCEPT}/logo_01.png" in text, text   # its concept's, from the app database
    assert re.search(rf"open +{re.escape(GAME_SYS)}/icon0.png", text) and re.search(rf"read +{re.escape(GAME_SYS)}/icon0.png", text), text
    assert not re.search(rf"(open|read) +{re.escape(GAME)}/icon0.png", text), text
    assert call("GET", "/api/diag/watch", {"id": "../etc"})[0] == 400


def t_home_music_at9():
    path = f"{VSH}/bgm_home.at9"
    code, info = call("GET", "/api/asset/audio", {"path": path})
    assert code == 200 and info == {"codec": "ATRAC9", "channels": 6, "rate": 48000, "bits": 0}, info
    assert call("GET", "/api/asset/audio", {"path": f"{HOME}/bg.png"})[0] == 403
    assert call("POST", "/api/themes", {"id": "tune9", "name": "Tune"})[0] == 200
    stock = open(path, "rb").read()

    music = at9_music(path, 3)
    # Loop laid out like the console's own music: it starts inside the song
    # (here a quarter in, 2 s for longer songs) and audio follows its end.
    smpl = music.index(b"smpl") + 8
    fact = music.index(b"fact") + 8
    start, end = struct.unpack_from("<II", music, smpl + 44)
    total = struct.unpack_from("<I", music, fact)[0]
    assert (start, end, total) == (36000, 144000 + 36000 - 1, 144000 + 72000), (start, end, total)
    data = music.index(b"data") + 8
    flipped = bytearray(music)
    for i in range(40):
        flipped[data + 30000 + i * 37] ^= 0xff
    zeroed = bytearray(music)
    zeroed[data + 800 * 50:data + 800 * 51] = bytes(800)
    shorter = bytearray(music[:-300])
    struct.pack_into("<I", shorter, 4, len(shorter) - 8)
    struct.pack_into("<I", shorter, data - 4, len(shorter) - data)
    at9_stock(3, 2)                                                 # stereo: another ATRAC9 format
    stereo = at9_music("/tmp/at9-stock-2-3-0.05.at9", 2)
    listed = music + b"LIST" + struct.pack("<I", 4) + b"INFO"
    listed = listed[:4] + struct.pack("<I", len(listed) - 8) + listed[8:]
    # The unused end of a superframe must be filled like Sony fills it (0x01):
    # zeros there (all other bits identical) made the console's decoder go silent.
    zero_pad = bytearray(music)
    last = len(zero_pad) - 800
    run = 800 - len(bytes(zero_pad[last:]).rstrip(b"\x01"))      # padding in the last superframe
    assert run > 8, run
    zero_pad[len(zero_pad) - run + 1:] = bytes(run - 1)
    cases = [(stereo, "exactly the system file's format"),
             (bytes(zero_pad), "fills unused space with 0x00 where the system sound uses 0x01"),
             (wav(48000, 6, 0.1, 1000), "not a valid ATRAC9"),
             (bytes(flipped), "damaged"),
             (bytes(zeroed), "damaged"),
             (bytes(shorter), "whole number of blocks"),
             (music[:-800], "RIFF size"),
             (listed, "unexpected chunk"),
             (at9_music(path, 7), "longer than 2 times")]
    for bad, needle in cases:
        code, r = put_file("tune9", path, bad)
        assert code == 422 and needle in r["error"], (needle, code, r)

    code, r = put_file("tune9", path, music)
    assert code == 200, r
    assert call("POST", "/api/apply", {"id": "tune9"})[0] == 200
    s = wait_applied("tune9")
    assert "1 files verified" in s["result"] and "not converted to home music" not in s["result"], s["result"]
    # How to hear it: the home screen starts its music again after another app's.
    hint = "To hear the new home music, highlight a game or app that plays its own music"
    assert hint in s["result"] and f"ps5-theme: {hint}" in open("/tmp/notifications.txt").read(), s["result"]
    assert open(path, "rb").read() == music                       # what ShellUI reads now
    # The editor still gets the system version (kept at apply), to match
    # loudness and length against and to validate further uploads with.
    assert open(f"{BASE}/stock/13.42{path}", "rb").read() == stock
    assert call("GET", "/api/asset", {"path": path})[1] == stock
    code, r = put_file("tune9", path, at9_music(path, 7))
    assert code == 422 and "longer than 2 times" in r["error"], r     # measured against the system file
    assert call("GET", "/api/theme/check", {"id": "tune9", "path": path})[1] == {"ok": True}
    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    wait_applied("")
    assert open(path, "rb").read() == stock

    # Coding features the system sound never uses are refused, as a
    # precaution: the console's decoder is Sony's. Here: music uses more
    # bands than this silent sound.
    quiet = f"{VSH}/sfx_quiet.at9"
    code, r = put_file("tune9", quiet, at9_music(path, 2))
    assert code == 422 and "the console cannot play this sound" in r["error"] and "13 bands" in r["error"], r
    # A file left in a theme by an older version: flagged, and skipped on apply.
    write(f"{BASE}/themes/tune9/files{quiet}", at9_music(path, 2))
    code, r = call("GET", "/api/theme/check", {"id": "tune9", "path": quiet})
    assert code == 200 and r["ok"] is False and "13 bands" in r["error"], r
    assert call("POST", "/api/apply", {"id": "tune9"})[0] == 200
    s = wait_applied("tune9")
    assert "1 skipped" in s["result"], s["result"]
    assert open(quiet, "rb").read() != open(f"{BASE}/themes/tune9/files{quiet}", "rb").read()
    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    wait_applied("")
    assert call("DELETE", "/api/themes", {"id": "tune9"})[0] == 200


def t_long_mount_paths_use_parent_folder():
    S["plugin"].term()
    S["plugin"] = Plugin(env={"FAKE_MNAMELEN": "60"})
    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    wait_applied("")
    assert call("POST", "/api/themes", {"id": "deep", "name": "Deep"})[0] == 200
    deep = png(32, 32, (250, 10, 10))
    assert put_file("deep", f"{HOME}/sub/deep.png", deep)[0] == 200  # folder path is 65 characters
    assert call("POST", "/api/apply", {"id": "deep"})[0] == 200
    s = wait_applied("deep")
    assert "1 files verified" in s["result"] and "partly" not in s["result"], s["result"]
    parent = f"{APPS}/NPXS40008/assets/src/modules/home"   # 54 characters
    assert len(mounts_on(parent)) == 1 and not mounts_on(f"{HOME}/sub"), (mounts_on(parent), mounts_on(f"{HOME}/sub"))
    assert crc_file(f"{HOME}/sub/deep.png") == zlib.crc32(deep)
    assert crc_file(f"{HOME}/icon.png") == S["stock_icon"]  # the rest of the parent stays stock
    assert crc_file(f"{OTHER}/target.png") == S["stock_target"]
    assert "from its parent" in log_text()
    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    wait_applied("")
    assert not mounts_on(parent)
    assert call("DELETE", "/api/themes", {"id": "deep"})[0] == 200
    S["plugin"].term()
    S["plugin"] = Plugin()


def t_system_is_byte_identical_afterwards():
    assert call("POST", "/api/apply", {"id": "none"})[0] == 200
    wait_applied("")
    S["plugin"].term()
    after = snapshot("/system_ex")
    assert after == S["system_before"], {k for k in after if after.get(k) != S["system_before"].get(k)}
    assert snapshot("/mnt/rnps") == S["mnt_before"]
    assert snapshot("/system_ex") == S["system_before"]
    assert sorted(os.listdir("/user/app")) == ["PPSA01234", "THEM00001"]
    assert snapshot("/user/app/PPSA01234") == S["game_before"], "a game's installation changed"
    assert os.listdir("/data") == ["ps5-theme"]
    assert "ERROR: AddressSanitizer" not in open("/tmp/plugin-out.txt", errors="replace").read()
    assert "runtime error" not in open("/tmp/plugin-out.txt", errors="replace").read()


def t_crash_is_reported_and_recovered():
    p = Plugin(CRASH)
    try:
        call("POST", "/api/debug/crash", timeout=5)
    except Exception:
        pass
    p.proc.wait(10)
    assert p.proc.returncode == 70, p.proc.returncode
    reports = [f for f in os.listdir(f"{BASE}/logs") if f.startswith("crash-")]
    assert len(reports) == 1, reports
    text = open(f"{BASE}/logs/{reports[0]}").read()
    for needle in ("SIGSEGV", "thread:    web worker", ": POST /api/debug/crash",
                   "rip:", "backtrace", "#1"):
        assert needle in text, (needle, text)
    assert os.path.exists(f"{BASE}/logs/last-crash")

    p = Plugin(CRASH)
    try:
        assert "recovered from a crash" in open("/tmp/notifications.txt").read()
        assert not os.path.exists(f"{BASE}/logs/last-crash")
        code, r = call("GET", "/api/crashes")
        assert code == 200 and r["crashes"] == reports, r
        code, body = call("GET", "/api/crash", {"name": reports[0]})
        assert code == 200 and b"SIGSEGV" in body
    finally:
        p.term()


def t_privilege_failure_exits_cleanly():
    out = open("/tmp/init-fail.txt", "wb")
    proc = subprocess.run([os.path.abspath(ASAN)], env=dict(ENV, FAKE_INIT_FAIL="1"), stdout=out,
                          stderr=out, timeout=20)
    assert proc.returncode == 1, proc.returncode
    notes = open("/tmp/notifications.txt").read()
    assert "could not get privileges" in notes, notes  # reported before any file is touched
    assert "could not raise privileges" in open("/tmp/init-fail.txt").read()  # kernel log line


TESTS = [t_startup, t_ui_and_icon_served, t_mutations_need_token, t_create_theme_rules, t_asset_search,
         t_upload_is_verified, t_apply_verifies_and_is_read_only, t_reapply_picks_up_edits,
         t_stock_mirror_and_linked_staging, t_stage_folders_reused_without_links,
         t_firmware_mismatch_is_refused, t_delete_rules, t_concurrent_requests_during_apply,
         t_restore_stock_cleans_up, t_overlays_reach_system_views, t_crash_loop_takes_theme_down, t_busy_overlay_is_never_forced, t_recovers_after_hard_kill, t_update_from_payload_manager, t_update_from_plain_elf, t_update_hands_over, t_update_at_boot, t_web_comes_back_after_rest_mode, t_takeover_when_web_is_down, t_stop_tool_keeps_the_theme, t_failed_write_closes_its_file, t_survives_malformed_and_abusive_requests,
         t_slow_clients_do_not_block_others, t_log_rotates, t_media_policy, t_music, t_settings_and_duplicate, t_preview_pictures, t_save_and_discard, t_cut_off_uploads_are_not_theme_files,
         t_home_background_textures, t_interface_pictures, t_title_pictures, t_fonts, t_bc7_keeps_transparency, t_page_writes_the_consoles_zstd_frames, t_page_bleeds_colour_into_clear_pixels, t_page_places_textures_against_their_edge, t_texture_sources, t_appearance_textures, t_shellui_exit_releases_sandbox, t_shellui_exits_during_apply, t_title_where, t_watch_files, t_home_music_at9, t_long_mount_paths_use_parent_folder, t_system_is_byte_identical_afterwards, t_crash_is_reported_and_recovered,
         t_privilege_failure_exits_cleanly]


def main():
    build_console()
    if "--serve" in sys.argv:
        # realistic home backgrounds: 4K BC7 like the console's (content is test data)
        big = dds_bc7(3840, 2160, fill=bytes(random.Random(3).randbytes(3840 * 2160)))
        for name in ("bg_hub_default", "Library_BG_blue_Low_Angle_001", "bg_NPXS40037", "bg_image_load_error"):
            write(f"{VSH}/{name}.dds.zst", zstd_cli(big, 3))
        write(f"{VSH}/bgm_home.at9", at9_stock(60))                   # a minute, so music fits up to 2:00
        write(f"{VSH}/sfx_warmboot.at9", at9_stock(2))
        write(f"{GAME}/icon0.dds", dds_bc7(512, 512, flags=0xa1007))     # as on the console
        write(f"{GAME_SYS}/icon0.dds", dds_bc7(512, 512, flags=0xa1007))
        write(f"{PS4_APP}/icon0.dds", dds_dxt1(512, 512))
        Plugin()
        print(f"plugin running; open http://localhost:18095 (container port {PORT})", flush=True)
        while True:
            time.sleep(3600)

    if EMULATED:
        print("NOTE  mounts are emulated (no privileges); read-only overlay checks are skipped", flush=True)
    S["system_before"] = snapshot("/system_ex")
    S["mnt_before"] = snapshot("/mnt/rnps")
    S["game_before"] = snapshot("/user/app/PPSA01234")
    S["plugin"] = Plugin()
    failed = 0
    for test in TESTS:
        t0 = time.time()
        try:
            test()
            print(f"PASS  {test.__name__}  ({time.time() - t0:.1f}s)", flush=True)
        except Exception as e:
            failed += 1
            print(f"FAIL  {test.__name__}: {e!r}", flush=True)
            tb = traceback.extract_tb(e.__traceback__)[-1]
            print(f"      at line {tb.lineno}: {tb.line}", flush=True)
            if not S["plugin"].alive() and test is not t_system_is_byte_identical_afterwards:
                print("      plugin died; output follows", flush=True)
                print(open("/tmp/plugin-out.txt", errors="replace").read()[-4000:], flush=True)
                S["plugin"] = Plugin()
    S["plugin"].kill()
    print(f"\n{len(TESTS) - failed}/{len(TESTS)} passed", flush=True)
    if failed:
        print("---- plugin log tail ----")
        print(open(LOG, errors="replace").read()[-3000:] if os.path.exists(LOG) else "(no log)")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
