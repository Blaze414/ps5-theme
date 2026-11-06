#!/usr/bin/env python3
"""Check how the plugin's C modules are connected through their headers.

    tools/check_includes.py          check (exit 1 on a problem)
    tools/check_includes.py --fix    rewrite each file's list of the plugin's own headers to what it uses

Each module is a .c file and the header that declares what it offers (src/log.c and src/log.h). A file includes the
plugin's headers it uses names from, directly, and no others; every function a header declares is defined in one of
the plugin's .c files. Names are told apart as the compiler would see them: comments and strings don't count.
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SRC = os.path.join(ROOT, "src")
# Files outside src/ that are built with the plugin's headers.
EXTRA = ["tools/stop/stop.c", "tests/host/platform_linux.c", "tests/fuzz/stubs.c", "tests/fsguard/race.c"]
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "__attribute__", "format", "printf", "defined",
            "typedef", "struct", "enum", "const", "static", "extern", "void", "int", "char", "unsigned", "long"}


def code_only(text):
    """The text with comments and string/char literals blanked (line breaks kept)."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text[i] in "\"'":
            q, j = text[i], i + 1
            while j < n and text[j] != q and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(" " * (j + 1 - i))
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def declared(header_text):
    """Names a header offers: functions, macros, types, enum constants, variables."""
    code = code_only(header_text)
    names, funcs = set(), set()
    for m in re.finditer(r"^\s*#\s*define\s+([A-Za-z_]\w*)", code, re.M):
        names.add(m.group(1))
    body = "\n".join(l for l in code.split("\n") if not l.lstrip().startswith("#"))
    for m in re.finditer(r"enum\s*\{([^}]*)\}", body):
        names.update(re.findall(r"([A-Za-z_]\w*)\s*(?:=[^,]*)?(?:,|$)", m.group(1)))
    for m in re.finditer(r"\}\s*([A-Za-z_]\w*)\s*;", body):
        names.add(m.group(1))
    for m in re.finditer(r"typedef\s+[^;{]*?\b([A-Za-z_]\w*)\s*;", body):
        names.add(m.group(1))
    for m in re.finditer(r"\bstruct\s+([A-Za-z_]\w*)\s*\{", body):
        names.add("struct " + m.group(1))
    flat = re.sub(r"\{[^{}]*\}", "{}", body)   # declarations only, no struct members
    for m in re.finditer(r"([A-Za-z_]\w*)\s*\(", flat):
        if m.group(1) not in KEYWORDS:
            names.add(m.group(1))
            funcs.add(m.group(1))
    for m in re.finditer(r"extern\s+[^;(]*?\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;", flat):
        names.add(m.group(1))
    return names, funcs


def used(c_text):
    code = code_only(c_text)
    code = "\n".join(l for l in code.split("\n") if not re.match(r"\s*#\s*include", l))
    return set(re.findall(r"[A-Za-z_]\w*", code)) | {"struct " + s for s in re.findall(r"struct\s+(\w+)", code)}


def own_includes(text):
    return re.findall(r'^#include "([a-z_0-9]+\.h)"', text, re.M)


def main():
    fix = "--fix" in sys.argv
    headers = {}
    for f in sorted(os.listdir(SRC)):
        if f.endswith(".h"):
            headers[f] = declared(open(os.path.join(SRC, f), encoding="utf-8").read())
    owner = {}
    for h, (names, _) in headers.items():
        for n in names:
            owner.setdefault(n, set()).add(h)
    clash = {n: hs for n, hs in owner.items() if len(hs) > 1}
    problems = [f"{n} is declared in more than one header: {', '.join(sorted(hs))}" for n, hs in sorted(clash.items())]

    files = [os.path.join("src", f) for f in sorted(os.listdir(SRC)) if f.endswith((".c", ".h"))] + EXTRA
    defined_anywhere = set()
    for rel in files:
        if not rel.endswith(".c"):
            continue
        code = code_only(open(os.path.join(ROOT, rel), encoding="utf-8").read())
        defined_anywhere.update(re.findall(r"^(?:[A-Za-z_][\w \*]*?[\s\*])?([A-Za-z_]\w*)\s*\([^;]*?\)\s*(?:__attribute__\(\([^)]*\)\)\s*)?\{",
                                           code, re.M | re.S))
        defined_anywhere.update(re.findall(r"^([a-z_]\w*)\s*\(", code, re.M))
    for h, (_, funcs) in headers.items():
        for fn in sorted(funcs - defined_anywhere):
            if h != "platform.h" or fn not in defined_anywhere:
                problems.append(f"{h} declares {fn}(), which no .c file defines")

    for rel in files:
        path = os.path.join(ROOT, rel)
        text = open(path, encoding="utf-8").read()
        base = os.path.basename(rel)
        uses = used(text)
        if base.endswith(".h"):
            uses -= headers[base][0]
        need = {h for n in uses for h in owner.get(n, ()) if h != base and n not in clash}
        have = own_includes(text)
        want = sorted(need)
        self_h = base[:-2] + ".h" if base.endswith(".c") else None
        if self_h in headers:   # a module's own header first
            want = [self_h] + [h for h in want if h != self_h]
        if fix and base.endswith(".c") and have != want:
            block = "".join(f'#include "{h}"\n' for h in want)
            lines = text.split("\n")
            idx = [i for i, l in enumerate(lines) if re.match(r'#include "[a-z_0-9]+\.h"', l)]
            if idx:
                lines = lines[:idx[0]] + [block.rstrip("\n")] + [l for i, l in enumerate(lines) if i > idx[0] and i not in idx]
                open(path, "w", encoding="utf-8").write("\n".join(lines))
            have = want
        missing = sorted(set(want) - set(have))
        extra = sorted(set(have) - set(want))
        if missing:
            problems.append(f"{rel} uses names from {', '.join(missing)} without including it")
        if extra:
            problems.append(f"{rel} includes {', '.join(extra)} but uses nothing from it")
    for p in problems:
        print(p)
    if problems:
        sys.exit(1)
    print(f"{len(files)} files: every header used is included, and every one included is used")


if __name__ == "__main__":
    main()
