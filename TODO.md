# Native render: what is left

Scored by `scripts/refdiff_all.sh` (curated states, 1:1 against the emulated frame buffer) and
`dumps/scratch/sweep.sh` (the F9 captures, shot at frame 6). Every run of the sweep reports
`unsupported: 0`. A capture score moving by under 5 needs a second run first (M5).

## Scores

As of b8322c6. Signed is ours minus the emulator's.

| scene | mean abs | signed |
|---|---|---|
| battle unknown realm | 4.264 | +3.50 |
| field underground facility | 4.367 | +3.40 |
| field unknown realm | 4.226 | +3.55 |
| menu loading file 1 | 2.080 | |
| menu main | 2.018 | +1.83 |
| title screen | 1.221 | +1.05 |

Captures (`captures/labels.tsv` names them): 003 results 8.1, 005 underwater 7.3, 006 kodama 5.3,
007 amala cutscene 5.9, 008 elevator shadow 4.7, 009 megidolaon 19.1, 010 mabufudyne 31.7,
011 mediarahan 15.6, 012 samael 12.4, 013 focus 14.3, 014 mahamaeon 5.0, 015 warcry 16.1,
016 ambush 10.9, 017 19.6, 018 tarukaja 9.3, 019 tower fog 6.7, 020 demifiend eyes 3.0,
021 demon eyes 2.5.

## Open

- [ ] **V8: the 3D scenes draw about 3.5 too bright.** Near-uniform on all three channels, zero at
      black and white, largest in the upper mid-tones; the 2D scenes do not share it. Suppressing
      the mesh pass's AFAIL second draw takes the signed mean to about zero but loses real content.
      The mesh alpha is the knot: `texA * color.a * kPs2Alpha * uDiffuseAlpha` looks like a double
      scale, yet removing `kPs2Alpha` costs underground 4.27 -> 22.7. The per-batch alpha the GS
      runs (128, 0, 70, 20, 104, 49, ...) matches none of `diffuse.a`, the node's `drawColor.a`,
      their product, or VU1's `drawColour.w` (mem[1005]). Next: pick one batch of the 0 and one of
      the 70 group, isolate each with `SDF_DRAW_ONLY`/`PCSX2HOOK_GS_ONLY`, and read every alpha the
      engine has for it. Black having hidden 005 and 007 (see the feedback seed) says those draw too
      bright as well.

- [ ] **V23: `flatten` reaches only light 0.** `shading.inc` applies `keep = 1 - flatten` to light 0
      alone; the base and lights 1 and 2 go unscaled. Settle against lightVertex 0x0132..0x0172
      (0x0139, 0x0146, 0x0148..0x014B); mem[910].w is non-zero on field unknown realm.

- [ ] **P8: the reflection pass samples column 0.** `MeshLighting::specularAxis` is never set.
      `corePktEmitRefMapPass` builds it from the light packet's last TM row, rotated by
      `corePktWorldLightMtxCache` unless the mesh is weighted (0x2B8D2C..0x2B8D80, COP2). Worth 88
      pixels on field unknown realm; judge it on a scene that leans on it.

- [ ] **P3: effect polys' second texture.** `sdf3DPrimMakePoly` reports `normal` and `multiUv` when
      set; the effects' second texture (`sdfMatSetMultiTex`) is unmodelled, which is capture 009's
      yellow burst. 010, 017, 009 and 015 are the worst captures.

- [ ] **B1: battle effects.** 010's ice crystals draw dull where the emulator's are bright, and its
      bloom is missing. The character's hair, drawn from its weighted stream, is lit lighter on one
      side where the emulator's is near black.

- [ ] **V19: field shadows are decoded and off.** `fldSdwDraw`'s streams read (5700 a run) and draw
      under `SDF_DRAW_SHADOWS`. The volumes take per-vertex alpha and the chain's prim transform,
      and the fans run under DATE (P7). Drawn, they last cost about 22 points, before DATE and the
      transform were in: re-measure on capture 008 (elevator shadow) before turning them on.
      `ModelParser::isWorldSpace` is kept for the next producer that needs the test.

- [ ] **P7: 2D paths no savestate measures.** DATE is implemented but no curated frame shows a GS
      shadow. TFX 2/3, TCC and fog in the sprite shader are unexercised. REGION_REPEAT is reported.
      A mesh sampling a 24-bit texture is reported, since only the sprite shader applies TEXA.

- [ ] **V2: the bloom mask pass.** `dds3ObjectBaseModelLinkOtSvs` clears alpha, stamps the character
      into alpha under FBMSK 0x00FFFFFF, then lays a full-screen tint the alpha gates. FBMSK is
      modelled and the 2D is decoded from the chain; check that the silhouette meshes and the tint
      both draw. `SDF_DRAW_DIRECT_BOX` still gates the hooked `aDrawBoxF_Direct`.

- [ ] **M2: `cmpDrawBoxG` is traced, not hooked.** Drawn through its hook it cost the menu 6.2 points
      when the menu scored 13.6. The 2D is decoded from the chain now, so its packets draw anyway:
      confirm, then drop the hook entry and this note.

