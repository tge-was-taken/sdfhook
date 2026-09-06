# sdfhook

A native renderer for SDF, the engine SMT3 Nocturne (SLUS-20911) is built on, with pcsx2-hook as the
runner. `README.md` is the contributor doc; this file is the agent-only operational reference.

## Fail loudly

A `default:` branch that quietly picks something reasonable is how a gap survives a hundred runs
looking healthy. When a switch enumerates a set, list every case the set actually has and give the
default `reportUnsupported(what, value)`, which prints once per distinct value and totals into the
run summary. The same goes for any "treat the rest as X" fallback.

This is not defensive coding for its own sake: the whole method here is measuring what is not yet
handled, and a silent fallback is a measurement that lies. `applyBlend` mapping every unknown GS
ALPHA to ordinary alpha blending, and the texture decode treating every non-paletted format as
32-bit, both looked like working code.

## Unfinished work is written down, not left in the tree

`applyGsRegisterWrites` and `ModelParser::getGsState` were built to keep the running GS state a
frame's A+D packets leave in force, and then nothing called either of them for months. Every mesh
kept the hardcoded `sdfGsStateForOt` table instead, and AREF was wrong on two thirds of the batches
the hardware ran. Nothing was broken; something was simply never connected, and no artefact said so.

So, in order:

- **`scripts/lint.sh` names every function nothing calls.** cppcheck's `unusedFunction` is
  whole-program, which is what this needs: `-Wall` only warns about unused statics, and clang-tidy
  runs per translation unit so a public member always looks like it might be called elsewhere. Run
  it before calling a piece of work done. **The list is expected to be empty.**
- **A thing that has to stay uncalled says why**, either an inline `cppcheck-suppress unusedFunction`
  with a reason where it is complete by design (a level in a logging API, one field decoder of a
  family), or an `@todo` naming the TODO.md item where it is genuinely unfinished. `applyGsRegisterWrites`
  and `getGsState` carry the second kind and stay on the list until they are wired.
- **A deferral becomes a TODO.md entry the moment it is made**, not when someone next notices. The
  entry says what was left, what it costs, and how the cost was measured. An item nobody can put a
  number against is a note, not a task.

## Reimplement the whole function, not the parts that fire

A branch that measures zero in every sample savestate is not a branch that does not exist. Skipping
it because nothing reached it is how it gets forgotten, and the savestates are a window, not the
game: the same reasoning that makes a clean caller net mean "nothing unexpected happened in this
window" applies to our own code.

So model every path, and where a path genuinely cannot be modelled, `reportUnsupported` it so a run
that trips it says so in the summary instead of rendering short in silence. Zero is then a
**measurement**, not an assumption. `sdfModelTempPacketLinkOt` and `sdfModelLinkShapedDrawPacket`
were both skipped on exactly that reasoning and are now walked; `faceDivBias` and `corePktSoftenBias`
are reported when non-zero, which is what proves they are inert here rather than merely unseen.

## Colour stays in the shader

Never scale a colour channel on the way into a vertex buffer. Attributes carry the guest's value
normalised and nothing else; every conversion, the PS2's 128-is-opaque alpha included, happens in
the shader where the rest of the rendering decisions are. A scale applied during attribute packing
is invisible to anyone reading the shader and cannot be switched per draw.

## Names

Functions are verbs, and an accessor is `getX()`, never a bare noun: `getHandle`, `getFrame`,
`getEdges`, `getArg`. A bare `handleFor()` or `bounds()` reads as a value at the call site and hides
that work happens. Predicates keep `is`/`has` (`isRunning`, `isMapped`); a function that does
something keeps its own verb (`add`, `submit`, `record`, `enter`). This matches pcsx2-hook's C API,
where every entry point is verb-first.

"Sprite" is the term throughout, at the hooks and in the renderer: it is what the engine calls
these (`DrwSprite2D`, `nbPnlSprite`, `sdf2DAllocSprite`) and it is the ordinary game-development
word for a textured quad. Boxes and glyphs are sprites too.

Name a thing for what it holds, not for the first case it held. `Frame::textures` was a list of
textures being sent to the render thread this frame, so it is `textureUploads`; `Frame::sprites`
grew to carry boxes and glyphs, so it is `quads` of `DrawQuad`; `texId` was always a guest handle
address, so it is `texHandle`; `apiSprites` counts every draw the API hooks produce, so it is
`apiDraws`. A name that was accurate when written and quietly stopped being so is worse than a
vague one, because it is still believed.

## Comments

Never explain what or how. The code says that already, and a comment repeating it goes stale and
gets in the way. Comment only **why**, only where the reason cannot be inferred from the code, and
in as few words as will do. One line is usually enough; three is almost always too many.

No essays above a function. If a function needs paragraphs to justify itself, the explanation
belongs in this file or in a commit message, not in the source.

When a comment does document a declaration, write it Doxygen style (`///`, and `@param`/`@return`
only where the answer is not already in the signature), matching pcsx2-hook's public header. This
is a rule about form, not a reason to write more: an undocumented declaration whose name and types
already say everything stays undocumented.

## Hook handlers

Name a handler for the symbol it hooks, `<symbol>Hook`, so the table and the body cannot drift
apart. A handler that deliberately serves several symbols takes a generic name instead
(`spriteHook` covers `DrwSprite2D` and `nbPnlSprite`, which share a signature).

**Pull every argument into a named local before using it.** `c.getArg(5)` says nothing, `const u32
Col = c.getArg(5)` restates the signature where a reader can check it against the decompilation,
and a wrong index becomes visible rather than buried in an expression. Use the engine's own
parameter names, since that is what the reader is comparing against.

## Pre/post hook pairs

A post-handler often needs the arguments, which are only in registers at entry. Do not keep them in
`HookContext`: that is per-call state in a per-context field, one member per pair, and it is wrong
under recursion. Call `calls.enter(c)` in the pre-handler and `calls.leave(c)` in the post; the
stack is keyed by pc, so nothing has to be declared per hook.

## The migration boundary: hook the engine's API, not the packets it builds

**Draw work is taken from SDF's own drawing functions, by reading their arguments. It is not
reconstructed from the GIF packets those functions write.** This is the same rule the sibling p3fes
port follows: it hooks RenderWare and CRI, never VIF or GIF.

The reason is that the semantics are already stated at the API and are destroyed by the translation
below it. `DrwSprite2D(x, y, w, h, u, v, uw, vh, rgba, sdfTexHandle_t* th)` hands over the rectangle,
the UVs, the colour and the texture. `frCreateDrawFontPacket(X, Y, Width, Height, ClutNo, Col, Pri,
Filter, pUv, frGsTex_t* pGsTex, Ot)` does the same for a glyph. Recovering those numbers back out of
the quadwords is re-deriving a translation whose input was in a register, and a renderer built that
way is a worse PCSX2: it inherits every hardware constraint the port exists to escape, and can never
do better than the thing it is copying.

SDF is not built to be portable; it is written against the PS2's hardware. Undoing that is the work.
Sidestepping it by emulating the hardware is not.

So when a draw is missing, the question is **which engine function draws it**, not which packet.
`P3FES`-style caller tracing answers that: hook `sdfDrawTagLink`, resolve `$ra`, and every render
function in the frame names itself.

### Where the line is

**Hook at the lowest point where the draw semantics are still well defined**, meaning stated as
arguments rather than merely recoverable. A filled GIF sprite packet does define a draw, but only
once tags are walked and PACKED, REGLIST and XYOFFSET are decoded; that is recovering semantics,
not reading them, and it is how a renderer turns into a GS.

The useful check on any candidate is what you would have to implement to consume it. That is not
how high up the call sits. `sdfTexTransData` and `sdfPrimLinkFt4` are both engine functions one level under a
producer, and one is clearly wrong while the other is clearly right: consuming the first means
owning GS memory addressing, consuming the second means owning "a quad with four vertices".

Two criteria, in this order:

