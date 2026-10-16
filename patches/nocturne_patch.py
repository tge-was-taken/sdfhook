#!/usr/bin/env python3
"""Hard patches for SLUS_209.11 (Shin Megami Tensei: Nocturne, NTSC-U, CRC E8FCF8EC).

Every site is checked against the original bytes before it is written, so a different build of the
ELF is refused rather than corrupted. The same table patches a savestate's RAM, for testing the
patches that do not depend on boot-time setup.
"""

import argparse
import hashlib
import io
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import dataclass

ORIGINAL_MD5 = "6a4d861fb950eea5247f74a860a5bac4"
ELF_NAME = "SLUS_209.11"


@dataclass(frozen=True)
class Site:
    addr: int
    old: bytes  # the original bytes, or for a whole region their SHA-1 as bytes.fromhex
    new: bytes
    what: str
    hashed: bool = False


@dataclass(frozen=True)
class Segment:
    """Code loaded as its own ELF segment, past the end of the game's data."""
    base: int
    data: bytes
    memsz: int  # data, then zeros the loader fills in


def words(*values):
    return b"".join(struct.pack("<I", v) for v in values)


NOP = words(0)

# sdfGrSetDisp 0x2A9C08, rewritten to show a 512x224 buffer line-doubled in 480p. Columns 16..495
# at 3 VCK each fill the 1440 VCK active line exactly; wider would put picture in the porches.
SET_DISP = words(
    0x3C030053,  # lui    v1, 0x53
    0x24639388,  # addiu  v1, v1, -0x6C78        aSrcBuffWidth 0x529388
    0x00041080,  # sll    v0, a0, 2
    0x00621021,  # addu   v0, v1, v0
    0x8C420008,  # lw     v0, 8(v0)              frameHandle[aEvenOdd]
    0x8C42000C,  # lw     v0, 12(v0)             ->addr
    0x3C040058,  # lui    a0, 0x58
    0x24840FF0,  # addiu  a0, a0, 0xFF0          display env 0x580FF0
    0x000212C2,  # srl    v0, v0, 11
    0x304201FF,  # andi   v0, v0, 0x1FF          FBP
    0x34421000,  # ori    v0, v0, 0x1000         FBW 8, PSMCT32
    0x64030010,  # daddiu v1, zero, 16
    0x0003183C,  # dsll32 v1, v1, 0              DBX 16
    0x00431025,  # or     v0, v0, v1
    0xFC820010,  # sd     v0, 0x10(a0)           DISPFB
    0x64020066,  # daddiu v0, zero, 0x66
    0xFC820000,  # sd     v0, 0(a0)              PMODE
    0x64020002,  # daddiu v0, zero, 2
    0xFC820008,  # sd     v0, 8(a0)              SMODE2, progressive
    0x3C02001B,  # lui    v0, 0x1B
    0x3442F59F,  # ori    v0, v0, 0xF59F         DH 447, DW 1439
    0x0002103C,  # dsll32 v0, v0, 0
    0x3C030903,  # lui    v1, 0x903
    0x346330E8,  # ori    v1, v1, 0x30E8         MAGV 1, MAGH 2, DY 51, DX 232
    0x00431025,  # or     v0, v0, v1
    0xFC820018,  # sd     v0, 0x18(a0)           DISPLAY
    0x080B21F6,  # j      sceGsPutDispEnv 0x2C87D8
    0xFC800020,  # sd     zero, 0x20(a0)         BGCOLOR
) + NOP * 8

SET_DISP_ORIGINAL = bytes.fromhex(
    "e0ffbd275300033c0000b0ff5800103c1000bfff889363240800b1ff80100400ff0f043cf00f1026211062"
    "00ffff84340800518c0020023c040067842420040200006684252082002d2800002d40000018210b0c2d48"
    "00000c00228e1000038e00fe0524c21202002d20000224186500ff014230251862000800b1df100003ae10"
    "00bfdf0000b0dff6210b082000bd27"
)

ASPECT = 0x2F59B4  # dds3ScreenParam.aspect
ASPECT_ORIGINAL = 1.165


