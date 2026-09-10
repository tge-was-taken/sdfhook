#!/usr/bin/env python3
"""Checks a reimplementation against the facts extracted from the guest function: every read,
write, call and branch is either satisfied by the code or opted out with a justification.

A fact is satisfied by the code itself wherever the tree already binds guest names to ours:
SDF_STATIC_ASSERT_OFFSETOF resolves an offset to a field name, and the pcsx2hook_find_symbol
calls resolve a global to its member. Only a branch needs a citation, in vucheck's grammar.

Usage: scripts/guestcheck.py [function ...] (defaults to every function in the facts file)
"""
import glob
import math
import re
import sys

FACTS = "assets/SLUS-20911.facts"
AUTONAME = ("unk_", "asc_", "off_", "flt_", "dbl_", "qword_", "dword_", "word_", "byte_",
            "xmmword_", "stru_")

# VU1 is the only opt-out, and it is one because the work is accounted for somewhere that is itself
# checked: docs/vu1/ under scripts/vucheck.py. Every other class was a way of not modelling
# something. "packetbuf", "dma", "gif" and "gs" declined a packet whose *contents* are the draw, and
# that is how corePktSoftenBias came to be dropped inside an opted-out region. "vu0" is inline
# arithmetic we do in the shader. "abi" is not a fact, so the extractor stops emitting it.
# "enginestate" is a write to guest memory, which we never do by construction, so the checker
# answers it rather than the author.
CLASSES = ("vu1",)

FACT = re.compile(r"^\s+(read|write|addr|glob|call)\s+0x([0-9A-Fa-f]+)\s+(\S+)(?:\s+addrs=(\S+))?")
SIG = re.compile(r"^\s+sig\s+\((.*)\)\s*$")
SHAPE = re.compile(r"^\s+shape\s+loops=(\d+)\s+calls=(\d+)\s*$")
# {function: (guest parameter types, loops, calls)}
SHAPES = {}

BLK  = re.compile(r"^\s+(blk)\s+0x([0-9A-Fa-f]+)\.\.0x([0-9A-Fa-f]+)(?:\s+depth=(\d+))?(?:\s+cond=(\d+))?\s*$")
BRCH = re.compile(r"^\s+(brch)\s+0x([0-9A-Fa-f]+)\s+(\S.*?)\s*$")
FIELD = re.compile(r"SDF_STATIC_ASSERT_OFFSETOF\((\w+),\s*(\w+),\s*(0x[0-9A-Fa-f]+)\)")
BOUND = re.compile(r"(\w+)\s*=\s*\{?\s*pcsx2hook_find_symbol\(\w+,\s*\"(\w+)\"\)")
OPTOUT = re.compile(r"//\s*@optout\s+0x([0-9A-Fa-f]+)(?:\.\.0x([0-9A-Fa-f]+))?\s+(\w+)(?:\((\w+)\))?:\s*(\S.*)")
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
        g = SIG.match(line)
        if g and fn:
            SHAPES[fn] = ([a.strip() for a in g.group(1).split(",") if a.strip()], 0, 0)
            continue
        h = SHAPE.match(line)
        if h and fn:
            args = SHAPES.get(fn, ([], 0, 0))[0]
            SHAPES[fn] = (args, int(h.group(1)), int(h.group(2)))
            continue
        b = BLK.match(line)
        if b and fn:
            lo, hi = int(b.group(2), 16), int(b.group(3), 16)
            DEPTH[(fn, lo)] = (int(b.group(4) or 0), int(b.group(5) or 0))
            out[fn].append(("blk", lo, "0x%X..0x%X" % (lo, hi), list(range(lo, hi + 1))))
            continue
        m = FACT.match(line) or BRCH.match(line)
        if m and fn:
            extra = [int(a, 16) for a in m.group(4).split(",")] if m.lastindex > 3 and m.group(4) else []
            out[fn].append((m.group(1), int(m.group(2), 16), m.group(3), extra))
    return out, span


LOOP = re.compile(r"^\s*(for|while|do)\b")
COND = re.compile(r"^\s*(if|else|switch)\b")

# {(function, block start): loop nesting the guest runs it at}
DEPTH = {}


