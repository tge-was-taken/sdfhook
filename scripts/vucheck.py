#!/usr/bin/env python3
"""Checks a VU transcription against the listing: every address in its declared ranges covered,
and every instruction it names at an address actually there.

Usage: scripts/vucheck.py docs/vu1/lighting.c [docs/vu1/vu1_image.c]
"""
import re
import sys

RANGE = re.compile(r"0x([0-9A-Fa-f]{4})\.\.0x([0-9A-Fa-f]{4})")
SINGLE = re.compile(r"0x([0-9A-Fa-f]{4})")
CITE = re.compile(r"0x([0-9A-Fa-f]{4})\s+([A-Z][A-Za-z0-9]*)(\.[xyzw]+)?")
LISTED = re.compile(r"^/\* (?:([0-9A-F]{4}) [0-9A-F]{8} [0-9A-F]{8}|\s+) *(\S*)\s*\*/")


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


def instructions(listing):
    """{address: {mnemonic}} from the listing, taking the continuation lines with their address.

    A pair prints its lower half on a line of its own with no address, so both halves belong to
    the address above them.
    """
    out, at, delayed = {}, None, None
    for line in listing.splitlines():
        m = LISTED.match(line)
        if not m:
            continue
        if m.group(1):
            at = int(m.group(1), 16)
            out.setdefault(at, set())
            # vu2c prints a delay slot before the branch it belongs to, so the mnemonics on the
            # continuation lines below such a line can belong to the branch's address, not this one.
            d = re.search(r"delay slot of ([0-9A-F]{4})", line)
            delayed = int(d.group(1), 16) if d else None
        elif delayed is not None and m.group(2):
            out.setdefault(delayed, set()).add(m.group(2))
        if at is not None and m.group(2):
            out[at].add(m.group(2))
    return out


def miscited(body, listed):
    """Citations naming an instruction that is not at the address they name.

    Only a token the listing uses as a mnemonic somewhere counts as a citation, so prose that
    happens to follow an address is not read as one.
    """
    known = {n.split(".")[0] for names in listed.values() for n in names}
    bad = []
    for addr, name, mask in CITE.findall(body):
        if name not in known:
            continue
        at = int(addr, 16)
        there = listed.get(at, set())
        if not there:
            continue
        if any(n.split(".")[0] == name and (not mask or n.endswith(mask)) for n in there):
            continue
        bad.append((at, name + (mask or ""), sorted(there)))
    return bad


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
    listing = open(argv[2] if len(argv) > 2 else "docs/vu1/vu1_image.c").read()
    listed = instructions(listing)
    real = set(listed)

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
    for at, cited, there in miscited(body, listed):
        bad += 1
        print("0x%04X is cited as %s; the listing has %s" % (at, cited, " ".join(there)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
