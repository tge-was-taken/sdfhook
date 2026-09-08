---
name: vu-translate
description: Translate SDF's VU1 microcode into an annotated transcription with full attribution, using vu2c.py with the instruction and memory traces. Use when reading VU1 to answer what a draw computes, when writing or updating anything in docs/vu1/, or when a shading result has to be checked against the microcode.
---

# Translating VU1

The output of this is a **transcription**, not a summary. A summary of microcode is how a gap
survives: `docs/vu1/lighting.c` described the sum at `0x0158..0x015B` and then the clamp at
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

Keep captures in `dumps/scratch/`; only a curated result belongs in `docs/vu1/`.

## 2. Check the decode before trusting a mnemonic

```bash
vu2c.py docs/vu1/vu1_images.jsonl --check     # re-derives every mnemonic against PCSX2's own
vu2c.py docs/vu1/vu1_images.jsonl --list      # hash, range, instruction count
vu2c.py --selftest                            # every simplification against the shape it claims
```

`--check` reporting 0 mismatches over 1884 instructions is what makes the listing quotable.
`--selftest` is what makes the simplified form quotable: each pass is run against the shape it
folds and against the near miss it has to refuse.

## 3. Translate with the data attached

```bash
vu2c.py <images> --exec vu1_exec.jsonl --mem vu1_watch.jsonl --simplify all \
        --names docs/vu1/vu1_names.txt
vu2c.py <images> --exec vu1_exec.jsonl --only-ran        # drop what never executed
vu2c.py <images> --exec vu1_exec.jsonl --trace 0         # one run, in execution order
```

- `--mem` substitutes the **recorded quadwords** into loads from those addresses, so `LQ mem[923]`
  reads as the value it actually held. This is what turns "the base term" into `(0.2969, 0.1953,
  0.1953)` without inferring anything.
- `--exec` annotates branch counts and real targets, so an `IBNE` stops being a guess about which
  way it goes, and `--only-ran` separates a path that is dead here from one that is dead.
- `--instance N` picks which sample of a program the memory comes from.
- `--names docs/vu1/vu1_names.txt` prints the names `memory-map.md` proves, so `mem[1003]` reads
  as `lightBase` and `mem[vi04 + 848]` as `texcoord[vi04]`. Each becomes a `#define` for the
  subscript it stands for, so the file still compiles and the address is one line away. Its header
  lists the fixed addresses **nothing** names, by how often the code reaches them, which is the
  worklist for `memory-map.md`.
- **Records.** `!<entry> <register> <name>` says a routine is handed a pointer in that register,
  and `<name>.<offset> <field>` names a quadword of it, so `mem[vi02 + 3]` inside `unpackTexture`
  reads as `matPacket[3]` with the field on the line. Scoped to that routine's body, since the
  register is only that pointer inside it.

A branch you have not seen counted is a branch you are guessing at. Say which it is.

### The simplifications, and what each one is worth

Nothing is folded unless asked for. `--simplify all` turns on every pass below; `--simplify
const,mat` takes two; `--no-simplify join` drops one from a group. `vu2c.py` with no options
prints one statement per instruction, which is the form to diff against when a fold looks wrong.

| pass | what it recovers |
|---|---|
| `const` | `vi01 = vi03 & 128` instead of `& vi01`, `1.0f - vf02.w`, `min(v, 255.0f)`, and a dispatch `JR` resolved to `goto L_0708` |
| `join` | the same across a label, when every path into it carries the value. This is what turns `vi01 = vi06 & vi01` at 0x0148 into `& 2048` |
| `alias` | a register loaded from a named location prints under that name while it holds it: `indexList_vi12 = indexList`, then `mem_w[indexList_vi12].x`, until 0x0271 loads it from somewhere else and it becomes `retryHead_vi12`. A step that walks the value keeps the name, and so does an update from itself, so 0x0148 reads `lightBase_vf22.xyz = ACC.xyz + lightBase_vf22.xyz * vf03.w`, which is lighting.c's `base = t + base * keep`. A name is printed only where the register is read under it more than once |
| `call` | a `BAL` prints as `clipPlane()`, and each routine entry as `subroutine(clipPlane, mac, vf13, vi06, vi07, vi10, vi11);` — the name, then the registers it reads before writing them. One define in the preamble declares the name and drops the rest, so it parses. The map above gives extent, returns, callees, second entries and what a call writes; that last is what lets a name or a constant survive a `BAL`, since `clipReject` at 0x04A8 writes `vi01` and nothing else |
| `acc` `mat` | an ACC chain as one expression, and the matrix form as `mul3x3`/`mul4x4` |
| `cross` | the OPMULA/OPMSUB pair as `cross(a, b)` |
| `ptr` `clamp` | `mem[vi04++]`, and an adjacent MAX/MINI as `clamp` |
| `flow` `else` | `do`/`while`, `if`, `break`, and the two arms of a branch as `if`/`else` |
| `loops` | a comment naming each loop's counter and step, which is how an unroll factor is read off rather than guessed |

