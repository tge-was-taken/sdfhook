#!/usr/bin/env python3
"""Pairs a GS batch to the mesh that produced it by screen position, then diffs the colors.

Needs PCSX2HOOK_GS_VERTS=1 for the per-vertex rows. A bounding-box match cannot separate several
meshes of one model that overlap; the centroid of the vertices VU1 emitted can.
Usage: vertdiff.py <trace.jsonl> <gs.jsonl> [--tbp N] [--top N]
"""
import argparse
import json
from collections import defaultdict


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("gs")
    ap.add_argument("--tbp", type=int, default=None)
    ap.add_argument("--top", type=int, default=10)
    args = ap.parse_args()

    meshes = defaultdict(list)
    for line in open(args.trace):
        if '"ev":"mesh"' not in line or '"ndcMid"' not in line:
            continue
        row = json.loads(line)
        if not row.get("onScreen"):
            continue
        mid = row["ndcMid"]
        meshes[row["frame"]].append(((mid[0] + 1) / 2 * 512, (mid[1] + 1) / 2 * 224, row))

    # The vertex rows precede their batch row, so hold one until its state arrives.
    pending = {}
    paired = defaultdict(lambda: [0, 0.0, None])
    unpaired = 0
    for line in open(args.gs):
        if '"type":"gsverts"' in line:
            v = json.loads(line)
            pending[(v["f"], v["i"])] = v
            continue
        if '"type":"gs"' not in line:
            continue
        b = json.loads(line)
        if b.get("prim") not in (3, 4, 5):
            continue
        if args.tbp is not None and b.get("tbp") != args.tbp:
            continue
        v = pending.pop((b.get("f"), b.get("i")), None)
        if not v or not v["x"]:
            continue
        # Vertex XY still carry the context's XYOFFSET; the batch's own bbox does not, so the
        # difference between them is the offset for this batch.
        ox = b["bbox"][0] - min(v["x"])
        oy = b["bbox"][1] - min(v["y"])
        cx = sum(v["x"]) / len(v["x"]) + ox
        cy = sum(v["y"]) / len(v["y"]) + oy

        best, bestd = None, 1e18
        for mx, my, mesh in meshes.get(b.get("f"), ()):
            if mesh.get("tbp") != b.get("tbp"):
                continue
            d = (mx - cx) ** 2 + (my - cy) ** 2
            if d < bestd:
                best, bestd = mesh, d
        if best is None or bestd > 64 * 64:
            unpaired += 1
            continue

        lo, hi = best["litLo"], best["litHi"]
        worst = 0.0
        for rgba in v["rgba"]:
            for c in range(3):
                x = (rgba >> (8 * c)) & 0xFF
                worst = max(worst, lo[c] - 1 - x, x - hi[c] - 1)
        key = (best.get("type"), best.get("attrBits"), best.get("litcase"),
               bool(best.get("streamColors")))
        e = paired[key]
        e[0] += 1
        e[1] += max(worst, 0.0)
        if e[2] is None and worst > 1:
            e[2] = ([round(x, 1) for x in lo], [round(x, 1) for x in hi],
                    [(v["rgba"][0] >> (8 * c)) & 0xFF for c in range(3)])

    total = sum(e[0] for e in paired.values())
    print(f"{total} batches paired by centroid, {unpaired} with no mesh within 64px")
    print()
    print(f"{'type':>5} {'attr':>5} {'case':>5} {'vcol':>5} {'batches':>8} {'mean miss':>10}  ours vs hardware")
    for key, (count, err, sample) in sorted(paired.items(), key=lambda kv: -kv[1][1])[: args.top]:
        t, attr, case, vcol = key
        note = ""
        if sample:
            lo, hi, actual = sample
            note = f"  {lo}..{hi} vs {actual}"
        print(f"{t:>5} {attr:>5} {case:>5} {str(vcol):>5} {count:>8} {err / count:>10.1f}{note}")


if __name__ == "__main__":
    main()
