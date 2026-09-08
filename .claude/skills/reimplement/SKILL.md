---
name: reimplement
description: Reimplement a guest function from the SLUS-20911 disassembly completely and provably, with every branch and field accounted for. Use when porting or auditing any sdf/dds3/fld/nb/cmp/fcl function, when a render defect points at one, or when checking that an existing reimplementation still matches its source.
---

# Reimplementing a guest function

A reimplementation that covers the branches that happened to fire is not a reimplementation, it is
a guess that has not been caught yet. The cost is not a wrong pixel, it is a **claim in a comment**
that stops the next reader looking: "both are inert at zero" was written about a field the engine
never reads, while the field it does read was set to 7 the whole time.

Work in this order. Do not skip step 2 because the decompiler output looks complete.

## 1. Read both the decompilation and the disassembly

`mcp__ida__decompile` and `mcp__ida__disasm`, on `SLUS_209.11`.

**A `decompile` of a large body can wedge the instance**, after which every query to that database
times out. Restart it rather than working around it: close the process, promote the newest
`SLUS_209.11_<date>.i64` snapshot to `SLUS_209.11.i64` if it is newer than the main file, move the
unpacked `.id0`/`.id1`/`.id2`/`.nam`/`.til` aside so IDA opens clean, and reopen with
`E:\Projects\PS2\IDB\open-idbs.ps1 -Filter SLUS_209.11`. Loading takes a few minutes and the MCP
port only appears at the end; clear `~/.cache/ida-mcp-instances.json` before looking for it.

The decompiler drops what it cannot type. COP2 (VU0 macro) shows up as `cop2 0x1E0032C` and the
whole body reads as if nothing happens: `corePktApplyFaceDivColor` looked like an empty loop and is
a two-color ramp. Inline `__asm` blocks hide `lqc2`/`sqc2` pairs that are the real data flow.

**If the decompilation contains `cop2`, `__asm`, or an unnamed `unk_`, the disassembly is the
source of truth for that region.**

## 2. Extract the facts, do not enumerate by hand

The checklist is machine-derived, so it cannot be quietly narrowed. Add the function to `TARGETS`
in `scripts/ida_facts.py` and run it in IDA; it rewrites `assets/SLUS-20911.facts` with one entry
per **distinct** target, every address it appears at listed:

- `read` / `write`, a struct and an offset, from the decompiler's ctree, which types the base
- `glob`, a data symbol
- `call`, a callee, or `indirect` with the vtable offset
- `blk`, a basic block: a branch says a decision exists, a block says the code in each arm was read

```bash
# in IDA, over MCP
mcp__ida__py_exec_file \\wsl.localhost\Debian\home\wsl\projects\sdfhook\scripts\ida_facts.py
scripts/guestcheck.py sdfModelTempPacketLinkOt
```

`sdfModelTempPacketLinkOt` has 81 facts. Reading it for "the node walk" gets about twenty.

**The extractor is as able to wedge the instance as `decompile` is**, and it holds the facts file
open while it does, so a hang leaves the file empty. Regenerate it after any restart before
trusting a score.

## 2a. Account for every fact

`scripts/guestcheck.py` holds the reimplementation to the list. A fact is satisfied by the code
itself wherever the tree already binds guest names to ours: the struct mirrors in `libsdf.h` resolve
an offset to a field, and the `pcsx2hook_find_symbol` calls resolve a global to its member. Only a
block needs a citation, in `vucheck`'s grammar, `0x2B1A2C..0x2B1A7C` or a bare address.

Anything left is written down at the place the guest does it, never in a list at the top, because
**citations and opt-outs are order-checked against the guest**: reading the guest out of order is
either a comment in the wrong place or logic that moved. That guard is what found our group-array
test sitting after the transform where the engine does it before.

```cpp
// @optout 0x2B1954 packetbuf: sdfFrameId picks which of the two per-frame packets to build
```

and, where the region is several named calls rather than one fact, the call list itself, which is
what a reviewer auditing the migration boundary wants to read:

```cpp
#if 0  // @optout 0x2B1B88..0x2B1C53 packetbuf: we carry the transform on the draw instead
    tag = sdfPrimAllocDrawTag(0);
    packet = sdfGrAllocPacketBuff(0xC0);
    sdf3DPrimSetTmPacket(packet + 0x10);
    packet[0x98] = attrMask | (corePktSoftenBias << 16) | 0x6102C000;
    sdfDrawTagLink2(tag, packet, packet + 0x90);
#endif
```

**The class is a closed set and every member names hardware**: `dma`, `vif`, `gif`, `gs`,
`packetbuf`, `enginestate`, `abi`, `hooked`, `elsewhere(<fn>)`. If you cannot name which hardware,
it is in scope. **There is no class for "not yet"**: an unfinished fact fails the check and belongs
in TODO.md, not in a comment that reads as finished.

Three rules keep the annotations honest, and the third is the one that matters over years:

1. a fact neither satisfied nor opted out fails,
2. an opt-out citing an address that is not a fact fails: the database moved, or the code did,
3. a fact both satisfied and opted out fails: you modelled it, so delete the excuse.

**A span covers control flow only.** It says a region of the guest is hardware we replace, so it
binds the blocks inside it; the reads, globals and calls each name their own address. Otherwise
drawing a box round a region silences the data it carries, which is how an opt-out over the
material REF packet came to swallow the `matIndex` we do model. `elsewhere(<fn>)` is the exception,
since it claims the reads are modelled in the function it names rather than skipped.

## 2b. Read the score as two numbers

