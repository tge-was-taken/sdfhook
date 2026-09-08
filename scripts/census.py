#!/usr/bin/env python3
"""Summarises a chain census run (SDF_CHAIN_CENSUS=1 SDF_LOG=census=dump).

One frame's DMA chain, by what each entry carries and who linked it, so what a chain-walk
renderer would have to decode can be counted before one is written."""
import collections
import json
import sys

ID = {0: "refe", 1: "cnt", 2: "next", 3: "ref", 4: "refs", 5: "call", 6: "ret", 7: "end"}


def load(path):
    rows = []
    for line in open(path):
        at = line.find('{"dump":"census"')
        if at >= 0:
            rows.append(json.loads(line[at:]))
    return rows


def main(argv):
    for path in argv[1:]:
        rows = load(path)
        if not rows:
            print("%s: no census rows" % path)
            continue
        frame = max(r["frame"] for r in rows)
        one = sorted((r for r in rows if r["frame"] == frame), key=lambda r: r["step"])
        carrying = [r for r in one if r["qwc"]]

        print("== %s, frame %d: %d entries, %d carrying data"
              % (path, frame, len(one), len(carrying)))
        kinds = collections.Counter(r["type"] for r in carrying)
        for kind, count in kinds.most_common():
            print("   %-14s %5d  %5.1f%%" % (kind, count, 100.0 * count / len(carrying)))
        untyped = [r for r in carrying if r["type"] == "unknown"]
        print("   typed: %d of %d (%.1f%%)"
              % (len(carrying) - len(untyped), len(carrying),
                 100.0 * (len(carrying) - len(untyped)) / len(carrying)))
        if untyped:
            for name, n in collections.Counter(r["by"] for r in untyped).most_common(6):
                print("      untyped from %-34s %d" % (name, n))

        plumbing = collections.Counter(ID[r["dmaId"]] for r in one if not r["qwc"])
        print("   plumbing (qwc 0): " + ", ".join("%s %d" % kv for kv in plumbing.most_common()))

        named = collections.Counter(r["by"] for r in carrying)
        anonymous = named.pop("-", 0)
        print("   named by a link hook: %d of %d (%.0f%%) across %d producers"
              % (len(carrying) - anonymous, len(carrying),
                 100.0 * (len(carrying) - anonymous) / len(carrying), len(named)))
        for name, count in named.most_common(8):
            print("      %-38s %d" % (name, count))
        print()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
