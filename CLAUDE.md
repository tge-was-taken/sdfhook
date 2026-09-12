# sdfhook

A native renderer for SDF, the engine SMT3 Nocturne (SLUS-20911) runs on, with pcsx2-hook as the
runner. `README.md` is the contributor doc; this file is the agent-only operational reference.
`TODO.md` holds the open work and the evidence behind it.

## Rules

**Fail loudly.** A `default:` that quietly picks something reasonable is how a gap survives a hundred
healthy-looking runs. Enumerate every case a set actually has and give the default
`reportUnsupported(what, value)`. The same goes for any "treat the rest as X" fallback: the method
here is measuring what is not yet handled, and a silent fallback is a measurement that lies.

**A default that shades plausibly is the worst kind of bug.** `faceDivSoften` sat behind an early
return for an unrelated attr bit, so meshes without a second UV set kept `DrawMesh`'s default of 1.0
while their neighbours got 7.01: same material, one soft ramp against one steep, and a seam along
the mesh edges. Nothing failed and nothing was reported. So: **a guard may only skip the fields it
guards** (hence `readMultiTexturePacket` split out), and a struct default must be implausible or
checked (`ModelTrace::countMaterialShading` warns when two meshes of one material disagree).

**"Inert", "zero" and "unused" are measurements, never adjectives.** Report at the site that reads
the field, or the check never fires.

**A reimplementation is held to the guest's own facts.** `scripts/ida_facts.py` extracts every
read, write, global, call and basic block of a function into `assets/SLUS-20911.facts`, and
`scripts/guestcheck.py` fails on any the code neither models nor opts out of. An opt-out names
hardware from a closed set and sits where the guest does the work, so it is order-checked with the
citations. See `.claude/skills/reimplement`.

**Unfinished work is written down, not left in the tree.** `scripts/lint.sh` names every function
nothing calls and the list is expected to be empty. A thing that must stay uncalled says why: an
inline `cppcheck-suppress unusedFunction` with a reason where it is complete by design, or an
`@todo` naming the TODO.md item. A deferral becomes a TODO.md entry when it is made, saying what was
left and what it costs.

**Model every path, not the ones that fire.** A branch measuring zero in every savestate is still a
branch; the savestates are a window, not the game. Where a path cannot be modelled,
`reportUnsupported` it so a run that trips it says so.

**Color stays in the shader.** Never scale a color channel into a vertex buffer. Attributes carry
the guest's value normalised; every conversion, the PS2's 128-is-opaque alpha included, happens in
the shader.

## The migration boundary

**Packets are decoded by default; a hook is an enhancement of ours, not the source.** Everything
GIF-path reaches the GS through the chain: `gs_decoder.cpp` reads it as VIF1 and the GIF do, keeps
the whole register file, and kicks vertices into primitives the renderer draws. Decoding the 2D took
the main menu from 11.263 to 2.099 and the title from 5.574 to 2.941, and matched the hooks pixel for
pixel in battle and loading, because a hook reproduces its producer's arithmetic and the packet is
that arithmetic's result. `SDF_SPRITE_HOOKS=1` puts the 2D hooks back, and a packet no hook drew is
decoded either way.

**Hook where the packet cannot carry the draw.** VU1 builds the model, billboard and poly geometry
from its input, so what the chain holds is VU1's input, not the draw: those stay spans with their
hooks (`sdfModelLinkOt`, `sdfCorePacketGen`, `drawSprite3D`). A hook is also worth it where it adds
what a packet has lost: an identity to interpolate across frames, a HUD element to anchor for
widescreen, a glyph to replace. Write down which, since the default is the packet.

**Well defined also means complete.** One call carries the whole draw. Billboards are the case:
`drawSprite3D` links three packets, and the rotation reaches the transform packet through VU0's
register file, never through `sdfBillBoardMake1`. So the floor is `drawSprite3D`, whose
`Sprite3D_t` holds position, size, rotation and color together.

