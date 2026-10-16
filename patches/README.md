# SLUS_209.11 hard patches

`nocturne_patch.py` patches the retail NTSC-U executable (md5 `6a4d861f...`, PCSX2 CRC `E8FCF8EC`)
for real hardware. Every site is checked against its original bytes first, so another build is
refused rather than corrupted. The `native` and `weave` groups compile `native.c` or `weave.c` with
the PS2 EE toolchain (`mips64r5900el-ps2-elf-gcc` on `PATH`, or under `$PS2DEV/ee/bin`).

```sh
python3 patches/nocturne_patch.py SLUS_209.11 patches/out/SLUS_209.11     # default groups
python3 patches/nocturne_patch.py nocturne.iso nocturne_patched.iso        # copies, then patches the ELF inside
python3 patches/nocturne_patch.py state.p2s state_patched.p2s              # RAM only, for testing
python3 patches/nocturne_patch.py SLUS_209.11 out --only widescreen progressive weave noblur
```

| group | default | what |
|---|---|---|
| `progressive` | yes | `sdfGrGsReset` calls `sceGsResetGraph(0, NOINTERLACE, DTV480P, FRAME)`. Without `native` or `weave`, `sdfGrSetDisp` shows each 512x224 field buffer line-doubled and `sdfGrSetDrawFramePacket` drops the odd field's half-line XYOFFSET, so both passes render the same picture. |
| `native` | yes | Renders 448 lines, below. Needs `progressive`, excludes `weave` and `60fps`. |
| `weave` | no | 448 lines woven from the two field passes, below. Needs `progressive`, excludes `native` and `60fps`. |
| `smooth` | no | Needs `native` or `weave`. Display circuit 1 reads W too, a line lower and 1 VCK right, mixed 50/50 with circuit 2 (PMODE ALP 0x80): a vertical and slight horizontal smoothing pass at no rendering or memory cost. Skipped on GS revision 1, where libgraph uses circuit 1 alone. PCSX2 undoes this kind of offset while its Anti-Blur option is on (the default); turn it off to see the effect. |
| `widescreen` | yes | `dds3ScreenParam.aspect` 16/9 with `progressive` (5/3 with `native`, whose 512 columns fill 480), 1.165 x 4/3 without. |
| `noblur` | yes | `ooyTestDrawProc` skips `fldDrawFrameSprite`, which blends the previous field buffer over every field frame at 48/128 through a bilinear sprite offset by a fraction of a pixel. |
| `noclip` | yes | Hold L3 and press R3: the field player flies through walls, below. |
| `ragdoll` | yes | Needs `noclip`. Turning noclip off in the air drops the player, who goes limp as they fall, below. |
| `freecam` | yes | Hold L3 and press Select: the camera flies free, in the field, in battle and in events, below. |
| `60fps` | no | `dds3VblanksPerFrame` 2 -> 1, and `stfDestroy` restores 1 after the staff roll. |

## Noclip and the free camera

Both toggle with a combo that starts on L3, which the game does not read: hold L3, then press R3
(noclip) or Select (free camera). While L3 is held, and while a mode is driving, the game sees a
neutral pad, so the other buttons do nothing else. Turning one on turns the other off.

| | noclip | free camera |
|---|---|---|
| left stick | fly, relative to the camera | fly where the camera looks |
| right stick | turn the field camera | look around |
| R1 / L1 | up / down | up / down |
| L2 / R2 | | zoom out / in |
| cross | fast | fast |
| square | | slow |

- **Noclip** takes the place of `fldPlayerCalc` while the player has control of a walking field
  (not the world map, a menu, the automap or an event). Collision, the walk's floor snap,
  encounter rolls and door and event triggers are off while flying.
- **Turned off in mid air, the player falls** under gravity, with `fldHitChkUnit` finding the
  floor as `fldPlayerFastSet` does, and lands there. Nothing below for ten seconds puts the player
  back where noclip let go.
- **The ragdoll** takes over a few frames into a fall. Each of the player's 56 model nodes
  becomes a point mass: bones keep their length, links to each joint's grandparent and between
  siblings keep the shape, and joints further apart keep out of each other by their thickness,
  taken from each node's own mesh bounds, so the body does not fold into itself. In the air, small
  parts meet more drag and the body starts a slow forward tumble, so it goes limp as it falls; the
  floor is the plane the fall landed on.
  Each frame the joints go back into the bones' local matrices just before `mdlLink` evaluates
  the skeleton, so the engine skins, culls and draws the body as usual. The camera follows the
  body while it is loose. Once it has settled, any button stands the player up where it lies.
  Walls and slopes are not part of the simulation, and elbows and knees can bend backwards.
- **The free camera** sets the view's eye and target just before `dds3DrawFirstProcess` builds the
  view matrix from them, so culling, billboards and shadows follow it. The game's own camera keeps
  running underneath, and turning the mode off hands the view back. 3D sound and a few battle
  visibility checks still use the game's camera.

**The code segment.** They run from a fourth ELF segment at 0x5A0340, where the heap used to begin:
the tool moves the start of the heap (`_start`'s SetupHeap argument and `sbrk`'s first break) past
it. The segment's bytes replace the VU overlay tables and section headers at the end of the file,
which no loader reads, so the ELF keeps its size and fits the disc image as before. For a
savestate, whose heap is already in use, the segment goes to 0x1F00000 instead, above the heap's
end.

## Native 448 lines

