---
name: vu-translate
description: Translate SDF's VU1 microcode into an annotated transcription with full attribution, using vu2c.py with the instruction and memory traces. Use when reading VU1 to answer what a draw computes, when writing or updating anything in dumps/vu1/annotated/, or when a shading result has to be checked against the microcode.
---

# Translating VU1

The output of this is a **transcription**, not a summary. A summary of microcode is how a gap
survives: `annotated/lighting.c` described the sum at `0x0158..0x015B` and then the clamp at
`0x0172`, and the twenty-two instructions between them, which are the second half of a
two-vertices-at-a-time unroll, were simply not there. Nobody could see they were missing, because
prose has no address column.

**Every instruction in the range you claim is accounted for.** A gap is a defect, not a shortening.

## 1. Capture

```bash
PCSX2HOOK_VU_TRACE=1 \
PCSX2HOOK_DUMP_DIR=dumps/scratch/vu \
PCSX2HOOK_STATE=states/<scene>.p2s ./build/bin/sdfhook "$SDF_ISO"
```

gives `vu1_images.jsonl` (the uploaded micro memory) and `vu1_programs.jsonl` (what MSCAL reached).
The **image** is the unit worth reading: SDF's MSCAL entries are dispatch stubs (`XITOP`, `IADDIU`,
`JR`) and the bodies they reach never appear as a program of their own.

Add data to it. Both are worth the disc space and neither is on by default:

```bash
# Per instruction, with vf[32], vi[32] and ACC at every step. Needs the interpreter: the
# recompiler never calls back, so without INTERP the file is written and stays empty.
PCSX2HOOK_VU1_INTERP=1 PCSX2HOOK_VU_EXEC_TRACE=<hash prefix>   -> vu1_exec.jsonl

# The quadwords at named VU1 data addresses, per batch.
PCSX2HOOK_VU_WATCH="920,921,922,923,924,925,926,927,928,1002,1003,1004,1005"  -> vu1_watch.jsonl
```

`PCSX2HOOK_VU_EXEC_TRACE` matches a **prefix of the program hash**, uppercase, from `--list`.
A value that matches nothing (`=1`) records nothing and looks like the feature is broken.

Keep captures in `dumps/scratch/`; only a curated result belongs in `dumps/vu1/`.

## 2. Check the decode before trusting a mnemonic

```bash
vu2c.py dumps/vu1/vu1_images.jsonl --check     # re-derives every mnemonic against PCSX2's own
vu2c.py dumps/vu1/vu1_images.jsonl --list      # hash, range, instruction count
```

`--check` reporting 0 mismatches over 1884 instructions is what makes the listing quotable.

## 3. Translate with the data attached

```bash
vu2c.py <images> --exec vu1_exec.jsonl --mem vu1_watch.jsonl --clean --flow
vu2c.py <images> --exec vu1_exec.jsonl --only-ran        # drop what never executed
vu2c.py <images> --exec vu1_exec.jsonl --trace 0         # one run, in execution order
```

- `--mem` substitutes the **recorded quadwords** into loads from those addresses, so `LQ mem[923]`
  reads as the value it actually held. This is what turns "the base term" into `(0.2969, 0.1953,
  0.1953)` without inferring anything.
- `--exec` annotates branch counts and real targets, so an `IBNE` stops being a guess about which
  way it goes, and `--only-ran` separates a path that is dead here from one that is dead.
- `--instance N` picks which sample of a program the memory comes from.

A branch you have not seen counted is a branch you are guessing at. Say which it is.

## 4. Write the transcription

Into `dumps/vu1/annotated/<routine>.c`, and it must satisfy all of:

- **Complete over its stated range.** Say the range in the header comment and cover every address
  in it. If a region is genuinely uninteresting, transcribe it and say so on the line; do not omit
  it.
- **Every line carries its VU address**, as `/* 0158 */`, so any line can be put beside the raw
  listing. This is the attribution that makes the file checkable rather than believable.
- **Delay slots are put back in program order**, and the header says that was done. vu2c prints
  them where they execute, which is not where they are written.
- **Unrolls are written as the loop they are**, with the factor named. SDF's vertex loop does two
  at a time; transcribing it straight doubles the apparent work and hides the tail.
- **Every register read is traced to where it was written**, in the same file or named in the
  header. An unexplained `vf22` is a hole.
- **Fixed memory addresses use the names from `annotated/memory-map.md`**, and any new name is
  added there with what proves it.

Header block, every time, and the ranges go under a line saying `Ranges covered here` so
`scripts/vucheck.py` can read them:

```c
/* <routine>, 0x0132..0x0172 of dumps/vu1/vu1_image.c (SLUS-20911).
 * Cleaned up by hand: delay slots in program order, the two-at-a-time unroll folded, fixed
 * addresses named per memory-map.md. Captured on <scene>; verified with --check (0 mismatches).
 * The raw listing is the authority; this is for reading. */
```

## 5. Verify against data, not against reading

The microcode says what is computed; the traces say what came out. Both, before it is written down:

- `PCSX2HOOK_VU_WATCH` on the inputs. This settled the whole lighting sum in one run after a dozen
  measurements against the picture had not.
- The **GS trace's post-VU vertex colour** (`rgbaq`, `r`/`g`/`b_`) is the output of the routine.
  `color[3] * 128` appearing as a literal dominant batch colour is proof; a screenshot is not.

## Not VU1

COP2 in EE code (`cop2 0x1E0032C`, `vmulaw.xyzw`, `lqc2`/`sqc2` around a `__asm` block) is VU0
macro mode running on the EE, and none of the above reaches it. It belongs to the `reimplement`
skill, read from `mcp__ida__disasm`. `corePktApplyFaceDivColor` is the case that matters: the
decompiler renders it as an empty loop, the disassembly shows a two-colour ramp, and no amount of
VU1 reading would ever have found it.

## The gap check is mechanical

```bash
scripts/vucheck.py dumps/vu1/annotated/lighting.c
```

reads the ranges the header declares, expands every span the body cites, and prints any address in
between that the transcription never mentions. It ignores a span that merely restates a declared
range, because a routine's own header comment is not a transcription of it. Run it before calling a
transcription done; it found four unaccounted addresses in `lighting.c` the moment it existed.

## Done means

- `scripts/vucheck.py` reports every declared range complete.
- `--check` was run and is quoted.
- Every claim about a value is backed by a watch or GS-trace number, in the file or in TODO.md.
- `dumps/vu1/README.md` points at the new file and says how to regenerate the raw listing.