def citation_lines(text, lo, hi):
    """[(line, every address it cites)], since a span covers blocks its first address does not."""
    out = []
    for i, line in enumerate(text.splitlines()):
        if "//" not in line:
            continue
        seen = set()
        comment = line[line.index("//"):]
        for a, b in SPAN.findall(comment):
            seen.update(range(int(a, 16), int(b, 16) + 1))
        seen.update(int(a, 16) for a in ADDR.findall(SPAN.sub("", comment)))
        inside = {a for a in seen if lo <= a <= hi}
        if inside:
            out.append((i, inside))
    return out


def strip_comments(text):
    return re.sub(r"//[^\n]*", "", text)


def nesting(text, opener):
    """Brace nesting per line, counting only blocks a matching statement opened."""
    out, stack, depth = [], [], 0
    for line in text.splitlines():
        opening = opener.search(line) is not None
        out.append(depth)
        for ch in line:
            if ch == "{":
                stack.append(opening)
                depth += opening
                opening = False
            elif ch == "}" and stack:
                depth -= stack.pop()
    return out


def loop_depth(text):
    """Loop nesting per line of the body.

    A block the guest runs inside two loops is transcribed inside two loops, or the citation is
    parked somewhere that does not describe it: a comment at the top of a function accounts for a
    loop body only in the sense that someone typed the address.
    """
    out, stack, depth = [], [], 0
    for line in text.splitlines():
        opening = LOOP.search(line) is not None
        out.append(depth)
        for ch in line:
            if ch == "{":
                stack.append(opening)
                depth += opening
                opening = False
            elif ch == "}" and stack:
                depth -= stack.pop()
    return out


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
        comment = line[line.index("//"):]
        spans = len(SPAN.findall(comment)) + len(ADDR.findall(SPAN.sub("", comment)))
        prose = re.sub(r"[^:]*:", "", comment, count=1).strip(" /*") if ":" in comment else ""
        here = [int(a, 16) for a in ADDR.findall(SPAN.sub(r"0x\1 0x\2", line[line.index("//"):]))]
        inside = [a for a in here if lo <= a <= hi]
        for a in here:
            if a > 0xFFFF and a not in inside:
                out.append((i, a, None, spans, prose, bool(OPTOUT.search(line))))
        if inside:
            out.append((i, min(inside), marked, spans, prose, bool(OPTOUT.search(line))))
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


ALIAS = re.compile(r"using\s+(\w+)\s*=\s*guest_ptr<(?:struct\s+)?(\w+)>\s*;")
STRUCT = re.compile(r"struct\s+(\w+)\s*\{(.*?)\n\};", re.S)
MEMBER = re.compile(r"^\s*(?:const\s+)?([\w:<>]+)\s+([\w,\s\[\]]+);", re.M)
DECLARATOR = re.compile(r"(\w+)((?:\s*\[\d+\])*)")
# A raw `T* name` too: the shadow declares its parameters the way the guest declares its own.
DECL = re.compile(r"\b(?:const\s+)?(guest_ref<(\w+)>|guest_ptr<(\w+)>|(\w+_t))\s*[*&]?\s*(\w+)\s*[={;,)]")
# A guest struct is reached through a reference as often as a pointer, and a read is a read
# either way, so both separators count.
CHAIN = re.compile(r"\b(\w+)((?:(?:->|\.)\w+)+)")


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
                for field, dims in DECLARATOR.findall(names):
                    member[(struct, field)] = type_
                    # uv[15][4] is sixty elements, not fifteen.
                    count = math.prod(int(n) for n in re.findall(r"\d+", dims))
                    decl.setdefault(struct, []).append((type_, field, count))
    resolve = lambda t: alias.get(t, t)
    # A pointer alias is four bytes whatever it points at.
    for name in alias:
        WIDTH.setdefault(name, 4)
    for struct in list(decl):
        decl[struct] = [(t if t in WIDTH or t in decl else resolve(t), f, c)
                        for t, f, c in decl[struct]]
    offsets = {(st, off): f for st in decl for off, f in layout(st, decl).items()}
    return {(s, f): resolve(t) for (s, f), t in member.items()}, resolve, offsets


def unshadow(struct):
    """xsdfGrOt_t is our shadow of sdfGrOt_t and reads the same fields at the same offsets."""
    return struct[1:] if struct.startswith("x") else struct


