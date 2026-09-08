#!/usr/bin/env python3
"""Checks a reimplementation against the facts extracted from the guest function: every read,
write, call and branch is either satisfied by the code or opted out with a justification.

A fact is satisfied by the code itself wherever the tree already binds guest names to ours:
SDF_STATIC_ASSERT_OFFSETOF resolves an offset to a field name, and the pcsx2hook_find_symbol
calls resolve a global to its member. Only a branch needs a citation, in vucheck's grammar.

Usage: scripts/guestcheck.py [function ...] (defaults to every function in the facts file)
"""
import glob
import re
import sys

FACTS = "assets/SLUS-20911.facts"
AUTONAME = ("unk_", "asc_", "off_", "flt_", "dbl_", "qword_", "dword_", "word_", "byte_",
            "xmmword_", "stru_")

CLASSES = ("dma", "vif", "gif", "gs", "packetbuf", "enginestate", "abi", "hooked",
           "elsewhere")

FACT = re.compile(r"^\s+(read|write|glob|call)\s+0x([0-9A-Fa-f]+)\s+(\S+)(?:\s+addrs=(\S+))?")
BLK  = re.compile(r"^\s+(blk)\s+0x([0-9A-Fa-f]+)\.\.0x([0-9A-Fa-f]+)\s*$")
BRCH = re.compile(r"^\s+(brch)\s+0x([0-9A-Fa-f]+)\s+(\S.*?)\s*$")
FIELD = re.compile(r"SDF_STATIC_ASSERT_OFFSETOF\((\w+),\s*(\w+),\s*(0x[0-9A-Fa-f]+)\)")
BOUND = re.compile(r"(\w+)\s*=\s*\{?\s*pcsx2hook_find_symbol\(\w+,\s*\"(\w+)\"\)")
OPTOUT = re.compile(r"//\s*@optout\s+0x([0-9A-Fa-f]+)(?:\.\.0x([0-9A-Fa-f]+))?\s+(\w+)(?:\([^)]*\))?:\s*(\S.*)")
SPAN = re.compile(r"0x([0-9A-Fa-f]{4,8})\.\.0x([0-9A-Fa-f]{4,8})")
ADDR = re.compile(r"0x([0-9A-Fa-f]{4,8})")


def facts(path):
    out, span, fn = {}, {}, None
    for line in open(path):
        if line.startswith("fn "):
            _, fn, where = line.split()[:3]
            lo, _, hi = where.partition("..")
            span[fn] = (int(lo, 16), int(hi or lo, 16))
            out[fn] = []
            continue
        b = BLK.match(line)
        if b and fn:
            lo, hi = int(b.group(2), 16), int(b.group(3), 16)
            out[fn].append(("blk", lo, "0x%X..0x%X" % (lo, hi), list(range(lo, hi + 1))))
            continue
        m = FACT.match(line) or BRCH.match(line)
        if m and fn:
            extra = [int(a, 16) for a in m.group(4).split(",")] if m.lastindex > 3 and m.group(4) else []
            out[fn].append((m.group(1), int(m.group(2), 16), m.group(3), extra))
    return out, span


def sequence(text, lo, hi):
    """Citations and opt-outs in source order. They are a guard on the order of the transcription
    too: reading the guest out of order is either a comment in the wrong place or logic that moved.

    An opt-out is a citation that happens to carry a class, so it sits where the guest does the
    work it declines, not in a list at the top."""
    out = []
    for i, line in enumerate(text.splitlines()):
        if "//" not in line:
            continue
        marked = "reordered" in line
        here = [int(a, 16) for a in ADDR.findall(SPAN.sub(r"0x\1 0x\2", line[line.index("//"):]))]
        inside = [a for a in here if lo <= a <= hi]
        for a in here:
            if a > 0xFFFF and a not in inside:
                out.append((i, a, None))
        if inside:
            out.append((i, min(inside), marked))
    return out


def fields(paths):
    """{(type, offset): field} from the offsetof asserts, which already pin our names to the IDB."""
    out = {}
    for path in paths:
        for t, f, off in FIELD.findall(open(path).read()):
            out[(t, int(off, 16))] = f
    return out


def globals_(paths):
    """{guest symbol: our member}, from where the symbol is resolved by name."""
    out = {}
    for path in paths:
        for member, symbol in BOUND.findall(open(path).read()):
            out[symbol] = member
    return out


ALIAS = re.compile(r"using\s+(\w+)\s*=\s*guest_ptr<(\w+)>\s*;")
STRUCT = re.compile(r"struct\s+(\w+)\s*\{(.*?)\n\};", re.S)
MEMBER = re.compile(r"^\s*(?:const\s+)?([\w:<>]+)\s+([\w,\s\[\]]+);", re.M)
DECLARATOR = re.compile(r"(\w+)\s*(?:\[(\d+)\])?")
DECL = re.compile(r"\b(?:const\s+)?(guest_ref<(\w+)>|guest_ptr<(\w+)>|(\w+_t))\s+(\w+)\s*[={;,)]")
CHAIN = re.compile(r"\b(\w+)((?:->\w+)+)")