The engine draws every game frame twice, once per field, into two 512x224 buffers. `native.c` has
the two passes draw the top and bottom halves of one 512x448 frame instead, so the picture is
rendered at full height rather than put together from fields. Frames alternate between A, the two
field buffers (adjacent in GS memory), and W, a second 512x448 buffer; the display shows the last
finished one. It runs from the same dead code regions as the weave.

- **3D**: the VU1 viewport packets (`sdf3DMakeViewportPacket`) get twice the vertical scale, and
  each pass's FRAME, XYOFFSET and SCISSOR (`sdfGrStdDrawEnvPredraw`) select its half.
- **2D and EE-projected geometry** is written by game code into packets in 224-line coordinates.
  Before `dds3DrawProcess` hands the frame to `sdfGrReqDraw`, `submit_frame` walks the frame's DMA
  chain as VIF1 and the GIF read it and moves every vertex drawn into the frame to 448-line
  coordinates. That includes the screen fade and the clear sprites, whose packets live outside the
  per-frame packet buffer; for those the walk remembers what it wrote, so a packet sent again
  without being rebuilt is not moved twice.
- **The HUD fits the screen**: 480p shows 480 of the 512 columns, so the same walk and the viewport
  map the 512-column screen into the middle 480. Nothing is cropped, at 15/16 of the width; the 3D
  aspect is corrected to match.
- **Effects that read a frame** (the encounter smear, the screen blur, crossfades, the sword trail)
  get the whole 448-line frame as their texture, with their texture coordinates mapped the same way
  as positions. A copy of the last frame into the next one is sampled half a line lower, so it
  blends two lines, as the field passes did when they read the other field.
- **GS memory**: W is allocated and given back exactly as in the weave. Without W, frames draw
  into A while it is shown, which can tear until W comes back.

## The weave

The engine draws every game frame twice into two 512x224 buffers, the second pass half a line
lower, and interlace used to combine them. `weave.c` keeps both passes and, at the start of the
render thread's next vblank, copies them line by line into a 512x448 buffer W that the display
shows. It runs from dead debug and unused library code: `weave.c` in one region, its DMA chain in
another, and the copy list across the rest. The patch tool lists the regions and checks each one's
original bytes by hash.

- **The copy** is 448 GS local-to-local transfers in one DMA chain: the chain's head and two
  BITBLTBUF words are in the chain's region, the constant TRXPOS/TRXDIR list is spread over the
  other regions by REF tags. Transfer registers are all a transfer uses and every game upload sets
  them, so no drawing state is disturbed.
- **Hooks**: `sdfDeliverDrawEnd` records which buffer carries the half-line offset and when both
  passes are done; `sdfDrawThread` calls the weave before `sdfGrDrawFrame`; `sdfGrSetDisp` shows W.
- **GS memory**: W is 896 KB, allocated at boot next to the frame buffers. `sdfGsErrorHandler`,
  null in the retail game, becomes `gs_full`: when an allocation fails it moves the display off W,
  frees W and restarts the allocation. W comes back once a free slice holds it with 128 KB to spare.
  Until then the display shows the newest field buffer line-doubled, one line lower when it is the
  offset pass. Of the six test scenes the title screen is tightest, with 124 KB left beside W.
- **Latency**: a frame reaches the screen one vblank later than before, once both passes exist.

## What to know before running it

- **480p needs a component (YPbPr) cable** and a display that takes 480p. Composite and S-video
  show nothing usable. Set the display to 16:9 (full or stretch).
- **Untested on hardware.** For `weave`, the copy must finish writing a line before the beam reads
  it, about 1.9 ms after the vblank interrupt for the first visible line; if the render thread
  starts it late, the top of that one refresh tears. For `native`, the chain walk adds EE time
  to every frame, which PCSX2 does not time faithfully.
- **`weave` crops 16 columns each side.** 480p has 1440 VCK of active line; 512 columns at 3 VCK
  each would be 1536, and picture outside the active line lands in the sync porches. The battle
  "PLAYER" plate and the menu's LV column lose a few pixels at the right edge. `native` fits all
  512 into the 480 shown.
- **Feedback effects build up at half the rate in `native`.** The field passes ran a frame's
  effects twice and `native` runs them once, so a draw that accumulates on the last frame, like
  the darkening and smear before a battle, gets there more slowly. The encounter smear is also
  finer, since it no longer mixes two fields.
- **`60fps` doubles the game's speed.** Game logic advances once per main-loop frame and nothing
  scales by elapsed time, so movement, animation and timers all run twice as fast. FMV pacing
  reads the same variable and stays correct. It leaves one pass per frame, so it falls back to
  line doubling.
- **`sdfGrCapture`**, the engine's screenshot, still reads two 224-line buffers; nothing in the
  retail game calls it.
- **Updating at 30 and rendering at 60 gives no new frames.** `sdfDrawThread` already renders every
  update twice, once per vblank. Splitting the process list instead (calc processes below priority
  10000 on alternate ticks, draw processes every tick) was measured: the draw band moves the player
  and runs effect, particle and animation timers, so the field image changed exactly as much per
  tick as with full 60fps, and walking covered more ground than at 30.
- **The ELF has no section headers** once `noclip` or `freecam` is in, since the new segment
  takes their place. Loaders use the program headers only; tools that list sections will show none.
- Left alone on purpose: the event-scripted blur (`evtSetBokasi`, also `fldDrawFrameSprite`), the
  per-area radial and colour passes behind `fldDbg_ExBlurOn`, and the encounter and crossfade
  transitions that sample the previous frame.
