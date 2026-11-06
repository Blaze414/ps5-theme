#!/usr/bin/env python3
"""Fails a PS5 build that calls one of the SDK libc's own system call stubs.

ps5-payload-sdk's libc makes some system calls itself (openat, mkdirat, unmount ...) and returns what the kernel hands
back without looking at it: on a failure the error number, as if it were the result, with errno left as it was. An
openat of a missing folder gave 2, which reads as an open file (1.81 to 1.84, see docs/challenges.md). The plugin
makes those calls through platform_ps5.c instead (the end of src/platform.h), and this checks that nothing else does:

  - nothing refers to a function with a `syscall` instruction of its own (a call, a jump or its address taken) but
    ps5_syscall, the plugin's own, which says a failure;
  - only the ps5_* wrappers refer to ps5_syscall.

Calls the SDK's libc makes into its own stubs would show up too: there are none in this build (its directory and
file functions come from the console's libSceLibcInternal and libkernel, which say failures).

  tools/check_syscalls.py build/ps5-theme.elf [more.elf ...]

OBJDUMP names the disassembler; otherwise llvm-objdump-18, llvm-objdump or objdump, whichever is found first."""

import os
import re
import shutil
import subprocess
import sys

FUNC = re.compile(r"^[0-9a-f]+ <([^>]+)>:$")
REF = re.compile(r"<([^>+]+)(?:\+0x[0-9a-f]+)?>")


def objdump():
    for name in [os.environ.get("OBJDUMP"), "llvm-objdump-18", "llvm-objdump", "objdump"]:
        if name and shutil.which(name):
            return name
    sys.exit("check_syscalls: no objdump found (set OBJDUMP)")


def functions(path):
    out = subprocess.run([objdump(), "-d", "--no-show-raw-insn", path], check=True, capture_output=True, text=True).stdout
    funcs, name = {}, None
    for line in out.splitlines():
        m = FUNC.match(line.strip())
        if m:
            name = m.group(1)
            funcs[name] = []
        elif name and "\t" in line and ":" in line.split("\t", 1)[0]:
            insn = line.split("\t", 1)[1]
            funcs[name].append((insn.split("#", 1)[0] if "#" in insn else insn).strip())
    return funcs


def check(path):
    funcs = functions(path)
    raw = {name for name, insns in funcs.items()
           if name != "ps5_syscall" and any(i == "syscall" or i.startswith("syscall ") for i in insns)}
    bad = []
    for name, insns in funcs.items():
        if name in raw:
            continue
        for insn in insns:
            for target in REF.findall(insn):
                if target in raw:
                    bad.append(f"{name} uses {target}, which returns a failure as a result: "
                               f"add it to the end of src/platform.h")
                elif target == "ps5_syscall" and not name.startswith("ps5_"):
                    bad.append(f"{name} uses ps5_syscall directly: only the ps5_* wrappers should")
    if "ps5_syscall" not in funcs:
        bad.append("ps5_syscall is missing: src/platform_ps5.c wasn't linked?")
    for line in sorted(set(bad)):
        print(f"check_syscalls: {os.path.basename(path)}: {line}", file=sys.stderr)
    return not bad


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    results = [check(p) for p in sys.argv[1:]]
    sys.exit(0 if all(results) else 1)