1. Take the lowest level that needs **no** PS2 hardware model. Not negotiable.
2. Among the levels that pass, take the lowest, since that owns the least game code.

The practical form: write down what you would have to implement to consume the call. If the list
contains a PS2 acronym, go up a level.

**Well defined also means complete.** One call has to carry the whole draw, not one packet of
several. Billboards are the case: `drawSprite3D` links three packets into one draw tag, a texture
reference, a transform from `sdfBillBoardMakeTMPacket`, and geometry from `sdfBillBoardMake1`.
`sdfBillBoardMake1(packet, centre, colour, uvList, xyOffsetList)` does state its arguments, but it
is handed only the geometry: the rotation reaches the transform packet through VU0's register file
from `SetRotateMatrixZ` and never passes through it. Hooking it alone gives a billboard with no
orientation, and recovering the rest means correlating packets by tag, which is reassembling the
chain. So the floor here is `drawSprite3D`, whose `Sprite3D_t` holds position, size, rotation and
colour together.

**Read what a function is handed, not what it writes** — but check that it is handed all of it. An
argument list can be perfectly semantic and still be one limb of a draw.

Judge the ownership cost by whether it is work the renderer would do anyway. `aDrawBoxF` makes us
own `x * 16 + 28672`, which is pure translation overhead and buys nothing. `drawSprite3D` makes us
own a camera-facing billboard projection, which any renderer needs regardless. The second is not a
cost in the same sense, and should not be avoided the way the first should.

This is why a hand-built packet loses to its producer even though the producer costs us arithmetic:
`aDrawBoxF` asks for `x * 16 + 28672`, while its packet asks for GIF tags, PACKED and REGLIST
layouts, XYOFFSET and 12.4 fixed point. The arithmetic is an annoyance; the packet is the whole
reason this project exists. The GIF walk stays fine as a **check**, where being hardware shaped
costs nothing because it counts vertices and never draws them.

### Three levels, and the lowest that still means something wins

There are three, not two. **Registers** (`sdf2DGetPrimPtr` writes, GIF tags) carry no meaning and
are always wrong to build on. **Primitives** (`sdfPrimLinkFt4`, `sdfPrimLinkG4`: explicit vertices,
UVs, colour, alpha) are engine code and still semantic. **Producers** (`fldDrawEATexQuad`,
`DrwSprite2D`) hold the richest intent and are game code.

Prefer the primitive level wherever the game uses it. Hooking a producer means **taking ownership
of a piece of game logic**: `fldDrawEATexQuad`'s hook had to redo a VU0 rotation by hand,
`DrwSprite2D`'s redoes its coordinate scaling, `frCreateDrawFontPacket`'s its colour swizzle. That
list only grows and is per disc, while `sdfPrim*` is a fixed, finite family the game keeps feeding
itself. Moving `fldDrawEATexQuad` down to `sdfPrimLinkFt4` deleted the rotation maths, and
`sdfPrimLinkG4` then covered `DispCommCursor` and `DispHelpPanel` without either being read.

Go up to a producer only when no primitive call carries the draw, which for this game is most of
2D: those paths take `sdf2DAllocSprite` or `sdf2DMakePrimPacketHeader` and write the registers
themselves.

`aDrawBoxF` is the case that shows how arbitrary that is. It builds PRIM `0x4D` with `RGBAQ` and
`XYZ2` over four vertices, the same draw `DispCommCursor` reaches through `sdfPrimLinkG4`. It is
not an inlined copy of that call: `sdfPrimLinkG4` writes its own DMA and GIF tags (`0x20000006`,
`0x5000000610000000`) and calls `sdfPrimSetG4Elem`, while `aDrawBoxF` goes through
`sdf2DGetPrimPacketSize` and `sdf2DMakePrimPacketHeader`. Two paths to one primitive, and the
coordinate maths the producer hook makes us own is a per-disc cost with no principle behind it.

**Inlining limits what a hook can enumerate.** "Never called" from a hook means never called *out
of line*; a copy the compiler inlined into its caller has no entry point to hook and is invisible.
So read the claim that this game barely uses `sdfPrimLinkSprite` and `sdfPrimLinkRect` as "never
called out of line", and check a body before concluding an engine surface is unused.

The coverage numbers do not have this problem. The `[todo]` list attributes vertices by the `$ra` at
`sdfDrawTagLink`, so an inlined producer is counted under whichever function inlined it, and shows
up by that name until it is hooked. Trust the `[todo]` ranking over any call-count enumeration.

### Prove the boundary is complete: net the layer below it

Hooking the right level leaves one question open, and it is the dangerous one: **is this the only
path in?** Answer it by hooking the internals *underneath* the chosen boundary purely to record who
calls them, resolving `$ra` to a symbol, and printing the edges at shutdown. Every caller should be
either the entry point already hooked or another internal of the same module. Anything else is a
registration path the hooks never saw, and it names itself.

This is cheap (the handlers only resolve a symbol and count), it stays armed, and it turns "did we
miss something" from a guess into a line in the run summary. It found `sdfTexDup` within one run:
textures registered by duplicating an existing handle never reach `sdfTexSetEx` at all. The same
technique on `sdfDrawTagLink` found `frCreateDrawFontPacket`.

**A post-handler costs the caller attribution of anything the function tail-calls.** pcsx2-hook
returns through a stub, so installing one replaces `$ra` with the return sentinel; a tail call then
hands that sentinel to its callee, whose own pre-handler reads it as the caller and reports
`0x000FFF00`. `sdfTexAlloc` tail-calls `sdfTexNew`, so watching the returns of both silently turned
`sdfTexNew <- sdfTexSetEx+0x50` into nonsense. Watch the return of a function nothing tail-calls, or
watch entry only.

Watch both ends. `observeHook`/`observePost` record the caller at entry and the result at exit, and
the results are what make the check numeric rather than circumstantial: `sdfTexNew` returned 33
distinct handles in a window where 33 textures decoded, and `sdfTexMakeGIFPacket` returned 41,
which is those 33 plus the 8 `sdfTexDup` copies. So **41 is the count a run must account for**, and
any shortfall is a texture the renderer will draw untextured.

**The net only sees what runs inside the capture window.** A savestate starts past initialization,
so a path taken once at boot leaves no edge and the net reports clean. `dds3AllocRenderTexture` is
exactly that: it creates a texture handle through `sdfTexNew` rather than `sdfTexAlloc`, and a field
savestate never runs it. A clean net means "nothing unexpected happened in this window", not
"nothing else exists" - widen the window before trusting it.

Do not let the net become the capture path. `sdfTexTransData` will happily hand over pixels, but its
arguments are a GS destination and a raw pointer, and building on it means owning the transfer layer
forever. Capture above; verify below.

### Cross-checking the decode

`~/projects/ps2-runtime` has an independent GIF, VIF and GS implementation. It is a hardware
register model rather than a packet walker, so there is little to lift directly, but it is a second
opinion on field extraction and `src/gif.cpp` agrees with it exactly: `loops = low & 0x7FFF`,
`descriptors = (low >> 60) & 0xF` with zero meaning sixteen, `format = (low >> 58) & 3`, the
descriptor slot cycling `% descriptors` across quadwords within a tag, and an A+D register address
taken from the **high** quadword's low byte with the value in the low one.

The two differ on FLG 3, and both are right for what they do: ps2-runtime consumes a stream that is
GIF data by definition and treats it like IMAGE, while this walks memory that may not be a GIF list
at all, so FLG 3 is the signal to stop. Its VIF unpack is the reference if that is ever needed, but
the boundary rule above says it should not be.

### Where the GIF decoder fits

`gif.cpp` stays, as a **coverage check and ground truth**, not as the renderer's input. It walks the
packets a frame links and counts what the GS will rasterize, by primitive type and pixel format. That
number is what says whether the API-level hooks have found every draw: sprites reached 42, 104 and 34
per frame on field, battle and menu, matching the GS trace exactly, and the gap it exposed is what
identified `frCreateDrawFontPacket` as an unhooked producer. Use it to find what is missing. Do not
grow it into a renderer.

