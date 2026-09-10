# Runs inside IDA, like export_symbols.py. Writes the IDB's type for every packet the game links,
# so the runtime names packets from the IDB; "void" marks a packet the IDB has not typed yet.
import idautils
import idaapi
import idc
import ida_hexrays as H
import ida_typeinf

SERIAL = "SLUS-20911"
OUT = rf"C:\Users\Chris\AppData\Local\Temp\{SERIAL}.packets"

LINKS = {"sdfDrawTagLink", "sdfDrawTagLink2", "sdfDrawTagLinkRef", "sdfDrawTagLinkRefGif",
         "sdfDrawTagLinkCall"}

# Builders the link dataflow cannot reach, since what they build is referenced by another packet
# or built once at setup rather than handed to a link. (function, argument, or None for a return)
EXTRA_BUILDERS = [
    ("sdfModelAllocTmPacket", None),
    ("sdfModelNewPacketHeader", None),
    ("sdfModelSetNormalDrawPacket", 2),
    ("sdfModelSetOneSkinDrawPacket", 2),
    ("sdfModelSetTermPacket", 0),
    ("sdfGrSetClearPacket", 0),
    ("sdfGrSetFramePacket", 0),
    ("sdfGrSetDrawFramePacket", 0),
    ("sdfGrSetDrawDefParmPacket", 0),
    ("sdfGrSetStdDrawEnv", 0),
    ("sdfGrLinkDrawFrameTex", 2),
    ("sdfGrLinkDispFrameTex", 2),
    ("sdfGrSetRefGifPacket", 0),
    ("sdf3DPrimSetTmPacket", 0),
    ("sdfBillBoardMake1", 0),
    ("sdfBillBoardVcMake1", 0),
    ("sdfBillBoardVcMakeN", None),
    ("sdfMultiBillBoardVcMakeN", None),
    ("sdfDevVprintf", None),
    ("sdfDrawModelWithMatrix", None),
]

# A global's type comes from the IDB, except where applying one would swallow the names inside it.
GLOBALS = {"sdf3DPrimIdentityTm": None, "vu1MicroPacket": "sdf3DVu1MicroPacket_t"}

# These hand out memory, not a packet of any type.
ALLOCATORS = {"sdfGrAllocPacketBuff", "sdfGrOpenPacketBuff", "sdfDrawTagInit", "memset"}


def strip(e):
    while e.op == H.cot_cast:
        e = e.x
    return e


def callee(call):
    x = strip(call.x)
    return idc.get_name(x.obj_ea) if x.op == H.cot_obj else None


def pointed(tif):
    if tif is None or tif.empty():
        return "void"
    if tif.is_ptr():
        target = tif.get_pointed_object()
        if target.is_void() or not target.get_type_name():
            return "void"
        return target.get_type_name()
    return "void"


def func_type(name):
    """The tinfo and its details; an empty details is a function with no arguments, not a miss."""
    ea = idc.get_name_ea_simple(name)
    tif = ida_typeinf.tinfo_t()
    if ea == idc.BADADDR or not (idaapi.get_tinfo(tif, ea) or ida_typeinf.guess_tinfo(tif, ea)):
        return None, None
    details = ida_typeinf.func_type_data_t()
    return (tif, details) if tif.get_func_details(details) else (None, None)


def arg_type(name, index):
    tif, details = func_type(name)
    return pointed(details[index].type) if details is not None and index < details.size() else "void"


def ret_type(name):
    tif, details = func_type(name)
    return pointed(details.rettype) if details is not None else "void"


class Calls(H.ctree_visitor_t):
    def __init__(self):
        H.ctree_visitor_t.__init__(self, H.CV_FAST)
        self.calls = []
        self.assigns = []

    def visit_expr(self, e):
        if e.op == H.cot_call:
            self.calls.append(e)
        elif e.op == H.cot_asg:
            self.assigns.append(e)
        return 0


def var_index(e):
    e = strip(e)
    # LODWORD(v) = x assigns v's low half, which is where a 64-bit local gets a pointer.
    if e.op == H.cot_call and e.x.op == H.cot_helper and e.x.helper in ("LODWORD", "HIDWORD"):
        e = strip(e.a[0])
    return e.v.idx if e.op == H.cot_var else None


