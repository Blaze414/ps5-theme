"""RCO resource containers (RCOF 0x110) for the tests: build a small one shaped
like ShellUI's Sce.PlayStation.PUI_UI3.rco, read one, and rebuild one with
some embedded files replaced (as the manager does in the browser)."""
import struct
import zlib

HEADER = 0x50
TABLES = ["tree", "idstr", "idint", "str", "char", "style", "intarr", "floatarr", "file"]


def _align(b, n):
    while len(b) % n:
        b.append(0)


def build(root):
    """root: (name, [(attr, kind, value)], [children]). kind is "int", "float",
    "str", "id", "ints" or "file" (value: bytes)."""
    strs, str_off = bytearray(), {}
    ids, ints, files, file_off = bytearray(), bytearray(), bytearray(), {}

    def s(text):
        if text not in str_off:
            str_off[text] = len(strs)
            strs.extend(text.encode() + b"\0")
        return str_off[text]

    def f(data):
        if data not in file_off:
            _align(files, 16)
            file_off[data] = len(files)
            files.extend(data)
        return file_off[data], len(data)

    # Pre-order layout: offsets first, then the bytes.
    flat = []
    def walk(node, parent):
        index = len(flat)
        flat.append([node, parent, []])
        if parent is not None:
            flat[parent][2].append(index)
        for child in node[2]:
            walk(child, index)
    walk(root, None)
    offsets, at = [], 0
    for node, _, _ in flat:
        offsets.append(at)
        at += 28 + 16 * len(node[1])
    tree = bytearray()
    none = 0xFFFFFFFF
    for i, (node, parent, children) in enumerate(flat):
        siblings = flat[parent][2] if parent is not None else [i]
        k = siblings.index(i)
        tree += struct.pack("<7I", s(node[0]), len(node[1]),
                            offsets[parent] if parent is not None else none,
                            offsets[siblings[k - 1]] if k > 0 else none,
                            offsets[siblings[k + 1]] if k + 1 < len(siblings) else none,
                            offsets[children[0]] if children else none,
                            offsets[children[-1]] if children else none)
        for name, kind, value in node[1]:
            if kind == "int":
                v = (1, value & 0xFFFFFFFF, 0)
            elif kind == "float":
                v = (2, struct.unpack("<I", struct.pack("<f", value))[0], 0)
            elif kind == "str":
                v = (3, s(value), len(value))
            elif kind == "id":
                v = (9, len(ids), 0)
                ids.extend(struct.pack("<I", offsets[i]) + value.encode() + b"\0")
                _align(ids, 4)
            elif kind == "ints":
                v = (6, len(ints) // 4, len(value))
                ints.extend(struct.pack(f"<{len(value)}i", *value))
            elif kind == "file":
                v = (8,) + f(value)
            else:
                raise ValueError(kind)
            tree += struct.pack("<4I", s(name), *v)
    _align(files, 16)   # the file table ends on a 16-byte boundary, as in the console's files
    tables = {"tree": tree, "idstr": ids, "idint": bytearray(8), "str": strs, "char": b"",
              "style": b"", "intarr": ints, "floatarr": b"", "file": files}
    out, layout = bytearray(HEADER), []
    for name in TABLES:
        if name != "tree":
            _align(out, 16)
        layout.append((len(out), len(tables[name])))
        out += tables[name]
    out[:8] = b"RCOF" + struct.pack("<I", 0x110)
    for k, (off, size) in enumerate(layout):
        struct.pack_into("<II", out, 8 + 8 * k, off, size)
    return bytes(out)


class Rco:
    def __init__(self, data):
        assert data[:4] == b"RCOF"
        self.b = bytes(data)
        v = struct.unpack_from("<18I", self.b, 8)
        self.t = {n: (v[2 * i], v[2 * i + 1]) for i, n in enumerate(TABLES)}

    def _str(self, table, off):
        base, size = self.t[table]
        raw = self.b[base + off: base + size]
        return raw[: raw.index(b"\0")].decode()

    def files(self):
        """(element, id, type, attr, slot position, offset, size) for every embedded file."""
        base, size = self.t["tree"]
        at, out = 0, []
        while at < size:
            name, nattr = struct.unpack_from("<2I", self.b, base + at)
            attrs = [struct.unpack_from("<4I", self.b, base + at + 28 + 16 * i) for i in range(nattr)]
            named = {self._str("str", a[0]): a for a in attrs}
            eid = self._str("idstr", named["id"][2] + 4) if "id" in named else ""
            etype = self._str("str", named["type"][2]) if "type" in named and named["type"][1] == 3 else ""
            for i, a in enumerate(attrs):
                if a[1] == 8:
                    out.append((self._str("str", name), eid, etype, self._str("str", a[0]),
                                base + at + 28 + 16 * i + 8, a[2], a[3]))
            at += 28 + 16 * nattr
        return out

    def data(self, off, size):
        base = self.t["file"][0]
        return self.b[base + off: base + off + size]

    def replaced(self, new):
        """A copy with files replaced: new maps (id, attr) to bytes."""
        entries = self.files()
        order = sorted({(off, size) for *_, off, size in entries})
        content = {k: self.data(*k) for k in order}
        for _, eid, _, attr, _, off, size in entries:
            if (eid, attr) in new:
                content[(off, size, eid, attr)] = new[(eid, attr)]
        table, where = bytearray(), {}
        for _, eid, _, attr, _, off, size in sorted(entries, key=lambda e: (e[5], e[6])):
            key = (off, size, eid, attr) if (eid, attr) in new else (off, size)
            if key not in where:
                _align(table, 16)
                where[key] = (len(table), len(content[key]))
                table += content[key]
        _align(table, 16)
        out = bytearray(self.b[: self.t["file"][0]])
        for _, eid, _, attr, slot, off, size in entries:
            key = (off, size, eid, attr) if (eid, attr) in new else (off, size)
            struct.pack_into("<II", out, slot, *where[key])
        struct.pack_into("<I", out, 8 + 8 * 8 + 4, len(table))
        return bytes(out + table)


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def palette_png(w, h, index=lambda x, y: (x + y) % 3, depth=4, level=6, extra=b""):
    """A palette PNG like many of ShellUI's pictures: 3 colours, partly
    transparent, `depth` bits per pixel; `extra` chunks go after tRNS."""
    rows = b""
    for y in range(h):
        bits = 0
        row = bytearray()
        acc = 0
        for x in range(w):
            acc = (acc << depth) | index(x, y)
            bits += depth
            if bits == 8:
                row.append(acc)
                acc = bits = 0
        if bits:
            row.append(acc << (8 - bits))
        rows += b"\0" + bytes(row)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, 3, 0, 0, 0))
            + png_chunk(b"PLTE", bytes([20, 20, 20, 255, 255, 255, 240, 0, 120])) + png_chunk(b"tRNS", bytes([0, 255, 200]))
            + extra + png_chunk(b"IDAT", zlib.compress(rows, level)) + png_chunk(b"IEND", b""))