## Textures come from sdfTexSetEx, and the handle is their identity

`sdfTexSetEx(void* aTex, s32 memMode) -> sdfTexHandle_t*` registers a texture asset. Hook it with a
post-handler: the pre-hook keeps the asset pointer, and `$v0` is the handle the call just filled in,
so one hook has both the pixels and the description of them. This is the only moment the pixels are
in main memory; afterwards the engine keeps the GS allocation alone, which is why a handle read at
draw time is useless (`image` reads `0x10000000`).

**Do not hook `sdfTexTransCLUT` or `sdfTexTransData`.** They work, but they are the transfer layer:
their arguments are a GS destination and a raw pointer, which is transport rather than meaning. The
same rule that keeps the renderer off GIF packets keeps textures off those.

**The handle is the key.** `sdfTexSetEx` returns it and `DrwSprite2D` is handed the same pointer, so
a draw finds its texture by handle address and no GS address enters the picture. Reaching for
`pixelGsHandle->addr` instead was a wrong turn: it read the same value for all 33 textures.

Reading the asset needs three facts, all from the decompilation: its data starts at
`asset + (byte[1] & 0xF0)`, the engine then skips 64 bytes, and the CLUT (if `numOfCLUT`) precedes
the pixels. Sizes come from the handle. **The asset stores pixels in plain row order** because the
GS tiles them as the transfer lands, so p3fes's `unswizzlePSMT8` must not be applied here; that was
the difference between noise and a readable atlas. The CLUT unswizzle (exchanging two of every four
8-entry groups in a 256-entry palette) still applies, and `skygl_texture_decoder.cpp` remains the
reference for it and for the RGBA5551 widening.

**There are two kinds of texture, and only one has pixels to decode.** `sdfTexSetEx` registers an
asset, via `sdfTexAlloc`. `dds3AllocRenderTexture` instead takes a buffer from `sdfGsAllocFrameBuff`
and wraps it with `sdfTexNew`: that is a render target, and it has no main-memory pixels by
construction. Draws that sample one need the renderer to render into its own target and sample that,
which is ordinary renderer work rather than a decode problem. Do not go looking for its asset.

Reproducing a load: `smt3_field_underground_facility` begins mid-load and needs no input, but the
save is on the host card, so pass `PCSX2HOOK_MCD=smt3_host.ps2` (copied from the Windows PCSX2
memcards folder). That run decodes 33 of 33 textures and the compass draws correctly.

### Recovering a texture from GS memory

A savestate taken after a load never runs `sdfTexSetEx`, so the GS holds the only copy. That is what
made four of six scenes measurable rather than one: field unknown realm went 167.60 to 11.47 and the
title screen 54.05 to 8.44. `sdfTexGetTex0Reg` says where to look, at `+32` in the handle's own
upload packet, and `pcsx2hook_read_gs_pixels` reads it. Two rules, both learned by crashing:

**Never read through the renderer's CLUT cache.** `GSClut::Read32` leaves it loaded for a TEX0 the
game never asked for, and the emulated draws after that die: the first read succeeded and every one
after it took the EE down inside `recExecute` with a corrupt stack, which reads as a recompiler bug
and is not one. Read raw values per texel through the format's own `readPixel`, the way
`GSLocalMemory::SaveBMP` does, and apply the palette on our side.

**Never read from inside a hook.** The read runs on the GS thread and blocks for it. Queue the
handle at the lookup, drain it in the runner's frame callback, and let the uploads reach the next
frame the builder opens; a draw goes untextured once and resolves after that.

**A recovered texture is not always its own.** The GS reuses memory, so a TEX0 recorded at upload
can name a region the display buffer has since taken: the battle scene recovers slices of its own
frame buffer, UI panels included, and every mesh sampling one saturates to white.
`SDF_DUMP_TEXTURES` writes the recovered ones as `gs_*.png`, which is how to tell.

## What is out of reach at this level

3D world geometry is produced by VU1 from the model packets, so no GIF geometry exists at link time
(~11k triangle vertices a frame on field, the "packets not GIF lists" in the run summary).
Billboards are the same at the packet level, but they **do** have a way in that the world geometry
lacks: their producers (`drawSprite3D`, and the `dds3Billboard*` callers) take positions and sizes
outright, so the projection can be done natively instead of read back. Both need
their own engine-level entry points (`sdfCorePacketGen`, `sdfBillBoardMakeN` and the layers above),
which is where the semantics still exist. That is the next boundary to hook, not a VU1 interpreter.

## Reimplementing a function: `.claude/skills/reimplement`

Enumerate every branch, field, global, call and cast **before** writing code, and mark each as
modelled, deliberately not modelled, or not yet. Read the disassembly wherever the decompilation
shows `cop2` or `__asm`, since COP2 bodies vanish entirely: `corePktApplyFaceDivColor` reads as an
empty loop and is a two-colour ramp.

**"Inert", "zero" and "unused" are measurements, never adjectives.** The face-division check read
`faceDivBias`, found 0, and a comment then claimed face division was inert; `sdfModelTempPacketLinkOt`
reads `faceDivSoften`, whose global default is 7. Report at the site that reads the field, or the
check never fires.

## Correct the database when you prove it wrong

Much of the IDB is inferred rather than known, and an inferred fact that contradicts the code is
worse than no fact at all, because it is believed. **Anything proven wrong here gets fixed there**,
in the same sitting: the IDB outlives the session, so a correction made in it is worth more than one
made in this tree, and a workaround left only in the source means the next reader re-derives the
same thing from the same wrong starting point.

That covers every kind of discrepancy, not just names:

- **Names.** Rename in the IDB when the body says otherwise: `cmpDrawQuad` draws one gouraud
  triangle (PRIM `0x14B`, `REGS 0x515151`, three RGBAQ and XYZ2 pairs), so it is `cmpDrawTriG`.
  `frFontTable` names the first field of an `frGsTex_t`, not a table.
- **Layouts and sizes.** `sdfMatPacket_t::texTm` was `f32**[1]` at `+0x68` with `cmd_call_spec` and
  `cmd_call_kind` behind it, neither of which had a single xref. `sdfMatSetUvwParm` (0x002B3620)
  writes six floats through that address, a 2x2 and a 2-vector offset, and stores three qwords of
  identity in its else branch, so `texTm` is `f32[6]` spanning `0x68..0x80` over both of those and
  the packet is 128 bytes, not 120. **Corrected in the IDB**, with the evidence on the two functions
  that prove it.
- **A missing header.** `sdf3DLightParallelPacket_t` was 212 bytes with `dmatag` as one word, which
  put every field 12 bytes early. `LOWORD(dmatag) = 13` is the DMA tag's QWC, and 13 quadwords is
  208 bytes, exactly `normal[3]` + `colorBase` + `color[5]` + `TM`; the writes the decompiler showed
  as `normal[0].y` and `.z` were the two VIF codes. It is 224 with a 16-byte header, and the four
  offsets measured at runtime then land exactly, the specular axis included at `TM.row[3]`.
  `sdf3DFogPacket_t` is the same shape and is 80, not 44 (QWC 4). **Both corrected in the IDB.**
- **Argument meaning.** `sdfPrimLinkFt4`'s argument 2 and `sdfPrimLinkG4`'s argument 1 are `aPrim`,
  a GS PRIM word, and reading them as an ALPHA register picked a blend at random.

Record the evidence and the old value in a comment on the IDB item, so the correction can be checked
rather than taken on faith, then re-export with `scripts/export_symbols.py` and update the hooks. A
discrepancy that cannot be proven yet stays a comment in this tree saying what is uncertain and why,
not a silent workaround.

## Naming

