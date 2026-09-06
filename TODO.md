# Native render: what is left

The score is `scripts/refdiff_all.sh`, the native render against the emulated frame buffer at
512x448. A change that cannot move it does not go in, and an A/B against a broken baseline says
nothing: fog measured as a 1.3 point cost while the blend was wrong and a 0.54 gain once it was
right.

| scene | mean abs | signed | was |
|---|---|---|---|
| **title screen** | **8.44** | -4.41 | 54.05 |
| **field underground facility** | **11.00** | +5.40 | 32.43 |
| **field unknown realm** | **11.47** | +1.31 | 167.60 |
| menu loading file 1 | 30.38 | +26.56 | 30.38 |
| menu main | 35.55 | -20.56 | 35.55 |
| battle unknown realm | 174.47 | +172.08 | 174.47 |

## Open

- [ ] **C1: fold gsdiff back into pcsx2-hook instead of keeping a third script.** pcsx2-hook already
      ships `drawmatch.py` (pairs native draws to GS batches) and `gs_diff.py` (compares the census
      of render states), written for its other consumer. `scripts/gsdiff.py` here reinvented both
      because our trace uses different field names than their capture manifest (`ev: mesh|sprite`
      and `ndc` against `type: mesh|im2d`, `scr`, `gsAlpha`, `pass`).

      Two things ours does that theirs does not, both worth contributing rather than keeping:

      - **It diffs the registers of a matched pair, ranked by field.** `gs_diff.py` compares a
        census, `drawmatch.py` pairs without diffing. Matching and *then* diffing is what produced
        the `aref` finding; neither existing script would have.
      - **It scores overlap as a fraction of the smaller box.** `drawmatch.py` uses IoU with a
        containment special case, which is the same problem solved twice: a merged batch has low IoU
        against any one of its parts.

      Their hard filters on `tme` and blend are the trap I hit from the other side. Gating the match
      on a state field turns a state *difference* into a phantom missing draw, and their own comment
      says they added it because bbox alone paired unrelated washes. Whichever way it is resolved, it
      should be resolved once, in their tree.

      The cheapest route is to emit their manifest field names from `Trace` so `drawmatch.py` and
      `gs_diff.py` run on our output unchanged, then contribute the field diff to `drawmatch.py`.

