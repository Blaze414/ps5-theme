#!/usr/bin/env python3
"""Checks run by CI on every push, and by hand: python3 tools/check_repo.py

- every local link in the README and the docs points at a file that exists,
  and every #anchor at a heading in it;
- the version is the same everywhere it is written (the Makefile, the
  plugin, the README and the tests' expectations).
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def slug(heading):
    """A heading's anchor, as GitHub makes it."""
    h = re.sub(r"[`*_]", "", heading.strip().lower())
    return re.sub(r"[^\w\- ]", "", h).replace(" ", "-")


def anchors(path):
    with open(path, encoding="utf-8") as f:
        return {slug(m.group(2)) for line in f for m in [re.match(r"^(#{1,6})\s+(.*)", line)] if m}


def check_links():
    problems = []
    docs = os.path.join(ROOT, "docs")
    files = [os.path.join(ROOT, "README.md"), os.path.join(ROOT, "CONTRIBUTING.md")] + sorted(os.path.join(docs, f) for f in os.listdir(docs) if f.endswith(".md"))
    for path in files:
        with open(path, encoding="utf-8") as f:
            text = re.sub(r"```.*?```", "", f.read(), flags=re.S)       # code blocks are not links
        for target in re.findall(r"\]\(([^)\s]+)\)", text):
            if re.match(r"^[a-z]+:", target):                           # https:, mailto: and the like
                continue
            file, _, frag = target.partition("#")
            dest = os.path.normpath(os.path.join(os.path.dirname(path), file)) if file else path
            where = os.path.relpath(path, ROOT)
            if not os.path.exists(dest):
                problems.append(f"{where}: {target} points at a file that doesn't exist")
            elif frag and dest.endswith(".md") and frag not in anchors(dest):
                problems.append(f"{where}: {target} names a heading that doesn't exist")
    return problems, len(files)


def check_version():
    tests = read("tests/host/test_e2e.py")
    found = {
        "Makefile VERSION": re.search(r"^VERSION\s*:=\s*(\S+)", read("Makefile"), re.M),
        "src/theme.h PLUGIN_VERSION": re.search(r'#define PLUGIN_VERSION "([^"]+)"', read("src/theme.h")),
        "README.md": re.search(r"^Version (\d+\.\d+)\. ", read("README.md"), re.M),
        "tests: the status's version": re.search(r's\["version"\] == "([\d.]+)"', tests),
        "tests: the start-up line": re.search(r'"ps5-theme ([\d.]+) starting"', tests),
    }
    versions = {k: m.group(1) for k, m in found.items() if m}
    problems = [f"{k}: version not found" for k, m in found.items() if not m]
    if len(set(versions.values())) > 1:
        problems.append("the version differs: " + ", ".join(f"{k} {v}" for k, v in versions.items()))
    return problems, versions.get("Makefile VERSION", "?")


def main():
    links, count = check_links()
    version, number = check_version()
    for p in links + version:
        print(f"FAIL  {p}")
    print(f"{count} documents' links checked; version {number} everywhere it is written" if not links + version
          else f"{len(links + version)} problem(s)")
    return 1 if links + version else 0


if __name__ == "__main__":
    sys.exit(main())
