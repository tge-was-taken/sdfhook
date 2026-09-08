# Runs inside IDA. Writes the fact inventory scripts/guestcheck.py holds a reimplementation to:
# basic blocks, calls, global reads and struct member accesses, per function.
#
# Members come from the decompiler's ctree, which types the base pointer for us. COP2 loads are
# invisible there, so they are listed as comments for a human to turn into facts.
import ida_funcs
import ida_gdl
import ida_hexrays
import ida_lines
import idaapi
import idautils
import idc

# The repo over UNC, so the checker reads what IDA wrote with no copy step.
OUT = r"\\wsl.localhost\Debian\home\wsl\projects\sdfhook\assets\SLUS-20911.facts"
# The decompiler output, kept beside the facts as the reference a reimplementation is
# written against. It carries what the block graph drops: prototypes and array indices.
PSEUDO = r"\\wsl.localhost\Debian\home\wsl\projects\sdfhook\assets\SLUS-20911.pseudo.c"

TARGETS = [
    "sdfTexSetEx", "sdfTexFree", "sdfTexDup", "sdfGsAllocFrameBuff", "frTranslateUserData",
    "frCopyFontClutToVram", "sdfTexAlloc",
    "MultMatrixRev", "corePktEmitMultiTexPass", "sdfModelCheckBounds", "sdfModelLinkOt",
    "sdfMatMakePacket", "corePktSkinVertFirst", "corePktSetFaceDivParm", "corePktApplyFaceDivColor",
    "sdfCorePacketGen", "sdfModelTempPacketLinkOt", "sdfModelLinkShapedDrawPacket",
    "drawSprite3D", "sdf3DPrimMakePoly", "dds3BillboardLinkOt", "sdfBillBoardMakeN",
    "sdfModelSetNormalDrawPacket", "corePktBuildFaceList", "rsetFrameMaskRgbOn",
    "dds3BillboardTagLink", "fldDrawPoly3",
    # 2D producers, reimplemented in libsdf_2d.cpp
    "DrwSprite2D", "DrwSprite2DF", "aDrawBoxG", "aDrawBoxF", "aDrawBoxF_Direct",
    "frCreateDrawFontPacket_20550848", "sdfPrimLinkFt4", "sdfPrimLinkG4", "fclDrawSpriteCore",
    "dds3DrawFade", "cmpDrawBoxG", "cmpDrawTriG", "fldDrawFrameSprite",
    "fldDrawFrameSpriteAlpha",
    # engine state and the texture layer
    "rsetAlpha", "rsetOtOfs", "rsetTest", "rsetFrameMaskRgbOn", "rsetFrameMaskRgbOff", "sdfGrSetFrameMaskRGB",
    "sdf2DLinkTexPacket", "sdfGrInitExPacket", "sdfGrOtNormalCallback", "sdfPrimAllocDrawTag",
    "sdfGrLinkDispFrameTex", "sdf2DGetPrimPacketSize", "sdf2DMakePrimPacketHeader",
    "sdf2DGetPrimPtr", "sdf2DAllocSprite", "sdfPrimSetFt4Elem", "sdfPrimSetG4Elem",
    "sdfTexSetFilter", "sdfDrawTagLink", "sdfGrAllocPacketBuff", "dds3GetFrameID",
    "dds3FontReqAllocDrawTag", "fclDrawPanelSprite2",
    # the command queue, reimplemented in libsdf_ot.cpp
    "frSwapColor", "cmpExpandColor", "sdfDrawTagInit", "sdfGrOtInit", "sdfGrOtNormalReg",
    "sdfGrSortOtReg", "sdfGrOtClose", "sdfGrJoinDrawTag",
]


class Members(ida_hexrays.ctree_parentee_t):
    """Every x->field and x.field the body reaches, as (struct, offset, is_write)."""

    def __init__(self):
        ida_hexrays.ctree_parentee_t.__init__(self)
        self.found = {}

    def visit_expr(self, e):
        if e.op not in (ida_hexrays.cot_memptr, ida_hexrays.cot_memref):
            return 0
        t = e.x.type
        if e.op == ida_hexrays.cot_memptr:
            t = t.get_pointed_object()
        name = t.dstr().replace("struct ", "").strip()
        # parent_expr() answers the same node every call, so walking it is a loop that never ends.
        # One level up is enough: an assignment's target is the memref itself.
        parent = self.parent_expr()
        write = bool(parent and ida_hexrays.cot_asg <= parent.op <= ida_hexrays.cot_asgumod
                     and parent.x is e)
        # &x->field computes an address and never loads it. Recording that as a read invents a
        # fact: sdfGrSetFrameMaskRGB passes two such addresses and dereferences neither.
        taken = bool(parent and parent.op == ida_hexrays.cot_ref)
        key = (name, e.m)
        at = e.ea if e.ea != idaapi.BADADDR else 0
        prev = self.found.get(key)
        if prev is None or (at and at < prev[0]):
            self.found[key] = (at, write or (prev[1] if prev else False),
                               taken and (prev[2] if prev else True))
        elif write:
            self.found[key] = (prev[0], True, prev[2])
        return 0


