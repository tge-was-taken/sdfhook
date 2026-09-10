# Runs inside IDA. For every packet type made from an sdfGrAllocPacketBuff buffer, lists what the
# allocating function stores into it, by offset, with DMA, VIF, GIF and A+D words decoded.
import re
import idautils
import idaapi
import idc
import ida_hexrays as H
import ida_typeinf

OUT = r"C:\Users\Chris\AppData\Local\Temp\packet_layouts.txt"
WIDTH = {"_BYTE": 1, "_WORD": 2, "_DWORD": 4, "_QWORD": 8, "_OWORD": 16, "float": 4, "f32": 4,
         "int": 4, "__int64": 8, "unsigned int": 4, "u32": 4, "u64": 8, "s32": 4}
VIF = {0x00: "NOP", 0x01: "STCYCL", 0x04: "ITOP", 0x05: "STMOD", 0x10: "FLUSHE", 0x11: "FLUSH",
       0x13: "FLUSHA", 0x14: "MSCAL", 0x15: "MSCALF", 0x17: "MSCNT", 0x20: "STMASK", 0x30: "STROW",
       0x31: "STCOL", 0x4A: "MPG", 0x50: "DIRECT", 0x51: "DIRECTHL"}
GS = {0x00: "PRIM", 0x01: "RGBAQ", 0x06: "TEX0_1", 0x07: "TEX0_2", 0x08: "CLAMP_1", 0x09: "CLAMP_2",
      0x0E: "A+D", 0x14: "TEX1_1", 0x15: "TEX1_2", 0x18: "XYOFFSET_1", 0x1A: "PRMODECONT",
      0x3B: "TEXA", 0x3F: "TEXFLUSH", 0x42: "ALPHA_1", 0x43: "ALPHA_2", 0x44: "DIMX",
      0x47: "TEST_1", 0x48: "TEST_2", 0x4C: "FRAME_1", 0x4D: "FRAME_2", 0x4E: "ZBUF_1",
      0x4F: "ZBUF_2", 0x3D: "FOGCOL", 0x49: "PABE", 0x4A: "FBA_1", 0x50: "BITBLTBUF",
      0x51: "TRXPOS", 0x52: "TRXREG", 0x53: "TRXDIR", 0x60: "SIGNAL", 0x61: "FINISH"}


def number(text):
    text = text.strip().rstrip("LU").rstrip("L").rstrip("u")
    try:
        return int(text, 0) & 0xFFFFFFFFFFFFFFFF
    except ValueError:
        return None


def describe(offset, width, value):
    n = number(value)
    if n is None:
        return value[:40]
    if width == 4 and offset % 16 >= 8:
        cmd = (n >> 24) & 0x7F
        if (cmd & 0x60) == 0x60:
            return "VIF UNPACK vn=%d vl=%d num=%d addr=%X" % ((cmd >> 2) & 3, cmd & 3, (n >> 16) & 0xFF, n & 0x3FF)
        if cmd in VIF:
            return "VIF %s %X" % (VIF[cmd], n & 0xFFFF)
    if width == 8 and offset % 16 == 8 and n < 0x80:
        return "A+D reg %s" % GS.get(n, hex(n))
    if width == 8 and (n >> 60) and not ((n >> 16) & 0x3FFFFFFF):
        return "GIF tag nloop=%d eop=%d flg=%d nreg=%d" % (n & 0x7FFF, (n >> 15) & 1, (n >> 58) & 3, n >> 60)
    return hex(n)


def stores(code, name):
    n = re.escape(name)
    found = []
    for m in re.finditer(r"\*\(\(([\w ]+?) \*\)%s \+ (\d+)\) = ([^;]+);" % n, code):
        w = WIDTH.get(m.group(1).strip(), 0)
        found.append((int(m.group(2)) * w, w, m.group(3)))
    for m in re.finditer(r"\*\(([\w ]+?) \*\)\(%s \+ (\d+)\) = ([^;]+);" % n, code):
        found.append((int(m.group(2)), WIDTH.get(m.group(1).strip(), 0), m.group(3)))
    for m in re.finditer(r"\*\(([\w ]+?) \*\)%s = ([^;]+);" % n, code):
        found.append((0, WIDTH.get(m.group(1).strip(), 0), m.group(2)))
    for m in re.finditer(r"(?<![\w*.>])%s\[(\d+)\] = ([^;]+);" % n, code):
        found.append((int(m.group(1)), 1, m.group(2)))
    calls = [c for c in re.findall(r"(\w+)\(([^;]*?\b%s\b[^;]*?)\);" % n, code)
             if c[0] not in ("sdfGrAllocPacketBuff", "sdfDrawTagLink", "sdfDrawTagLinkRef",
                             "sdfDrawTagLinkRefGif", "sdfDrawTagLink2", "sdfDrawTagLinkCall")]
    return sorted(found), calls


u8ptr = ida_typeinf.tinfo_t()
u8ptr.create_ptr(ida_typeinf.tinfo_t(ida_typeinf.BT_INT8 | ida_typeinf.BTMT_USIGNED))

results = []
alloc = idc.get_name_ea_simple("sdfGrAllocPacketBuff")
for start in sorted(set(idaapi.get_func(x).start_ea for x in idautils.CodeRefsTo(alloc, 0)
                        if idaapi.get_func(x))):
    fn = idc.get_func_name(start)
    cf = H.decompile(start, flags=H.DECOMP_NO_CACHE)
    for lv in cf.get_lvars():
        t = lv.type()
        if not t.is_ptr():
            continue
        tname = t.get_pointed_object().get_type_name() or ""
        if not tname.startswith(fn) or not tname.endswith("_t") or "Packet" not in tname:
            continue
        # Read the raw offsets through a byte pointer, then put the struct type back.
        info = H.lvar_saved_info_t()
        info.ll = lv
        info.type = u8ptr
        H.modify_user_lvar_info(start, H.MLI_TYPE, info)
        code = str(H.decompile(start, flags=H.DECOMP_NO_CACHE))
        found, calls = stores(code, lv.name)
        info.type = t
        H.modify_user_lvar_info(start, H.MLI_TYPE, info)
        size = ida_typeinf.tinfo_t()
        size.get_named_type(None, tname)
        results.append((tname, size.get_size(), found, calls))

with open(OUT, "w") as out:
    for tname, size, found, calls in results:
        out.write("== %s (%d)\n" % (tname, size))
        for offset, width, value in found:
            out.write("   +0x%02X w%-2d %s\n" % (offset, width, describe(offset, width, value)))
        for callee, args in calls:
            out.write("   call %s(%s)\n" % (callee, args[:80]))
print("wrote %d packet types to %s" % (len(results), OUT))