`guestcheck.py` reports data facts and blocks apart, because a citation says the code was read and
a data fact says the value was modelled, and one must never stand in for the other. `sdfModelLinkOt`
scores 22 of 50 data facts with no annotation at all: that is the baseline name-matching gives you
for free.

**The check pushes toward more reimplementation, not less.** Two of its failures are work, not
paperwork:

- `no offsetof assert for corePktCb_t+0x44` means we do not mirror the struct. Thirty-one such
  facts across `sdfCorePacketGen`, `corePktBuildFaceList` and `corePktEmitMultiTexPass` are one
  missing mirror. There is no struct-level opt-out and there should not be one.
- `unnamed in the database` means the IDB has `flt_54C1E8` where a name belongs. Name it, re-export,
  and it becomes a fact you can satisfy. See step 8.

## 3. Casts and widths are load bearing

`attr_mask = (s16)drawAttr & node->drawAttr` sign-extends: `0xFFFF` becomes `0xFFFFFFFF` and the
node's high bits survive. Written as `u16(...)` it truncates them, and the soften bias lives at bit
16. The C looks equivalent and is not.

Read the width off the disassembly (`lh` against `lhu`, `lb` against `lbu`), not off the
decompiler's chosen type.

## 4. Check the siblings

A field usually has a neighbour, and a function usually has a variant. Both get read.

- `sdfModelLinkOt` takes `faceDivBias`; `sdfModelTempPacketLinkOt` takes `faceDivSoften`. Two
  functions, two globals, and checking one and reporting on "face division" is wrong.
- `sdfDrawTagLink` has `Link2`, `LinkRef`, `LinkRefGif` and `LinkCall`.
- A `getX` next to a `setX` says what the field means.

`mcp__ida__xrefs_to` on the function answers "who calls this and what gates it". That is how
`corePktApplyFaceDivColor` was found to belong to `corePktEmitMultiTexPass` rather than to VU1.

## 5. Never write "zero", "inert" or "unused" without a runtime measurement

A value is only inert if a run says so. Put the number in the summary, not an adjective in a
comment. `reportUnsupported(what, value)` prints once per distinct value and totals into the run
summary, so a scene that sets the field says so by itself.

**The report is an alarm, not a licence.** Its fact stays unaccounted in `guestcheck.py` until the
value is modelled, which is the difference between measuring a gap and closing one.

Report **where the field is read**, not where it is convenient. A check in the wrong function is a
check that never fires.

Instruments for actually measuring:

- `SDF_LOG=<channel>=info` plus a `log.info` at the site, for a one-off count.
- `PCSX2HOOK_VU_WATCH="920,921,..."` dumps VU1 data memory per batch as `vu1_watch.jsonl`. This is
  ground truth for anything VU1 reads, and it confirmed the whole lighting sum in one run.
- `PCSX2HOOK_VU_EXEC_TRACE=<program>` writes `vu1_exec.jsonl` for `vu2c.py --exec`.
- `PCSX2HOOK_GS_TRACE` carries each batch's resolved registers **and its post-VU vertex color**,
  which is the output of whatever you are reimplementing.

## 6. Attribute every piece

Per CLAUDE.md, and it is what makes step 2 checkable later:

```cpp
/// Walks the node list the engine walks, so what it files into an ordering table we turn into
/// meshes instead.
/// @fn sdfModelLinkOt
/// @note SLUS-20911 0x002B1DB8
```

and the hex offset inline where the constant or the branch is the hard part:
`// 0x002B1E44: flags & 0x11 hides the node`.

## 7. Make it greppable

A function named for a guest symbol carries `@fn` and `@note` above its definition, and
`scripts/attrcheck.py` fails the build's check if it does not:

```bash
scripts/attrcheck.py            # every function whose name is in the map needs both tags
```

That makes `grep -n "@fn" src/*.cpp` the index of what has been reimplemented, and each entry
names the address to recheck it against. Eight functions in `ModelParser` are attributed this way.

**A helper that is not a reimplementation says so.** `readRigidBatches`, `readWeightedBatches`,
`readBlendShapes`, `readVifStreamMesh`, `walkChain`, `finishMesh`, `readSecondUv`, `readLighting`,
`readFog` and `readMaterialPacket` are ours: they decode a format or split up a larger engine
function, and no single symbol corresponds. Where one splits a named function, say which in the
comment (`walkChain` is `sdfModelLinkOt`'s inner loop) so the pair can still be rechecked together.
Silence is the thing to avoid: a reader cannot tell an unattributed reimplementation from a helper.

## 8. Correct the database

Anything proven wrong in the IDB gets fixed there in the same sitting, with the evidence and the
old value in a comment, then `scripts/export_symbols.py`. The IDB outlives the session.

`guestcheck.py` routes work here on purpose. An auto-named global is not skipped, it fails as
`unnamed in the database`, and the only way to clear it is to name the symbol and re-export.
`fldSdwDraw` alone carries about forty. A `@note` that disagrees with the map is the same kind of
error: `dds3BillboardLinkOt` is at 0x0015B7C0, not the 0x0022E2E8 two of our comments claim.

## Done means

- `scripts/guestcheck.py <fn>` reports every fact accounted for, and every opt-out names hardware.
  A `reportUnsupported` is the alarm that a scene sets a field we skip; it does **not** discharge
  the fact, which stays failing until it is modelled.
- `scripts/lint.sh`, `scripts/attrcheck.py` and `scripts/guestcheck.py` all print nothing to fix.
- `scripts/refdiff_all.sh` has been run and the numbers are in the TODO.md entry, including the
  ones that did not move.
- The claim you are making is a measurement, and the entry says how it was measured.