RESTORE = ("ld", "lw", "lq", "lwc1", "lqc2", "addiu", "jr", "nop", "daddu", "move")


def epilogue(fn, lo, hi):
    """A block that only unwinds the frame is the calling convention, not the guest's semantics.

    Our reimplementation has an epilogue too, emitted by the compiler. There is nothing to model
    and nothing to decline, so it is not a fact.
    """
    saw_return = False
    for ea in idautils.Heads(lo, hi):
        mnem = idc.print_insn_mnem(ea)
        if mnem not in RESTORE:
            return False
        if mnem == "jr" and idc.print_operand(ea, 0) == "$ra":
            saw_return = True
        if mnem in ("ld", "lw", "lq", "lwc1") and "$sp" not in idc.print_operand(ea, 1):
            return False
    return saw_return


def depths(chart, order):
    """Loop nesting depth per block, from the back edges.

    A back edge is a successor at or before its own block, and the blocks between the two are the
    loop body. Compiled MIPS keeps a loop's blocks contiguous, so the address interval is the body
    and nesting is how many intervals a block falls in.
    """
    loops = []
    for b in chart:
        for succ in b.succs():
            if succ.start_ea <= b.start_ea:
                loops.append((succ.start_ea, b.end_ea))
    return {lo: sum(1 for a, z in loops if a <= lo < z) for lo, _hi in order}


def conditionals(chart):
    """How many `if` bodies a block sits inside, which is not how many guards it is past.

    A branch whose arms rejoin is an if body, and its blocks belong inside one on our side. A branch
    whose arm leaves for good is a guard, and `if (a || b) continue;` is the faithful flat form of a
    chain of them. The branch's immediate post dominator tells the two apart: a merge inside the
    function, or the exit.
    """
    ids = {b.start_ea: b for b in chart}
    # Back edges are loop nesting, counted already, and never an if body.
    succ = {a: sorted(x.start_ea for x in b.succs() if x.start_ea in ids and x.start_ea > a)
            for a, b in ids.items()}
    order = sorted(ids)

    post = {a: (set(order) if succ[a] else {a}) for a in order}
    for _ in range(len(order) + 1):
        changed = False
        for a in reversed(order):
            if succ[a]:
                new = {a} | set.intersection(*(post[x] for x in succ[a]))
                if new != post[a]:
                    post[a], changed = new, True
        if not changed:
            break

    def ipdom(a):
        cand = post.get(a, set()) - {a}
        # The nearest post dominator is the one with the most post dominators of
        # its own; the exit has the fewest, so min() picks the exit every time.
        return max(cand, key=lambda x: len(post[x])) if cand else None

    # A loop's pre test is the loop's own condition, which a for statement already carries.
    headers = {x.start_ea for b in chart for x in b.succs() if x.start_ea <= b.start_ea}

    depth = {a: 0 for a in order}
    for a in order:
        if len(succ[a]) < 2:
            continue
        merge = ipdom(a)
        if merge is None or not succ.get(merge):
            continue
        if any(h for h in headers if min(succ[a]) <= h < merge):
            continue
        for arm in succ[a]:
            at, steps = arm, 0
            while at is not None and at != merge and steps <= len(order):
                depth[at] = depth.get(at, 0) + 1
                at, steps = ipdom(at), steps + 1
    return depth


def shape(cfunc):
    """The structure the decompiler sees: parameters, loops and calls.

    The pseudocode is the closest thing to our own C that the database holds, and it keeps what the
    block graph loses: the prototype, the array index in aEnv[9].header, and the fact that a body
    is two calls rather than a flag assignment.
    """
    loops = calls = 0

    class Walk(ida_hexrays.ctree_visitor_t):
        def __init__(self):
            ida_hexrays.ctree_visitor_t.__init__(self, ida_hexrays.CV_FAST)

        def visit_insn(self, i):
            nonlocal loops
            if i.op in (ida_hexrays.cit_for, ida_hexrays.cit_while, ida_hexrays.cit_do):
                loops += 1
            return 0

        def visit_expr(self, e):
            nonlocal calls
            if e.op == ida_hexrays.cot_call:
                calls += 1
            return 0

    Walk().apply_to(cfunc.body, None)
    args = []
    for a in cfunc.arguments:
        t = a.type() if callable(getattr(a, "type", None)) else a.type
        args.append(str(t.dstr()) if t is not None else "?")
    return args, loops, calls


def blocks(fn):
    """Only the blocks of the function's own range: FlowChart also walks tail chunks."""
    out = [(b.start_ea, min(b.end_ea, fn.end_ea)) for b in ida_gdl.FlowChart(fn, flags=ida_gdl.FC_PREDS)]
    return sorted(b for b in out if fn.start_ea <= b[0] < fn.end_ea and b[1] > b[0]
                  and not epilogue(fn, b[0], b[1]))