def discover(packet, v):
    """Every builder behind a linked packet: a call it came back from, or one it was handed to."""
    if packet.op == H.cot_call and callee(packet) not in ALLOCATORS | {None}:
        builders[(callee(packet), None)] = True
    index = var_index(packet)
    if index is None:
        return
    for a in v.assigns:
        source = strip(a.y)
        if var_index(a.x) == index and source.op == H.cot_call and callee(source) not in ALLOCATORS | {None}:
            builders[(callee(source), None)] = True
    for other in v.calls:
        name = callee(other)
        if not name or name in LINKS or name in ALLOCATORS:
            continue
        for i in range(other.a.size()):
            if var_index(other.a[i]) == index:
                builders[(name, i)] = True


def resolve(e, v, depth):
    """The packet's type: the expression's own, else whatever it was built or copied from."""
    e = strip(e)
    typ = pointed(e.type)
    if typ != "void" or depth > 8:
        return typ
    if e.op == H.cot_call and callee(e):
        name = callee(e)
        if name not in ALLOCATORS:
            builders[(name, None)] = True
            return ret_type(name)
        return "void"
    if e.op in (H.cot_add, H.cot_sub):
        return resolve(e.x, v, depth + 1)
    index = var_index(e)
    if index is None:
        return "void"
    for other in v.calls:
        name = callee(other)
        if not name or name in LINKS or name in ALLOCATORS:
            continue
        for i in range(other.a.size()):
            if var_index(other.a[i]) == index:
                builders[(name, i)] = True
                if arg_type(name, i) != "void":
                    return arg_type(name, i)
    for a in v.assigns:
        if var_index(a.x) == index and a.y is not e:
            typ = resolve(a.y, v, depth + 1)
            if typ != "void":
                return typ
    return "void"


functions = set()
for link in LINKS:
    for ref in idautils.CodeRefsTo(idc.get_name_ea_simple(link), 0):
        f = idaapi.get_func(ref)
        if f:
            functions.add(f.start_ea)

sites, tails, builders = [], set(), {}
for start in sorted(functions):
    fn = idc.get_func_name(start)
    # Uncached, or a type applied to a global since the last decompile is not seen.
    cf = H.decompile(start, flags=H.DECOMP_NO_CACHE)
    v = Calls()
    v.apply_to(cf.body, None)
    for call in v.calls:
        link = callee(call)
        if link not in LINKS or call.a.size() < 2:
            continue
        packet = strip(call.a[1])
        discover(packet, v)
        typ = resolve(packet, v, 0)
        # A j to the link leaves $ra naming the caller, so only a jal can key a site.
        if idc.print_insn_mnem(call.ea) == "jal":
            sites.append((call.ea + 8, link, typ))
        else:
            tails.add((fn, link))

for entry in EXTRA_BUILDERS:
    builders[entry] = True


def tail_callee(name):
    """The function a builder tail-calls, if any. Its packet is the callee's, and a post-handler on
    both would stack two return sentinels on one call."""
    f = idaapi.get_func(idc.get_name_ea_simple(name))
    for ea in idautils.FuncItems(f.start_ea):
        if idc.print_insn_mnem(ea) == "j":
            target = idc.get_operand_value(ea, 0)
            g = idaapi.get_func(target)
            if g and g.start_ea == target:
                return idc.get_func_name(target)
    return None

types = set()
with open(OUT, "w") as out:
    for ra, link, typ in sites:
        out.write(f"site 0x{ra:08X} {link} {typ}\n")
        types.add(typ)
    for fn, link in sorted(tails):
        out.write(f"tail {fn} {link}\n")
    for (fn, index) in sorted(builders, key=lambda k: (k[0], -1 if k[1] is None else k[1])):
        typ = ret_type(fn) if index is None else arg_type(fn, index)
        types.add(typ)
        inner = tail_callee(fn) if index is None else None
        if inner and (inner, None) in builders:
            out.write(f"wrap {fn} {inner}\n")
        elif index is None:
            out.write(f"ret {fn} {typ}\n")
        else:
            out.write(f"arg {fn} {index} {typ}\n")
    for name, fixed in GLOBALS.items():
        tif = ida_typeinf.tinfo_t()
        typ = fixed
        if typ is None:
            ok = idaapi.get_tinfo(tif, idc.get_name_ea_simple(name)) and tif.get_type_name()
            typ = tif.get_type_name() if ok else "void"
        out.write(f"global {name} {typ}\n")
        types.add(typ)
    # Sized, so an entry inside a recorded packet is known to be part of it.
    for typ in sorted(types - {"void"}):
        tif = ida_typeinf.tinfo_t()
        if tif.get_named_type(None, typ):
            out.write(f"size {typ} {tif.get_size()}\n")

print(f"wrote {len(sites)} sites, {len(tails)} tail calls, {len(builders)} builders to {OUT}")