- [ ] **E1: the end-to-end state diff, ranked.** `scripts/gsdiff.py <trace.jsonl> <gs.jsonl>` matches
      every GS batch to the draw that should have produced it, on screen bounds with the texture as
      a tiebreak, and diffs the registers field by field.

      **There is no geometry gap.** Every batch the hardware drew matches one of our draws:
      1848/1848 on field underground facility, 41121/41121 on field unknown realm. What looked like
      a gap was three measurement faults, not missing draws: comparing our *submitted* triangles
      against the GS's *post-cull* count, a greedy one-to-one match that consumed a draw a merged
      batch needed again, and a texture key that filtered candidates instead of breaking ties.

      What is left is entirely register state: **81 of 1848 batches agree on every register.**
      Ranked by how many matched batches each field is wrong in, on the underground facility:

      | field | wrong in | what it looks like |
      |---|---|---|
      | `aref` | 1101 | gs 128 (and 96), ours always 127 |
      | `psm` | 730 | gs 19/20, ours 0 where no material was read, 19 where gs says 20 |
      | `atst` | 587 | gs 0/1, ours 5 |
      | `tme` | 307 | gs 0, ours 1 |
      | `tcc` | 264 | gs 1, ours 0 |
      | `fge` | 209 | gs 0, ours 1 |
      | `afail` | 132 | gs 1, ours 3 |

      **`aref`: chased, and it lands on packet attribution.** Four things measured, in order:

      1. `sdfModelDrawEnvLinkOt(aOt, aEnv)` links one 96-byte env set per ordering table. Dumped and
         decoded: a GIF tag at +48 (NLOOP 2, NREG 1, REGS A+D), then TEST (register 0x47) at +64 and
         ALPHA (0x42) at +80. The values are **exactly** `sdfGsStateForOt`: OT 0/1 `0x44`/`0x717FB`,
         OT 2 `0x48`/`0x71801`, OT 3 `0x42`/`0x71801`. **The hardcoded table is right.**
      2. The mesh batches do not run it. The GS trace has them on TEST `0x5180B` (1027), `0x51801`
         (884) and `0x5160B` (358, AREF 96), against only 77 on the env's `0x717FB`. The depth test
         differs too: ZTST 2, not 3.
      3. Those are **`rsetTest`** values. `applyGsRegisterWrites` is now wired into `collectPackets`,
         which tracks the running state across a frame's whole link order, and the A+D scan
         attributes `05180B` and `05160B` to `rsetTest` directly.
      4. But `rsetTest` cannot be read where the meshes are stamped. At decode time its value is
         whatever the **2D** code last set, and stamping from it moved agreement 81 -> 175 while
         bleeding 2D state into 3D (`aref: gs=128 ours=96`, `afail: gs=1 ours=3`), so it was
         reverted. Capturing at the engine's own `sdfCorePacketGen` instead is a **no-op**: measured,
         `state.test` is still zero there, because `rsetTest` is called after the 3D walk in call
         order while its packet still lands before the model draws in ordering-table order.

      5. Attributing the link-order state to meshes by position **was tried and reverted**, measured
         with gsdiff over every field rather than on `aref` alone:

         | field | before | attempt |
         |---|---|---|
         | aref | 1101 | **798** |
         | atst | 587 | **314** |
         | afail | 132 | 1090 |
         | prim, iip | 121 | 736 |
         | tme, tcc, fge | 307, 264, 209 | 753, 736, 761 |
         | **total** | **3768** | **7780** |

         `aref` and `atst` improved and everything else collapsed. The cause is not the attribution:
         it is that settling a mesh's state needs the row written at **submit**, and a submit happens
         once per `sdfGrReqDraw` (29 times in 120 frames) while the GS draws every frame. Bunching a
         run's meshes onto 29 frame numbers stops them lining up with the batches they produced, and
         GS triangle batches start matching sprites instead (`prim: gs=3 ours=6`, +615).

      So `aref` needs a mesh's state settled **without** moving when its row is written, which means
      knowing which packet each mesh became rather than inferring it from position. That is the same
      mesh-to-packet identity the exact GS attribution needs, so **aref is blocked behind it**. The
      unblock is the pcsx2-hook change: stamp each GS batch with the DMA source address of the tag
      that fed it, since our meshes already know the packet address they came from.

      Kept from the attempt: `applyGsRegisterWrites` is now wired into `collectPackets` and tracks
      the running state across a frame's link order, and `LinkedPacket::isRef` tells a node's
      transform packet from its geometry. Both are needed by whatever finishes this.

      Corrected in the IDB along the way: `ooyTestRGBMaskOn` and `ooyTestRGBMaskOff` touch no alpha
      test at all. They call `sdfGrSetFrameMaskRGB` and `sdfGrSetFrameMaskReset`, the GS frame buffer
      write mask, and are used by `dds3ObjectBaseModelLinkOtSvs` as well as the field code, so they
      are now `dds3FrameMaskRgbOn` / `dds3FrameMaskRgbOff` with `dds3FrameMaskOtOffset`. **We do not
      model FBMSK at all**, and a draw between that pair writes no colour: worth checking against the
      `fbmsk` column the GS trace already carries.

      Still to correct: `sdfModelDrawEnv_t` is 20 bytes of pointers in the IDB, but the code strides
      **96** per set and puts the terminator at `aEnv[19].set[1]` = 96*4.

- [ ] **X2: the sweep is not reproducible.** The title screen scored 54.05 and then 8.44 from the
      same binary and the same savestate, one run apart; the battle scene has once returned an
      all-black emulated reference at frame 230 and 29.8 at every other frame tried. A score that
      moves 45 points on its own is worse than a wrong one, because every A/B in this file is
      measured against it. Suspect the shot frame racing the render thread, which is plausible now
      that recovered textures reach the frame *after* the lookup that queued them. **Fix before
      trusting any further measurement**, and re-check the numbers in the table above.

- [ ] **S1: material packets are read one generation stale.** The walk runs at `sdfModelLinkOt`'s
      entry, deliberately, so the engine and it see the same node list; but the engine's *own* next
      call is `sdfMatListUpdatePacket`, so a material whose `duty` bit for this frame is set has a
      packet we read before it was rebuilt. Measured: **264 of 9240 on field underground facility
      (2.9%), zero on the battle and unknown-realm scenes.** The fix is narrow, because almost
      everything in the packet is copied from somewhere we already read: `sdfMatUpdatePacketBasic`
      takes ambient, diffuse, specular, refmap and constant from the material itself and the three
      TEX registers from the texture handle, and `sdfMatSetUvwParm` builds `texTm` from `uvwParm`.
      Only the uvw transform actually needs redoing.