def aspect_for(widescreen, progressive, native=False):
    """dds3ScreenParam.aspect for the display the other groups set up."""
    if native:
        # 480 visible columns hold the whole 512-column projection, so it is 15/16 narrower.
        return (16 / 9 if widescreen else 4 / 3) * 15 / 16
    if progressive:
        # At 3 VCK a column and 2 lines a row the 512x448 picture has the screen's own aspect
        # times 512/448, so the parameter is the screen's aspect.
        return 16 / 9 if widescreen else 4 / 3
    # Interlaced, the game's own 4:3 value scaled, as PCSX2's widescreen patch does.
    return ASPECT_ORIGINAL * 4 / 3 if widescreen else ASPECT_ORIGINAL


LINE_DOUBLE = [
    Site(0x2A9C08, SET_DISP_ORIGINAL, SET_DISP, "sdfGrSetDisp: 480p DISPLAY, line-doubled"),
    # Both field passes then render the same image; offset, they would bob a line apart.
    Site(0x2AD288, words(0x008B180B), NOP,
         "sdfGrSetDrawFramePacket: drop the odd field's half-line XYOFFSET"),
]

# Dead debug and unused library code: no call, no reference, and no word in the image holding an
# address inside it. The first holds weave.c, the last its DMA chain, the rest the per-line copy
# list.
WEAVE_REGIONS = [
    (0x12B8E8, 0x12BF0C, "7b6818c89f0a37c747789f15d0067fc9d7f30bb5", "fldDbgPrint2DC..fldDbgPrintStr2DCF"),
    (0x2CEF20, 0x2CF4F8, "7d55af3b1ceef0b4d8937d39a70b7ab62eb7bf5b", "sceMcFlush..sceMcGetEntSpace"),
    (0x2E0FB8, 0x2E1554, "bafd1705ce3a9b9b192cd49a459b294c870a6661", "sceRemove..sceDelDrv"),
    (0x2E08E0, 0x2E0E0C, "94fb504058ee18d5e331b21cb097a265cb04014c", "sceIoctl, sceIoctl2"),
    (0x1DB140, 0x1DB65C, "95dabf6fca9947ea93f3c0c5c21ca8f9e9852b38", "pbInitMCard..pbWriteMCard"),
    (0x28C3E8, 0x28C894, "2a0dab2609e2613266facef912f88636d6522d83", "TestProcess3..8"),
    (0x1DE5B8, 0x1DEA58, "9ffa66d2bd34f65c5f0a157337ed312a6c3d44e6", "mdlMakeHddFileList"),
    (0x1D1E58, 0x1D22A4, "7386227dc7ce701db52bf1d3b4b8c87e75188b9d", "pbDebugPrintPanel3"),
    (0x2E1A80, 0x2E1ECC, "51217a5d4d49596277a06fba28c1b800ef03d925", "sceChstat, sceRename, sceChdir"),
    (0x22C190, 0x22C574, "c01fc8e24745f25a2dd7c0f17f838a897eb3794e", "movieViewerDebugDraw"),
    (0x1CEC78, 0x1CF058, "a42ee8460e9cfc31b1c9686c019750c0bd2ff29e", "itfMesMngDebugProc, itfMesMngDebugShutdown"),
    (0x2E2768, 0x2E2B10, "8727384de92d0dc35e9277c554dcbfd936fc5fe3", "sceSymlink, sceReadlink"),
    (0x246550, 0x2468DC, "862b100d1a2d1a9c3f6c89f35a0b12da22b12484", "datDebugUnitProcess.."),
    (0x1DAB30, 0x1DAE30, "66dd8e4e38743c70bfc08ecc5f600e93ae80474b", "pbDumpRecordData"),
    (0x2C3388, 0x2C3660, "515a012b483431338d8cb2dc8e510c0b262ef02e", "sdfDumpGsMemory.."),
    (0x1C9248, 0x1C9530, "4f157ff7ce1b48d4b158d63c9d95aa8060bc9173", "frDumpFontTexture"),
]

HERE = pathlib.Path(__file__).parent
GS_ERROR_HANDLER = 0x55185C  # sdfGsErrorHandler, null in the retail game
TRXPOS, TRXREG, TRXDIR, BITBLTBUF = 0x51, 0x52, 0x53, 0x50
FIELD_LINES = 224


def a_d(data, reg):
    return struct.pack("<QQ", data, reg)


def dma_tag(qwc, tag_id, addr=0, vif=(0, 0)):
    return struct.pack("<QII", qwc | tag_id << 28 | addr << 32, *vif)


