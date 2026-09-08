#!/usr/bin/env python3
"""Checks every function named for a guest symbol carries @fn and @note, so a reimplementation
can be found and rechecked by grep.

Usage: scripts/attrcheck.py [src/...] (defaults to every .cpp and .h under src/)
"""
import glob
import re
import sys

MAP = "assets/SLUS-20911.map"

# The engine's module prefixes, per CLAUDE.md. The map also carries libc, so a bare `write` or
# `open` is not a reimplementation of anything.
PREFIXES = ("sdf", "dds3", "fld", "nb", "cmp", "fcl", "core", "ooy", "egl", "fr", "Drw", "Disp")
DEF = re.compile(r"^[\w:<>,&*\s]*?\b(\w+)::(\w+)\s*\(|^[\w<>,&*\s]+?\b(\w+)\s*\([^;]*\)\s*\{?\s*$")


def symbols(path):
    out = {}
    for line in open(path):
        m = re.match(r"(\w+)\s*=\s*(0x[0-9A-Fa-f]+)", line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def docblock(lines, at):
    """The comment lines immediately above a definition, stopping at a blank line."""
    out, i = [], at - 1
    while i >= 0:
        text = lines[i].strip()
        if not text:
            break
        if not text.startswith(("///", "//", "*", "/*")):
            break
        out.insert(0, text)
        i -= 1
    return "\n".join(out)


def main(argv):
    syms = symbols(MAP)
    files = argv[1:] or sorted(glob.glob("src/**/*.cpp", recursive=True)
                               + glob.glob("src/**/*.h", recursive=True))
    bad = []
    seen = 0
    for path in files:
        lines = open(path).read().splitlines()
        for i, line in enumerate(lines):
            m = DEF.match(line)
            if not m:
                continue
            name = m.group(2) or m.group(3)
            if not name or name not in syms or not name.startswith(PREFIXES):
                continue
            if line.rstrip().endswith(";"):
                continue
            seen += 1
            doc = docblock(lines, i)
            missing = [t for t in ("@fn", "@note") if t not in doc]
            if missing:
                bad.append((path, i + 1, name, syms[name], missing))

    for path, line, name, addr, missing in bad:
        print("%s:%d: %s reimplements %s (%s) without %s"
              % (path, line, name, name, addr, " and ".join(missing)))
    print("%d functions named for a guest symbol, %d unattributed" % (seen, len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
