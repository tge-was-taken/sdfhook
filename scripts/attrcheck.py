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
    wrong = []
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
                continue
            # The address is the whole point of the tag: a @note that disagrees with the map sends
            # the next reader to the wrong function.
            told = re.search(r"@note\s+\S+\s+(0x[0-9A-Fa-f]+)", doc)
            if told and int(told.group(1), 16) != int(syms[name], 16):
                wrong.append((path, i + 1, name, told.group(1), syms[name]))

    for path, line, name, addr, missing in bad:
        print("%s:%d: %s reimplements %s (%s) without %s"
              % (path, line, name, name, addr, " and ".join(missing)))
    # A hook table name the map no longer carries installs nothing, and the only sign is a count
    # in the run log. Renaming a symbol in the IDB is what breaks it.
    stale = []
    for path in files:
        if not path.endswith("hooks.cpp"):
            continue
        for i, line in enumerate(open(path).read().splitlines()):
            m = re.search(r'\{"(\w+)",\s*&\w+', line)
            if m and m.group(1) not in syms:
                stale.append((path, i + 1, m.group(1)))
    for path, line, name in stale:
        print("%s:%d: %s is hooked but not in the map" % (path, line, name))

    # Two entries for one address install only the second, which the run reports as an error and
    # is easy to miss in a log.
    for path in files:
        if not path.endswith("hooks.cpp"):
            continue
        first = {}
        for i, line in enumerate(open(path).read().splitlines()):
            m = re.search(r'\{"(\w+)",\s*&\w+', line)
            if not m:
                continue
            if m.group(1) in first:
                stale.append((path, i + 1, "%s, already hooked at line %d"
                              % (m.group(1), first[m.group(1)])))
            else:
                first[m.group(1)] = i + 1

    for path, line, name, told, real in wrong:
        print("%s:%d: %s says %s, the map says %s" % (path, line, name, told, real))
    print("%d functions named for a guest symbol, %d unattributed, %d with a wrong address, "
          "%d hook entries wrong" % (seen, len(bad), len(wrong), len(stale)))
    return 1 if bad or wrong or stale else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
