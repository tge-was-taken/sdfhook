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
memcards folder). That run decodes 33 of 33 textures and the compass draws correctly. The moon icon
and the location label do not, so their textures are registered by some path other than
`sdfTexSetEx`; that is the open end.

## What is out of reach at this level

3D world geometry is produced by VU1 from the model packets, so no GIF geometry exists at link time
(~11k triangle vertices a frame on field, the "packets not GIF lists" in the run summary).
Billboards are the same at the packet level, but they **do** have a way in that the world geometry
lacks: their producers (`drawSprite3D`, and the `dds3Billboard*` callers) take positions and sizes
outright, so the projection can be done natively instead of read back. Both need
their own engine-level entry points (`sdfCorePacketGen`, `sdfBillBoardMakeN` and the layers above),
which is where the semantics still exist. That is the next boundary to hook, not a VU1 interpreter.

## Fix wrong names in the database

Many symbols are inferred rather than known, and an inferred name that contradicts the code is worse
than `sub_239FD0`, because it is believed. Rename it in the IDB when the body says otherwise, note
the evidence and the old name in a comment there, then re-export with `scripts/export_symbols.py`
and update the hooks: `cmpDrawQuad` draws one gouraud triangle (PRIM `0x14B`, `REGS 0x515151`, three
RGBAQ and XYZ2 pairs), so it is `cmpDrawTriG`. `frFontTable` names the first field of an
`frGsTex_t`, not a table.

The IDB outlives the session, so a correction made there is worth more than one made here.

## Naming

`nb` is the new battle system, `fld` the field, `cmp` the camp/menu screens, `fcl` the facilities
(shop, terminal, recovery), `dds3` the game kernel, `sdf` the engine library. A prefix says which
scene a function serves, which is why the sample states have to cover more than one: `nbPnlSprite`
is the single biggest sprite producer in the game and never runs outside battle.

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

## Run and debug reference

See `README.md` for the environment variables. Additionally:

- `SDF_DUMP_PACKETS=<caller substring>` hexdumps packets from a matching linker that yielded no
  geometry, following REF tags to their data. This is how the A+D geometry case was found.
- The IDB for SLUS-20911 is open over MCP; `assets/SLUS_209.11.map` is exported from it and is what
  the hooks resolve names against. Record findings back into the IDB, since it outlives a session.
- States in `states/` are curated per scene and named for it; `scripts/dump_scenes.sh` runs them all
  into `dumps/<scene>/` (trace, GS trace, both screenshots, texture dumps, run log). See
  `states/README.md` for what each scene is and which one answers what.
- **Develop against `smt3_menu_main`.** It submits its geometry through GIF (28143 triangle verts a
  frame, 413 packets unread) where a field scene routes through VU1 (35481 unread), so far more of a
  frame is reachable there. Its producers are also different: `DrwSprite2D` draws the field and title
  HUDs and is unused by the menu, which draws through `cmpDrawQuad`, `frCreateDrawFontPacket`,
  `fclDrawSpriteCore` and `cmpDrawBoxG`. Hooking one scene's producers proves nothing about another.