def reached(text, member_types, resolve):
    """{(struct, field)} the body actually reads, by resolving each chain's base declaration.

    Comments are stripped first: an opt-out that names the field it skips must not thereby count
    as reading it.
    """
    text = re.sub(r"//[^\n]*", "", text)
    var = {}
    for whole, ref, ptr, alias, name in DECL.findall(text):
        var[name] = unshadow(ref or ptr or resolve(alias))
    out = set()
    for head, rest in CHAIN.findall(text):
        at = var.get(head)
        for field in re.split(r"->|\.", rest)[1:]:
            if not at:
                break
            out.add((at, field))
            at = unshadow(member_types.get((at, field)) or "")
    return out


CALL = re.compile(r"\b(\w+)\s*\(")


def call_order(fn, table, text):
    """The guest's calls in address order against ours in source order.

    Static order, so a call inside a loop appears once on both sides and the two are comparable.
    A call we make out of the guest's order is logic that moved: the citation guard only sees the
    regions someone thought to cite, this sees every call whether or not it was annotated.
    """
    guest = sorted((a, t) for k, at, t, extra in table.get(fn, []) if k == "call"
                   for a in [at] + list(extra) for t in [t])
    want = [t for _a, t in guest]
    ours = [n[1:] if n.startswith("x") and n[1:] in want else n for n in ours_in_order(text)]
    ours = [n for n in ours if n in want]

    at = 0
    for i, name in enumerate(ours):
        try:
            at = want.index(name, at) + 1
        except ValueError:
            earlier = want.index(name) if name in want else -1
            return name, ours[i - 1] if i else None, earlier
    return None


def ours_in_order(text):
    """Our calls in evaluation order, which is where each one's argument list closes.

    f(g(x)) reads f first and runs g first, so textual order reports an inversion that is not one.
    """
    code = strip(text)
    found = []
    for m in CALL.finditer(code):
        depth, i = 0, m.end() - 1
        while i < len(code):
            depth += (code[i] == "(") - (code[i] == ")")
            if not depth:
                break
            i += 1
        found.append((i, m.group(1)))
    return [name for _close, name in sorted(found)]


def strip(text):
    """Comments are not calls: an @optout naming sdfDrawTagLink2 is not a call to it."""
    return re.sub(r"//[^\n]*", "", text)


def body(fn, paths):
    """The reimplementation's text: the definition whose name is the guest symbol, brace matched.

    An @fn tag alone is not the target. It also sits above a hook handler and above the struct a
    function reads, where it is attribution rather than a claim to reimplement anything.
    """
    # Unindented, so a call site inside a hook is not mistaken for the definition. x<fn> is our
    # shadow of the guest symbol and is the same claim.
    define = re.compile(r"^[\w:<>,&*][\w:<>,&*\s]*?\b(?:\w+::)?x?%s\s*\(" % fn)
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
                # From the definition line, not the brace: a multi-line signature declares
                # parameters above it, and those are what type the field chains below.
                depth, out = 0, lines[i:start]
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


UNKNOWN = re.compile(r"^(unk|pad|field_|gap)")


def opaque(kind, target, field_map):
    """A read whose field our own mirror cannot name. An opt-out may not assert what it is: if we
    have not identified the field, neither has the sentence explaining it away."""
    if kind not in ("read", "write") or "+" not in target:
        return None
    type_, off = target.split("+")
    name = (field_map.get((type_, int(off, 16)))
            or field_map.get((type_.replace("_tag", "_t"), int(off, 16))))
    return name if name and UNKNOWN.match(name) else None


