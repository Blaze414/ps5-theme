#!/usr/bin/env python3
"""Generate the ATRAC9 Huffman tables for web/atrac9.js and src/atrac9_tables.h.

The codebooks are part of the ATRAC9 format. They are taken from LibAtrac9
(https://github.com/Thealexbarney/LibAtrac9, MIT License, (c) 2018 Alex
Barney), file C/src/huffCodes.c:

    tools/gen_atrac9_tables.py path/to/LibAtrac9/C/src/huffCodes.c

Both outputs are rewritten between their GENERATED markers.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
JS = ROOT / "web" / "atrac9.js"
C = ROOT / "src" / "atrac9_tables.h"

ENTRY = re.compile(
    r"\{\{0\}\}|\{0\}|\{(\w+)Bits, \w+Codes, \w+Lookup, (\d+), (\d+), (\d+), (\d+), (\d+), (\d+)\}")


def arrays(src, ctype, suffix):
    out = {}
    for m in re.finditer(r"static const %s (\w+)%s\[(\d+)\]\s*=\s*\{([^}]*)\};" % (ctype, suffix), src):
        values = [int(v, 0) for v in m.group(3).replace("\n", " ").split(",") if v.strip()]
        assert len(values) == int(m.group(2)), m.group(1)
        out[m.group(1)] = values
    return out


def books(src, name, count, bits, codes):
    body = src[src.index(name + "["):]
    body = body[body.index("= {") + 3:body.index("\n};")]
    out = []
    for m in ENTRY.finditer(body):
        if m.group(0) == "{{0}}":
            out += [None] * 4
        elif m.group(0) == "{0}":
            out.append(None)
        else:
            n = m.group(1)
            length, vc, vcp, vb, vmax, maxbits = (int(m.group(i)) for i in range(2, 8))
            assert len(bits[n]) == length == len(codes[n]) == 1 << (vb * vc), n
            assert vmax == 1 << vb and max(bits[n]) == maxbits, n
            for b, c in zip(bits[n], codes[n]):
                assert c < 1 << max(b, 1), n
            out.append(dict(name=n, vc=vc, vcp=vcp, vb=vb, maxbits=maxbits, bits=bits[n], codes=codes[n]))
    assert len(out) == count, (name, len(out))
    return out


def js_book(b):
    if not b:
        return "null"
    bits = "".join("0123456789abcdefghij"[x] for x in b["bits"])
    codes = "".join("%03x" % x for x in b["codes"])
    return '[%d,%d,%d,%d,"%s","%s"]' % (b["vc"], b["vcp"], b["vb"], b["maxbits"], bits, codes)


def c_book(b):
    if not b:
        return "{0}"
    return "{at9_%s_bits, at9_%s_codes, %d, %d, %d, %d, %d}" % (
        b["name"], b["name"], len(b["bits"]), b["vc"], b["vcp"], b["vb"], b["maxbits"])


def replace(path, begin, end, text):
    src = path.read_text()
    a = src.index(begin) + len(begin)
    b = src.index(end)
    path.write_text(src[:a] + "\n" + text + src[b:])


def main():
    src = pathlib.Path(sys.argv[1]).read_text()
    bits = arrays(src, "uint8_t", "Bits")
    codes = arrays(src, "uint16_t", "Codes")
    sf_unsigned = books(src, "HuffmanScaleFactorsUnsigned", 7, bits, codes)
    sf_signed = books(src, "HuffmanScaleFactorsSigned", 6, bits, codes)
    spectrum = books(src, "HuffmanSpectrum", 64, bits, codes)

    js = ["// Codebook: [values per symbol, log2 of that, bits per value, longest code,",
          "//            code length per symbol (base 20), code per symbol (3 hex digits)].",
          "const SF_UNSIGNED = [" + ",\n  ".join(js_book(b) for b in sf_unsigned) + "];",
          "const SF_SIGNED = [" + ",\n  ".join(js_book(b) for b in sf_signed) + "];",
          "const SPECTRUM = ["]
    for s in range(2):
        js.append("  [")
        for p in range(8):
            row = spectrum[s * 32 + p * 4:s * 32 + p * 4 + 4]
            js.append("    [" + ",\n     ".join(js_book(b) for b in row) + "],")
        js.append("  ],")
    js.append("];")
    replace(JS, "// BEGIN GENERATED TABLES (tools/gen_atrac9_tables.py)",
            "// END GENERATED TABLES", "\n".join(js) + "\n")

    c = []
    for b in sf_unsigned + sf_signed + spectrum:
        if b:
            c.append("static const uint8_t at9_%s_bits[%d] = {%s};" % (
                b["name"], len(b["bits"]), ",".join(map(str, b["bits"]))))
            c.append("static const uint16_t at9_%s_codes[%d] = {%s};" % (
                b["name"], len(b["codes"]), ",".join(map(str, b["codes"]))))
    c.append("")
    c.append("static const at9_huff_t at9_sf_unsigned[7] = {%s};" % ", ".join(map(c_book, sf_unsigned)))
    c.append("static const at9_huff_t at9_sf_signed[6] = {%s};" % ", ".join(map(c_book, sf_signed)))
    c.append("static const at9_huff_t at9_spectrum[2][8][4] = {")
    for s in range(2):
        c.append("  {")
        for p in range(8):
            row = spectrum[s * 32 + p * 4:s * 32 + p * 4 + 4]
            c.append("    {%s}," % ", ".join(map(c_book, row)))
        c.append("  },")
    c.append("};")
    replace(C, "/* BEGIN GENERATED TABLES (tools/gen_atrac9_tables.py) */",
            "/* END GENERATED TABLES */", "\n".join(c) + "\n")


if __name__ == "__main__":
    main()