**Every fold is checkable on its own line**: the instruction words stay in the address column, a
folded-away upper half says which line it moved to, and `--audit` prints the statements a fold
replaced and where each substituted value was written. Use `--audit` before quoting a folded line
in a transcription.

A simplification is still a reading. **The unsimplified listing is the authority**: where the two
disagree, the raw form wins and the pass is the bug.

### The routine map is where a transcription's ranges come from

`--simplify call` prints one entry per routine with its extent, and those extents agree with the
ranges the files here already declare: `0x056E..0x05C9` is `clip.c`'s `clipPlane` exactly, and
`0x043A`, `0x04B7`, `0x031C`, `0x0329` and `0x033C` each come out as the transcriptions have them.
Take a new range from the map rather than deciding it by eye.

Name the routines in `vu1_names.txt` with `@<entry> <name>`, hex, and the calls, banners and map
all use the name. The 77 already there were taken from the `Ranges covered here` headers of the
files in `docs/vu1/`, so the listing and the transcriptions agree by construction.

**Only 8 of the 30 call targets are function-shaped**, and the map says which and why. SDF's code
mostly is not: it enters one body at several points to skip work, falls from one routine into the
next, and transfers away without coming back. `clipPlane` and `copyBlock` are functions and the
transcriptions write them as `static void`; `transformVerticesUv` is not, and a transcription that
gives it a function body is claiming something the code does not do.

Two more things the map says that are worth reading before trusting a routine:

- **NOT complete** means the walk hit a jump it could not follow, so the body is a lower bound.
  Nothing may be concluded from what such a routine does or does not write.
- **also entered at** means SDF enters the same code at several points to skip work. Those are not
  separate routines, and a transcription that treats them as such will double-count.

## 4. Write the transcription

Into `docs/vu1/<routine>.c`, and it must satisfy all of:

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
- **Fixed memory addresses use the names from `docs/vu1/memory-map.md`**, and any new name is
  added there with what proves it.

Header block, every time, and the ranges go under a line saying `Ranges covered here` so
`scripts/vucheck.py` can read them:

```c
/* <routine>, 0x0132..0x0172 of docs/vu1/vu1_image.c (SLUS-20911).
 * Cleaned up by hand: delay slots in program order, the two-at-a-time unroll folded, fixed
 * addresses named per memory-map.md. Captured on <scene>; verified with --check (0 mismatches).
 * The raw listing is the authority; this is for reading. */
```

## 5. Verify against data, not against reading

The microcode says what is computed; the traces say what came out. Both, before it is written down:

- `PCSX2HOOK_VU_WATCH` on the inputs. This settled the whole lighting sum in one run after a dozen
  measurements against the picture had not.
- The **GS trace's post-VU vertex color** (`rgbaq`, `r`/`g`/`b_`) is the output of the routine.
  `color[3] * 128` appearing as a literal dominant batch color is proof; a screenshot is not.

## Not VU1

COP2 in EE code (`cop2 0x1E0032C`, `vmulaw.xyzw`, `lqc2`/`sqc2` around a `__asm` block) is VU0
macro mode running on the EE, and none of the above reaches it. It belongs to the `reimplement`
skill, read from `mcp__ida__disasm`. `corePktApplyFaceDivColor` is the case that matters: the
decompiler renders it as an empty loop, the disassembly shows a two-color ramp, and no amount of
VU1 reading would ever have found it.

## The gap check is mechanical

```bash
scripts/vucheck.py docs/vu1/lighting.c
```

reads the ranges the header declares, expands every span the body cites, and prints any address in
between that the transcription never mentions. It also checks every instruction the body **names**
at an address against the listing, over 2000 citations across `docs/vu1/`, so a `0x0263 B` that is
not a `B` is a defect rather than a detail nobody rereads. It ignores a span that merely restates
a declared range, because a routine's own header comment is not a transcription of it. Run it before calling a
transcription done; it found four unaccounted addresses in `lighting.c` the moment it existed.

## Done means

- `scripts/vucheck.py` reports every declared range complete and no citation miscited.
- `--check` was run and is quoted, and `--selftest` passes.
- The listing compiles: `gcc -fsyntax-only -std=c11 -Wall docs/vu1/vu1_image.c` is silent. This is
  not pedantry, it caught a helper declared to return the wrong type and a comment left unclosed.
- Every claim about a value is backed by a watch or GS-trace number, in the file or in TODO.md.
- `docs/vu1/README.md` points at the new file and says how to regenerate the raw listing.