def direct(qwc):
    return (0, 0x50000000 | qwc)  # VIF NOP, DIRECT


def cross_tool(name):
    tool = shutil.which("mips64r5900el-ps2-elf-gcc")
    prefix = pathlib.Path(tool).parent if tool else pathlib.Path(
        os.environ.get("PS2DEV", pathlib.Path.home() / "projects/ps2dev/ps2dev")) / "ee/bin"
    path = prefix / f"mips64r5900el-ps2-elf-{name}"
    if not path.exists():
        raise SystemExit(f"the weave and native groups need the PS2 EE toolchain: {path} not found "
                         f"(set PS2DEV)")
    return str(path)


CFLAGS = ["-O2", "-G0", "-march=r5900", "-mno-abicalls", "-fno-pic", "-ffreestanding",
          "-fno-builtin", "-fno-common", "-nostdlib", "-Wall", "-Wextra", "-Werror",
          "-ffunction-sections", "-fdata-sections"]


def compile_source(source, defines, obj):
    defs = [f"-D{k}=0x{v:X}" for k, v in defines.items()]
    subprocess.run([cross_tool("gcc"), *CFLAGS, *defs, "-c", str(source), "-o", str(obj)], check=True)


def symbol_table(elf):
    table = subprocess.run([cross_tool("nm"), str(elf)], check=True, capture_output=True,
                           text=True).stdout
    fields = (line.split() for line in table.splitlines())
    return {f[2]: int(f[0], 16) for f in fields if len(f) == 3}


def build_segment(source, defines, base):
    """source linked at base as one segment, its zeroed data left out of the file."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        compile_source(source, defines, tmp / "code.o")
        (tmp / "code.ld").write_text(
            f"SECTIONS {{\n  .load 0x{base:X} : {{ *(.text*) *(.rodata*) *(.data*) }}\n"
            f"  .bss (NOLOAD) : ALIGN(16) {{ bss_start = .; *(.bss*) . = ALIGN(16); }}\n"
            f"  segment_end = .;\n"
            f"  /DISCARD/ : {{ *(*) }}\n}}\n")
        elf = tmp / "code.elf"
        subprocess.run([cross_tool("ld"), "-T", str(tmp / "code.ld"), "-o", str(elf),
                        str(tmp / "code.o")], check=True)
        subprocess.run([cross_tool("objcopy"), "-O", "binary", "-j", ".load", str(elf),
                        str(tmp / "code.bin")], check=True)
        data = (tmp / "code.bin").read_bytes()
        symbols = symbol_table(elf)
    data += bytes(-len(data) % 16)
    return Segment(base, data, symbols["segment_end"] - base), symbols


def build(source, defines, regions):
    """source compiled one section per function and packed first-fit into regions, as
    ({region start: bytes filling the region}, {symbol: address})."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        obj = tmp / "code.o"
        compile_source(source, defines, obj)
        headers = subprocess.run([cross_tool("objdump"), "-h", str(obj)], check=True,
                                 capture_output=True, text=True).stdout
        sections = []
        for line in headers.splitlines():
            f = line.split()
            if len(f) >= 7 and f[0].isdigit() and f[1].startswith((".text", ".rodata", ".data", ".bss")):
                size, align = int(f[2], 16), 2 ** int(f[6].split("**")[1])
                if size:
                    sections.append((f[1], size, max(align, 8)))
        # First-fit, largest first, keeping each region's space and its sections.
        free = [[(start + 15) & ~15, end, []] for start, end, _, _ in regions]
        for name, size, align in sorted(sections, key=lambda s: -s[1]):
            for region in free:
                pos = (region[0] + align - 1) & ~(align - 1)
                if pos + size <= region[1]:
                    region[2].append(name)
                    region[0] = pos + size
                    break
            else:
                raise SystemExit(f"{source.name}: {name} ({size} bytes) fits no free region")
        script = ["SECTIONS {"]
        for i, ((start, end, _, _), region) in enumerate(zip(regions, free)):
            if region[2]:
                inputs = " ".join(f"*({name})" for name in region[2])
                script.append(f"  .r{i} 0x{(start + 15) & ~15:X} : {{ {inputs} }}")
        script += ["  /DISCARD/ : { *(*) }", "}"]
        (tmp / "code.ld").write_text("\n".join(script) + "\n")
        elf = tmp / "code.elf"
        subprocess.run([cross_tool("ld"), "-T", str(tmp / "code.ld"), "-o", str(elf), str(obj)],
                       check=True)
        region_bytes = {}
        for i, ((start, end, _, _), region) in enumerate(zip(regions, free)):
            if not region[2]:
                continue
            out = tmp / f"r{i}.bin"
            subprocess.run([cross_tool("objcopy"), "-O", "binary", "-j", f".r{i}", str(elf), str(out)],
                           check=True)
            code = out.read_bytes()
            aligned = (start + 15) & ~15
            region_bytes[start] = bytes(aligned - start) + code + bytes(end - aligned - len(code))
        symbols = symbol_table(elf)
    return region_bytes, symbols


