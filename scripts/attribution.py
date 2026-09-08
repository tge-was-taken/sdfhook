#!/usr/bin/env python3
"""How much of a frame's linked-packet set can be identified, per state.

A packet handed to sdfDrawTagLink* that we cannot name has to be run on the hardware that
consumes it, so this is the number a packet-driven renderer lives or dies by."""
import collections
import glob
import json
import os
import sys


def load(path):
    rows = []
    for line in open(path):
        at = line.find('{"dump":"linked"')
        if at >= 0:
            rows.append(json.loads(line[at:]))
    return rows


# Producers that build their packet inline, so no builder function exists to hook and the
# packet's type comes from the call that linked it.
INLINE_BUILDERS = ("fldDrawPoly3", "fldSdwDraw", "dds3TrackPolyLinkOt")


def identified(row):
    """Named by its builder, or self-describing: GIF is typed by its own tag, and a VIF transfer
    by its codes. Only an opaque payload needs to be named, and where the producer inlines the
    build there is no builder to hook, so the caller names it instead."""
    if row["built"] != "-":
        return "builder"
    if row["kind"] in ("gif", "vif"):
        return "self-describing"
    if row["kind"] == "empty":
        return "empty"
    if row["by"] in INLINE_BUILDERS:
        return "caller"
    return None


def main(argv):
    paths = argv[1:] or sorted(glob.glob("dumps/scratch/linked_*.log"))
    totals = collections.Counter()
    unknown = collections.Counter()
    for path in paths:
        rows = load(path)
        if not rows:
            continue
        frame = max(r["frame"] for r in rows)
        one = [r for r in rows if r["frame"] == frame]
        how = collections.Counter(identified(r) or "UNKNOWN" for r in one)
        named = len(one) - how["UNKNOWN"]
        print("%-38s %4d linked, %4d identified (%5.1f%%)   %s"
              % (os.path.basename(path)[7:-4], len(one), named, 100.0 * named / len(one),
                 ", ".join("%s %d" % kv for kv in how.most_common())))
        totals.update(how)
        for r in one:
            if identified(r) is None:
                unknown["%s / %s" % (r["by"], r["kind"])] += 1

    total = sum(totals.values())
    if total:
        print("\nall states: %d linked, %d identified (%.1f%%)"
              % (total, total - totals["UNKNOWN"], 100.0 * (total - totals["UNKNOWN"]) / total))
    if unknown:
        print("\nstill unidentified, by caller:")
        for name, count in unknown.most_common(15):
            print("   %-46s %d" % (name, count))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
