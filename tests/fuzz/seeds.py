"""Stock files and starting corpora for the fuzz targets.
Usage: seeds.py <stock dir> <corpus dir>"""
import os
import random
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../host"))
from rco_fixture import Rco, hevag, palette_png, sample as rco_sample  # noqa: E402
import font_fixture  # noqa: E402

stock_dir, corpus = sys.argv[1], sys.argv[2]
os.makedirs(stock_dir, exist_ok=True)
r = random.Random(7)


def put(target, name, data):
    os.makedirs(f"{corpus}/{target}", exist_ok=True)
    open(f"{corpus}/{target}/{name}", "wb").write(data)


def zstd(data, *args):
    return subprocess.run(["zstd", "-c", "-q", *args], input=data, capture_output=True, check=True).stdout


def png(w, h, rgb):
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def wav(rate, channels, frames, bits=16, fmt=1, extra=b""):
    data = bytes(r.getrandbits(8) for _ in range(frames * channels * bits // 8))
    block = channels * bits // 8
    fmt_chunk = struct.pack("<HHIIHH", fmt, channels, rate, rate * block, block, bits) + extra
    body = (b"WAVE" + b"fmt " + struct.pack("<I", len(fmt_chunk)) + fmt_chunk
            + b"data" + struct.pack("<I", len(data)) + data)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def dds_bc7(w, h):
    blocks = ((w + 3) // 4) * ((h + 3) // 4)
    hdr = bytearray(148)
    hdr[0:4] = b"DDS "
    struct.pack_into("<7I", hdr, 4, 124, 0x81007, h, w, blocks * 16, 1, 1)
    struct.pack_into("<2I4s", hdr, 76, 32, 4, b"DX10")
    struct.pack_into("<I", hdr, 108, 0x1000)
    struct.pack_into("<5I", hdr, 128, 98, 3, 0, 1, 0)
    return bytes(hdr) + bytes((i * 37) & 255 for i in range(blocks * 16))


def dds_dxt1(w, h):
    hdr = bytearray(128)
    hdr[0:4] = b"DDS "
    struct.pack_into("<7I", hdr, 4, 124, 0x81007, h, w, 0, 1, 1)
    struct.pack_into("<2I4s", hdr, 76, 32, 4, b"DXT1")
    return bytes(hdr) + bytes(((w + 3) // 4) * ((h + 3) // 4) * 8)


# zstd
for name, data in [("text", b" ".join(r.choice([b"theme", b"home", b"music", b"ps5"]) for _ in range(3000))),
                   ("random", r.randbytes(5000)), ("zeros", bytes(20000)),
                   ("mixed", b"".join(r.randbytes(r.randint(1, 300)) if r.random() < .3 else bytes([r.randint(0, 255)]) * r.randint(1, 500)
                                      for _ in range(60)))]:
    for level in ("-1", "-19"):
        put("zstd", f"{name}{level}", zstd(data, level))
put("zstd", "nosize", zstd(b"abc" * 4000, "--no-content-size"))
put("zstd", "checksum", zstd(b"xyz" * 4000, "-C"))

# textures
stock_dds = dds_bc7(64, 32)
open(f"{stock_dir}/stock.dds.zst", "wb").write(zstd(stock_dds, "-19"))
put("texture", "same", zstd(stock_dds, "-3"))
put("texture", "raw", stock_dds)
put("texture", "dxt1", dds_dxt1(32, 32))
put("texture", "nosize", zstd(stock_dds, "--no-content-size"))

# ATRAC9: the builder's own encoder makes a stock file and a replacement
tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../host/at9_fixture.js")
subprocess.run(["node", tool, "stock", f"{stock_dir}/stock.at9", "1", "3", "0.05"], check=True, capture_output=True)
subprocess.run(["node", tool, "music", f"{stock_dir}/stock.at9", "/tmp/music.at9", "1", "440"], check=True, capture_output=True)
put("at9", "stock", open(f"{stock_dir}/stock.at9", "rb").read())
put("at9", "music", open("/tmp/music.at9", "rb").read())
subprocess.run(["node", tool, "stock", "/tmp/stereo.at9", "1", "2", "0.05"], check=True, capture_output=True)
put("at9", "stereo", open("/tmp/stereo.at9", "rb").read())

# WAV
open(f"{stock_dir}/stock.wav", "wb").write(wav(48000, 2, 200))
put("wav", "stock", wav(48000, 2, 200))
put("wav", "music", wav(48000, 2, 20000))
put("wav", "mono", wav(44100, 1, 300))
put("wav", "extensible", wav(48000, 2, 100, fmt=0xfffe, extra=struct.pack("<HHI", 22, 16, 3) + bytes(16)))

# images
put("image", "png", png(8, 4, (1, 2, 3)))
put("image", "jpeg", b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00"
                     b"\xff\xc0\x00\x11\x08\x00\x10\x00\x20\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01\xff\xd9")
put("image", "webp-lossy", b"RIFF\x24\x00\x00\x00WEBPVP8 \x18\x00\x00\x00\x30\x01\x00\x9d\x01\x2a\x10\x00\x08\x00" + bytes(14))
put("image", "webp-lossless", b"RIFF\x1a\x00\x00\x00WEBPVP8L\x0d\x00\x00\x00\x2f\x0f\xc0\x01\x00" + bytes(8))
put("image", "webp-extended", b"RIFF\x16\x00\x00\x00WEBPVP8X\x0a\x00\x00\x00\x10\x00\x00\x00\x0f\x00\x00\x07\x00\x00")

# JSON
open(f"{stock_dir}/stock.json", "wb").write(b'{"v":"5.7","w":100,"h":100,"layers":[]}')
put("json", "lottie", b'{"v":"5.7","w":100,"h":100,"layers":[{"ty":4,"ks":{"o":{"a":0,"k":100}}}]}')
put("json", "array", b'[1, -2.5e3, "a\\u00e9\\n", true, false, null, {"x": []}]')
put("json", "bom", b'\xef\xbb\xbf{"w": 100, "h": 100}')

# PNG pictures checked against the console's own (a palette picture)
open(f"{stock_dir}/stock-palette.png", "wb").write(palette_png(20, 20))
def png_filtered(w, h, ch, filt):
    """An 8-bit RGB (ch 3) or RGBA (ch 4) PNG whose rows use row filter `filt`."""
    rows = []
    for y in range(h):
        row = bytes(((x * 37 + y * 11 + c * 53) & 255) for x in range(w) for c in range(ch))
        rows.append(bytes([filt]) + row)   # (the filter byte with raw bytes: decodes to some picture, which is enough)
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2 if ch == 3 else 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))


for ch in (3, 4):
    for filt in range(5):
        put("png-pixels", f"ch{ch}-filter{filt}", png_filtered(9, 7, ch, filt))
put("png-pixels", "icon-like", png_filtered(64, 64, 3, 0))
put("png", "same", palette_png(20, 20))
put("png", "other-pixels", palette_png(20, 20, lambda x, y: 2 * (x > y)))
put("png", "stored", palette_png(20, 20, level=0))
put("png", "8-bit", palette_png(20, 20, depth=8))

# Interface resource files
svg_icon = b'<svg width="128px" height="128px" viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg"><path d="M8 8h48v48H8z"/></svg>'
stock_rco = rco_sample(lambda w, h: png(w, h, (90, 90, 90)), lambda: svg_icon, hevag(), b'{"button.fill.enabled": 268435455}')
open(f"{stock_dir}/stock.rco", "wb").write(stock_rco)
put("rco", "stock", stock_rco)
put("rco", "pictures", Rco(stock_rco).replaced({("emoji_game", "src"): png(24, 24, (255, 0, 255)),
                                                 ("iconid_home", "src"): b'<svg viewBox="0 0 64 64"><rect width="9" height="9"/></svg>'}))
put("rco", "sound", Rco(stock_rco).replaced({("snd_focus_move", "src"): bytes(2048)}))
put("rco", "sound-ok", Rco(stock_rco).replaced({("snd_focus_move", "src"): hevag(30, seed=9)}))
for name in ("stock", "pictures", "sound", "sound-ok"):   # rco-data: the pointers, then the file table
    r = Rco(open(f"{corpus}/rco/{name}", "rb").read())
    pointers = b"".join(struct.pack("<II", off, size) for *_, off, size in r.files())
    put("rco-data", name, pointers + r.b[r.t["file"][0]:])

# SVG
put("svg", "plain", b'<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"><rect width="5" height="5"/></svg>')
put("svg", "style", b'<?xml version="1.0"?><svg><style>.a{fill:red}</style><use xlink:href="#a"/></svg>')

# fonts: the stock font, and one-thing-wrong variants for the mutator to start from
open(f"{stock_dir}/stock.ttf", "wb").write(font_fixture.font("tt", glyphs=150))
for name, data in [("cff", font_fixture.font("cff")), ("tt", font_fixture.font("tt")), ("cmap12", font_fixture.font(cmap_format=12)),
                   ("no-a", font_fixture.font(missing="A")), ("few-glyphs", font_fixture.font(glyphs=90)),
                   ("no-cmap", font_fixture.font(drop=("cmap",))), ("dup", font_fixture.font(duplicate="name")),
                   ("short", font_fixture.font()[:300]), ("ttc", font_fixture.collection()), ("woff", b"wOFF" + bytes(80)),
                   ("hmtx-cut", font_fixture.font(hmtx_cut=40)), ("loca-cut", font_fixture.font("tt", loca_cut=10)),
                   ("mac-map", font_fixture.font(platform=(1, 0)))]:
    put("font", name, data)
