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

    matchedState = matchedBox = unmatched = 0
    mismatch = collections.Counter()
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
            matchedBox += 1
            if state(best) == state(batch):
                matchedState += 1
            else:
                diff = tuple(f"{k}: gs={b} ours={o}" for k, b, o
                             in zip(STATE, state(batch), state(best)) if b != o)
                mismatch[diff] += 1

    total = matchedBox + unmatched
    print(f"GS batches {total}: {matchedBox} matched to a draw, {unmatched} matched nothing")
    print(f"of the matched, {matchedState} agree on every register, "
          f"{matchedBox - matchedState} differ\n")

    print("STATE DIFFS on matched draws, by field set:")
    for diff, n in mismatch.most_common(15):
        print(f"  x{n:<6} " + ' | '.join(diff))

    print("\nUNMATCHED GS batches (we draw nothing there):")
    for st, n in missing.most_common(15):
        print(f"  x{n:<6} {st}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
