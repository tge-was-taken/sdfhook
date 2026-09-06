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

The decompiler drops what it cannot type. COP2 (VU0 macro) shows up as `cop2 0x1E0032C` and the
whole body reads as if nothing happens: `corePktApplyFaceDivColor` looked like an empty loop and is
a two-colour ramp. Inline `__asm` blocks hide `lqc2`/`sqc2` pairs that are the real data flow.

**If the decompilation contains `cop2`, `__asm`, or an unnamed `unk_`, the disassembly is the
source of truth for that region.**

## 2. Enumerate before you write

Write the checklist first, in the TODO.md entry or the commit message. One line per:

- **every branch**, including the else that does nothing
- **every field read** off every struct, with its offset
- **every global read**, by symbol
- **every call out**, and whether it is modelled or hooked
- **every cast**, with its width and signedness

Then mark each: modelled / deliberately not modelled / not yet. Nothing is unmarked.

`sdfModelLinkOt` has 23 such items. Reading it for "the node walk" gets about eight of them.

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

Report **where the field is read**, not where it is convenient. A check in the wrong function is a
check that never fires.

Instruments for actually measuring:

- `SDF_LOG=<channel>=info` plus a `log.info` at the site, for a one-off count.
- `PCSX2HOOK_VU_WATCH="920,921,..."` dumps VU1 data memory per batch as `vu1_watch.jsonl`. This is
  ground truth for anything VU1 reads, and it confirmed the whole lighting sum in one run.
- `PCSX2HOOK_VU_EXEC_TRACE=<program>` writes `vu1_exec.jsonl` for `vu2c.py --exec`.
- `PCSX2HOOK_GS_TRACE` carries each batch's resolved registers **and its post-VU vertex colour**,
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

## 7. Correct the database

Anything proven wrong in the IDB gets fixed there in the same sitting, with the evidence and the
old value in a comment, then `scripts/export_symbols.py`. The IDB outlives the session.

## Done means

- Every item from step 2 is marked, and the not-modelled ones each `reportUnsupported` at their
  own read site.
- `scripts/lint.sh` prints nothing.
- `scripts/refdiff_all.sh` has been run and the numbers are in the TODO.md entry, including the
  ones that did not move.
- The claim you are making is a measurement, and the entry says how it was measured.