`nb` is the new battle system, `fld` the field, `cmp` the camp/menu screens, `fcl` the facilities
(shop, terminal, recovery), `dds3` the game kernel, `sdf` the engine library. A prefix says which
scene a function serves, which is why the sample states have to cover more than one: `nbPnlSprite`
is the single biggest sprite producer in the game and never runs outside battle.

### End to end: diff the GS state, not the picture

`scripts/gsdiff.py <trace.jsonl> <gs.jsonl>` compares the state we believe each draw runs under
against the state the hardware ran. Both traces name their fields identically (`prim`, `tme`, `abe`,
`psm`, `tfx`, `tcc`, `a`/`b`/`c`/`d`, `ate`/`atst`/`aref`/`afail`), so it is a set comparison rather
than a translation: a state in the GS trace we never produce is something unmodelled, and a state we
produce the hardware never ran is something modelled wrongly. **The goal is no state diffs at all**,
leaving only float differences in the picture.

Match on screen bounds, with the texture as a **tiebreak and never a filter**, and let one batch
match several draws. VU1 culls, clips and merges before the GS sees anything, so counts never line
up and a merged batch needs a draw a greedy match has already consumed. Get any of that wrong and
the tool invents missing geometry: a one-to-one match with a texture filter reported 302 phantom
missing batches on a scene where every one of the 1848 matches.

Never compare our triangle count against the GS's. Ours is what we submit, the GS's is what survived
VU1; the GPU culls ours after the fact, so the two are not the same quantity.

### The producer set is closed, and the draw-tag family defines it

Nothing reaches the GS without linking a draw tag, and there are exactly five entry points:
`sdfDrawTagLink`, `sdfDrawTagLink2`, `sdfDrawTagLinkRef`, `sdfDrawTagLinkRefGif` and
`sdfDrawTagLinkCall`. Hook all five, resolve `$ra`, and **the set of callers is the complete set of
producers**: a function missing from that list cannot draw. That is the worklist, and it is finite.

Rank it by packets, not by vertices. A producer whose geometry VU1 builds carries none, so a ranking
by vertices reports an empty list while thousands of packets go undrawn: `ooyTestPoly3` at 25596
packets, `fldSdwDraw` at 14962 and the billboard path at 2010 all read as nothing. Pure A+D register
writes are excluded, since a state setter legitimately draws nothing and `applyGsRegisterWrites`
already tells them apart.

This is also why the packet level is not an option for these: their geometry only exists after VU1
runs, so there is nothing to read at link time. The caller is the only place the draw is stated.

### What is left, ranked

Every render function links its packet through `sdfDrawTagLink`, so the work remaining is a
subtraction, not a search: walk each linked packet, count the vertices the GS will draw from it, and
attribute them to the caller. Any caller not in the handled set is printed at shutdown as a `[todo]`
line, ranked by vertices. The list shrinks by itself as producers are hooked, and it is per scene,
which is the only way to see that `cmpDrawTriG` owns the menu and `nbPnlSprite` owns battle.

Do not rank producers by intuition. `aDrawBoxG` is the largest gap in both the field and battle and
reads like a minor UI helper; the font path, which looks like the obvious next job, is a sixth of it.

## The 2D hooks are game code, and that is forced

SDF is the engine and `sdf*` is where a portable hook belongs, which is where the textures (
`sdfTexSetEx`), the 3D (`sdfModelLinkOt`, `sdfCorePacketGen`) and the render state (`rset*`) are
hooked. **The 2D producers are not**, and it is worth knowing why before trying to move them.

The engine does publish a semantic 2D API: `sdfPrimLinkSprite`, `sdfPrimLinkRect`, `sdfPrimLinkGt4`
and the rest of that family take positions, UVs and colours outright. Measured across the field,
battle and menu states, this game **calls almost none of it**: `sdfPrimLinkSprite` and
`sdfPrimLinkRect` are never called at all, only `sdfPrimLinkFt4` (from `fldDrawEATexQuad`) and
`sdfPrimLinkG4` (from `DispCommCursor` and `DispHelpPanel`) fire, and the main menu uses none of
them. Everything else takes `sdf2DAllocSprite` and writes the GS registers itself, in `fld`, `nb`,
`cmp` and `fcl` code.

So for 2D there is no engine-level call that carries the semantics, because the game did not put
them there. Hooking `sdf2DAllocSprite` gets an empty packet; hooking below it gets registers. The
game-level producers are the boundary by elimination, not by preference, and they are per-title:
none of `DrwSprite2D`, `nbPnlSprite` or `aDrawBoxG` would exist in another SDF game. Keep them
grouped and labelled as such, and expect to rewrite that group per disc while the `sdf*` hooks carry
over.

## Sprite producers

Two families, and both are needed to cover the sprite class:

1. Through `sdf2DAllocSprite`, which is the chokepoint worth observing to enumerate them.
   `DrwSprite2D`, `nbPnlSprite` (identical signature, different ordering table), `DrwSprite2DF`
   (float rectangle, so its integer arguments start one register over), `fldDrawFrameSprite`,
   `fldDrawBokasi`, `fldDrawFrameSpriteAlpha`, `dds3CrossfadeRun`, `dds3DrawProcess` (the screen
   blur).
2. Building their own packet: `frCreateDrawFontPacket`, `fclDrawSpriteCore`. These never reach
   `sdf2DAllocSprite`, so enumerating that chokepoint alone will miss them.

`fldDrawFrameSprite` takes no arguments and draws a fixed full-screen quad at alpha 48 over the
display frame texture: it is the frame-feedback blit, so it belongs to the render-target class and
needs a target to sample, not a texture to decode.

## Layer map

Game (`dds3DrawProcess`, once a frame) -> scene (`sdfModelLinkOt`) -> object (`sdfCorePacketGen`) ->
per-vertex (`corePktXfmLightClip`, `corePktSkinVert*`) -> assembly (`corePktBuildFaceList`) -> emit
(`corePktEmitTris`) -> transport (`sdfGrReqDraw`). `sdfCorePacketGen` is the floor worth hooking;
below it `corePktInitCb` flattens the packet into untyped scratch offsets and the semantics are gone.
`sdfModelLinkOt` itself is reimplemented rather than hooked for information; see the next section.

## The scene walk

`ModelWalk` (`src/model.{h,cpp}`) is `sdfModelLinkOt` walked again on our side: the same node list,
the same `flags & 0x11` test, the same VU0 clip-code bounds cull, the same packet chain, so what the
engine files into an ordering table we turn into meshes instead. It runs from the entry hook, over
state the engine has not touched yet.

`ModelLedger` is how that claim is checked rather than asserted. `sdfCorePacketGen` and
`sdfModelLinkShapedDrawPacket` have no caller but `sdfModelLinkOt`, so their own hooks count what the
engine reached and the walk counts what it reached; **equal counts mean the traversal is faithful**,
and a difference is our walk visiting a different set of nodes, not a different way of drawing them.
Everything else is bucketed: node outcomes, packet types, mesh shapes, and per shape a triangle
count seen against drawn. A scene that renders short names the branch that swallowed it.

## Mesh bodies

Three sources, in order of authority. The **reconstructed SMT3 HD sources**
(`~/projects/smt3hd-reconstructed-sources`) carry the engine's own struct and field names in their
headers, even though the function bodies are Il2Cpp stubs: `sdfCorePacketHeader_t`,
`sdfCorePrimHeader_t` and `sdfCorePacketCb_t` are what named `packetType`, `mpgType`, `attrBits`,
`packetBits`, `faceCount`, `pointCount`, and the cb's `facePtr` / `vertexPtr` / `normalPtr` /
`uvPtr` / `uv2Ptr` / `vertColPtr`. They carry no enum for the packet type values. The **DDS3 format
work** (`tge-was-taken/010-Editor-Templates` `templates/dds3_model.bt` and
`tge-was-taken/DDS3-Model-Studio` `Source/DDS3ModelLibrary/Models/MeshTypes/`) is authoritative for
the body layouts. The disassembly settles anything those two disagree on: the format library merges
`attrBits` and `packetBits` into one 32-bit `MeshFlags` and mis-names the high half, calling
`packetBits & 0x100` (uv) `FieldTexture` and `& 0x20` (the face list) `RequiredForField`.

