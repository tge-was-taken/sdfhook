#!/usr/bin/env python3
"""Diffs what we draw against what the GS drew, per draw.

Both traces name their register fields identically, so a comparison is a set operation rather than
a translation nobody maintains. A draw is matched to a batch on its screen bounds, because that is
the one thing VU1 does not destroy: it culls, clips and merges before the GS sees anything, so
counts never line up, but a batch still covers the pixels its source draw covered.

    scripts/gsdiff.py <trace.jsonl> <gs.jsonl>
"""
import collections
import json
import sys

# The registers that decide how a fragment is produced.
STATE = ('prim', 'iip', 'tme', 'fge', 'abe', 'psm', 'tfx', 'tcc',
         'a', 'b', 'c', 'd', 'ate', 'atst', 'aref', 'afail')


# Whether the GS was fed a list, a strip or a fan is transport: the same triangles reach the
# rasterizer either way, and we always index a list. Comparing it reports a difference that is not
# one, so the three collapse.
TRIANGLE = {3: 3, 4: 3, 5: 3}


def state(row):
    return tuple(TRIANGLE.get(row[k], row[k]) if k == 'prim' else row.get(k) for k in STATE)


def show(row):
    return ' '.join(f"{k}={v}" for k, v in zip(STATE, state(row)))


def overlap(a, b):
    """Fraction of the smaller box the two share, so a merged batch still matches its parts."""
    x0, y0 = max(a[0], b[0]), max(a[1], b[1])
    x1, y1 = min(a[2], b[2]), min(a[3], b[3])
    if x1 <= x0 or y1 <= y0:
        return 0.0
    inter = (x1 - x0) * (y1 - y0)
    areas = [(p[2] - p[0]) * (p[3] - p[1]) for p in (a, b)]
    smallest = min(areas)
    return inter / smallest if smallest > 0 else 0.0


def load(path):
    ours, gs = collections.defaultdict(list), collections.defaultdict(list)
    for line in open(path[0]):
        row = json.loads(line)
        if row.get('ev') in ('mesh', 'sprite') and row.get('onScreen'):
            ours[row['frame']].append(row)
    for line in open(path[1]):
        row = json.loads(line)
        if row.get('type') != 'gs':
            continue
        # The GS box is in screen pixels bounded by its own scissor; ours is already normalised.
        sx, sy = row['scissor'][1] + 1, row['scissor'][3] + 1
        bx = row['bbox']
        row['ndc'] = [bx[0] / sx * 2 - 1, 1 - bx[3] / sy * 2,
                      bx[2] / sx * 2 - 1, 1 - bx[1] / sy * 2]
        gs[row['f']].append(row)
    return ours, gs


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 2:
        print(__doc__)
        return 1
    ours, gs = load(args)

    matched = collections.Counter()
    agreed = collections.Counter()
    unmatched = 0
    mismatch = collections.defaultdict(collections.Counter)
    fields = collections.defaultdict(collections.Counter)
    missing = collections.Counter()
    for frame in sorted(set(ours) & set(gs)):
        pool = ours[frame]
        for batch in gs[frame]:
            # The texture is the strongest key we share, so draws that agree on it are preferred.
            # It only breaks ties: narrowing to it outright turns a recorded-texture difference into
            # a phantom missing draw. A batch may cover several of our draws, since VU1 merges before
            # the GS sees anything, so nothing is consumed by matching.
            keyed = [d for d in pool if d['tbp'] and d['tbp'] == batch['tbp']]
            best, bestScore = None, 0.25
            for candidates in (keyed, pool):
                for draw in candidates:
                    score = overlap(batch['ndc'], draw['ndc'])
                    if score > bestScore:
                        best, bestScore = draw, score
                if best is not None:
                    break
            if best is None:
                missing[show(batch)] += 1
                unmatched += 1
                continue

            # Bounds alone pair plenty of world geometry with a neighbour that merely covers the
            # same pixels. A pair that also agrees on the texture is the source draw; the rest are
            # reported apart, because counting them as state diffs invents modelling bugs. Measured:
            # every psm, tcc and ate difference on the field scene is in the untrusted half.
            trust = 'texture' if best['tbp'] and best['tbp'] == batch['tbp'] else 'bounds'
            matched[trust] += 1
            if state(best) == state(batch):
                agreed[trust] += 1
                continue
            diff = tuple(f"{k}: gs={b} ours={o}" for k, b, o
                         in zip(STATE, state(batch), state(best)) if b != o)
            mismatch[trust][diff] += 1
            for k, b, o in zip(STATE, state(batch), state(best)):
                if b != o:
                    fields[k][trust] += 1

    total = sum(matched.values()) + unmatched
    print(f"GS batches {total}: {sum(matched.values())} matched to a draw, "
          f"{unmatched} matched nothing")
    for trust in ('texture', 'bounds'):
        print(f"  paired by {trust:8} {matched[trust]:5}, of which {agreed[trust]} "
              f"agree on every register")

    print("\nFIELDS, on pairs that agree on the texture (the ones worth fixing):")
    print(f"  {'field':6} {'texture':>8} {'bounds':>8}")
    for k in sorted(fields, key=lambda k: -fields[k]['texture']):
        print(f"  {k:6} {fields[k]['texture']:8} {fields[k]['bounds']:8}")

    print("\nSTATE DIFFS on texture-agreeing pairs, by field set:")
    for diff, n in mismatch['texture'].most_common(12):
        print(f"  x{n:<6} " + ' | '.join(diff))

    if missing:
        print("\nUNMATCHED GS batches (we draw nothing there):")
        for st, n in missing.most_common(10):
            print(f"  x{n:<6} {st}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