def copy_line(src_y, dst_y):
    return a_d(src_y << 16 | dst_y << 48, TRXPOS) + a_d(2, TRXDIR)


def weave_sites(smooth):
    cave_start, cave_end = WEAVE_REGIONS[0][:2]
    lists = [[copy_line(y, 2 * y) for y in range(FIELD_LINES)],
             [copy_line(y, 2 * y + 1) for y in range(FIELD_LINES)]]

    # Whole lines into the list regions, even list first; a region may take the end of one list
    # and the start of the other as two pieces.
    pieces = [[], []]
    region_bytes = {}
    queue = [(half, line) for half in (0, 1) for line in lists[half]]
    for start, end, _, _ in WEAVE_REGIONS[1:-1]:
        pos, content = (start + 15) & ~15, bytearray()
        while queue and pos + len(content) + 32 <= end:
            half = queue[0][0]
            piece_start = pos + len(content)
            count = 0
            while queue and queue[0][0] == half and pos + len(content) + 32 <= end:
                content += queue.pop(0)[1]
                count += 1
            pieces[half].append((piece_start, count * 2))
        if content:
            region_bytes[start] = bytes(pos - start) + bytes(content) + bytes(end - pos - len(content))
    if queue:
        raise SystemExit("the weave's copy list does not fit the dead regions")

    total = 2 + 1 + 2 * 2 * FIELD_LINES  # BITBLTBUF, TRXREG, BITBLTBUF, then the lines
    gif_tag = struct.pack("<QQ", total | 1 << 15 | 1 << 60, 0xE)  # PACKED, one A+D register, EOP
    trxreg = a_d(512 | 1 << 32, TRXREG)
    chain = dma_tag(3, 1, vif=direct(3)) + gif_tag + a_d(0, BITBLTBUF) + trxreg
    chain += b"".join(dma_tag(qwc, 3, addr, direct(qwc)) for addr, qwc in pieces[0])
    bbb_offset = len(chain) + 16
    chain += dma_tag(1, 1, vif=direct(1)) + a_d(0, BITBLTBUF)
    chain += b"".join(dma_tag(qwc, 3, addr, direct(qwc)) for addr, qwc in pieces[1])
    chain += dma_tag(0, 7)

    chain_start, chain_end = WEAVE_REGIONS[-1][:2]
    chain_addr = (chain_start + 15) & ~15
    if chain_addr + len(chain) > chain_end:
        raise SystemExit("the weave's DMA chain does not fit its region")
    region_bytes[chain_start] = (bytes(chain_addr - chain_start) + chain
                                 + bytes(chain_end - chain_addr - len(chain)))
    defines = {"CHAIN": chain_addr, "BBA": chain_addr + 32, "BBB": chain_addr + bbb_offset,
               "SMOOTH": int(smooth)}
    return region_sites(HERE / "weave.c", defines, region_bytes, WEAVE_REGIONS[:1]) + hook_sites(
        "the woven frame, else a field buffer line-doubled", "record each pass for the weave")