# As 13.42's shape_default: corner radii by component and a multiplier for all.
SHAPES = (b'{\r\n  "bar.fill": 1.0,\r\n  "slider.thumb": 8.0,\r\n  "switch.track": 16.0,\r\n'
          b'  "tooltip.fill": 4.0,\r\n  "borderRadiusRatio": 1.0\r\n}')

# As 13.42's color_default and color_ps2 (14 of their 92 names): colours as 0xAARRGGBB
# whole numbers, the PlayStation 2 Appearance theme's tinted with its blue.
COLORS_DEFAULT = b'{\r\n  \"background.enabled\": 4278190080,\r\n  \"base-mat.area.enabled\": 4294967295,\r\n  \"base-mat.overlay.enabled\": 4278322184,\r\n  \"button.fill.enabled\": 268435455,\r\n  \"button.indicator.track.focused\": 4290295992,\r\n  \"button.label.focused\": 4279505940,\r\n  \"dialog.fill.enabled\": 4279309338,\r\n  \"focus.fill.enabled\": 4278190080,\r\n  \"icon.primary.enabled\": 4294967295,\r\n  \"menu.fill.enabled\": 4279309338,\r\n  \"menu.label.primary.enabled\": 4294967295,\r\n  \"slider.thumb.enabled\": 4291611852,\r\n  \"text.primary.enabled\": 4294967295,\r\n  \"tooltip.error.fill.enabled\": 4294967295\r\n}'
COLORS_PS2 = b'{\r\n  \"background.enabled\": 4278190080,\r\n  \"base-mat.area.enabled\": 4281761494,\r\n  \"base-mat.overlay.enabled\": 4279109919,\r\n  \"button.fill.enabled\": 255229654,\r\n  \"button.indicator.track.focused\": 4280769434,\r\n  \"button.label.focused\": 4279505940,\r\n  \"dialog.fill.enabled\": 4279706194,\r\n  \"focus.fill.enabled\": 4281761494,\r\n  \"icon.primary.enabled\": 4294967295,\r\n  \"menu.fill.enabled\": 4279706194,\r\n  \"menu.label.primary.enabled\": 4294967295,\r\n  \"slider.thumb.enabled\": 4291611852,\r\n  \"text.primary.enabled\": 4294967295,\r\n  \"tooltip.error.fill.enabled\": 4294967295\r\n}'