**A decoded draw is only as good as its texture.** TEX0 resolves to the 2D texture packet that wrote
it, a frame buffer (the engine's type-1 GS slices and every FRAME the chain writes), the font sheet the
glyph uploads built, an engine handle whose upload wrote the same TEX0 (`sdfTexHandleLink`), or GS
memory read through the register. GS memory holds only what was uploaded last, so a texture streamed
in mid-frame reads wrong from there: the font did until it was routed to the sheet.

**What the 2D renderer cannot do is painted magenta**, as the mesh shader paints an unmodelled
state, and counted in `unhonouredDraws` by producer: a destination alpha test, a texture function
other than MODULATE, fog, a texture that cannot be found. Depth is only reported, since the chain's
order already layers what the menus depth-test against each other; a draw whose tests pass no
color is not drawn.

**Inlining limits what a hook can enumerate.** "Never called" from a hook means never called out of
line. Trust the `[todo]` ranking over any call-count enumeration.

**Prove the boundary is complete by netting the layer below it.** Hook the internals underneath,
resolve `$ra`, print the edges at shutdown. Every caller should be the entry point already hooked or
another internal of the same module. It found `sdfTexDup` in one run. **A post-handler costs the
caller attribution of anything the function tail-calls**, since `$ra` becomes the return sentinel:
watch the return of a function nothing tail-calls, or watch entry only. **The net only sees what
runs inside the capture window**; a clean net means "nothing unexpected happened here".

**The producer set is closed.** Nothing reaches the GS without linking a draw tag, and there are
five entry points: `sdfDrawTagLink`, `sdfDrawTagLink2`, `sdfDrawTagLinkRef`, `sdfDrawTagLinkRefGif`
and `sdfDrawTagLinkCall`. Hook all five and the caller set is the complete producer set.

**Rank producers by the area they cover**, not by packets or vertices. A producer whose geometry VU1
builds carries no GIF vertices; `ooyTestPoly3` heads the packet ranking and is worth 0.010 of a point
because its triangles are 4x5 pixels.

**The game writes its own 2D.** The engine publishes `sdfPrimLinkSprite` and friends, but only
`sdfPrimLinkFt4` and `sdfPrimLinkG4` fire; the rest take `sdf2DAllocSprite` and write registers
themselves. That is per-disc game code a hook has to follow and a decoder does not.

## The frame is the chain

**The renderer draws the frame's DMA chain, not the order the hooks fire in.** Every draw records the
packet that carries it: `linkAddr`, or `linkIndex` for a generated mesh, whose address is known only
once it links. At `sdfGrReqDraw`, `processCommandStream` (`packet_processor.cpp`) walks the chain as
channel 1 sends it, replays the A+D writes per GS context, and handles each packet by its recorded
type in one switch, a `process*Packet` per type. A model node is a span, checked for first and
consumed from its first link to its draw tag's tail. Each packet's draws go into `Frame::order`
under the ALPHA and TEST in force there, and the renderer draws `order` as it stands.

- **A model stream is keyed by the REF that names it**, not by the call chain it sits in.
- **A producer's draws are claimed by the first draw-carrying packet it links**, so a texture or ALPHA
  packet linked first is passed over (`isDrawType`). Each producer's impl opens a `ProducerDraws`:
  draws that another producer's hook arrives before stay unlinked rather than land on its packet.
- **A draw the chain never reaches is not drawn.** The summary counts placed, unlinked and unreached
  draws by producer; the expected losses are `sdfCorePacketGen` meshes the engine never links.
- **Unhandled means transmitted and unrendered**: a packet of a draw-carrying type with nothing placed
  at it, at its REF target, or in the chain it calls. That is the `[todo]` ranking.
- **GS state is the chain's, never the call's.** ALPHA, TEST and the FRAME mask per context, and the
  texture a 2D texture packet binds, are replayed at each draw's packet; a producer says only whether
  its PRIM blends and which texture it names. Two-sided polys took `rsetTest`'s 0x33801 at the call,
  and the GS trace has no batch under it.
- **Lighting and fog are not chain state for a generated mesh.** `sdfCorePacketGen` lights on the
  EE with `corePktCurrLight`, which `sdfCoreDrawSetLight` sets at generation time, so a mesh keeps its
  root's light. Replaying light and fog packets from the chain for every mesh cost 0.8 to 4.4 points.
- **Every transfer is decoded.** The register file replaces the old A+D replay, VIF codes say which
  data is GIF, and a primitive kicked in a packet nothing hooked is drawn from the packet.
- **Every packet type is consumed or reported.** State the renderer takes from a producer's call
  counts as read only when a producer with recorded draws linked it; anything else is
  `reportUnsupported`, as is every register a state packet sets that the replay drops.
- **A GIF tag's writes can arrive in the next DMA transfer.** `sdfGrSetRefGifPacket` sends the A+D
  tag in its own packet and REFs the writes, which is how the frame-mask pair reaches FRAME. A
  transfer is state only if no write kicks a vertex: `dds3DrawFade` sends its quad as A+D.

## Naming and attribution

`nb` battle, `fld` field, `cmp` camp/menu, `fcl` facilities, `dds3` kernel, `sdf` engine library. A
prefix says which scene a function serves, which is why the samples must cover more than one.

Functions are verbs and an accessor is `getX()`, never a bare noun. Predicates keep `is`/`has`.
"Sprite" is the term throughout: it is what the engine calls these and the ordinary word for a
textured quad. **Name a thing for what it holds, not the first case it held.**

Name a handler for the symbol it hooks, `<symbol>Hook`; a handler serving several symbols takes a
generic name. **Pull every argument into a named local before using it**, with the engine's own
parameter names. **Read a producer's arguments against the decompilation, not its parameter names**:
`sdfPrimLinkFt4`'s argument 2 and `sdfPrimLinkG4`'s argument 1 are `aPrim`, a GS PRIM word, and
reading them as ALPHA picked a blend at random.

**Pre/post pairs** use `calls.enter(c)` / `calls.leave(c)`, keyed by pc, rather than per-call state
in `HookContext`, which is wrong under recursion.

A reimplementation is named for the engine function and carries `@fn` and `@note` (serial and
address) on the declaration; `scripts/attrcheck.py` enforces it. Key inline logic carries the hex
offset it came from. A helper that is not a reimplementation says so.

**Correct the database when you prove it wrong**, in the same sitting: the IDB outlives the session.
Record the evidence and the old value on the IDB item, re-export with `scripts/export_symbols.py`.
This covers names, layouts, sizes, missing headers and argument meaning.

## Code conventions

**Transport is separate from what a hook does.** `hooks.cpp` names a hooked function's arguments
out of the registers and calls into `hooks_impl.cpp`; `hooks_impl_model.cpp` is the same split for
the model half, as `ModelHooks` over the parser's state. `model.cpp` keeps only what decodes a mesh
body (`readRigidBatches`, `readBlendShapes`, `readVifStreamMesh`, `readSecondUv`, `finishMesh`) and
the trace, with `model_internal.h` for the helpers both halves need. **No inline logic in a
handler**: a handler that does more than name arguments and delegate is one that will be read as
transport and edited as logic.

**Hooks stay minimal, and a guest function is not reimplemented to replace one.** The
`shadow-refactor` branch tried it and was abandoned: guest code is known-correct, so a hook that
reads its arguments has one unknown, our rendering, and a translation adds a second, after which a
moved score no longer says which broke. If it is ever wanted again, the reimplementation runs
beside the hook and is compared field by field until silent, before anything switches over.

**Guest memory is reached through `guest_ptr<T>`, never a threaded accessor.** `installGuestRam` puts
the RAM base in thread-local storage on the CPU thread the hooks and frame callback share; the render
thread never installs it, so a pointer reaching it reads as unmapped rather than as a race. Hold the
`guest_ptr`, not a host pointer: `->`, `*` and the bool conversion are the whole interface, plus
`slice(n)` for a run whose bounds are checked once.

**A guest address is a typed pointer, not a `u32`, including in a hook's own arguments.** Name the
type at the boundary. `u32` is right only where there is no struct: a DMA chain, a packed index list,
raw pixels. **A primitive pointer is a sign the struct is missing.** A guest pointer is named by an
alias beside its struct (`sdfMatPtr_t`, `frGsTex_PTR_t`), not by the template. **Watch `sizeof` when
a local changes from a value to a pointer.**

**Log through a channel**, never `printf` and never a hand-written tag. `src/log.h` gives each
translation unit a `LogChannel`; `SDF_LOG` sets levels globally or per channel. **A log level taken
at construction is no level at all**, every channel is a file-scope static built before `main` reads
the environment, so levels resolve on use.

**Dumps are JSONL, through `src/json.h`.** One object per line, each field named. Note `addReal`'s
`digits` argument is **significant digits** (`%g`), not decimal places.

**No stray `std::getenv`.** Everything the environment can change is a field of `Config`, read once
by `Config::readEnvironment()`.

**The run summary is a row of the run's own trace**, not only terminal text: one `{"ev":"summary"}`
object with every counter, zeros included, so two runs are diffed rather than eyeballed.

**A trace observes; it never decides.** `ModelTrace::Geom` is a reporting bucket, private to the
trace; the parser dispatches on the engine's own `sdfCorePacketType_t`.

**Depend on pcsx2-hook's public header only.** `pcsx2hook/pcsx2hook.h` is the whole supported
surface.

**Blank line around anything with a documentation comment.**

**Comments carry WHY only, never what or how.** What and how belong in the code: rename it, split
it, extract a function until it reads without help. A comment that could be deleted by improving a
name is a missing rename.

**Keep every comment short.** Fragments, not sentences. No prose, no narrative, no measurements or
score history (those go in TODO.md), no account of what the code used to do. One line where one
line will do, never more than two. Doxygen (`///`) on declarations, `@param`/`@return` only where
the signature does not already answer it. Guest and VU1 addresses are the exception worth keeping:
they are attribution, not explanation.

## Textures

`sdfTexSetEx(void* aTex, s32 memMode) -> sdfTexHandle_t*` registers an asset. Hook it with a
post-handler: the pre-hook keeps the asset pointer and `$v0` is the handle. **This is the only moment
the pixels are in main memory.** **The handle is the key**, `DrwSprite2D` is handed the same
pointer, so no GS address enters the picture.

**Do not hook `sdfTexTransCLUT` or `sdfTexTransData`.** They are the transfer layer; their arguments
are a GS destination and a raw pointer.

Reading the asset: data starts at `asset + (byte[1] & 0xF0)`, the engine skips 64 bytes, and the CLUT
(if `numOfCLUT`) precedes the pixels. Sizes come from the handle. **The asset stores pixels in plain
row order**, so p3fes's `unswizzlePSMT8` must not be applied; the CLUT unswizzle and the RGBA5551
widening still do, with `skygl_texture_decoder.cpp` as the reference.

**Two kinds of texture.** `sdfTexSetEx` registers an asset via `sdfTexAlloc`. `dds3AllocRenderTexture`
wraps a frame buffer with `sdfTexNew`: a render target, with no main-memory pixels by construction.

**Recovering from GS memory**, for a savestate taken after a load: `sdfTexGetTex0Reg` says where to
look, at `+32` in the handle's upload packet, and `pcsx2hook_read_gs_pixels` reads it. Two rules,
both learned by crashing: **never read through the renderer's CLUT cache** (read raw per texel and
apply the palette on our side, as `GSLocalMemory::SaveBMP` does), and **never read from inside a
hook** (queue at the lookup, drain in the frame callback). **A recovered texture is not always its
own**: the GS reuses memory, so a TEX0 recorded at upload can name a region the display buffer has
taken. `SDF_DUMP_TEXTURES` writes recovered ones as `gs_*.png`.

