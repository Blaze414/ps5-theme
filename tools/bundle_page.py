#!/usr/bin/env python3
"""Join the manager's page into the one file the plugin serves.

    tools/bundle_page.py web/index.html build-meta/index.html

web/index.html lists the page's style sheets (<link rel="stylesheet" href="css/...">) and scripts
(<script src="js/..."></script>), one per line. Each run of those lines becomes one <style> or one <script> with
the files' text in the listed order, so the page loads in one request and every script shares one scope, as one
file would. Every script starts with "use strict"; the joined script says it once.

The build fails if a file under web/css or web/js isn't listed, is listed twice, or a script doesn't start with
"use strict". Same input, same output: the bundle is part of the reproducible build.
"""
import os
import re
import sys

STYLE = re.compile(r'^<link rel="stylesheet" href="(css/[a-z0-9/-]+\.css)">$')
SCRIPT = re.compile(r'^<script src="(js/[a-z0-9/-]+\.js)"></script>$')
STRICT = '"use strict";\n\n'


def fail(msg):
    sys.exit(f"bundle_page: {msg}")


def read(root, rel):
    with open(os.path.join(root, rel), encoding="utf-8") as f:
        text = f.read()
    if not text.endswith("\n"):
        fail(f"{rel} doesn't end with a new line")
    return text


def bundle(template):
    root = os.path.dirname(template)
    with open(template, encoding="utf-8") as f:
        lines = f.read().split("\n")
    out, listed, run, kind = [], [], [], None

    def flush():
        nonlocal run, kind
        if kind == "css":
            out.append("<style>\n" + "".join(read(root, r) for r in run) + "</style>")
        elif kind == "js":
            parts = []
            for r in run:
                text = read(root, r)
                if not text.startswith(STRICT):
                    fail(f'{r} must start with "use strict"; and a blank line')
                parts.append(text[len(STRICT):])
            out.append("<script>\n" + STRICT + "".join(parts) + "</script>")
        run, kind = [], None

    for line in lines:
        m, k = STYLE.match(line), "css"
        if not m:
            m, k = SCRIPT.match(line), "js"
        if not m:
            flush()
            out.append(line)
            continue
        if kind != k:
            flush()
        kind = k
        run.append(m.group(1))
        listed.append(m.group(1))
    flush()

    on_disk = sorted(os.path.relpath(os.path.join(d, f), root)
                     for top in ("css", "js") for d, _, fs in os.walk(os.path.join(root, top))
                     for f in fs if f.endswith((".css", ".js")))
    twice = sorted({r for r in listed if listed.count(r) > 1})
    if twice:
        fail("listed twice: " + ", ".join(twice))
    unlisted = sorted(set(on_disk) - set(listed))
    if unlisted:
        fail("not listed in " + template + ": " + ", ".join(unlisted))
    return "\n".join(out)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    page = bundle(sys.argv[1])
    os.makedirs(os.path.dirname(sys.argv[2]) or ".", exist_ok=True)
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write(page)