def sample(png, svg, sound, colours, colours_ps2=None):
    """A container shaped like ShellUI's: png(w, h) and svg() make pictures.
    It also holds a palette picture and, as 13.42's does, a picture declared
    as SVG that holds PNG data."""
    return build(("resource", [("id", "id", "rco"), ("type", "str", "normal"), ("version", "float", 0.11)], [
        ("texturetable", [], [
            ("texture", [("type", "str", "texture/png"), ("id", "id", "image_button_base"), ("src", "file", png(16, 16)),
                         ("margin", "ints", [0, 0, 0, 0]), ("src_4k", "file", png(32, 32))], []),
            ("texture", [("type", "str", "texture/png"), ("id", "id", "emoji_game"), ("src", "file", png(24, 24))], []),
            ("texture", [("id", "id", "iconid_home"), ("type", "str", "texture/svg"), ("src", "file", svg())], []),
            ("texture", [("id", "id", "iconid_search"), ("type", "str", "texture/svg"), ("src", "file", svg())], []),
            ("texture", [("type", "str", "texture/png"), ("id", "id", "image_checkbox_checked"),
                         ("src", "file", palette_png(20, 20)), ("src_4k", "file", palette_png(40, 40, depth=8))], []),
            ("texture", [("id", "id", "iconid_texture_app_fallback"), ("type", "str", "texture/svg"), ("src", "file", png(32, 32))], []),
        ]),
        ("msgidtable", [], [
            ("locale", [("id", "id", "default"), ("src", "file", b"strings" * 50), ("compress", "str", "zstd")], []),
            ("locale", [("id", "id", "en-US"), ("src", "file", b"strings" * 50), ("compress", "str", "zstd")], []),
        ]),
        ("sounddatatable", [], [
            ("sounddata", [("id", "id", "snd_focus_move"), ("src", "file", sound), ("type", "str", "sound/vag")], []),
        ]),
        ("filetable", [], [
            ("file", [("id", "id", "color_default"), ("src", "file", colours), ("type", "str", "color")], []),
        ] + ([("file", [("id", "id", "color_ps2"), ("src", "file", colours_ps2), ("type", "str", "color")], [])] if colours_ps2 else []) + [
            ("file", [("id", "id", "shape_default"), ("src", "file", SHAPES), ("type", "str", "shape")], []),
        ]),
    ]))