**Never free a GPU resource at the engine's free.** `sdfTexFree` says the guest is done with a handle,
not that nothing is drawing it. Forget the cache entry and replace the GL object at its next upload.

## Meshes

Three sources in order of authority: the **reconstructed HD sources**
(`~/projects/smt3hd-reconstructed-sources`) for struct and field names; the **DDS3 format work**
(`tge-was-taken/010-Editor-Templates`, `DDS3-Model-Studio`) for body layouts; the disassembly settles
disagreements. The in-memory packet is the file record shifted forward by four bytes.

- **`attrBits`** is the GS PRIM word in its low bits with engine flags above: `0x1000` a second UV
  set, `0x4000` draw back faces too. `sdfModelLinkOt` masks it with the node's `drawAttr`.
- **`packetBits`** says which arrays the body carries: `0x20` faces, `0x80` normals, `0x100` uv,
  `0x800` weights, `0x1000` blend shapes.

A body is **VIF-tagged and batched**: each array is preceded by an UNPACK naming its width and count,
so a misread offset is refused rather than decoded into plausible numbers. **Read the count from the
UNPACK, not from `packetBits`.** A mesh over VU0 memory arrives split across batches whose vertices
concatenate.

- **Type 8, rigid**: header UNPACK `[pointCount, 0]`, float3 positions, float3 normals, float2
  texcoords, kick, flush.