WIDTH = {"u8": 1, "s8": 1, "char": 1, "bool": 1, "u16": 2, "s16": 2, "u32": 4, "s32": 4,
         "f32": 4, "float": 4, "int": 4, "u64": 8, "s64": 8, "f64": 8, "double": 8}


def layout(struct, members, seen=()):
    """{offset: field} for a mirrored struct, from its own declaration.

    The offsetof asserts exist to catch drift, not to be the checker's index: a field without one
    is still a field. Natural alignment, which is what the asserts confirm.
    """
    out, at = {}, 0
    if struct in seen:
        return out
    for type_, field, count in members.get(struct, []):
        size = WIDTH.get(type_) or size_of(type_, members, seen + (struct,))
        align = min(size if size in (1, 2, 4, 8) else 4, 8) or 1
        at = (at + align - 1) // align * align
        out[at] = field
        for i in range(count):
            for o, f in layout(type_, members, seen + (struct,)).items():
                out.setdefault(at + i * size + o, field)
        at += size * count
    return out


def size_of(type_, members, seen=()):
    if type_ in WIDTH:
        return WIDTH[type_]
    if type_ in seen or type_ not in members:
        return 4
    at = 0
    for t, _f, count in members[type_]:
        size = size_of(t, members, seen + (type_,))
        align = min(size if size in (1, 2, 4, 8) else 4, 8) or 1
        at = (at + align - 1) // align * align + size * count
    return at


def types(paths):
    """{alias: struct} and {(struct, field): struct}, enough to resolve a `a->b->c` chain to the
    struct each arrow lands on. Without this, `flags` matches on any struct that has one."""
    alias, member, decl = {}, {}, {}
    for path in paths:
        text = open(path).read()
        for name, target in ALIAS.findall(text):
            alias[name] = target
        for struct, fields in STRUCT.findall(text):
            for type_, names in MEMBER.findall(fields):
                # One declaration can name several fields: f32 x, y, z, w;
                for field, count in DECLARATOR.findall(names):
                    member[(struct, field)] = type_
                    decl.setdefault(struct, []).append((type_, field, int(count or 1)))
    resolve = lambda t: alias.get(t, t)
    # A pointer alias is four bytes whatever it points at.
    for name in alias:
        WIDTH.setdefault(name, 4)
    for struct in list(decl):
        decl[struct] = [(t if t in WIDTH or t in decl else resolve(t), f, c)
                        for t, f, c in decl[struct]]
    offsets = {(st, off): f for st in decl for off, f in layout(st, decl).items()}
    return {(s, f): resolve(t) for (s, f), t in member.items()}, resolve, offsets


def reached(text, member_types, resolve):
    """{(struct, field)} the body actually reads, by resolving each chain's base declaration."""
    var = {}
    for whole, ref, ptr, alias, name in DECL.findall(text):
        var[name] = ref or ptr or resolve(alias)
    out = set()
    for head, rest in CHAIN.findall(text):
        at = var.get(head)
        for field in rest.split("->")[1:]:
            if not at:
                break
            out.add((at, field))
            at = member_types.get((at, field))
    return out


def body(fn, paths):
    """The reimplementation's text: the definition whose name is the guest symbol, brace matched.

    An @fn tag alone is not the target. It also sits above a hook handler and above the struct a
    function reads, where it is attribution rather than a claim to reimplement anything.
    """
    define = re.compile(r"^[\w:<>,&*\s]*?\b(?:\w+::)?%s\s*\(" % fn)
    tag = re.compile(r"\s*///\s*@fn\s+%s\s*$" % fn)
    for pick in ("name", "tag"):
        for path in paths:
            lines = open(path).read().splitlines()
            for i, line in enumerate(lines):
                if pick == "name":
                    if not define.match(line) or line.rstrip().endswith(";"):
                        continue
                else:
                    # A split-out piece keeps its own name (readMultiTexturePacket), so the tag is
                    # the only link. A hook handler and a struct are not reimplementations.
                    if not tag.match(line):
                        continue
                    i = next((j for j in range(i + 1, min(i + 4, len(lines)))
                              if "(" in lines[j] or lines[j].startswith(("struct", "using"))), i)
                    line = lines[i]
                    if (line.startswith(("struct", "using", "enum")) or "Hook(" in line
                            or line.rstrip().endswith(";") or "(" not in line):
                        continue
                start = next((j for j in range(i, min(i + 8, len(lines))) if "{" in lines[j]), None)
                if start is None:
                    continue
                depth, out = 0, []
                for j in range(start, len(lines)):
                    out.append(lines[j])
                    depth += lines[j].count("{") - lines[j].count("}")
                    if depth == 0:
                        return path, i + 1, "\n".join(out)
    return None, 0, ""


