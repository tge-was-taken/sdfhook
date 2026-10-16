# sdfhook

> **An AI experiment, and deprecated.** Almost all of this was written by Claude Code, as a test of
> how far an agent gets on a reverse engineering problem that has a number attached to it. It got
> further than expected: the quiet scenes land within a few channel values of the emulated frame,
> from the game's own geometry, textures, lighting and draw order. It is still not a renderer to
> play the game on, and it is no longer worked on. `CLAUDE.md` is the operational reference it was
> written against and `TODO.md` is where it stopped, with the evidence for each open item.

A native renderer for SDF, the engine SMT3 Nocturne (SLUS-20911) is built on. SDF is Atlus's own
library, statically linked into the ELF, so there is no middleware boundary to cut at the way
RenderWare gives one; the cut is at `sdfModelLinkOt` instead.

Nothing of the engine is reimplemented. The renderer hooks the draw path, decodes the DMA chain the
game sends to the GS, and draws that with GL; the guest keeps running under a PCSX2 fork
(`pcsx2-hook`) that provides the hooks and the reference frame buffer.

## Status

`scripts/refdiff_all.sh` scores each scene against the emulated frame buffer at 512x448, as the
mean absolute channel difference out of 255. Lower is better.

| scene | score |
|---|---|
| title screen | 1.22 |
| menu main | 2.02 |
| menu loading file 1 | 2.08 |
| field unknown realm | 4.23 |
| battle unknown realm | 4.26 |
| field underground facility | 4.37 |

So a quiet scene is within a few channel values of the hardware, and the error that is left is
mostly the uniform brightness offset below. Battle effects are a different story, 8 to 32 on the
captures TODO.md lists.

### What works

- **The 2D is decoded, not hooked.** `gs_decoder.cpp` reads the chain as VIF1 and the GIF do and
  keeps the whole register file, so a primitive kicked in a packet nothing hooked still draws.
- **Order is the DMA chain**, replayed at `sdfGrReqDraw` with GS state per context, not the order
  the hooks fired in.
- **VU1 is read, not guessed.** All 1884 instructions of the one microprogram SDF uploads are
  transcribed in `docs/vu1/`, checked address by address against the listing by
  `scripts/vucheck.py`, and the shader's lighting carries the VU addresses it came from. That is
  where the three-light sum, the per-vertex clamp, light 0's color being a difference, and the
  sphere-map reflection pass come from. The toon ramp is the one deliberate departure, sampled per
  fragment where VU1 has it per vertex.
- **Meshes**: all four packet types (rigid, weighted, blend shape, VIF stream), skinned on the GPU,
  with the engine's lighting, fog and toon ramp, and back-face culling off the per-draw sign.
- **Depth** holds GS z as `1 - z / 2^24`, so decoded draws and meshes test against each other the
  way the hardware has them.
- **Textures** from registered assets, from the font sheet, and recovered out of GS memory for a
  savestate taken after a load.
- **Frame feedback**, the previous-frame blend the field and several effects depend on.
- **Tooling**: a mesh picker in the window, per-vertex and per-register diffs against the hardware
  (`vertdiff.py`, `gsdiff.py`), draw isolation on both sides, F9 captures, and a scoring sweep.
- **`patches/`** is independent of all of the above and does work: hard patches of the retail ELF
  for progressive 480p, widescreen, no blur, noclip and a free camera, for real hardware or any
  emulator. See `patches/README.md`.

### What does not

- **The 3D scenes draw about 3.5 too bright** on every channel, and the mesh alpha is the unsolved
  knot behind it (TODO.md V8). It is the largest single error left.
- **Battle effects** are the worst case: dull ice, missing bloom, and effect polys' second texture
  unmodelled (P3, B1).
- **Field shadows** are decoded but off by default, last measured at about 22 points (V19).
- **The reflection pass** samples the wrong axis (P8), and `flatten` reaches only light 0 (V23).
- **Recovered battle textures** may be wrong, and are unverified (B2).
- **The native shot sometimes lands a frame late**, worth several points on two captures (M5).
- Anything no savestate reaches is reported rather than drawn, which is the point of the
  `unsupported` channel, but it means play meets states the curated scenes never measured (X10).

### What a checkout does not include

- **`vendor/pcsx2-hook`**, the runner. Clone it there or set `PCSX2HOOK_TREE`.
- **A BIOS, the disc and the savestates.** All user-supplied. `states/README.md` names the scenes
  but the `.p2s` files are not in the tree, so no scored run is reproducible from a checkout alone.
  `refdiff_all.sh` and `dump_scenes.sh` default `SDF_BIOS` and `SDF_ISO` to paths from this
  machine; override both.
