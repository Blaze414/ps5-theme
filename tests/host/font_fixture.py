"""Small but structurally complete TrueType and OpenType (CFF) fonts for the
tests of the font check (src/font.c): every table a font needs, a character
map for printable ASCII, and knobs that break one thing at a time."""
import struct


def be16(*v):
    return b"".join(struct.pack(">H", x & 0xffff) for x in v)


def be32(*v):
    return b"".join(struct.pack(">I", x & 0xffffffff) for x in v)


def cmap4(missing=None):
    """A format 4 map of U+0020..U+007E to glyphs 1..95; `missing`, a
    character left out of it."""
    segs = [(0x20, 0x7e)]
    if missing is not None:
        c = ord(missing)
        segs = [(a, b) for a, b in [(0x20, c - 1), (c + 1, 0x7e)] if a <= b]
    segs.append((0xffff, 0xffff))
    n = len(segs)
    delta = [(1 - 0x20) & 0xffff if a != 0xffff else 1 for a, b in segs]
    body = be16(4, 16 + 8 * n, 0, 2 * n, 0, 0, 0)
    body += b"".join(be16(b) for a, b in segs) + be16(0)
    body += b"".join(be16(a) for a, b in segs)
    body += b"".join(be16(d) for d in delta)
    body += b"".join(be16(0) for _ in segs)
    return body


def cmap12(missing=None):
    """The same map in format 12 (groups)."""
    groups = [(0x20, 0x7e)]
    if missing is not None:
        c = ord(missing)
        groups = [(a, b) for a, b in [(0x20, c - 1), (c + 1, 0x7e)] if a <= b]
    body = be32(12 << 16, 16 + 12 * len(groups), 0, len(groups))
    for a, b in groups:
        body += be32(a, b, a - 0x20 + 1)
    return body


def font(flavor="cff", glyphs=100, missing=None, cmap_format=4, drop=(), em=1000, duplicate=None,
         head_magic=0x5f0f3cf5, nhm=None, hmtx_cut=0, platform=(3, 1), loca_cut=0, edit=None):
    """A font as bytes. `flavor`: "cff" (OTTO) or "tt" (TrueType). `drop`:
    tables to leave out; `duplicate`: a table listed twice; the rest each
    break one thing the check looks at."""
    nhm = glyphs if nhm is None else nhm
    head = be32(0x00010000, 0x00010000, 0, head_magic) + be16(0, em) + bytes(16) + be16(0, 0, 1000, 1000, 0, 8, 2, 0, 0)
    hhea = be32(0x00010000) + be16(800, -200, 0, 1000, 0, 0, 1000, 1, 0, 0, 0, 0, 0, 0, 0, nhm)
    maxp = be32(0x00005000) + be16(glyphs) if flavor == "cff" else be32(0x00010000) + be16(glyphs) + bytes(26)
    hmtx = (be16(500, 0) * nhm + be16(0) * (glyphs - nhm))
    hmtx = hmtx[:len(hmtx) - hmtx_cut]
    sub = cmap4(missing) if cmap_format == 4 else cmap12(missing)
    cmap = be16(0, 1) + be16(platform[0], platform[1]) + be32(12) + sub
    tables = {
        "cmap": cmap, "head": head, "hhea": hhea, "hmtx": hmtx, "maxp": maxp,
        "name": be16(0, 0, 6), "OS/2": bytes(78), "post": be32(0x00030000) + bytes(28),
    }
    if flavor == "cff":
        tables["CFF "] = bytes([1, 0, 4, 1]) + bytes(40)
    else:
        tables["glyf"] = bytes(8)
        tables["loca"] = bytes(2 * (glyphs + 1))[:2 * (glyphs + 1) - loca_cut]
    for d in drop:
        tables.pop(d, None)
    names = sorted(tables)
    entries = [(n, tables[n]) for n in names]
    if duplicate:
        entries.append((duplicate, tables[duplicate]))
    count = len(entries)
    off = 12 + 16 * count
    directory, blobs = b"", b""
    for tag, data in entries:
        directory += tag.encode("latin1") + be32(0, off + len(blobs), len(data))
        blobs += data + bytes(-len(data) % 4)
    version = b"OTTO" if flavor == "cff" else be32(0x00010000)
    out = version + be16(count, 0, 0, 0) + directory + blobs
    return edit(out) if edit else out


def collection():
    return b"ttcf" + be32(0x00010000, 1, 16) + bytes(64)