- [ ] **X10: what play meets that no savestate has.** A 12,511-frame window run reported materials
      with the multi-texture bit whose `mtexAlgo` reads as a color (0x80XXXXXX), whose `mtexAlpha`
      has reserved terms and whose second TEX0 is past 1024 texels. The guest reads the same words
      (0x2B8C6C..0x2B8C8C), so either the engine sends that pass or we read the wrong one of the
      material's two packets. The walk reads last frame's (`sdfMatListUpdatePacket` rewrites this
      frame's after the entry), so a material's first frame reads a packet nothing filled. Related:
      264 of 9240 packets on field underground were read before their rebuild (S1), and long runs
      once reported ALPHA values that are floats and ASCII (X4). Needs an F9 capture of such a scene.

- [ ] **B2: are recovered textures right?** Battle's recovered atlases once looked pale where the
      scene is dark. CSA is 0 on all of them, so it is not the CLUT offset. Compare one recovered
      texture against the same texture decoded from its asset in a scene that has both.

- [ ] **M5: the native shot sometimes lands a frame late.** 006 scores 5.3 or 9.7 and 013 10.5 or
      14.3 run to run while the emulator's shot is identical.

- [ ] **P1: re-run the chain census.** `SDF_CHAIN_CENSUS=1 SDF_LOG=census=dump`, then
      `scripts/census.py`; the last numbers predate the frame-setup, store/load and billboard
      packet types. Every packet type is now consumed or reported, and none is reported.

- [ ] **P2: packet writes guestcheck cannot account for.** `sdfModelLinkOt` (13 facts,
      0x2B2018..0x2B20E4) and `sdfModelTempPacketLinkOt` (12, 0x2B1BD0..0x2B1C9C) write
      `sdfModelNodeDrawPacket_t`, `sdfModelTempTmPacket_t` and `sdfModelDrawParmPacket_t`; each
      needs modelling or an opt-out. `rsetTest` reads the flexible `sdfGrRegPacket_t::reg[]`.

- [ ] **C1: fold `scripts/gsdiff.py` into pcsx2-hook.** Emit their manifest names from `Trace` so
      `drawmatch.py` and `gs_diff.py` run on our output, then contribute the per-field register
      diff and the texture-agreeing split.

- [ ] **T1: decode tests.** Fabricate GS and VIF inputs and assert the decode, so a translation error
      fails at its own layer; and an on-demand VIF1 run through pcsx2-hook to compare against.

- [ ] **X7: a run whose GL is llvmpipe aborts at exit**, in a thread-local destructor of an unloaded
      library. With `GALLIUM_DRIVER=d3d12` it exits cleanly.

## Done

What the code relies on that is not in CLAUDE.md, with the evidence that settled it.

- **V7: the `sdf3DSpecularTex` overlay (tbp 16216) paints nothing.** Every vertex is RGBA 0 under
  ALPHA 0x44, so the blend is Cd; `PCSX2HOOK_GS_SKIP=tbp=16216` leaves the frame bit-identical.
  Drawing it scored better only by darkening too-bright walls. Off behind
  `SDF_DRAW_SPECULAR_OVERLAY`. `sdf3DSpecularTex` (0x005518BC) points at a handle; neither 16216
  nor 16348 is ever uploaded (`sdf3DInitSpecularTex` builds them in GS memory).
- **The toon ramp (V13).** The base pass colors by `lo + delta * clamp(soften * (bias + dot))`,
  sampled per fragment because face division samples the step densely. The second pass ramps
  below algo 3 where the packet's two diffuse colors differ, takes `corePktSetConstColor` at 3,
  and keeps the first pass's colors otherwise (unsigned compare, 0x2B8C74). The terms are the
  root's under flags & 2, else the globals (0x2B1E74).
- **The feedback seed.** `fldDrawFrameSprite` blends 0.375 of the previous frame; the first frame
  after a load reads it from GS memory, or the image converges from black (020 8.7 -> 3.1).
- **Held frames.** A frame is submitted at the vsync after its GS reads, so a recovered texture
  lands in the frame that asked.
- **The upload queue.** `sdfImageSetReq` queues uploads (the title's movie) that
  `sdfImageSetTrans` sends ahead of the chain; walking it at `sdfGrReqDraw` stales what was read
  from GS memory there. Title 2.941 -> 1.221.
- **Depth save and load.** `fldDrwZSave` reads the depth buffer back and `fldDrwZLoad` loads it
  after the shadow passes; the matched pair saves and restores ours.
- **Feedback zoom.** The frame is sampled as PSMCT24, alpha from TEXA's TA0.
- **Half-pixel offset (V16).** GL samples pixel centres; half a rendered pixel on each axis was the
  measured minimum, worth 7.95 -> 5.93 on field unknown realm.
- **Glyph sheet recovery.** The sheet is uploaded before any savestate, so it is read from GS
  memory as PSMT4 through `frGsTex_t`.
- **The frame ring (X2, X3).** Every frame is drawn in order, a frame acquired is returned, and
  the native shot is taken before the emulated one; the sweep is byte-identical run to run.
- **F9 captures (X6, X8, X9).** A save waits until no hooked call is in flight, runs when the
  last pending return is taken, numbers past existing files, and reads shots with pack
  alignment 1.

## Ruled out

- **Shadowing GS memory from the uploads themselves.** 1.274 on the title against 1.221: the queue
  goes out at the next `sdfGrDrawFrame`, which can serve an earlier chain.
- **Dropping `kPs2Alpha` from the mesh alpha.** Underground 4.27 -> 22.7 (V8).
- **ALPHA 0x44 as a producer signature.** It is the scene's most common blend.
- **`fclSprite_t::X`/`Y` as (u0, v0), (u1, v1).** Menu 13.56 -> 25.12; the documented per-axis
  reading is right.
- **The ramp midpoint `(base + colorBase) / 2`.** It fit one bounds-matched sample and cost
  underground 12.8 points.
- **Dropping `diffuse` and `draw` from the no-normals path.** Battle 6.9 -> 9.4.
- **MTVU.** pcsx2-hook's override leaves it off at VM ready, whatever the log says.
- **A batch search by bbox shape as a search for a draw.** `aDrawBoxG`'s grid is one merged
  `prim 5` batch of 336 verts.