The in-memory packet is the file record shifted forward by four bytes, so `MeshType8.Read`'s
offsets are ours minus four.

- **`attrBits`** is the GS PRIM word in its low bits (`IIP`, `TME`, `FGE`, `ABE`) with engine flags
  above it: `0x1000` a second UV set, `0x4000` draw back faces too. `sdfModelLinkOt` masks it with
  the node's own `drawAttr`.
- **`packetBits`** says which arrays the body carries: `0x20` faces, `0x80` normals, `0x100` uv,
  `0x800` weights, `0x1000` blend shapes.

A body is **VIF-tagged and batched**. Each vertex array is preceded by an UNPACK naming its element
width and count, so the stream states its own shape and a misread offset is refused rather than
decoded into plausible numbers. A batch caps at what fits in VU0 memory, so a mesh over that arrives
split across batches whose vertices concatenate; the triangle indices address the whole. Reading the
count from `packetBits` instead of the UNPACK is what limited the first version to small meshes.

- **Type 8, rigid**: per batch a header UNPACK (`[pointCount, 0]`), then float3 positions, float3
  normals, float2 texcoords, a microprogram kick and a flush.
- **Type 7, weighted**: the packet names the nodes it blends before its triangles, and every batch
  blends the same set, so the palette is built once. Per batch a header (`[usedNodes - 1,
  pointCount]`), then per used node a float4 position array whose w is that node's weight and a
  float3 normal array, then one float2 texcoord array for the batch.
- **Type 5, blend shape**: plain arrays, not VIF-tagged. `extData[0]` holds the shape count in its
  low half and a morph-controller index in its high half. Shape zero is the mesh and the rest are
  deltas; the controller (`root->morphList`, two `sdfModelMorphParm_t` per entry) says which two to
  mix and by how much, over `blend > 0.000001`.

- **Type 1, VIF stream**: not a generated packet at all. `sdfModelLinkOt` links it with
  `sdfDrawTagLinkCall` as a DMA source chain the hardware runs directly, so it reaches us as a run
  of REF tags rather than a body. The chain alternates the material's own packet (the buffer
  `sdfMatMakePacket` filled) with the mesh that draws under it, and `sdfModelSetNormalDrawPacket` is
  what says so: it reads the material index from the shape record's `matIndex` and the stream's
  address and length from `extData[0]` and `size`. A REF is identified by **what its data is**, not
  by its size: an address in the root's material-packet map is a material, an address opening with
  the batch-header unpack is a mesh, and anything else is setup (a 7-quadword transform block, 2950
  of them on the battle scene) that we already have by other means. The batches themselves are the
  same self-describing VIF, with a header of `[faceCount, pointCount, attrBits, packetBits]`, byte
  triangle indices local to the batch, and optional normals, one or two UV sets and vertex colours.
  These are the world geometry: they are three quarters of a field scene's triangles.

A second UV set, when `attrBits` asks for one, trails every batch as a plain array. Vertex colours
appear only in the type 1 streams, under `attrBits & 0x800`.

**A blended mesh must not write depth.** One with nothing to show still occludes everything behind
it, which is how a transparent panel in front of the camera turned a whole scene black while every
mesh in it decoded correctly. `attrBits & 0x40` (the GS ABE bit) says which meshes blend, and they
draw in a second pass with the depth write off.

**The engine's projection is not GL clip space.** Its z leaves GL's `[-w, w]` for near geometry,
which GL then throws away outright. Depth is rebuilt linearly from `w`, the view distance, and
deliberately not clamped: a vertex nearer than the near plane has to fall outside `[-w, w]` so that
GL clips it. Clamping instead pins it to the front of the depth range, where it covers the scene it
should have been behind.

**Skinning is on the GPU, and a rigid body is the one-bone case**, so `DrawMesh` always carries a
bone palette and the shader runs a single path. The format gives each bone **its own copy** of a
vertex's position and normal, already in that bone's space, rather than one position in a shared
space, so a vertex carries a position per influence and the shader sums transformed points. That
needs no bind pose, which is just as well because there is none to read. Measured over the three 3D
scenes: a palette is at most **4** bones and a vertex uses at most **3** influences, so
`kMaxBoneInfluences` of 4 has a slot of margin and the ledger prints `OVER THE CAP` if a scene ever
exceeds it.

Every draw packet in every sample scene decodes: **100% of triangles**, no rejects and no chain ref
that looked like a mesh and would not read. The field scenes come to 66,460 and 970,920 triangles a
run, the battle scene 193,933.

## What still has to be modelled

`TODO.md` holds the list and the score each item has to move. Read it before starting render work.

## Measuring a render change

`scripts/refdiff_all.sh` renders every scene at 512x448 and scores it against the emulated frame
buffer with `refdiff.py` from pcsx2-hook. **1:1 is the point**: `SDF_SCALE=1` makes the two
comparable, and anything larger is for looking at. Judging a render by eye is what let a
transparent panel writing depth pass for a depth-formula bug through a dozen runs.

**One entry per address in the hook table.** `installHooks` installs in order and a second entry for
the same address replaces the first, so a producer listed once with its real handler and again with a
bare `traceEntryHook` ends up traced and never drawn. `fclDrawSpriteCore` and `cmpDrawBoxG` were both
in that state; restoring the first took menu main from 30.02 to 13.56. A collision is now reported as
an error, and a producer that also needs tracing takes the post slot rather than a second row.

**The material's alpha is the vertex alpha.** VU1 writes it, and the GS trace confirms it exactly: a
scene's additive batches carry the alphas its materials hold. `gsRgb()` takes r, g and b, so passing
a material colour through it silently drops the one channel that decides how much of an additive draw
lands. That was worth 48 points on the battle scene.

**Draw every frame the game produces, in order.** Skipping to the newest pending frame is a
correctness bug rather than a latency tradeoff, because `fldDrawFrameSprite` samples the frame
before it: the same savestate scored 10.37 or 11.00 depending on whether the render thread had kept
up, and two scenes sat permanently in their bad mode (battle 174 against 67, menu loading 30 against
5). The shot is taken the same way: the emulator requests the native one and waits for it before
writing the emulated one, or the two are of different frames. Two full sweeps are now byte
identical, and a score that moves on its own means this broke again.

**A producer whose geometry VU1 builds contributes no GIF vertices, so a ranking by vertices cannot
see it.** The `[todo]` list attributes vertices at `sdfDrawTagLink` and ranks by them, which made the
battle scene report **nothing at all missing** while the specters' producer linked 19116 packets a run
and the billboard path another 992. Packets that yield no geometry are now ranked separately, minus
the ones that are pure A+D register writes, since a state setter legitimately draws nothing and
`applyGsRegisterWrites` already knows the difference. An empty vertex ranking means "no GIF producer
is missing", never "nothing is missing".

**Do the alpha test in the GS's own integer units.** AREF 128 against an alpha normalised to exactly
1.0 is a knife-edge: `(128/255) * (255/128)` lands either side of 1.0 per texel in float32, so the
fragments that write depth are chosen at random and everything drawn over them afterwards comes out
speckled. The protagonist rendered as a dithered ghost with only his tattoo showing. Quantise the way
the hardware does, `floor(min(a * 128, 255) + 0.5)` against the raw AREF, and it is solid. Worth 0.9
on the field scene, and it is why "missing meshes" looked like a geometry problem.

**ZTST is not decoration.** The GS compares a z where larger is nearer, so its GEQUAL is GL's LEQUAL,
and every 3D batch in the trace runs GEQUAL. Forcing GL_LESS throws away a decal that sits exactly on
the surface it decorates.