def calls_and_globals(fn):
    calls, globs = {}, {}
    for ea in idautils.Heads(fn.start_ea, fn.end_ea):
        if not idc.is_code(idc.get_full_flags(ea)):
            continue
        mnem = idc.print_insn_mnem(ea)
        # A tail call is a jump out of the function, and it is still a call: sdfGrSetFrameMaskRGB
        # is nothing but two of them, and reading only jal made its whole body invisible.
        if mnem in ("j", "b"):
            for ref in idautils.CodeRefsFrom(ea, 0):
                if not (fn.start_ea <= ref < fn.end_ea):
                    name = idc.get_func_name(ref)
                    if name:
                        calls.setdefault(name, []).append(ea)
            continue
        if mnem in ("jal", "jalr"):
            target = None
            for ref in idautils.CodeRefsFrom(ea, 0):
                target = idc.get_func_name(ref) or target
            if not target and mnem == "jalr":
                # The member the pointer came from, so the call can be matched rather than excused.
                # jalr names the link register first and the target second.
                reg = idc.print_operand(ea, 1)
                back = ea
                for _ in range(8):
                    back = idc.prev_head(back)
                    if idc.print_insn_mnem(back) == "lw" and idc.print_operand(back, 0) == reg:
                        disp = idc.get_operand_value(back, 1)
                        target = "indirect+0x%X" % disp
                        break
            calls.setdefault(target or "indirect", []).append(ea)
            continue
        for ref in idautils.DataRefsFrom(ea):
            name = idc.get_name(ref)
            # An auto-named global is still a fact. The check blocks on it, so the answer is to
            # name it in the database and re-export, never to drop it here.
            if name and not name.startswith(("loc_", "jpt_", "sub_")):
                globs.setdefault(name, []).append(ea)
    return calls, globs


def cop2(fn):
    out = []
    for ea in idautils.Heads(fn.start_ea, fn.end_ea):
        if idc.print_insn_mnem(ea) in ("lqc2", "sqc2"):
            out.append((ea, ida_lines.tag_remove(idc.GetDisasm(ea) or "")))
    return out


def emit(out, name):
    ea = idc.get_name_ea_simple(name)
    fn = ida_funcs.get_func(ea)
    if not fn:
        out.write("# %s: no function at %s\n" % (name, name))
        return
    out.write("\nfn %s 0x%08X..0x%08X\n" % (name, fn.start_ea, fn.end_ea - 1))

    members = {}
    try:
        cfunc = ida_hexrays.decompile(fn)
        if cfunc:
            v = Members()
            v.apply_to(cfunc.body, None)
            members = v.found
            # A failure reading the shape must not cost the member facts beside it.
            try:
                args, loops, calls = shape(cfunc)
                out.write("  sig   (%s)\n" % ", ".join(args))
                out.write("  shape loops=%d calls=%d\n" % (loops, calls))
            except Exception as exc:
                out.write("  # shape unavailable (%s)\n" % exc)
            with open(PSEUDO, "a") as ref:
                ref.write("// %s  SLUS-20911 0x%08X\n%s\n\n"
                          % (name, fn.start_ea, str(cfunc)))
    except Exception as exc:
        out.write("  # decompiler failed (%s); members are missing\n" % exc)

    for (struct, off), (at, write, taken) in sorted(members.items(),
                                                   key=lambda kv: (kv[1][0], kv[0])):
        if not struct or struct.startswith(("int", "unsigned", "__", "void", "char", "float")):
            continue
        if not at:
            # The ctree gave the access no address, so it cannot be cited or opted out.
            out.write("  # %s+0x%02X has no address in the ctree\n" % (struct, off))
            continue
        kind = "addr" if taken and not write else ("write" if write else "read")
        out.write("  %-5s 0x%X %s+0x%02X\n" % (kind, at, struct, off))

    calls, globs = calls_and_globals(fn)
    for sym, at in sorted(globs.items(), key=lambda kv: kv[1][0]):
        out.write("  glob  0x%X %s%s\n" % (at[0], sym, addrs(at)))
    for sym, at in sorted(calls.items(), key=lambda kv: kv[1][0]):
        out.write("  call  0x%X %s%s\n" % (at[0], sym, addrs(at)))
    # A FlowChart is consumed by one pass, and depths() goes first.
    chart = list(ida_gdl.FlowChart(fn, flags=ida_gdl.FC_PREDS))
    order = blocks(fn)
    deep = depths(chart, order)
    cond = conditionals(chart)
    for lo, hi in order:
        # A block inside a loop or an if body must be transcribed inside one, not cited from the top.
        out.write("  blk   0x%X..0x%X  depth=%d cond=%d\n"
                  % (lo, hi - 1, deep.get(lo, 0), cond.get(lo, 0)))
    for at, text in cop2(fn):
        out.write("  # cop2 0x%X %s\n" % (at, text))


def addrs(at):
    return "  addrs=" + ",".join("0x%X" % a for a in at[1:]) if len(at) > 1 else ""


open(PSEUDO, "w").close()
with open(OUT, "w") as out:
    out.write("# Generated by scripts/ida_facts.py. Do not edit.\n")
    for name in TARGETS:
        emit(out, name)
print("wrote " + OUT)