def satisfied(kind, target, text, field_map, global_map, addrs, touched=(), code=None, table=()):
    # Citations live in comments; a name that only appears in one is not a use of it.
    code = text if code is None else code
    if kind in ("read", "write", "addr"):
        type_, off = target.split("+")
        # The database spells some structs <name>_tag where our header has <name>_t.
        name = (field_map.get((type_, int(off, 16)))
                or field_map.get((type_.replace("_tag", "_t"), int(off, 16))))
        if name is None:
            return False, "no offsetof assert for %s" % target
        if kind == "write":
            # Reading a field is not writing it, and our side writes no guest memory, so a write
            # fact is an opt-out unless the body assigns the field.
            alt = type_.replace("_tag", "_t")
            ok = (bool(re.search(r"(->|\.)%s\s*[|&+^]?=[^=]" % name, code))
                  and ((type_, name) in touched or (alt, name) in touched))
            return ok, "%s::%s written" % (type_, name)
        # The database spells some structs <name>_tag, so the body's <name>_t is the same struct.
        alt = type_.replace("_tag", "_t")
        return ((type_, name) in touched or (alt, name) in touched), "%s::%s" % (type_, name)
    if kind == "glob":
        if target.startswith(AUTONAME):
            return False, "%s is unnamed in the database; name it and re-export" % target
        # A global that names a function we shadow is modelled by the shadow, not by a binding:
        # the guest loads the address to call it indirectly, we call ours directly.
        if re.search(r"\bx%s\b" % target, code):
            return True, "x" + target
        member = global_map.get(target)
        if member is None:
            return False, "%s is not resolved by name anywhere" % target
        return bool(re.search(r"\bx?%s\b" % member, code)), member
    if kind == "call":
        # A hooked symbol is modelled by the hook. The table answers that, so it is not a claim
        # for an author to make and there is no class for it.
        if target in table:
            return True, target + ", hooked"
        if target.startswith("indirect+"):
            off = int(target.split("+")[1], 16)
            member = field_map.get(("sdfGrOt_t", off))
            if member and re.search(r"->%s\s*\(" % member, code):
                return True, target
            return False, "%s, through no member call here" % target
        if target.startswith("indirect"):
            # A call through a local of function-pointer type is the indirect call, modelled.
            for var in re.findall(r"\bxsdf\w*Fn\s+(\w+)\s*=", code):
                if re.search(r"\b%s\s*\(" % var, code):
                    return True, target
            return False, target
        return bool(re.search(r"\bx?%s\s*\(" % target, code)), target
    return bool(addrs & cited(text)), target


def hooks(paths):
    """The symbols hooks.cpp installs, so `hooked` is a claim the table can answer."""
    out = set()
    for path in paths:
        if path.endswith("hooks.cpp"):
            out.update(re.findall(r'\{"(\w+)",\s*&\w+', open(path).read()))
    return out


def defined(name, paths):
    """Whether a function of this name is defined anywhere in our tree."""
    where = re.compile(r"^[\w:<>,&*\s]*?\b(?:\w+::)?x?%s\s*\(" % name, re.M)
    return any(where.search(open(p).read()) for p in paths)


