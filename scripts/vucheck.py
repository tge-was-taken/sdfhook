#!/usr/bin/env python3
"""Checks a VU transcription covers every address in the ranges its header declares.

Usage: scripts/vucheck.py dumps/vu1/annotated/lighting.c [dumps/vu1/vu1_image.c]
"""
import re
import sys

RANGE = re.compile(r"0x([0-9A-Fa-f]{4})\.\.0x([0-9A-Fa-f]{4})")
SINGLE = re.compile(r"0x([0-9A-Fa-f]{4})")


def split(text):
    """The header block and the body, since a declared range must not count as its own coverage."""
    end = text.find("*/")
    return (text[:end], text[end:]) if end > 0 else ("", text)


def covered(body, ranges):
    """Addresses the body mentions, expanding spans. A span that restates a declared range covers
    nothing: a routine's own header comment must not stand in for transcribing it."""
    declared_spans = {(lo, hi) for lo, hi, _ in ranges}
    seen = set()
    for lo, hi in RANGE.findall(body):
        lo, hi = int(lo, 16), int(hi, 16)
        if (lo, hi) not in declared_spans:
            seen.update(range(lo, hi + 1))
    seen.update(int(a, 16) for a in SINGLE.findall(RANGE.sub("", body)))
    return seen


def declared(header):
    """The ranges listed under "Ranges covered", so prose naming an excluded range is not a claim."""
    out, listing = [], False
    for line in header.splitlines():
        if "Ranges covered" in line:
            listing = True
            continue
        m = re.search(r"0x([0-9A-Fa-f]{4})\.\.0x([0-9A-Fa-f]{4})\s+(\w+)\s", line)
        if listing and m:
            out.append((int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
        elif listing and not line.strip(" *"):
            break
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    text = open(argv[1]).read()
    listing = open(argv[2] if len(argv) > 2 else "dumps/vu1/vu1_image.c").read()
    real = {int(a, 16) for a in re.findall(r"^/\* ([0-9A-F]{4}) ", listing, re.M)}

    header, body = split(text)
    ranges = declared(header)
    if not ranges:
        print("no ranges declared: the header must name each as 0xAAAA..0xBBBB <routine>")
        return 1
    seen = covered(body, ranges)

    bad = 0
    for lo, hi, name in ranges:
        want = {a for a in real if lo <= a <= hi}
        missing = sorted(want - seen)
        if missing:
            bad += 1
            print("%-20s 0x%04X..0x%04X  %d instructions, %d unaccounted: %s"
                  % (name, lo, hi, len(want), len(missing),
                     ", ".join("0x%04X" % a for a in missing)))
        else:
            print("%-20s 0x%04X..0x%04X  %d instructions, complete" % (name, lo, hi, len(want)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