- **Type 7, weighted**: the packet names the nodes it blends first and every batch blends the same
  set. Header `[usedNodes - 1, pointCount]`, then per node a float4 position array whose w is that
  node's weight and a float3 normal array, then one float2 texcoord array.
- **Type 5, blend shape**: plain arrays. `extData[0]` holds the shape count and a morph-controller
  index; shape zero is the mesh and the rest are deltas.
- **Type 1, VIF stream**: linked with `sdfDrawTagLinkCall` as a DMA source chain the hardware runs.
  A REF is identified by **what its data is**: an address in the root's material-packet map is a
  material, one opening with the batch-header unpack is a mesh, anything else is setup. Batch header
  `[faceCount, pointCount, attrBits, packetBits]`, byte triangle indices local to the batch. These
  are three quarters of a field scene's triangles.

**A blended mesh must not write depth** (`attrBits & 0x40`), or a transparent panel occludes the
scene behind it. **The engine's projection is not GL clip space**: rebuild depth linearly from `w`
and deliberately do not clamp, so a vertex nearer than the near plane leaves `[-w, w]` and GL clips
it. **Skinning is on the GPU and a rigid body is the one-bone case**: the format gives each bone its
own copy of a vertex's position and normal, already in that bone's space, so the shader sums
transformed points and needs no bind pose. Measured cap: 4 bones a palette, 3 influences a vertex.