def main(argv):
    src = sorted(glob.glob("src/**/*.cpp", recursive=True) + glob.glob("src/**/*.h", recursive=True))
    field_map, global_map, hooked = fields(src), globals_(src), hooks(src)
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
        for lo, hi, cls, into, why in OPTOUT.findall(text):
            outs[(int(lo, 16), int(hi or lo, 16))] = (cls, why, into)
        used = set()
        # The body alone. Our factoring is not an argument: if the guest reads it here, the
        # reimplementation of this function reads it here, which is what makes the two comparable.
        wide = text
        touched = reached(text, member_types, resolve)
        code = re.sub(r"//[^\n]*", "", text)
        data = [f for f in table[fn] if f[0] != "blk"]
        print("%s:%d: %s, %d data facts, %d blocks"
              % (path, line, fn, len(data), len(table[fn]) - len(data)))

        done = {"blk": 0, "data": 0}
        need = {"blk": len(table[fn]) - len(data), "data": len(data)}
        args, loops, calls = SHAPES.get(fn, (None, 0, 0))
        if args is not None:
            head = text.split("{", 1)[0]
            mine = len([a for a in head[head.find("(") + 1:head.rfind(")")].split(",") if a.strip()])
            if mine < len(args):
                print("  %s takes %d parameters, the guest takes %d (%s)"
                      % (fn, mine, len(args), ", ".join(args)))
                bad += 1
            body_loops = len(re.findall(r"^\s*(?:for|while|do)\b", strip_comments(text), re.M))
            if body_loops < loops:
                print("  %s has %d loops, the guest has %d" % (fn, body_loops, loops))
                bad += 1
        nest = nesting(text, LOOP)
        ifs = nesting(text, COND)
        seq = sequence(text, *span[fn])
        cites = citation_lines(text, *span[fn])
        for kind, at, target, extra in table[fn]:
            bucket = "blk" if kind == "blk" else "data"
            ok, shown = satisfied(kind, target, wide, field_map, global_map, {at} | set(extra),
                                  touched, code, hooked)
            # A span covers control flow: it says a region of the guest is hardware we replace.
            # Everything else names its own address, so drawing a box round a region can never
            # silence the data or the calls inside it. elsewhere() is the exception, since it
            # claims the reads are modelled somewhere else rather than skipped.
            want = {at} | set(extra)
            if kind == "blk":
                # The block's end, not just its start: otherwise a point opt-out sitting on the
                # first instruction silently covers every instruction after it.
                want.add(int(target.split("..")[1], 16))
                hits = [k for k in outs if k[0] <= at and k[1] >= max(want)]
            else:
                hits = [k for k in outs if k == (at, at)
                        or (k[0] <= at <= k[1] and outs[k][0] == "elsewhere")]
            opt = outs[hits[0]] if hits else None
            used.update(hits)
            deep, guarded = DEPTH.get((fn, at), (0, 0))
            if ok and kind == "blk" and guarded:
                # A ternary is the guest's if/else written without braces, so it nests nothing
                # the brace count can see. The statement a citation introduces counts as its body.
                rows = text.splitlines()
                def ternary(line):
                    near = " ".join(rows[line:line + 5])
                    return "?" in near and ":" in near.split("?", 1)[1]

                where = max((ifs[line] + ternary(line) for line, seen in cites
                             if line < len(ifs) and seen & want), default=0)
                if where < guarded:
                    print("  0x%07X blk   %-28s sits in %d if bodies, cited in %d"
                          % (at, target[:28], guarded, where))
                    bad += 1
            if ok and kind == "blk" and deep:
                where = max((nest[line] for line, seen in cites
                             if line < len(nest) and seen & want), default=0)
                if where < deep:
                    print("  0x%07X blk   %-28s runs %d loops deep, cited at %d"
                          % (at, target[:28], deep, where))
                    bad += 1
            if ok and opt:
                print("  0x%07X %-5s %-28s modelled as %s, and opted out" % (at, kind, target[:28], shown))
                bad += 1
            elif ok:
                done[bucket] += 1
            elif opt and opt[0] == "elsewhere" and not opt[2]:
                print("  0x%07X %-5s %-28s opts out elsewhere without naming where"
                      % (at, kind, target[:28]))
                bad += 1
            elif opt and opt[0] == "elsewhere" and not defined(opt[2], src):
                print("  0x%07X %-5s %-28s opts out to %s, which is not defined here"
                      % (at, kind, target[:28], opt[2]))
                bad += 1
            elif opt and opt[0] == "elsewhere" and opt[2] in table and not any(
                    t == target for k, a, t, _e in table[opt[2]]):
                print("  0x%07X %-5s %-28s opts out to %s, which never reads it"
                      % (at, kind, target[:28], opt[2]))
                bad += 1
            elif opt and opt[0] == "hooked" and target not in hooked:
                print("  0x%07X %-5s %-28s opts out as hooked, but nothing hooks %s"
                      % (at, kind, target[:28], target))
                bad += 1
            elif opt and opaque(kind, target, field_map):
                print("  0x%07X %-5s %-28s is %s in our own mirror; name the field before"
                      " opting out of it"
                      % (at, kind, target[:28], opaque(kind, target, field_map)))
                bad += 1
            elif opt and opt[0] not in CLASSES:
                print("  0x%07X %-5s %-28s opted out as %r, not a class" % (at, kind, target[:28], opt[0]))
                bad += 1
            elif opt:
                done[bucket] += 1
            else:
                print("  0x%07X %-5s %-28s unaccounted (%s)" % (at, kind, target[:28], shown))
                bad += 1
        for (lo, _), (cls, _why, _into) in sorted((k, v) for k, v in outs.items()
                                                  if k not in used):
            print("  0x%07X       %-28s opts out of no fact" % (lo, cls))
            bad += 1
        out_of_order = call_order(fn, table, text)
        if out_of_order:
            name, after, _at = out_of_order
            print("  %s calls %s%s, where the guest calls it earlier"
                  % (fn, name, " after %s" % after if after else " first"))
            bad += 1

        lo, hi = span[fn]
        last = lo
        for line, at, marked, spans, prose, isopt in sequence(text, lo, hi):
            if not isopt and (not prose or spans > 3):
                print("  %s:%d cites %d spans%s; a citation names what the code does, at most"
                      " three spans of it" % (path, line + 1, spans, "" if prose else " and no reason"))
                bad += 1
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