- [ ] **B1: the battle scene renders white.** Its arena geometry saturates to white over a reference
      that is nearly black, which is the whole 174. Measured, so the search is narrow:
      - The mesh pass owns it. Meshes off scores 21.4, sprites off scores 205.
      - It is **cumulative across meshes**, not one bad quad: 0..2 scores 29.8, 0..15 scores 119.7,
        0..30 scores 205.5 and saturates. No single mesh is the culprit.
      - Not fog, not specular, not the ordering tables: `SDF_NO_FOG`, `SDF_NO_SPECULAR` move it by
        0.002, and `SDF_OT_MAX=0` still scores 114.
      - Not a missing texture handle: 13950 of 14100 meshes resolve one.
      - **It is the recovered texture content.** Dumping them (`SDF_DUMP_TEXTURES` now writes the
        GS-recovered ones as `gs_*.png`) shows this scene's meshes sampling slices of the *frame
        buffer*, UI panels and all. Their TBP0 slots were reused after the upload, so the TEX0 in
        the handle's packet names memory the GS has since given to the display.
      - Next: decide per handle whether its TEX0 still names its own pixels. The frame buffer's own
        base is known, so an overlap test is cheap and would make the recovery refuse rather than
        hand back the screen. Refusing is strictly better than the current white.

- [ ] **B2: recovered textures are desaturated.** The field scene's `gs_*.png` dumps are structurally
      right (a character atlas reads as one) but pale. The CLUT read is the suspect: it goes through
      `pcsx2hook_read_gs_pixels` at CBP with CPSM and then the CSM0 unswizzle, and CSA is ignored.
      Worth checking against a texture that also decodes from its asset in the same run.

- [ ] **B3: the two menu scenes are untouched by any of this.** Both still score ~30-36 and recover
      only 2 textures each, so their producers (`cmpDrawQuad`, `frCreateDrawFontPacket`,
      `fclDrawSpriteCore`, `cmpDrawBoxG`) are asking for textures by some route the cache never sees.
      `menu_main` reports `0 glyph uploads, font clut never seen`, which is the font path entirely
      absent. Start there: it is the largest single contributor to a menu frame.

- [ ] **B4: residual on the field scenes, +5.40 and +1.31 on ~11.** The worst tiles are the HUD
      corners. What is left after the texture work is a shading difference, not a missing feature.

## Done