def native_sites(smooth):
    sites = region_sites(HERE / "native.c", {"SMOOTH": int(smooth)}, {}, WEAVE_REGIONS)
    symbols = BUILT_SYMBOLS
    call = lambda name: words(0x0C000000 | symbols[name] >> 2)
    sites += hook_sites("the last finished 448-line frame", "a frame is finished with its bottom half")
    sites += [
        Site(0x2AD600, words(0x0C0AB47E), call("set_half_frame_packet"),
             "sdfGrStdDrawEnvPredraw: FRAME_1 packet for the pass's half"),
        Site(0x2AD62C, words(0x0C0AB47E), call("set_half_frame_packet"),
             "sdfGrStdDrawEnvPredraw: FRAME_2 packet for the pass's half"),
        Site(0x2ADBD8, words(0x9382D6C9), words(0x24020001),
             "sdfGrDrawFrame: the first pass draws the top half, not the field's offset"),
        Site(0x10275C, words(0x0C0AB69A), call("submit_frame"),
             "dds3DrawProcess: remap the frame's 2D to 448 lines and 480 columns before sdfGrReqDraw"),
    ]
    sites += [Site(addr, words(0x0C0AE78C), call("viewport_packet"),
                   f"{name}: VU1 viewport at full height and 480 columns")
              for addr, name in VIEWPORT_CALLS]
    return sites


VIEWPORT_CALLS = [
    (0x101FB8, "dds3DrawFirstProcess"), (0x1020C4, "dds3DrawFirstProcess"),
    (0x102234, "dds3DrawFirstProcess"), (0x10475C, "dds3InitDrawEnvSub"),
    (0x1047C4, "dds3InitDrawEnvSub"), (0x104958, "dds3InitRenderDrawEnv"),
    (0x21B644, "SetCameraLightParam"), (0x239AA8, "cmpDrawSetup3DView"),
    (0x240758, "cmpSetStatusModelParam"),
]


BUILT_SYMBOLS = {}


def region_sites(source, defines, region_bytes, code_regions):
    """source packed into code_regions, and the regions' other contents."""
    code, symbols = build(source, defines, code_regions)
    BUILT_SYMBOLS.clear()
    BUILT_SYMBOLS.update(symbols)
    region_bytes = {**region_bytes, **code}
    return [Site(start, bytes.fromhex(sha1), region_bytes[start], f"dead {names}: {source.name}",
                 hashed=True)
            for start, end, sha1, names in WEAVE_REGIONS if start in region_bytes]


def hook_sites(shown, recorded):
    symbols = BUILT_SYMBOLS
    jump = lambda target: words(0x08000000 | (target >> 2) & 0x3FFFFFF, 0)
    return [
        Site(0x2A9C08, words(0x27BDFFE0, 0x3C030053), jump(symbols["set_disp"]),
             f"sdfGrSetDisp -> set_disp: {shown}"),
        Site(0x2A9FC8, words(0x14800005, 0x00000000), jump(symbols["deliver_draw_end"]),
             f"sdfDeliverDrawEnd -> deliver_draw_end: {recorded}"),
        Site(0x2AA0D4, words(0x0C0AB6E6), words(0x0C000000 | symbols["draw_frame"] >> 2),
             f"sdfDrawThread: draw_frame before sdfGrDrawFrame (state at 0x{symbols['g']:X})"),
        Site(GS_ERROR_HANDLER, words(0), words(symbols["gs_full"]),
             f"sdfGsErrorHandler = gs_full 0x{symbols['gs_full']:X}: give W back when GS memory runs out"),
    ]


# The code segment sits where the heap used to start, and the heap starts after it. Its bytes go
# over the VU overlay tables and section headers at the end of the file, which no loader reads, so
# the file keeps its size and the disc image needs no other change.
ELF_END = 0x5A0330              # _end: bss clear end, SetupHeap's start, sbrk's first break
CODE_BASE = 0x5A0340
STATE_CODE_BASE = 0x1F00000     # free in a running game, whose heap ends at 0x1EDF000
CODE_FILE_OFFSET = 0x452B90
ELF_TAIL = 0x452B84             # the unloaded tail, from the first overlay table to the end
ELF_TAIL_SHA1 = "366111ecf76008f12ad9763d09a40a7eec7b4ecc"
PHDR_FREE = 0x94                # after the three program headers, zero up to the first segment


def heap_sites(start):
    hi, lo = (start + 0x8000) >> 16, start & 0xFFFF
    upper = [Site(0x100180, words(0x3C04005A), words(0x3C040000 | hi),
                  f"_start: SetupHeap from 0x{start:X}, past the code segment (lui)")]
    return upper * (hi != 0x5A) + [
        Site(0x100188, words(0x24840330), words(0x24840000 | lo),
             f"_start: SetupHeap from 0x{start:X}, past the code segment (addiu)"),
        Site(0x52C2A8, words(ELF_END), words(start), f"sbrk's first break 0x{start:X}"),
    ]


