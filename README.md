# sdfhook

A native renderer for SDF, the engine SMT3 Nocturne (SLUS-20911) is built on. SDF is Atlus's own
library, statically linked into the ELF, so there is no middleware boundary to cut at the way
RenderWare gives one; the cut is at `sdfModelLinkOt` instead.

Nothing is reimplemented yet. This phase hooks the draw path and records what it does, so the data
flow is known before any of it is replaced.

## Build

```sh
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=<deps-prefix>
cmake --build build
```

`PCSX2HOOK_TREE` points at the runner and defaults to `vendor/pcsx2-hook`. The deps prefix is the one
the emulator's own dependencies were installed into; without it the configure fails looking for
plutovg. Qt is switched off here, so nothing needs a Qt6 install.

## Run

```sh
SDF_BIOS=<bios.bin> SDF_TRACE=dumps/sdf_trace.jsonl SDF_FRAMES=400 \
	./build/bin/sdfhook <nocturne.iso>
```

| variable | what |
|---|---|
| `SDF_BIOS` | BIOS image, full path |
| `SDF_TRACE` | where the JSONL trace goes |
| `SDF_FRAMES` | stop after this many frames |
| `SDF_SYMBOLS` | optional symbol map, a file or a directory holding `<ELF>.map` |
| `SDF_WINDOW` | open the native window and draw what the hooks decode |
| `SDF_SHOT_FRAME` | frame to capture at |
| `SDF_SHOT` | emulator framebuffer, PPM |
| `SDF_NATIVE_SHOT` | the native render, PPM |
| `SDF_SCALE` | window size as a multiple of the guest's 512x448; 1 to compare against the reference |
| `SDF_LOG` | log levels, one for everything (`trace`) or per channel (`model=trace,render=warn`) |
| `SDF_DUMP_TEXTURES` | directory for decoded textures as PNG; GS-recovered ones are named `gs_*` |
| `SDF_NO_GS_RECOVER` | stop reading textures back out of GS memory when the game registered none |

The window never takes keyboard focus, which needs the X11 backend: a Wayland toplevel cannot refuse
focus, so `SDL_WINDOW_NOT_FOCUSABLE` does nothing there. `x11,wayland` is set as a default only, so
`SDL_VIDEO_DRIVER` in the environment still wins.

`scripts/lint.sh` names every function nothing calls, which is how code written for a purpose and
never wired up gets found. It should print nothing.

`scripts/gsdiff.py <trace.jsonl> <gs.jsonl>` diffs the GS state we believe each draw runs under
against the state the hardware ran; run with `SDF_TRACE` and `PCSX2HOOK_GS_TRACE` both set.

Log channels: `run`, `hooks`, `model`, `tex`, `render`, `trace`, `todo`, `gs`, `packet`,
`unsupported`. Levels are `trace`, `debug`, `info`, `warn`, `error`, `off`, and the default is
`info`.

Everything the runner itself takes (`PCSX2HOOK_STATE` to boot a savestate, its own tracers) still
applies.

## What it traces

One JSONL record per event, in the order the game produced them, keyed by frame and a sequence
number. The order and the nesting are the point: which roots draw under which camera, what each
root's nodes emit, and which of the four ordering tables the result lands in.

| event | from | says |
|---|---|---|
| `frame` | the run loop | a frame boundary |
| `camera` | `sdf3DSetCamera` | the view the following draws use |
| `modelLinkOt` | `sdfModelLinkOt` | a model root: flags, drawAttr, colour, node count, its four OTs |
| `packetGen` | `sdfCorePacketGen` | the readable geometry path, kind 3 |
| `shapedPacket` | `sdfModelLinkShapedDrawPacket` | the preprocessed path, kind 2 |
| `otRegist` | `sdfSortOtAvlRegist` | a draw tag filed into an ordering table, with its z |
| `reqDraw` | `sdfGrReqDraw` | a finished chain pushed into the three-slot ring |
| `drawFrame` | `sdfGrDrawFrame` | the DMA kick |
| `matSet`, `texSet`, `clear` | the state entry points | material, texture and clear changes |
| `sprite` | `sdf2DAllocSprite`, read back at submit | one decoded 2D sprite: rect, UVs, colour |

## Rendering

`SDF_WINDOW=1` opens an SDL3 window with a GL 3.3 core context on its own thread. `RenderThread`
owns the window, the context and the loop; `Renderer` owns the GL objects and knows how to draw a
frame, on that thread and no other. The guest thread only ever touches the three-slot `FrameRing`,
so a hook never blocks on the GPU.