**A helper that splits a guest function is invisible to `guestcheck`.** `walkChain` was
`sdfModelLinkOt`'s inner loop as its own function, so the packet header reads and the type dispatch
counted against `sdfModelLinkOt` with nothing to satisfy them. Merge such a split back, or the
score lies by the size of the piece you moved out.

**Citations and opt-outs are order-checked, and a block citation cites every address inside it.**
So an opted-out call sitting inside a cited block reads as modelled and opted out at once: give
that block its own `@optout` span alongside the call's point opt-out, rather than citing it. A
span binds blocks only, never the calls or reads within.

**The scene walk.** `ModelWalk` is `sdfModelLinkOt` walked again on our side, from the entry hook,
over state the engine has not touched. `ModelLedger` checks that claim: `sdfCorePacketGen` and
`sdfModelLinkShapedDrawPacket` have no caller but `sdfModelLinkOt`, so equal counts mean the
traversal is faithful.

**Layer map.** Game (`dds3DrawProcess`) -> scene (`sdfModelLinkOt`) -> object (`sdfCorePacketGen`) ->
per-vertex (`corePktXfmLightClip`, `corePktSkinVert*`) -> assembly (`corePktBuildFaceList`) -> emit
(`corePktEmitTris`) -> transport (`sdfGrReqDraw`). `sdfCorePacketGen` is the floor worth hooking;
below it the semantics are gone.

**When a pass ends in `corePktEmitTexTrisCtx2`, it samples a texture.** The reflection pass is a
sphere-map lookup indexed by the normal, not a highlight: its scalar goes to the **texcoord** slot at
`+0x30`, not to a color.

## Ground truth

**VU1** (`PCSX2HOOK_VU_TRACE=1` with `PCSX2HOOK_DUMP_DIR`). The image is the unit worth reading, not
the programs: SDF's MSCAL entries are dispatch stubs. `docs/vu1/` keeps the annotated transcriptions;
`scripts/vucheck.py` requires every address in a declared range to appear. In `vu1_watch.jsonl`
**`batch` is always 0**, one sample per microprogram run, not a batch index; group by the `vu` hash.

- Lighting is three parallel directions plus a base, summed against the vertex normal, each term
  clamped at zero and the whole **clamped to 255 per vertex** before interpolation.
- The per-draw color multiplies the light colors *before* the sum and the clamp.
- **Light 0's color is a difference and the base is `color[3]`**: a vertex facing light 0 reaches
  `colorBase`, one facing away falls to `color[3]`, and a mesh with no normals takes `color[4]`,
  which the packet stores as the two summed.
- The diffuse dot is taken against the packet's matrix as stored, **with no negation**.
- Light and fog packets both open with a 16-byte DMA and VIF header no HD struct has: light
  directions at +16 as a matrix whose **columns** are the light vectors, base at +64, three colors
  at +80; fog parameters at +16 and a GIF A+D write of FOGCOL at +48.

**The GS trace** (`PCSX2HOOK_GS_TRACE=<file>`) records every batch with resolved registers.

