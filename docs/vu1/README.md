# VU1 microcode

`docs/vu1/vu1_image.c` is the pseudo-C of the one program SDF uploads, from `vu1_images.jsonl`
through `scripts/vu2c.py`. Captured on `smt3_field_unknown_realm`; the image is the same in every
scene, since SDF uploads one and dispatches inside it.

`scripts/vu2c.py` is pcsx2-hook's, copied in so this directory regenerates from a checkout of this
repo alone; it needs nothing but the standard library. pcsx2-hook has the maintained one.

Regenerate with:

    PCSX2HOOK_VU_TRACE=1 PCSX2HOOK_DUMP_DIR=dumps/vu1 ./build/bin/sdfhook "$SDF_ISO"
    python3 scripts/vu2c.py docs/vu1/vu1_images.jsonl \
        --clean --flow --keep-nops --names docs/vu1/vu1_names.txt > docs/vu1/vu1_image.c

The command above reproduces the checked-in `vu1_image.c` byte for byte, and
`scripts/vu2c.py --selftest` passes 32 cases.

**`--keep-nops` is not optional.** Without it 81 of the 1884 addresses are omitted, every range in
every transcription reads as incomplete, and `vucheck.py` reports gaps that are not there.
`--clean` folds matrix multiplies, cross products, constants, clamps and pointer steps, and `--flow`
shapes the branches into loops, if/else and calls; both keep the instruction words, so the file
stays checkable, and a folded instruction prints where it went rather than as a bare nop.
`--names docs/vu1/vu1_names.txt` names the fixed addresses and the routines, emits a `#define` for
each, and prints a `subroutine(f, ...)` banner over every routine entry listing what a caller has to
set. **That banner is the check for a transcription's own account of its inputs**, with one caveat:
`mac` and `clip` appear there whenever a flag-reading instruction comes before the arithmetic that
sets the flags, which the linear scan cannot see, so they are usually a scan artifact rather than a
caller obligation.

Other options worth knowing: `--audit` prints the statements each fold replaced, `--split DIR`
writes one file per program, and `--no-asm` / `--no-addr` / `--no-preamble` strip the columns.

`--names` prints `mem[1003]` as `lightBase` and `mem[vi04 + 848]` as `texcoord[vi04]`, from
`vu1_names.txt`, which restates the rows of `memory-map.md`. Each name is a `#define` at the top of
the file for the subscript it stands for, so the address is never further away than the definition
and the file still compiles clean under `gcc -fsyntax-only -Wall`. A lane read both as a float and
as an integer, `mem[914].w`, keeps its subscript, and the header says which.

`--clean` also renames a register to the location it was loaded from while it holds it, so
`vi12 = indexList` and the reads after it say `indexList_vi12` until 0x0271 loads the register from
somewhere else. A step keeps the name and so does an update from itself, which is why 0x0148 reads
`lightBase_vf22.xyz = ACC.xyz + lightBase_vf22.xyz * vf03.w`. A name is printed only where the
register is read under it more than once. The header lists the fixed addresses **nothing** names,
ranked by uses, which is the worklist for `memory-map.md`.

`--flow` writes each routine entry as `subroutine(clipPlane, mac, vf13, vi06, vi07, vi10,
vi11);`, the name followed by the registers it reads before writing them, and a `BAL` to it as
`clipPlane()`. One define in the preamble declares the name and drops the rest, so the file still
parses. A map in the header gives each routine's extent, returns, callees, second entries and what
a call to it writes. `vu1_names.txt` names them with `@<entry> <name>` in hex, taken from the
`Ranges covered here` headers of the files below. Only 8 of the 30 call targets are
function-shaped; the map says which, and why each of the others is not. The extents agree with the ranges the files below declare, `0x056E..0x05C9` for
`clipPlane` among them, so take a new range from the map rather than deciding it by eye. A routine
marked **NOT complete** has an exit the walk could not follow and its body is a lower bound.

`vu1_names.txt` also names records: `!00C2 vi02 matPacket` says `unpackTexture` is handed a
pointer in vi02, so `mem[vi02 + 3]` prints as `matPacket[3]` with the field name on the line, only
inside that routine.

The listing should compile: `gcc -fsyntax-only -std=c11 -Wall docs/vu1/vu1_image.c` silent. That is
a real check, not tidiness, it has caught a helper with the wrong return type and an unclosed
comment.