def cited(text):
    """Addresses the body cites, expanding spans, as vucheck reads them. An opt-out's own address
    is not a citation, or opting out of a branch would also satisfy it."""
    text = OPTOUT.sub("", text)
    seen = set()
    for lo, hi in SPAN.findall(text):
        seen.update(range(int(lo, 16), int(hi, 16) + 1))
    seen.update(int(a, 16) for a in ADDR.findall(SPAN.sub("", text)))
    return seen


def satisfied(kind, target, text, field_map, global_map, addrs, touched=()):
    if kind in ("read", "write"):
        type_, off = target.split("+")
        # The database spells some structs <name>_tag where our header has <name>_t.
        name = (field_map.get((type_, int(off, 16)))
                or field_map.get((type_.replace("_tag", "_t"), int(off, 16))))
        if name is None:
            return False, "no offsetof assert for %s" % target
        if kind == "write":
            # Reading a field is not writing it, and our side writes no guest memory, so a write
            # fact is an opt-out unless the body assigns the field.
            ok = bool(re.search(r"(->|\.)%s\s*[|&+^]?=[^=]" % name, text)) and (type_, name) in touched
            return ok, "%s::%s written" % (type_, name)
        return (type_, name) in touched, "%s::%s" % (type_, name)
    if kind == "glob":
        if target.startswith(AUTONAME):
            return False, "%s is unnamed in the database; name it and re-export" % target
        member = global_map.get(target)
        if member is None:
            return False, "%s is not resolved by name anywhere" % target
        return bool(re.search(r"\b%s\b" % member, text)), member
    if kind == "call":
        if target.startswith("indirect:"):
            return False, target
        return bool(re.search(r"\b%s\s*\(" % target, text)), target
    return bool(addrs & cited(text)), target


def main(argv):
    src = sorted(glob.glob("src/**/*.cpp", recursive=True) + glob.glob("src/**/*.h", recursive=True))
    field_map, global_map = fields(src), globals_(src)
    table, span = facts(FACTS)
    member_types, resolve, computed = types(src)
    # The asserts win where they exist; the computed layout fills the rest.
    field_map = {**computed, **field_map}
    wanted = argv[1:] or sorted(table)

    bad = 0
    for fn in wanted:
        path, line, text = body(fn, src)
        if not text:
            print("%s: no reimplementation carries /// @fn %s" % (FACTS, fn))
            bad += 1
            continue
        # An opt-out covers one address or a span, so a run of transport branches is one line.
        outs = {}
        for lo, hi, cls, why in OPTOUT.findall(text):
            outs[(int(lo, 16), int(hi or lo, 16))] = (cls, why)
        used = set()
        touched = reached(text, member_types, resolve)
        data = [f for f in table[fn] if f[0] != "blk"]
        print("%s:%d: %s, %d data facts, %d blocks"
              % (path, line, fn, len(data), len(table[fn]) - len(data)))

        done = {"blk": 0, "data": 0}
        need = {"blk": len(table[fn]) - len(data), "data": len(data)}
        for kind, at, target, extra in table[fn]:
            bucket = "blk" if kind == "blk" else "data"
            ok, shown = satisfied(kind, target, text, field_map, global_map, {at} | set(extra),
                                  touched)
            # A span covers control flow: it says a region of the guest is hardware we replace.
            # Everything else names its own address, so drawing a box round a region can never
            # silence the data or the calls inside it. elsewhere() is the exception, since it
            # claims the reads are modelled somewhere else rather than skipped.
            want = {at} | set(extra)
            if kind == "blk":
                hits = [k for k in outs if k[0] <= at and k[1] >= max(want)]
            else:
                hits = [k for k in outs if k == (at, at)
                        or (k[0] <= at <= k[1] and outs[k][0] == "elsewhere")]
            opt = outs[hits[0]] if hits else None
            used.update(hits)
            if ok and opt:
                print("  0x%07X %-5s %-28s modelled as %s, and opted out" % (at, kind, target[:28], shown))
                bad += 1
            elif ok:
                done[bucket] += 1
            elif opt and opt[0] not in CLASSES:
                print("  0x%07X %-5s %-28s opted out as %r, not a class" % (at, kind, target[:28], opt[0]))
                bad += 1
            elif opt:
                done[bucket] += 1
            else:
                print("  0x%07X %-5s %-28s unaccounted (%s)" % (at, kind, target[:28], shown))
                bad += 1
        for (lo, _), (cls, _why) in sorted((k, v) for k, v in outs.items() if k not in used):
            print("  0x%07X       %-28s opts out of no fact" % (lo, cls))
            bad += 1
        lo, hi = span[fn]
        last = lo
        for line, at, marked in sequence(text, lo, hi):
            if marked is None:
                print("  %s:%d cites 0x%07X, which is outside %s" % (path, line + 1, at, fn))
                bad += 1
            elif at < last and not marked:
                print("  %s:%d cites 0x%07X after 0x%07X; the guest runs them the other way round"
                      % (path, line + 1, at, last))
                bad += 1
            else:
                last = max(last, at)
        print("  %d of %d data facts modelled, %d of %d blocks read"
              % (done["data"], need["data"], done["blk"], need["blk"]))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