- **ALPHA and TEST come from the ordering table.** Tables 0 and 1 get ALPHA 0x44 with TEST 0x717FB
  (ATST GEQUAL 127, AFAIL FB_ONLY), table 2 gets 0x48 with 0x71801 (ATST NEVER), table 3 gets 0x42.
  **AFAIL is the depth mechanism**: FB_ONLY means a failing fragment still writes color, which is
  two GL draws. AREF does not fit this story and is unsettled.
- **The material's alpha is the vertex alpha.** `convertToGsUnits()` takes r, g and b, so passing a material
  color through it drops the channel that decides how much of an additive draw lands.
- **ZTST is not decoration.** The GS compares a z where larger is nearer, so its GEQUAL is GL's
  LEQUAL. Every 3D batch runs GEQUAL.
- **Fog is an offset plus a reciprocal**, `F = minimum + scale / w`.
- **Back-face culling is a signed area against a per-draw sign**, from `root->flags & 0x08`. GL's
  `glCullFace` reproduces it, but it must be disabled at the end of the mesh pass or it deletes half
  of every 2D quad.
- **A mesh carries less GS state than it looks like.** `corePktEmitTexTris` writes exactly TEX1, TEX0
  and CLAMP per batch, out of the material packet, and takes PRIM from `attrBits & 0x78`.
- **`rgbaq` on a batch row is vertex 0's color**, not a batch property. `rgbaLo`/`rgbaHi` carry the
  range, which is what says whether VU1 shaded a batch flat.

**Transmit order is the DMA chain**, not the link order and not the ordering tables. `sdfGrDrawFrame`
sends `*(u32*)(tag + 4) & 0x0FFFFFFF` down channel 1; walking it as a source chain and replaying the
A+D writes finds 654 of 654 linked packets a frame where walking each table's `tagHead` reached two
thirds. **The engine generates and links before `dds3DrawProcess` opens the frame**, so a per-frame
inventory resets at frame end. The renderer draws in this order; see "The frame is the chain".

**`~/projects/ps2-runtime`** has an independent GIF/VIF/GS implementation and agrees with `gif.cpp`
exactly on field extraction. The two differ on FLG 3 and both are right: it consumes a stream that is
GIF by definition, this walks memory that may not be.

**`gif.cpp` is a coverage count; `gs_decoder.cpp` is the renderer's input.** Keep the two apart:
the counts are per packet and heuristic, the decoder follows VIF1 and GIF state across transfers.

## Tooling

**Isolating a draw.** `PCSX2HOOK_GS_ONLY` and `SDF_DRAW_ONLY` take the same grammar, `i=N`,
`i=A-B`, `tbp=N`, and `prim=N` on the GS side, and cut a frame to matching draws on the emulator and
on our side. Two shots of one draw diff with no matching at all. **Suppressing a draw does not clear
the frame buffer**, so include the clear batch or the emulator's shot keeps what was there.