To check a folded line, drop options: `vu2c.py <images>` alone prints one statement per instruction
with nothing folded, which is what to diff against. `--audit` prints the statements each fold
replaced and where every substituted value was written. `vu2c.py --selftest` runs each pass against
the shape it folds and against the near miss it has to refuse.

## The lighting, at 0x0150..0x015F

    0x0150  vf03 = M * normal          // M's columns are the three light directions
    0x0156  vf05 = max(vf03, 0)        // each dot clamped at zero
    0x0158  ACC  = vf13 * 1            // the base
    0x0159  ACC += vf10 * vf05.x       // light color 0 times its dot
    0x015A  ACC += vf11 * vf05.y
    0x015B  vf05 = ACC + vf12 * vf05.z
    0x015F  vf05 = min(vf05, 255.0)    // clamped once, per vertex

`vf12` and `vf13` are built at 0x014E and 0x014F as `light color * vf15`, so the per-draw color
multiplies every term before the sum, exactly as the renderer does. This is the reference for
TODO.md's V5: the formula is settled, and what remains there is which packet a root resolves to.

## The base is not `colorBase`, at 0x01BB..0x01C5

Run once per material (a flag at `mem_w[917]`, set when a material is uploaded at 0x00A8 and
cleared here), the microcode rewrites two of the values the sum above reads:

    mem[1003] = mem[1004] * mem[927]                        // base    = ambient  * light color 3
    mem[924]  = mem[1002] * mem[923] - mem[1004] * mem[927] // light 0 = diffuse * colorBase - base

`mem[923]` is the packet's `colorBase` and `mem[924..928]` its `color[0..4]`, which independently
confirms the struct offsets: `colorBase` at +64 and `color` at +80.

So light 0's color is a **difference**, not a color. A vertex facing light 0 reaches
`diffuse * colorBase` and one facing away falls to `ambient * color[3]`, which is the harsh front
light with a dark back. The packet's `color[0]` is scratch and never survives.

`mem[1002]` and `mem[1004]` are the material packet's `diffuse` and `ambient`, loaded from its VIF
stream at 0x0090, `itof0` then scaled by `I = 1/128` at 0x0099. 0x009E folds `diffuse` into the draw
color as well, so it scales every term of the sum.

There is also a flatten factor at `mem[910].w` (0x0146, 0x0148): it lerps the base toward 1.0 and
scales all three light colors by `1 - t`. Not modelled; measure it before assuming it is zero.

## The annotated transcription

`docs/vu1/vu1_annotated.c` is the whole image written out by hand: **all 1884 instructions,
0x0000..0x075B, in 77 routines, with no gaps**. Values are named for what they hold, a routine takes
what it needs as arguments, and every line carries the addresses it came from. It compiles under
`gcc -fsyntax-only -x c`, so nothing in it is undeclared.

The listing stays the authority. `scripts/vucheck.py docs/vu1/vu1_annotated.c` keeps the two in
step: it reports any address in a declared range the transcription never mentions, and any
instruction it names at an address that does not have it. Both must be empty.

Three things the transcription settled that nothing else in the tree states:

- **mem[800 + v] is the vertex color and mem[848 + v].xy the texture coordinate.** The lighting
  routines clamp the first to 255 and the transform divides the second by w; `emitTexTriangles`
  emits them as three registers a vertex, [ST, RGBA, XYZ], which is what pins the pair.
- **Fog is computed once a vertex in the transform**, `fog.z + fog.w / w` clamped to [fog.x, fog.y],
  and carried in mem[848 + v].w. That is the offset-plus-reciprocal form the GS trace implied.
- **mem[907] is the GS PRIM word's flag bits**, `attrBits & 0x78`. Every packet builder adds a
  primitive type to it and writes it as A+D register 0, and the +512 the second passes add is
  PRIM's CTXT bit, so those passes draw through GS context 2.

And one correction to a name that was wrong: **mem[1007] is a color, not a texture size.** All
seven of its uses multiply a color by it (0x009E, 0x00F4, 0x0113, 0x0445, 0x05E2 among them) and
none scales a UV, so `vu1_names.txt` calls it `drawTint`.

## Checking a transcription

`scripts/vucheck.py docs/vu1/vu1_annotated.c docs/vu1/vu1_image.c` reports any address in the ranges
the header declares that the transcription never mentions, and any instruction it cites at an
address the listing does not have there. A gap or a miscite is a defect: see
`.claude/skills/vu-translate`.