- **Every branch of `sdfModelLinkOt` is modelled**, including the ones no sample scene reaches, so
  zero is a measurement rather than an assumption. `sdfCorePacketGen` reconciles exactly against the
  engine on all six scenes (9240/9240, 12138/12138, 15840/15840). `sdfModelTempPacketLinkOt` (the
  root's temp packet groups, `flags & 4`) and `sdfModelLinkShapedDrawPacket` (the type 2 path, whose
  morph controller picks up to two blend targets and chains their streams behind the mesh's own) are
  both walked and both report zero walks. `faceDivBias` and `corePktSoftenBias` are read per root and
  `reportUnsupported` when non-zero: they are zero in every scene, which is why face division can be
  called inert here rather than merely unseen. Nothing suppressed by `flags & 8`, no unknown packet
  type, no unreadable node. What is left is state fidelity, not traversal.

- **Textures recover from GS memory**, which is what made four of six scenes measurable at all.
  A savestate taken after a load never runs `sdfTexSetEx`, so the GS holds the only copy.
  Two things had to be right:
  - **Never read through the renderer's CLUT cache.** `GSClut::Read32` leaves it loaded for a TEX0
    the game never asked for, and the emulated draws after that crash: the first read succeeded and
    every one after it took the EE down inside `recExecute` with a corrupt stack. The accessor now
    reads raw values per texel through the format's own `readPixel`, the way
    `GSLocalMemory::SaveBMP` does, and the palette lookup happens on our side.
  - **Never read from inside a hook.** The read runs on the GS thread and blocks for it, so it is
    queued at the lookup and drained in the runner's frame callback; the uploads reach the next
    frame the builder opens. A draw goes untextured once and resolves after that.

  Worth 167.60 -> 11.47 on field unknown realm and 54.05 -> 8.44 on the title screen. On field
  underground facility, whose 33 textures already decoded, it took the unresolved `DrwSprite2D`
  draws from 506 to 8.

- **R1-R4, the raster state.** `sdfModelSetDrawEnv` builds one ALPHA and TEST pair per ordering
  table, in `sdfPrimSetOpaqueElem` and its three siblings, so a packet's own `otIndex` picks them:
  tables 0 and 1 get `0x44` with `0x717FB` (ATST GEQUAL 127, AFAIL FB_ONLY), table 2 `0x48` with
  `0x71801` (ATST NEVER), table 3 `0x42` with the same. They match the GS trace exactly. AFAIL is
  the depth mechanism: FB_ONLY means a failing fragment still writes colour and only its depth is
  suppressed, which is two draws back to back. Blend translator ported from SkyGL's
  `gsAlphaToBlend`, unit scale from its `3d.frag`.
- **R5 culling and R6 the two-sided pass.** `corePktBuildFaceList` drops a triangle whose
  screen-space signed area disagrees with `cb->backfaceSign`, which `sdfCorePacketGen` takes from
  `root->flags & 0x08`. On by default; a two-sided draw turns it off for itself. It looked
  catastrophic for six measurements because it leaked into the sprite pass and deleted half of every
  2D quad. It is what stops the character's face showing through the back of his head.
- **Lighting**, from the VU1 image at 0x0150-0x0194: three parallel directions, one colour each and
  a base term, clamped per vertex before interpolation. The packet's directions are a matrix whose
  *columns* are the light vectors, and it stores the direction light travels, so the diffuse dot is
  taken against its negation. That sign was worth 2.5 points, and the tell was the ceiling coming
  out too bright while the walls came out too dark.
- **Fog**: `F = minimum + scale / w`, which is what the GS trace's own per-batch fog values fit.
- **M1-M4**: TEX0's TFX and TCC, TEX1's filters, CLAMP's wrap, and the material's ambient and
  diffuse scaling the two halves of the light sum. All out of the material packet, which
  `sdfMatUpdatePacketBasic` fills from the **texture handle** rather than the material.
- **M5 and M6 are one pass, and it is not a texture lookup.** `corePktEmitRefMapPass`'s VU0 macro
  ops dot each row of the world light matrix with the light packet's own axis
  (`corePktCurrLight + 208`), then per vertex dot the normal against those three scalars: one
  scalar, a specular highlight, tinted by the material's `specularColor`.
- **M7 the multi-texture pass**, through the second UV set with its own texture, colour and blend.
- **M8 the texture matrix** is inline at the material packet's `+0x68`, not a pointer as the HD
  struct says, and it is six floats: a 2x2 followed by a 2-vector offset.
- **O1 ordering** by table then z, **O2** the root's z bias, **O3 needs nothing**: every sprite batch
  in the GS trace runs `ZTST=ALWAYS`, so SDF never depth-tests 2D and the order is which ordering
  table is transmitted when.
- **`sdfPrimLinkFt4` and `sdfPrimLinkG4` take a PRIM, not an ALPHA.** Argument 2 and 1 respectively
  are the GS PRIM word; feeding it to the blend translator reported `ALPHA 0x14C` as unsupported and
  picked a blend at random. The blend comes from the running render state, and PRIM's ABE bit says
  whether the draw blends at all.
- **`aDrawBoxF_Direct` draws a line, not a box.** It builds PRIM `0x41`, a GS LINE, from four
  vertices, so the hardware draws the top and bottom edges as two segments. Filling it painted a
  solid rectangle over whatever it framed: seventeen of them at 37% black sat on the character.
  Worth 1.95 points, and drawing the two segments beats dropping the call entirely.
- **The frame feedback is correct.** `fldDrawFrameSprite` confirms the alpha is 48, the rect is the
  full screen offset by the GS XYOFFSET and the UVs span the draw space. Fixed alongside it: the
  render thread redrew the held frame whenever the game had published nothing, running the tap again
  on a frame that never existed.
- **T1 render textures and T2 movie frames do not happen.** `dds3AllocRenderTexture` and
  `sdfIpuSetChainSource` are hooked and neither fires in any sample scene.

## Ruled out

- **The additive grid `aDrawBoxG` draws is what hardware draws.** Searching the GS trace for per-tile
  64x32 batches found none across 17,839 batches and 138 frames, which looked like proof it was
  never drawn; it is in fact one merged `prim 5` batch of 336 verts, 56 quads of 6, full-screen bbox,
  `rgba 0x3FEFB994` matching our first tile exactly. One tile's arithmetic matches the GS to the
  unit. **A batch search by bbox shape is not a search for a draw.**