**A debug switch that changes two things at once tests nothing.** `SDF_CULL=f` flipped `glCullFace`
*and* `glFrontFace`, which cancel out, so it scored identically to the default and looked like
evidence that the winding was right. It is right, but only the fixed switch says so: inverting the
winding alone scores 38.4 against 11.1.

**Transmit order is the DMA chain, not the link order and not the ordering tables.** `sdfGrDrawFrame`
sends `*(u32*)(tag + 4) & 0x0FFFFFFF` down channel 1, and `sdfGrReqDraw` is handed that tag before we
submit, so walking it as a source chain and replaying the A+D writes gives the GS state every packet
actually ran under. It finds 654 of 654 linked packets a frame, where walking each ordering table's
own `tagHead` reached two thirds. A mesh is joined to it by address for world geometry
(`sdfDrawTagLinkCall`'s argument) and, for a generated packet, by the `sdfDrawTagLink` that follows
its `sdfCorePacketGen`, since the engine links its own output buffer and the source packet names
nothing. **The engine generates and links before `dds3DrawProcess` opens the frame**, so a per-frame
inventory has to reset at frame end or it is cleared between the two.

**A frame acquired is a frame that must be returned.** `FrameBuilder::end()` dropped its pointer
without telling the `FrameRing`, so every frame built and not submitted kept its slot for the life of
the run. Three of those exhaust the ring: `acquireBuild` then fails forever, nothing is published, and
the render thread re-presents its last frame while the guest runs on at full speed. It reads as a
renderer freeze and is a leak. It also cost `menu_loading_file_1` 2.2 points without ever showing as
a discarded frame.

**Never free a GPU resource at the engine's free.** `sdfTexFree` says the guest is done with a
handle, not that nothing is drawing it: a `sdfTexDup` copy outlives its source, and a frame already
submitted still references it. Forget the cache entry so the address can be registered again, and
replace the GL object at its next upload, where the replacement is in hand and no draw is in flight.
Deleting at the free pulled textures out from under live draws.

**A run with a window is a different instrument from a scored run, and needs the opposite defaults.**
Scoring wants every frame and can stall the guest to get one, and runs unthrottled because nothing is
watching. Playing wants liveness, so it drops the oldest frame instead of waiting, and runs at 1x
(`pcsx2hook_set_speed_limit`) or the guest outruns a vsync-locked render thread every pass. `Config::window`
picks between them. Related: `getFrame()` must not be called from a hook that only wants the frame if
one exists (`getBuilding()`), or recording a texture free becomes a two second stall.

**The savestates cannot see a bug that takes minutes of play to appear.** Every scored scene runs 120
frames from a mid-scene state and discards nothing, so a leak per frame, a handle reused after a scene
change, and a resource freed while in use are all invisible to the whole sweep. Play a window run and
watch for a freeze; the guest heartbeat (`SDF_LOG=run=debug`) is what says whether the emulator or the
renderer stopped, and that one line separated a ring leak from every other theory.

**Split matched pairs by an independent key before reading a field diff.** `gsdiff.py` pairs a GS
batch to a draw on screen bounds, and world geometry has plenty of meshes that merely cover the same
pixels, so about half the pairs are not a batch beside its own source. Reporting the pairs that also
agree on the **texture** apart from the rest deleted 1426 phantom state diffs at once, `psm`, `tcc`
and `ate` entirely: all three were "wrong" only where the pair was. The fix is not to filter the
match on a state field, which turns a state difference into a phantom missing draw; it is to keep
every batch matched and say which pairs can be trusted.

**A counter placed before the value it reads is no counter at all.** `countMultiTexture` sat in the
generated-packet path, and `finishMesh` is what reads the material and sets `multi.on` two lines
later, so the run summary said **0 meshes on, 12138 off** for months while the second pass was in
fact running on 12,036 of them. The same class as the log level below, and the same cost: two rounds
of work were spent on the multi-texture pass "never running", and a texture with 7778 GS batches was
ranked the largest gap in the project on the strength of it. **Read a counter's line against the
line that assigns the field**, and prefer counting where the value is set.

**A coverage check that reads one texture per draw cannot see a second pass.** The mesh trace row
carried `tex` and `tbp` from the first pass alone, so every multi-texture second pass looked like a
texture the hardware samples and we never do: three of battle's seven "unsampled" bases, including
the largest, were passes already being drawn. The row now carries `mtex` and `mtbp` too.

**A log level taken at construction is no level at all.** Every `LogChannel` is a file-scope static,
so it is built before `main` reads `SDF_LOG`; caching the level in the constructor silently
discarded every `log.trace` and `log.debug` in the codebase, and a finding was recorded off an
instrument that printed nothing. Levels resolve on use.

Only `smt3_field_underground_facility` scores meaningfully today: the other five savestates start
after the game loaded its textures, so `sdfTexSetEx` never fires and they render untextured.

## Ground truth from VU1

`PCSX2HOOK_VU_TRACE=1` with `PCSX2HOOK_DUMP_DIR` writes `vu1_programs.jsonl` and
`vu1_images.jsonl`; pcsx2-hook's own `scripts/vu2c.py` reads the JSONL directly and `--check`
re-derives every mnemonic against PCSX2's disassembler (0 mismatches over 1884 instructions here).
**The image is the unit worth reading, not the programs**: SDF's MSCAL entries are dispatch stubs
(`XITOP`, `IADDIU`, `JR`) and the bodies they reach never appear as a program of their own.

What the image gave, at 0x0150-0x0194, which no struct in the game states:

- **Lighting is three parallel directions, one colour each, plus a base term**, summed against the
  vertex normal, each term clamped at zero, and the whole **clamped to 255 per vertex** before the
  rasterizer interpolates it. Clamping per fragment instead washes out a gradient's dark side.
- The per-draw colour multiplies the light colours *before* the sum and the clamp.
- The vertex-colour variant scales that draw colour by **1/128** first, which is the same 128-is-one
  convention the GS modulate uses.
- **Light 0's colour is a difference and the base is `color[3]`**, both written per material at
  0x01C5 over the packet's own fields. A vertex facing light 0 reaches `colorBase`, one facing away
  falls to `color[3]`, and a mesh with **no normals takes `color[4]`** outright, which the packet
  stores as the two summed. Taking the struct's names at face value gives `colorBase` to
  `2 * colorBase` instead, which is twice the real range and is what made the character flat.

`dumps/vu1/` keeps the image, the disassembly and hand-annotated copies of these routines with the
fixed addresses named. Regenerate the raw listing with `scripts/vu2c.py` and re-check the
annotations against it after a capture from a different build.

Neither the light nor the fog packet matches the HD build's struct, because both open with a
16-byte DMA and VIF header the struct does not have. Measured offsets: light directions at +16 as a
matrix whose **columns** are the light vectors, the base at +64, the three colours at +80; fog
parameters at +16 and a GIF A+D write of GS FOGCOL at +48.

## Ground truth from the GS trace

`PCSX2HOOK_GS_TRACE=<file>` records every batch with its resolved registers, which is the fastest
way to learn what state a draw actually ran under. One field frame, 164 triangle batches: ALPHA
0x44 x158 and 0x48 x6; ABE, IIP and TCC set on all; TFX MODULATE on all; **ZMSK 0 on all**, so
hardware writes depth for every 3D batch and suppresses it through AFAIL=FB_ONLY rather than a
depth mask; FGE on 148; TEX1 linear with no mipmaps on 159; CLAMP REPEAT on 163.

**ALPHA and TEST come from the ordering table, not the draw** — but AREF does not fit that story.
`scripts/gsdiff.py` puts the hardware's AREF at 128 on most batches and 96 on some, against the 127
that table 0/1's `0x717FB` decodes to, and it is wrong on 1101 of 1546 matched batches. Treat the
sentence below as measured for ALPHA and unsettled for AREF.

 `sdfModelSetDrawEnv` builds one pair
per table and `sdfPrimSetOpaqueElem` and its three siblings hold the values, so a packet's own
`otIndex` picks them: tables 0 and 1 get ALPHA 0x44 with TEST 0x717FB (ATST GEQUAL 127, AFAIL
FB_ONLY), table 2 gets 0x48 with 0x71801 (ATST NEVER), table 3 gets 0x42 with the same. They match
the GS trace exactly. **AFAIL is the depth mechanism**: FB_ONLY means a failing fragment still
writes colour and only its depth is suppressed, which is two draws in GL, the passing half writing
depth and the failing half not. Getting this right, rather than guessing at opaque and blended, took
the field scene from 32.43 to 15.46 mean abs and its signed mean from -24.59 to -0.90; the
brightness was never a lighting or a fog problem.

**The GS trace carries each batch's post-VU vertex colour** (`rgbaq`, `r`/`g`/`b_`), which is the
same quantity the mesh vertex shader computes. Comparing the two settled the lighting sum in one
pass after a dozen measurements against the picture had not: the packet's ambient times 128 appears
as a literal batch colour, and the old model's whole range appears in no batch of any scene. Diff
that column before arguing about a lighting term from a screenshot.

**Fog is an offset plus a reciprocal**, `F = minimum + scale / w`, not a clamped reciprocal. The GS
trace settles it: the per-batch fog values are 255 on 128 of the 148 fogged batches and 118-190 on
the rest, which only the offset form fits. It was worth 0.54 once the blend was right and cost 1.3
while the blend was wrong, which is the argument for never A/B testing against a broken baseline.

**Back-face culling is a signed area against a per-draw sign.** `corePktBuildFaceList` takes the
cross product of the triangle's three transformed screen positions and keeps it when that times
`cb->backfaceSign` is `>= 0`; `sdfCorePacketGen` sets that sign from `root->flags & 0x08`, the same
flag that suppresses its linked packets. GL's own `glCullFace` reproduces it. **It appeared to cost
26 points for half a dozen measurements because it leaked into the sprite pass** and deleted half of
every 2D quad; `glDisable(GL_CULL_FACE)` at the end of the mesh pass is what makes it free. Without
it the character's face shows through the back of his head.

**A mesh carries less GS state than it looks like it should.** `corePktEmitTexTris` writes exactly
three A+D registers per batch, TEX1, TEX0 and CLAMP, all out of the material packet, and takes PRIM
from `corePktPrimBase`, which is `attrBits & 0x78`. ALPHA and TEST come from neither the material
nor the mesh: the game links them as their own A+D packets into the draw tag chain, so reading them
means tracking a running GS state across the links rather than looking them up per draw.

**The diffuse dot is taken against the packet's matrix as stored, with no negation.** The microcode
is plain about it (`vf05 = max(M * normal, 0)` at 0x0150..0x0156) and it was negated here for a
while on the strength of a 2.5 point measurement, which was real but was compensating for a lighting
sum that was wrong in a second way. Once the base and light 0 were right, the negation cost 1.0,
3.0 and 0.4 points on the three 3D scenes.

**Measure a gradient, not a mean, when a sign is in question.** Both readings of the dot can give a
plausible mean brightness, and the score alone kept the wrong one for a dozen measurements. Taking
the character's own box and comparing left half against right settled it in one run: the reference
has L/R 1.163 and the negated version gave 0.972, flat and slightly the wrong way, where the
unnegated one gives 1.185.

**Match the shader's matrix convention when hand-checking a transform.** The screen matrix is
uploaded with `glUniformMatrix4fv(..., GL_FALSE, ...)`, so GLSL reads it column-major and computes
`sum_k m[k][r] * v[k]`. A CPU probe written row-major gives the transpose: for `fldDrawPoly3` it
reported `w = -261568`, which reads as geometry behind the eye, where the shader's own value is
`w = +1074` on screen. A probe in the wrong convention invents a defect and sends the next reader
after it.

**Rank producers by the area they cover, not by packets.** `ooyTestPoly3` heads the `[todo]` list at
38,556 packets and is worth 0.010 of a point: they are 4x5 pixel triangles. Packets count calls, and
a call is not a draw's worth of pixels. The same caution as ranking by vertices, in the other
direction.

**A batch search by bounding box is not a search for a draw.** Looking for the additive light grid
as 64x32 batches found nothing across 17,839 batches and 138 frames, which read as proof the engine
never drew it. It is one merged `prim 5` batch of 336 verts with a full-screen box.

**`aDrawBoxF_Direct` belongs to the bloom mask, and is not drawn because FBMSK is not modelled.**
It builds a LINE prim (`0x41`) **with ABE set**, gives every vertex z `0xFFFFFFFF`, and files into
`aOt2D + rsetFrameMaskOtOffset` — the table bracketed by `rsetFrameMaskRgbOn`/`Off`, which set FBMSK
to `0x00FFFFFF` so a pass writes alpha and no colour. The GS trace shows that pass running: **195
batches a field frame at `fbmsk 0x00FFFFFF`**, untextured blended triangles bounded exactly on the
character (x 218..302, y 108..224). It is the character's own silhouette re-drawn into the alpha
channel, which is a local bloom mask. Drawing any of it as visible colour paints the character twice,
so the producer stays off (`SDF_DRAW_DIRECT_BOX` turns it back on) until FBMSK is modelled. Do not
read "no line batch appears in the trace" as "the hardware skips it": the pass is real, and what it
writes is invisible only because of the mask.

**`sdfDrawTagLinkRefGif` is a fourth link entry point.** `sdfDrawTagLink`, `Ref` and `Call` are not
all of them: the frame-mask packet goes through this one, so without it the FBMSK pass is missing
from the frame inventory entirely. Check for others before believing an inventory is complete.

**`aDrawBoxF_Direct` is a line, not a box.** It builds a GS LINE prim (`sdf2DMakePrimPacketHeader`
with PRIM `0x41`) out of four vertices, so the hardware draws the top and bottom edges as two
segments. Filling it painted a solid rectangle over whatever it framed. A producer's name is not
enough to know what it draws: read the PRIM it builds.

**The reflection pass is a specular highlight, not a lookup.** Decoding
`corePktEmitRefMapPass`'s VU0 ops shows one scalar per vertex, the normal dotted against an axis
built from the world light matrix and the light packet's last row, tinted by `specularColor`.

## What the 2D pass still gets wrong

Every sprite and most meshes go into ordering table 0, and the table sorts by z, so 2D belongs
*among* the meshes rather than after them. Our sprites carry no z, so a character's shadow quads
draw on top of him. The z does exist: `fldDrawFrameSprite` writes `0x3FFF` into the sprite's XYZ
through `sdf2DGetPrimPtr`, so it is readable from the packet a producer links. Submission order is
not a substitute for it and costs 25 points, because the additive full-screen light tiles are
submitted early and belong last.

**Only draw a frame the game produced.** The render thread used to redraw whatever it held whenever
nothing new had been published, which runs the frame feedback again over a frame that never
existed and compounds the tap.

## Code conventions

These are not style preferences, they are how a fact stays findable a month later.

**Read a producer's arguments against the decompilation, not its parameter names.**
`sdfPrimLinkFt4`'s argument 2 and `sdfPrimLinkG4`'s argument 1 are `aPrim`, a GS PRIM word, and
both were being fed to the blend translator as if they were an ALPHA register. It reported
`ALPHA 0x14C` unsupported and picked a blend at random for every draw those produce.

**Guest memory is reached through `guest_ptr<T>`, never a threaded accessor.** `installGuestRam`
puts the emulator's RAM base in thread-local storage once, on the CPU thread the hooks and the frame
callback share, and `guest_ptr<T>` maps its address per access: `->` and `*` to read, `slice(n)` for
a run, `valueOr()` for a copy of its own. The render thread never installs it, so a pointer that
reaches it reads as unmapped rather than as a race. `guest_ref<T>` is the same thing already checked,
for a callee that should not check again.

An accessor object threaded as a parameter spreads without limit: it reached forty-odd signatures
across five files before this, and none of them wanted it for anything but a base pointer.

**Hold the `guest_ptr`, not a host pointer.** `->`, `*` and the bool conversion are the whole
interface; there is no `get()`, so nothing can keep a raw pointer past the hook that read it and
every access re-derives the mapping. `slice(n)` is the exception, for a run whose bounds are checked
once and then indexed.

**A primitive pointer is a sign the struct is missing.** `f32x4Ptr_t` into a light packet meant the
packet's own struct was wrong and nobody had noticed; `f32Ptr_t` for a node's bounds meant there was
no bounds type. Reach for `u8Ptr_t`/`u32Ptr_t` only where the engine really does keep the data
untyped: a raw quadword span, packed pixels, a stack argument. Everything else gets a struct.

**A guest pointer is named by an alias, not by the template.** `TMX_HEADER_PTR_t`, not
`guest_ptr<TMX_HEADER_t>`; the alias sits beside the struct it points at and takes that struct's own
casing, so `sdfMatPtr_t` but `frGsTex_PTR_t`. Plain data has them too (`u32Ptr_t`, `f32x3Ptr_t` for
a run of vec3s), in `guest_ptr.h`.

**A guest address is a typed pointer, not a `u32`, and that applies to a hook's own arguments.** A
new handler is where this slips: `c.getArg(0)` returns a `u32`, and passing it on as one spreads an
untyped address through every function it reaches. Name the type at the boundary, in the handler,
and let everything below take the pointer. If several arguments turn out to be offsets into one
allocation, they are one struct and the callee should take it as such: `sdfBillBoardMakeN`'s four
pointers are `sdfBillBoardBatch_t` at +0, +240, +300 and +540, and taking them separately hid that.

**A guest address is a typed pointer, not a `u32`.** A parameter is `sdfModelRootNodePtr_t`, not
`u32 rootAddr`, so the type says what is there and the callee cannot read the wrong struct off it.
`u32` is right only where there is no struct to name: a DMA chain, a packed index list, raw pixels.
**Watch `sizeof` when a local changes from a value to a pointer**: `addr + sizeof(header)` silently
became 8 instead of 16 and cost the whole world-geometry pass, with every other counter still green.

**Log through a channel, never `printf` and never a hand-written tag.** `src/log.h` gives each
translation unit a `LogChannel log("model")`; the channel prints its own name and level, and
`SDF_LOG` sets levels globally (`SDF_LOG=trace`) or per channel (`SDF_LOG=model=trace,render=warn`).
A literal `"[model] "` in a format string is invisible to that filter and drifts from the file it
sits in. `std::format` also type-checks, where one inserted `%llu` shifts every later field in a
summary that carries dozens of counters.

**The run summary is a row of the run's own trace, not only terminal text.** Every counter goes
into one `{"ev":"summary"}` object at the end of the JSONL, zeros included, so two runs are diffed
rather than eyeballed and `traversal.agrees` replaces grepping for `MISMATCH`. The text stays: it is
what a person reads first, and a mismatch has to be visible without `jq`. One pass writes both.

**A trace observes; it never decides.** `ModelTrace::Geom` is a reporting bucket, so it is private to
the trace and the parser dispatches on the engine's own `sdfCorePacketType_t`. A switch on a
reporting enum makes the report load-bearing: renaming a bucket then changes what gets drawn.

**Dumps are JSONL, through `src/json.h`.** One object per line, each field named beside its value.
An invented column format needs a parser written twice, once here and once in whatever reads it, and
the second one is always slightly wrong.

**Depend on pcsx2-hook's public header only.** `pcsx2hook/pcsx2hook.h` is the whole supported
surface and says so; everything under `pcsx2-hook/src` is internal and will move. `JsonObject`
started as an include of theirs and is now our own copy, with no rapidjson behind it.

**No stray `std::getenv`.** Everything the environment can change is a field of `Config`
(`src/config.h`), read once by `Config::fromEnvironment()` and threaded to whoever needs it. A
`static const bool x = std::getenv(...)` inside a draw call is invisible to anyone reading the
code, cannot be reported alongside the run's other settings, and lets the first call decide for
every later one.

**Blank line around anything with a documentation comment.** A wall of commented declarations with
no separation is unreadable regardless of how good each comment is.

## Source attribution

This code reimplements a specific binary, so a reader has to be able to check any line of it against
the disassembly without first working out which function it came from.

- **Use the engine's own name.** A reimplementation of `sdfModelLinkOt` is named for it, or plainly
  derived from it. Do not invent a name for a function that already has one in the map.
- **A Doxygen comment on the declaration names the source**: `@fn` for the engine function, `@note`
  for the disc serial and the address. The serial matters because the addresses are per build. Use
  these two commands and not invented ones like `@address`, since an editor only colours the tags
  Doxygen actually defines:

  ```cpp
  /// Walks the node list the engine walks, so what it files into an ordering table we turn into
  /// meshes instead.
  /// @fn sdfModelLinkOt
  /// @note SLUS-20911 0x002B1DB8
  ```
- **Key inline logic carries the hex offset it came from**, so a reader can put the two side by
  side: `// 0x002B1E44: flags & 0x11 hides the node`. Use it where the constant or the branch is
  the part that would be hard to re-derive, not on every line.

## Run and debug reference

See `README.md` for the environment variables. Additionally:

- `SDF_DUMP_PACKETS=<caller substring>` hexdumps packets from a matching linker that yielded no
  geometry, following REF tags to their data. This is how the A+D geometry case was found.
- `SDF_DUMP_MESH=<count>` prints that many mesh bodies as words and floats at once, narrowed by
  `SDF_DUMP_MESH_TYPE` and lengthened by `SDF_DUMP_MESH_WORDS`. Reading a stream side by side as
  both is what settles a layout; guessing a stride produces numbers that look almost right.
- `SDF_DUMP_CALL=<count>` does the same for DMA call chains, following each REF tag to the data it
  names, with `SDF_DUMP_CALL_WORDS` for the length.
- `SDF_MESH_ONLY=<n>`, `SDF_MESH_UPTO=<n>` and `SDF_OT_MAX=<n>` narrow the mesh pass to one mesh, a
  prefix, or the ordering tables at or below `n`. Bisecting a bad frame costs an emulator run per
  step, which is why these are run-time rather than a rebuild.
- `SDF_NO_SPRITES`, `SDF_NO_FOG`, `SDF_NO_SPECULAR`, `SDF_NO_FEEDBACK` and `SDF_DRAW_DIRECT_BOX` each
  take one pass out, which is how a contribution gets attributed rather than guessed at.
- Every setting is a field of `Config` (`src/config.h`) and is listed there, read once at startup.
- The IDB for SLUS-20911 is open over MCP; `assets/SLUS_209.11.map` is exported from it and is what
  the hooks resolve names against. Record findings back into the IDB, since it outlives a session.
- **One-off runs go in `dumps/scratch/`.** The top level of `dumps/` is for output with a name worth
  keeping: `dumps/<scene>/` per curated state, `dumps/refdiff/` for the sweep, `dumps/vu1/` for the
  microcode. A dozen directories called `a`, `c3` and `cull2` make the ones that matter unfindable.
- States in `states/` are curated per scene and named for it; `scripts/dump_scenes.sh` runs them all
  into `dumps/<scene>/` (trace, GS trace, both screenshots, texture dumps, run log). See
  `states/README.md` for what each scene is and which one answers what.
- **Develop against `smt3_menu_main`.** It submits its geometry through GIF (28143 triangle verts a
  frame, 413 packets unread) where a field scene routes through VU1 (35481 unread), so far more of a
  frame is reachable there. Its producers are also different: `DrwSprite2D` draws the field and title
  HUDs and is unused by the menu, which draws through `cmpDrawQuad`, `frCreateDrawFontPacket`,
  `fclDrawSpriteCore` and `cmpDrawBoxG`. Hooking one scene's producers proves nothing about another.