def add_segment(elf, segment):
    data = elf.data
    phnum, = struct.unpack_from("<H", data, 44)
    if phnum != 3 or hashlib.sha1(data[ELF_TAIL:]).hexdigest() != ELF_TAIL_SHA1:
        raise SystemExit("the ELF already has a code segment or is not the retail build")
    if CODE_FILE_OFFSET + len(segment.data) > len(data):
        raise SystemExit(f"the code segment ({len(segment.data)} bytes) does not fit the ELF's tail")
    data[ELF_TAIL:] = bytes(len(data) - ELF_TAIL)
    data[CODE_FILE_OFFSET:CODE_FILE_OFFSET + len(segment.data)] = segment.data
    struct.pack_into("<8I", data, PHDR_FREE, 1, CODE_FILE_OFFSET, segment.base, segment.base,
                     len(segment.data), segment.memsz, 7, 16)
    struct.pack_into("<H", data, 44, 4)
    struct.pack_into("<I", data, 32, 0)                # e_shoff: the section headers are gone
    struct.pack_into("<HHH", data, 46, 0, 0, 0)        # e_shentsize, e_shnum, e_shstrndx
    print(f"  added segment    0x{segment.base:08X}  {len(segment.data)} bytes, {segment.memsz} in memory, at file offset "
          f"0x{CODE_FILE_OFFSET:X}")


DEBUG_FEATURES = ["noclip", "ragdoll", "freecam"]


def debug_sites(features, base):
    defines = {name.upper(): 1 for name in features}
    segment, symbols = build_segment(HERE / "debug.c", defines, base)
    call = lambda name: words(0x0C000000 | symbols[name] >> 2)
    sites = [Site(0x1005A4, words(0x0C040ECC), call("pad_hook"),
                  f"dds3KernelMain: pad_hook reads the pad for {' and '.join(features)} "
                  f"(state at 0x{symbols['d']:X})")]
    if "noclip" in features:
        sites += [
            Site(0x124E70, words(0x0C04D2C8), call("player_calc"),
                 "ProcSequence: player_calc flies the player in place of fldPlayerCalc"),
            Site(0x124EA8, words(0x0C04FB34), call("event_hit"),
                 "ProcSequence: event_hit skips doors and events while flying"),
        ]
    if "ragdoll" in features:
        sites.append(Site(0x1DD33C, words(0x0C0AC5FA), call("eval_tm"),
                          "mdlLink: eval_tm poses the player's ragdoll"))
    if "freecam" in features or "ragdoll" in features:
        sites.append(Site(0x102150, words(0x0C0AE81C), call("screen_param"),
                          "dds3DrawFirstProcess: screen_param sets the free or ragdoll camera"))
    return sites, segment


GROUPS = {
    "widescreen": [],  # the aspect site, built by select() since progressive changes the value
    "progressive": [
        Site(0x2A9CC0, words(0x10600005), words(0x10000005),
             "sdfGrGsReset: always non-interlaced, frame mode"),
        Site(0x2A9CE0, words(0x24060002), words(0x24060050),
             "sdfGrGsReset: sceGsResetGraph omode NTSC -> DTV 480p"),
    ],
    "weave": [],  # built by weave_sites(), since it needs the EE toolchain
    "native": [],  # built by native_sites()
    "noclip": [],  # built by debug_sites()
    "ragdoll": [],
    "freecam": [],
    "smooth": [],  # a build option of weave and native
    "noblur": [
        Site(0x118D40, words(0x0C04AB02), NOP,
             "ooyTestDrawProc: skip fldDrawFrameSprite, the field's per-frame feedback blur"),
    ],
    "60fps": [
        Site(0x54E82C, struct.pack("<H", 2), struct.pack("<H", 1),
             "dds3KernelMain vblanks per frame 2 -> 1"),
        Site(0x22B59C, words(0x24020002), words(0x24020001),
             "stfDestroy: restore 1 vblank per frame after the staff roll"),
    ],
}