**Picking a draw by eye.** In a window run `]` and `[` step the highlighted mesh and `\` clears it;
the picked mesh draws solid magenta and its identity goes to the log. **Clicking** picks whatever is
under the cursor, by painting every mesh with its index into a hidden pass and reading the pixel back.
This is what found the shading seam: pick the wrong mesh, diff its shading case against its
neighbour's.

**Comparing against the hardware.** `PCSX2HOOK_GS_VERTS=1` writes the per-vertex stream VU1 produced;
`scripts/vertdiff.py` pairs a batch to the mesh whose projected centroid is nearest and diffs the
colors. **Bounds matching is not good enough**, it inflated a color spread eightfold and sent three
investigations after phantoms. `scripts/gsdiff.py` compares register state; report the pairs that
agree on the texture apart from the rest.

**Any per-mesh register comparison must count both passes on our side** or it invents a deficit. This
has bitten three times: `mtex`, `mtbp`, `malpha`.

**Measure a gradient, not a mean, when a sign is in question.** Both readings of a dot give a
plausible mean brightness; the character's own box, left half against right, settles it in one run.

**`Coverage::tbpByProducer`** records every texture a producer's packets bind, which answers "who
draws with this" directly. World geometry's TEX0 is written by VU1, so it does not appear there.

## Measuring a render change

`scripts/refdiff_all.sh` renders every scene at 512x448 and scores it against the emulated frame
buffer. **1:1 is the point**: `SDF_SCALE=1` makes the two comparable.

- **A frame score cannot localize a shading error.** It agreed with a wrong hypothesis repeatedly.
  Reach for the picker and `vertdiff.py` first.
- **An A/B against a broken baseline says nothing.** Fog measured as a 1.3 point cost while the blend
  was wrong and a 0.54 gain once it was right.
- **One entry per address in the hook table.** `installHooks` installs in order and a second entry
  replaces the first; a collision is now reported as an error.
- **Draw every frame the game produces, in order.** Skipping to the newest is a correctness bug:
  `fldDrawFrameSprite` samples the frame before it. The shot is taken the same way, the emulator
  requests the native one and waits for it, or the two are of different frames.
- **A frame acquired must be returned.** `FrameBuilder::end()` dropping its pointer without telling
  the `FrameRing` exhausts the ring after three, and it reads as a renderer freeze.
- **Do the alpha test in the GS's own integer units.** AREF 128 against an alpha normalised to
  exactly 1.0 is a knife-edge; quantise as `floor(min(a * 128, 255) + 0.5)` against the raw AREF.
- **A debug switch that changes two things at once tests nothing.** `SDF_CULL=f` flipped
  `glCullFace` *and* `glFrontFace`, which cancel out.
- **A counter placed before the value it reads is no counter at all.** Read a counter's line against
  the line that assigns the field.
- **A window run is a different instrument from a scored run** and needs the opposite defaults:
  scoring wants every frame and may stall the guest; playing wants liveness and runs at 1x.
  `Config::window` picks. `getFrame()` must not be called from a hook that only wants an existing
  frame, use `getBuilding()`.
- **The savestates cannot see a bug that takes minutes of play to appear.** Play a window run and
  watch; the guest heartbeat (`SDF_LOG=run=debug`) says whether the emulator or the renderer stopped.

## Run and debug reference

See `README.md` for the environment variables. Additionally:

- `SDF_DUMP_DECODED=<producer substring>` dumps every decoded primitive from a matching producer
  with the registers it drew under; `scripts/cropcmp.py` puts the same tile of several shots side by
  side.
- `SDF_DUMP_PACKETS=<caller substring>` hexdumps packets from a matching linker that yielded no
  geometry, following REF tags. `SDF_DUMP_MESH`, `SDF_DUMP_MESH_TYPE`, `SDF_DUMP_MESH_WORDS` and
  `SDF_DUMP_CALL` do the same for mesh bodies and DMA call chains. Reading a stream as both words and
  floats is what settles a layout.
- `SDF_MESH_ONLY`, `SDF_MESH_UPTO`, `SDF_OT_MAX`, `SDF_NO_SPRITES`, `SDF_NO_FOG`, `SDF_NO_SPECULAR`,
  `SDF_NO_FEEDBACK`, `SDF_DRAW_DIRECT_BOX`, `SDF_DRAW_SHADOWS` narrow a run or take one pass out, so
  a contribution is attributed rather than guessed. Every setting is a field of `Config`.
- The IDB for SLUS-20911 is open over MCP; `assets/SLUS-20911.map` is exported from it. A large
  `decompile` can wedge the instance, see `.claude/skills/reimplement` for restarting it.
- **One-off runs go in `dumps/scratch/`.** The top level is for output worth keeping.
- States in `states/` are curated per scene; `scripts/dump_scenes.sh` runs them all. See
  `states/README.md`. **A savestate may only be written when no hooked call is in flight**, since a
  post-handler holds the real `$ra` on the host and the state would carry only the sentinel;
  `pcsx2hook_save_state` waits for `Hooks::PendingReturns()` to reach zero and says so. **The renderer is only constructed when a window or `SDF_NATIVE_SHOT` is set**,
  so a trace-only run produces no mesh rows.
- **Develop against `smt3_menu_main`** for GIF-submitted geometry and a different producer set, and
  against a field scene for VU1-routed geometry. Hooking one scene's producers proves nothing about
  another.
- Reproducing a load: `smt3_field_underground_facility` needs `PCSX2HOOK_MCD=smt3_host.ps2`.

## Skills

- `.claude/skills/reimplement`, reimplementing a guest function completely and provably.
- `.claude/skills/vu-translate`, VU1 microcode as an annotated transcription with attribution.