- **`assets/SLUS-20911.facts`, `.packets` and `.pseudo.c`**, exported from an IDA database of the
  disc that is also not here. `scripts/guestcheck.py` needs the facts; regenerate with
  `scripts/ida_facts.py` and `scripts/export_packets.py`. The symbol map
  (`assets/SLUS-20911.map`) is in the tree, so the hooks resolve and the build runs without them.

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
| `SDF_TRACE` | where the JSONL trace goes; `dumps/trace.jsonl`, or none in a window run |
| `SDF_FRAMES` | stop after this many frames |
| `SDF_SYMBOLS` | optional symbol map, a file or a directory holding `<ELF>.map` |
| `SDF_WINDOW` | open the native window and draw what the hooks decode |
| `SDF_SHOT_FRAME` | frame to capture at |
| `SDF_SHOT` | emulator framebuffer, PPM |
| `SDF_NATIVE_SHOT` | the native render, PPM |
| `SDF_SCALE` | render size as a multiple of the guest's 512x448, default 1. Anything above it is for looking at: a capture is only comparable against the emulated frame at 1. A window run starts at this size and then renders at whatever size the window shows the picture, so resizing the window upscales |
| `SDF_LOG` | log levels, one for everything (`trace`) or per channel (`model=trace,render=warn`) |
| `SDF_DUMP_TEXTURES` | directory for decoded textures as PNG; GS-recovered ones are named `gs_*` |
| `SDF_NO_GS_RECOVER` | stop reading textures back out of GS memory when the game registered none |
| `SDF_SPRITE_HOOKS` | draw 2D from the producers' hooked arguments instead of the decoded packets; a packet no hook drew is decoded either way |
| `SDF_DUMP_DECODED` | dump every decoded primitive from a matching producer, with the registers it drew under |
| `SDF_DRAW_ONLY` | draw only matching meshes: comma separated `i=N`, `i=A-B`, `tbp=N`. Pairs with the emulator's `PCSX2HOOK_GS_ONLY`, which takes the same terms plus `prim=N`, so a frame can be cut to one draw on both sides and diffed without matching anything |

**Under WSL**, both GPUs go through D3D12. Our GL renderer needs `GALLIUM_DRIVER=d3d12`
(`MESA_LOADER_DRIVER_OVERRIDE=d3d12`, `LD_LIBRARY_PATH=/usr/lib/wsl/lib`). The emulator's software GS
presents through Vulkan: on lavapipe that costs about a sixth of the CPU and falls behind 1x when
the machine is busy, so install dzn (Mesa's `microsoft-experimental`, not in Debian's package) as a
user ICD. pcsx2-hook's PCSX2 allocates descriptor sets where dzn lacks `VK_KHR_push_descriptor`.
The emulator's shot is identical under both.

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
| `modelLinkOt` | `sdfModelLinkOt` | a model root: flags, drawAttr, color, node count, its four OTs |
| `packetGen` | `sdfCorePacketGen` | the readable geometry path, kind 3 |
| `shapedPacket` | `sdfModelLinkShapedDrawPacket` | the preprocessed path, kind 2 |
| `otRegist` | `sdfSortOtAvlRegist` | a draw tag filed into an ordering table, with its z |
| `reqDraw` | `sdfGrReqDraw` | a finished chain pushed into the three-slot ring |
| `drawFrame` | `sdfGrDrawFrame` | the DMA kick |
| `matSet`, `texSet`, `clear` | the state entry points | material, texture and clear changes |
| `sprite` | `sdf2DAllocSprite`, read back at submit | one decoded 2D sprite: rect, UVs, color |

## Rendering

`SDF_WINDOW=1` opens an SDL3 window with a GL 3.3 core context on its own thread. `RenderThread`
owns the window, the context and the loop; `Renderer` owns the GL objects and knows how to draw a
frame, on that thread and no other. The guest thread only ever touches the three-slot `FrameRing`,
so a hook never blocks on the GPU.

A frame's draws are decoded from the producers' arguments as the game calls them, each recording the
packet that carries it. At `sdfGrReqDraw`, the first moment the content is both complete and known
to be wanted, one walk follows the frame's DMA chain the way channel 1 sends it, replays the GS
state, and puts the draws in that order. The renderer draws that order and nothing else.
`dds3DrawProcess` is `sdfGrReqDraw`'s only caller, so returning from it without one means
`dds3DrawFlag & 0x2000000` skipped the submission: the game built the frame and threw it away, and
dropping the packets leaves the previous frame on screen the way the hardware does. None of the
three sample states exercises that path.

Every sprite the GS rasterizes is decoded: 42, 104 and 34 per frame on field, battle and menu,
matching the GS trace exactly.

The 2D inventory came from hooking `sdfDrawTagLink` rather than the producers. Every render
function links its packet through it, so one hook is a complete inventory of a frame: the return
address names the producer and the packet says what it draws. Hunting producers by name found four
candidates and missed the two that mattered.

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
rect.

Geometry VU1 builds carries no GIF vertices, so meshes and billboards come from their producers'
arguments (`sdfModelLinkOt`, `sdfCorePacketGen`, `drawSprite3D`) and everything else is read from
the chain. CLAUDE.md's "The migration boundary" says which is which and why.

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