class Elf:
    def __init__(self, data):
        self.data = bytearray(data)
        phoff, = struct.unpack_from("<I", data, 28)
        count, = struct.unpack_from("<H", data, 44)
        self.loads = []
        for i in range(count):
            kind, offset, vaddr, _, filesz, _, _, _ = struct.unpack_from("<8I", data, phoff + 32 * i)
            if kind == 1:
                self.loads.append((vaddr, offset, filesz))

    def offset(self, addr, size):
        for vaddr, offset, filesz in self.loads:
            if vaddr <= addr and addr + size <= vaddr + filesz:
                return offset + addr - vaddr
        raise ValueError(f"0x{addr:08X} is not in a loaded segment")


def select(names, for_state=False):
    """The sites for the named groups, and the code segment if any group needs one. A savestate's
    heap is already in use, so for one the segment goes to STATE_CODE_BASE and the heap stays."""
    unknown = set(names) - set(GROUPS)
    if unknown:
        raise SystemExit(f"unknown patch group: {', '.join(sorted(unknown))}")
    if "weave" in names and "progressive" not in names:
        raise SystemExit("weave builds on progressive")
    if "weave" in names and "native" in names:
        raise SystemExit("weave and native are two ways to the same 448 lines; pick one")
    if "native" in names and "progressive" not in names:
        raise SystemExit("native builds on progressive")
    if "smooth" in names and not {"weave", "native"} & set(names):
        raise SystemExit("smooth builds on weave or native")
    if "native" in names and "60fps" in names:
        raise SystemExit("native draws each frame's halves in two passes, which 60fps gives up")
    if "ragdoll" in names and "noclip" not in names:
        raise SystemExit("ragdoll catches the fall after noclip, so it needs noclip")
    if "weave" in names and "60fps" in names:
        raise SystemExit("weave needs both passes of each frame, which 60fps gives up; "
                         "pick one with --without")
    sites = [site for name in names for site in GROUPS[name]]
    if "progressive" in names:
        if "weave" in names:
            sites += weave_sites("smooth" in names)
        elif "native" in names:
            sites += native_sites("smooth" in names)
        else:
            sites += LINE_DOUBLE
    aspect = aspect_for("widescreen" in names, "progressive" in names, "native" in names)
    if aspect != ASPECT_ORIGINAL:
        sites.insert(0, Site(ASPECT, struct.pack("<f", ASPECT_ORIGINAL), struct.pack("<f", aspect),
                             f"dds3ScreenParam.aspect {ASPECT_ORIGINAL} -> {aspect:.4f}"))
    segment = None
    features = [name for name in DEBUG_FEATURES if name in names]
    if features:
        base = STATE_CODE_BASE if for_state else CODE_BASE
        debug, segment = debug_sites(features, base)
        sites += debug
        if not for_state:
            sites += heap_sites((base + segment.memsz + 63) & ~63)
    return sites, segment


def apply(read, write, sites):
    for site in sites:
        current = read(site.addr, len(site.new))
        if current == site.new:
            print(f"  already applied  0x{site.addr:08X}  {site.what}")
            continue
        original = hashlib.sha1(current).digest() == site.old if site.hashed else current == site.old
        if not original:
            shown = hashlib.sha1(current).hexdigest() if site.hashed else current.hex()
            raise SystemExit(f"0x{site.addr:08X} holds {shown}, expected {site.old.hex()}: "
                             f"not the NTSC-U build this patch is for")
        write(site.addr, site.new)
        print(f"  patched          0x{site.addr:08X}  {site.what}")


def patch_elf(data, sites, segment=None):
    if hashlib.md5(data).hexdigest() != ORIGINAL_MD5:
        print("warning: the input is not the retail SLUS_209.11, relying on the per-site checks")
    elf = Elf(data)
    if segment:
        add_segment(elf, segment)

    def read(addr, size):
        start = elf.offset(addr, size)
        return bytes(elf.data[start:start + size])

    def write(addr, value):
        start = elf.offset(addr, len(value))
        elf.data[start:start + len(value)] = value

    apply(read, write, sites)
    return bytes(elf.data)