A frame is accumulated in the hooks' pending list, not in a `Frame`: the game allocates its content
across several tasks before `dds3DrawProcess` assembles and submits it, so there is no call that
reliably starts one. A `Frame` is taken from the ring at `sdfGrReqDraw`, which is the first moment
the content is both complete and known to be wanted. `dds3DrawProcess` is `sdfGrReqDraw`'s only
caller, so returning from it without one means `dds3DrawFlag & 0x2000000` skipped the submission:
the game built the frame and threw it away, and dropping the packets leaves the previous frame on
screen the way the hardware does. None of the three sample states exercises that path.

Sprites are the only geometry drawn so far, and every one the GS rasterizes is decoded: 42, 104 and
34 per frame on field, battle and menu, matching the GS trace exactly.

That came from hooking `sdfDrawTagLink` rather than the producers. Every render function links its
packet through it, so one hook is a complete inventory of a frame: the return address names the
producer and the packet says what it draws. Hunting producers by name found four candidates and
missed the two that mattered.

A packet is read at the submit, not at the link, because it is linked before its contents are
written. It is a DMA tag followed by its own quadwords, and those hold **several GIF tags in a row**
in any of three encodings, all of which the walk handles:

| encoding | how a register arrives | who uses it here |
|---|---|---|
| PACKED | one register per quadword, fields in separate 32-bit words | `sdf2DAllocSprite`'s HUD sprites |
| REGLIST | two registers per quadword in the GS register's own bit layout | `frCreateDrawFontPacket`'s glyphs |
| A+D | a value and the register address to put it in | the full-screen blackout and clear quads |

Three things had to be read off real packets rather than assumed, and each was worth a fifth of the
coverage. A packet holds more than one GIF tag: a glyph sends its texture state as A+D writes and
its geometry as a second tag. PRIM carries across tags within a packet, so a geometry tag with
`PRE=0` is still geometry. And A+D carries geometry as readily as state: the blackout quad sets
PRIM, RGBAQ and two XYZ2 entirely through it.

`SDF_DUMP_PACKETS=<caller>` prints the raw quadwords of packets from a matching linker that yielded
no geometry, following REF tags to their data. That is how the A+D case was found.
`SDF_DUMP_TEXTURES=<dir>` writes each decoded texture as a PNG, alpha included.

The draw space is 512x224, which the GS magnifies 2x vertically to the 512x448 it presents;
`XYOFFSET` is 1792,1936 in whole pixels. That is measured rather than assumed: the blur sprite in
`dds3DrawProcess` is exactly 0,0..512,224 at zero zoom, and every battle sprite lands inside the
rect. Sprites still draw untextured, so they are flat quads in the right places.

Not yet drawn: triangle fans and strips (17464 and 472 vertices a run on field), and the billboards,
which go through VU1 as object-space corners and a transform packet, so there is no GIF geometry to
read at link time.

## Where the addresses come from

Every hook resolves its own symbol by name against a `.map` loaded at runtime (`SDF_SYMBOLS`), so
an unknown disc hooks nothing rather than hooking whatever sits at a hardcoded address.

`src/libsdf.h` carries the engine's structures under their own names and offsets, so a field can be
compared against the database without translating it first. Sizes are pinned by `static_assert`.

## Iterating on a scene

```sh
PCSX2HOOK_STATE=states/host.p2s SDF_FRAMES=60 ./build/bin/sdfhook <iso>
```

A state boot reaches geometry on its first frame instead of the ~2000 a boot needs. States from a
desktop PCSX2 load here, so pick a scene there and copy the `.p2s` across.

Input works from a state boot: one `square` press opens the main menu. Two things make it easy to
conclude otherwise, and both cost time here. A button with no effect where the character is standing
proves nothing, and the round-robin `PCSX2HOOK_AUTOPRESS` presses every 20 frames, which toggles a
menu open and shut so a screenshot can land either way. Use `PCSX2HOOK_AUTOPRESS_SCRIPT` with one
press at a known frame when the question is whether input arrives at all.

`PCSX2HOOK_NOMEMCARD` stalls the boot at the memory card check, so leave the card enabled.

`sdfMatSet` and `sdfTexSetEx` fire only when a scene loads, so a state boot never sees them.
Material and texture state has to be read from guest memory at draw time rather than observed
through those hooks.