def find_iso_file(iso, name):
    """Extent offset and size of a file in the ISO9660 root directory."""
    iso.seek(16 * 2048)
    pvd = iso.read(2048)
    if pvd[1:6] != b"CD001":
        raise SystemExit("not an ISO9660 image")
    root_lba, = struct.unpack_from("<I", pvd, 156 + 2)
    root_size, = struct.unpack_from("<I", pvd, 156 + 10)
    iso.seek(root_lba * 2048)
    directory = iso.read(root_size)
    pos = 0
    while pos < len(directory):
        length = directory[pos]
        if length == 0:
            pos = (pos // 2048 + 1) * 2048
            continue
        lba, = struct.unpack_from("<I", directory, pos + 2)
        size, = struct.unpack_from("<I", directory, pos + 10)
        ident = directory[pos + 33:pos + 33 + directory[pos + 32]].decode("ascii", "replace")
        if ident.split(";")[0] == name:
            return lba * 2048, size
        pos += length
    raise SystemExit(f"{name} not found in the image's root directory")


def patch_iso(source, target, sites, segment=None):
    with open(source, "rb") as iso:
        offset, size = find_iso_file(iso, ELF_NAME)
        iso.seek(offset)
        elf = iso.read(size)
    patched = patch_elf(elf, sites, segment)
    if len(patched) != size:
        raise SystemExit("patched ELF changed size")
    if source != target:
        print(f"copying {source} -> {target}")
        shutil.copyfile(source, target)
    with open(target, "r+b") as iso:
        iso.seek(offset)
        iso.write(patched)
    return patched


def read_state_entry(zf, info):
    """PCSX2 compresses savestate entries with zstd, which zipfile cannot read."""
    if info.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
        return zf.read(info)
    with open(zf.filename, "rb") as f:
        f.seek(info.header_offset)
        header = f.read(30)
        name_len, extra_len = struct.unpack_from("<HH", header, 26)
        f.seek(info.header_offset + 30 + name_len + extra_len)
        raw = f.read(info.compress_size)
    return subprocess.run(["zstd", "-dc"], input=raw, capture_output=True, check=True).stdout


def patch_state(source, target, sites, segment=None):
    with zipfile.ZipFile(source) as zf:
        entries = [(info.filename, read_state_entry(zf, info)) for info in zf.infolist()]
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=1) as zf:
        for name, data in entries:
            if name == "eeMemory.bin":
                ram = bytearray(data)

                def read(addr, size):
                    return bytes(ram[addr & 0x1FFFFFF:(addr & 0x1FFFFFF) + size])

                def write(addr, value):
                    ram[addr & 0x1FFFFFF:(addr & 0x1FFFFFF) + len(value)] = value

                apply(read, write, sites)
                if segment:
                    write(segment.base, segment.data + bytes(segment.memsz - len(segment.data)))
                data = bytes(ram)
            zf.writestr(name, data)
    with open(target, "wb") as f:
        f.write(out.getvalue())


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("input", help="SLUS_209.11, a disc image (.iso) or a savestate (.p2s)")
    parser.add_argument("output", help="where to write the patched copy")
    default = [name for name in GROUPS if name not in ("60fps", "smooth", "weave")]
    parser.add_argument("--only", nargs="+", metavar="GROUP", default=default,
                        help=f"patch groups to apply, from {' '.join(GROUPS)}; "
                             f"default {' '.join(default)}")
    parser.add_argument("--without", nargs="+", metavar="GROUP", default=[],
                        help="patch groups to leave out")
    args = parser.parse_args()

    names = [name for name in args.only if name not in args.without]
    lower = args.input.lower()
    sites, segment = select(names, lower.endswith(".p2s"))
    print(f"groups: {' '.join(names)}")
    if "60fps" in names:
        print("note: game logic is frame-locked, so 60fps also runs the game at twice its speed")
    if lower.endswith(".iso"):
        patched = patch_iso(args.input, args.output, sites, segment)
    elif lower.endswith(".p2s"):
        patch_state(args.input, args.output, sites, segment)
        return
    else:
        with open(args.input, "rb") as f:
            patched = patch_elf(f.read(), sites, segment)
        with open(args.output, "wb") as f:
            f.write(patched)
    crc = 0
    for (word,) in struct.iter_unpack("<I", patched[:len(patched) // 4 * 4]):
        crc ^= word
    print(f"{ELF_NAME}: md5 {hashlib.md5(patched).hexdigest()}, PCSX2 CRC {crc:08X}")


if __name__ == "__main__":
    sys.exit(main())
