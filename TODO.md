# Native render: what is left

The score is `scripts/refdiff_all.sh`, the native render against the emulated frame buffer at
512x448. A change that cannot move it does not go in, and an A/B against a broken baseline says
nothing: fog measured as a 1.3 point cost while the blend was wrong and a 0.54 gain once it was
right.

| scene | mean abs | signed | was |
|---|---|---|---|
| **title screen** | **8.43** | -4.38 | 54.05 |
| **field underground facility** | **7.83** | +3.06 | 32.43 |
| **field unknown realm** | **8.54** | +3.72 | 167.60 |
| **menu loading file 1** | **2.86** | | 30.38 |
| **menu main** | **13.56** | -5.38 | 35.55 |
| **battle unknown realm** | **9.79** | -0.01 | 174.47 |

## Open

- [x] **M1: two producers were hooked and silently unhooked again.** `installHooks` walks a table of
      `{name, pre, post}` and installs each in order, and a second entry for the same address
      **replaces** the first. `fclDrawSpriteCore` and `cmpDrawBoxG` each appeared twice: once with
      their real producer handler and once with a bare `traceEntryHook` further down the table, so
      both producers had been reduced to trace hooks and their draws never reached a frame. Nothing
      said so, because a trace hook is a legitimate thing to install.

      Restoring `fclDrawSpriteCore` alone took **menu main 30.02 -> 13.56** (signed -28.5 -> -5.4):
      it draws the menu's panels and background, which is why only one texture was ever recovered
      there while the GS sampled three.

      `installHooks` now reports a collision as an error rather than letting the later entry win,
      and it found two more the moment it was added (`rsetOtOfs`, `sdfGsAllocFrameBuff`). Each
      address now has exactly one entry, and where a producer needs tracing too it takes both the
      pre and the post slot rather than a second row.

- [x] **V1: the protagonist rendered as a dithered ghost, and it was the alpha test's precision.**
      Only his tattoo showed, speckled, with the corridor visible through him; the same artefact put
      grey streaks over the save terminal's dialogue box. It was not geometry, not culling, not the
      texture: his atlas decodes fully opaque, his UVs are sane, and no mesh in the scene is marked
      two-sided.

      His meshes run **ATST GEQUAL with AREF 128**, and the shader compared a normalised alpha
      against `128/128 = 1.0`. In float32 `(128/255) * (255/128)` lands either side of 1.0 depending
      on the texel, so pass one wrote depth for a random half of him and everything drawn over that
      speckled depth buffer stippled. Quantising to the GS's own units,
      `floor(min(a * 128, 255) + 0.5)` against the raw AREF, makes him solid: **11.06 -> 10.29** on
      the field scene, and field unknown realm's bias goes -2.42 -> +0.29.

      Two smaller things found on the way and fixed: **ZTST was ignored** (the GS's GEQUAL is GL's
      LEQUAL, since its z counts the other way, and every 3D batch runs GEQUAL), worth 0.09; and
      **`SDF_CULL=f` was a no-op** because it flipped `glCullFace` and `glFrontFace` together. With
      the switch fixed, inverting the winding scores 38.4, so the convention is right, but culling
      off still beats culling on by 0.2 and that gap is unexplained.

- [x] **X5: a savestate that cannot resume was wedging every sweep, and leaking an emulator each
      time.** `smt3_save_terminal.p2s` (captured through the broken save path) never resumes and
      spins at ~120% CPU forever. `refdiff_all.sh` runs `states/*.p2s` **alphabetically**, and that
      name sorts between `menu_main` and `title_screen`, so every full sweep hung there and never
      reached the title screen: the five-scene output I had been reading as "still running" was the
      sweep dying. Fourteen emulators had accumulated, about 1700% CPU between them.

      Fixed three ways: the state is quarantined in `states/unresumable/`, both sweep scripts wrap
      the emulator in `timeout "$SDF_TIMEOUT"` (180s, against well under a minute for 240 frames), so
      one bad state now costs one scene rather than the run, and the leaked processes are gone.

- [x] **V4: an unlit mesh was given white light, throwing the scene's tint away.** Reported from the
      picture ("missing red toned lighting on the floor"), and the shader said it outright: the lit
      branch computes `uLightBase * uAmbient` plus the directional terms, and the else branch threw
      all of it away for `vec3(1.0)`. **Every type 1 mesh is unlit** (2832 of them in battle, the
      whole arena: the floor, the walls and the lava), because "lit" is `packetBits & normals` and a
      VIF stream carries none. So the arena was rendered under white light while the scene's own
      light packet said `[0.9, 0.6, 0.3]`.

      Deleting the else branch, so an unlit mesh still takes the base term:

      | scene | before | after |
      |---|---|---|
      | battle unknown realm | 16.59 | **10.09**, signed +8.43 -> **-0.02** |
      | field underground facility | 10.17 | **7.83** |
      | field unknown realm | 10.34 | **8.46** |

- [x] **V5: light 0's colour is a difference, and the base is `color[3]`.** The character was evenly
      bright where the reference lights him hard from the front and leaves his back in shadow. Four
      candidates had already been ruled out by measurement (scaling the base, scaling the whole sum,
      the specular term, the sign of the dot), which left the inputs.

      The VU1 image says what no struct in the game does. At 0x01BB..0x01C5, run once per material
      under a flag set when one is unpacked (0x00A8) and cleared by the rebase that consumes it:

          mem[1003] = mem[1004] * mem[927]                        // base    = ambient * colour 3
          mem[924]  = mem[1002] * mem[923] - mem[1004] * mem[927] // light 0 = diffuse * colorBase - base

      `mem[923]` is `colorBase` and `mem[924..928]` are `color[0..4]`, which independently confirms
      the struct offsets. So **light 0's colour is a difference, not a colour**: a vertex facing it
      reaches `colorBase` and one facing away falls to `color[3]`, the ambient. The packet's own
      `color[0]` is scratch and never survives. `color[4]` is `colorBase + color[3]`, exactly, in
      every scene dumped, and 0x0188 shows a mesh with **no normals takes it outright** rather than
      running the sum or taking the base.

      Reading the struct's names at face value instead gave a range of `colorBase` to `2 * colorBase`,
      which is why the base looked twice too large and why no scalar on it fitted every scene.

      **Confirmed against the hardware, not the picture.** The GS trace already carries each batch's
      post-VU vertex colour. On field unknown realm the two dominant values over 34,586 textured
      triangle batches are `[38, 25, 25]` and `[128, 89, 64]`; that packet's `color[3] * 128` is
      `[38, 25, 25]` exactly and its `colorBase * 128` is `[128, 89, 89]`. Underground agrees, every
      value falling under its `[115, 128, 115]`. **No batch in either scene carries a value in the
      old model's range**, so the old reading was wrong everywhere and merely compensating.

      The materials are measured, not assumed: `ambient` is `0x80808080` on all 16,954 meshes and
      `diffuse` varies only in its alpha byte, so both coefficients are 1.0 and the two forms are
      byte-identical renders.

      `dumps/vu1/` now holds the image, the disassembly and annotated copies of these routines.

- [x] **V10: the dot was negated, and that was the rest of the lighting.** Correcting the sum (V5)
      left the three 3D scenes worse, with underground the worst at 7.82 -> 11.74 and a white haze
      over its lower two thirds. Fog and the frame feedback were both ruled out by measurement, each
      being needed rather than too strong, and the cause read as unattributable.

      It was the negation. `-uLightDir` had been in the shader since a 2.5 point measurement, and
      the microcode does not negate: `vf05 = max(M * normal, 0)` at 0x0150..0x0156. The old
      measurement was real and was compensating for the sum being wrong in a second way; with V5 in
      place the negation costs points everywhere.

      | scene | negated | as the microcode has it |
      |---|---|---|
      | field unknown realm | 9.373 | **8.347** |
      | field underground | 11.739 | **8.778** |
      | battle unknown realm | 9.209 | **8.834** |

      **A mean cannot settle a sign; a gradient can.** Both readings give a plausible average, which
      is why the score kept the wrong one so long. Taking the character's own box (215,225)-(300,340)
      and comparing the left half against the right: the reference is **1.163**, the negated version
      **0.972** (flat, and slightly the wrong way), the corrected one **1.185**. His mean brightness
      went from 62% of the reference to 75% at the same time.

      Still short: he is 75% of the reference's brightness, not 100%, and it is **not the lighting**.
      Eliminated since:

      - The sum's two ends match the hardware's own vertex colours exactly. His root resolves to
        light packet 00661D00, whose `colorBase * 128` is `[128, 89, 64]` and `color[3] * 128` is
        `[38, 25, 25]`; both are dominant values in the GS trace, 3539 and 3108 batches.
      - The deficit is his alone. The frame's signed mean is **+1.556**, so the room is if anything
        slightly bright; only his box is short.
      - The CLUT is read from the right place. His atlas is handle 006475C0 at `tbp0 0x2bce`, and
        we recover it with `cbp 0x294b cpsm 0 csm 0 csa 0`, which is what the hardware's own TEX0
        carries on all 6426 batches that sample it. The TEX0 field offsets were checked against the
        register layout at the same time and are right (CBP 37, CPSM 51, CSM 55, CSA 56).

      What is left is the CLUT's **contents** rather than its address. Every texture in this scene
      comes from GS recovery (0 decoded, 27 recovered), the read happens at a frame boundary, and
      `gs_006475C0_256x256_psm13.png` comes back close to greyscale where the reference is warm tan
      with cyan tattoos. That is B2's washed-out recovery and the same hazard CLAUDE.md already
      names: a recovered texture is not always its own. Compare the palette we read against the same
      256 entries in a GS dump taken at the moment of the draw before changing any decode.

- [x] **V11: the specters are additive, and drawing them as ordinary alpha made them plumes.**
      `sdfBillBoardMakeN` built its quads with `sdfGsStateForOt(0)`, which is ALPHA 0x44, ordinary
      source-alpha blending. The battle specters came out as opaque magenta plumes where the
      reference has compact glowing cores.

      The GS trace names the state rather than leaving it to be guessed. Taking every batch whose
      bounding box sits over the two hazes, the additive ones carry **ALPHA 0x48 with TEST 0x71801**
      at tbp 15606, 15609 and 16348, which is ordering table 2: ATST NEVER with AFAIL FB_ONLY, so
      the pass writes colour and never depth. Table 2 it is, and battle goes **9.408 -> 9.209** with
      every other scene byte identical.

      **The table is now read rather than assumed.** `dds3BillboardLinkOt` links each batch with
      `sdfDrawTagLink(node->tag[node->otIndex], ...)`, so the index is a `u16` at +60 of a node whose
      batch is at +68 and whose list is threaded at +72. Hooking it at entry and walking that list
      (the engine's own loop rewrites `next` as it consumes each node, so entry is the only safe
      moment) gives the index per batch address. It reports **ot=2 for every node**, which is what
      the GS trace said and what the hardcoded value was, so the score does not move: the value was
      right and is now derived. 12 batches a run still miss the map and take the fallback, and
      `reportUnsupported` names them rather than letting them pass.

      Still open: the specters' faces. They are not a billboard, and V6 says why nothing textures
      them: tbp 15606, 15609, 15698 and 16348 are their own glow textures and we sample none of them,
      so the quads draw as smooth blobs where the reference has structure.

- [x] **V12: GS ALPHA 0x89 was reported unsupported 109 times a battle run.** `gsAlphaToBlend`
      handled four of the term combinations and fell through to the default blend for the rest.
      0x89 is A=Cd, B=0, C=As, D=0, so `Cv = Cd * As`: the destination scaled by the source alpha,
      which GL writes as `src = GL_ZERO, dst = GL_SRC_ALPHA`. Added, and battle's unsupported report
      is now **empty**. The score does not move, so the fallback happened to be close on these draws;
      the point is that the run no longer reports a blend it is guessing at.

- [ ] **V13: the toon ramp is `corePktApplyFaceDivColor`, and it belongs to the second pass.**
      The character shades smoothly where the reference has hard two-tone bands. Measured on the
      character's own box, the reference spans 143.6 luminance against our 90.6, with sharper
      transitions (mean |dx| 3.41 against 2.07).

      **It is not the lighting sum.** `PCSX2HOOK_VU_WATCH` on the light slots confirms every input
      live: `base(1003) = matAmbient * color[3]` and `color0(924) = matDiffuse * colorBase - base`
      hold exactly, both material terms are 1.0, one light is active and its direction matches ours.
      It is not a ramp texture either: the scene has none. It is not subdivision density, since
      lighting per fragment scores 8.358/8.724/8.834 against 8.347/8.778/8.834.

      It is a second colouring pass we do not model at all. `corePktEmitMultiTexPass` recolours
      **every vertex** before the second pass draws:

          if (mtexAlgo < 3) {
              if (matPacket->diffuse != matPacket->mtexDiffuse) {
                  corePktMakeFaceDivParm(..., mtexDiffuse, ambient, constant,
                                         fdSoften + cb[+4], fdBias + cb[+8]);
                  corePktApplyFaceDivColor(cb, parm);
              }
          } else if (mtexAlgo == 3) {
              corePktSetConstColor(cb, mtexDiffuse);
          }

      `corePktApplyFaceDivColor` (0x2B72D8) is, per vertex over a 96-byte stride:

          t   = min(max(0.5 + 0.5 * point[+0x10].w, 0), 1)     // half Lambert
          col = parm[+0x10] + parm[+0x20] * t

      and `corePktMakeFaceDivParm` (0x2B6968) builds those two from the **same two endpoints as the
      main sum**, reading `corePktCurrLight` at +0x40 (`colorBase`) and +0x80 (`color[3]`):

          dark   = lerp(ambient * color[3], mtexDiffuse * colorBase, s) * constant
          +0x10  = dark
          +0x20  = min(mtexDiffuse * colorBase * constant, 255) - dark

      The same difference idiom as light 0, blended by a clamped half Lambert rather than
      `max(dot, 0)`, which is what makes the transition hard.

      **Which branch runs, measured on field unknown realm:** `mtexAlgo` 1 with differing diffuse
      x3103 and `mtexAlgo` 2 x230 take the ramp, `mtexAlgo` 3 x8665 takes the constant colour, and 2
      keep their vertex colours. So about 3,300 second passes a run are ramped and 8,700 are flat,
      and we draw all of them with the first pass's smooth `vLit`. `mtexAlgo` is now reported.

      To implement: carry `mtexAlgo`, `fdSoften`, `fdBias` and `constant` on the mesh, and give the
      second pass its own vertex colour rather than reusing the first's. The dot at `point[+0x10].w`
      is the one term still to locate; `corePktXfmLightClip` writes that buffer.

- [x] **V14: two defects found by auditing `sdfModelLinkOt` exhaustively rather than by symptom.**
      Prompted by V13, and both were invisible to every score:

      - `attr_mask = (s16)drawAttr & node->drawAttr` sign-extends, so a `drawAttr` of `0xFFFF`
        becomes `0xFFFFFFFF` and the node's high bits survive. We used `u16`, truncating them, and
        `corePktSoftenBias` rides at bit 16. Behaviour-neutral on all six scenes, and wrong.
      - The face-division check read `faceDivBias` and reported "inert" for face division as a
        whole. `sdfModelLinkOt` reads `faceDivBias` (0 here) but `sdfModelTempPacketLinkOt` reads
        `faceDivSoften`, whose global default is **7**. Each is now reported at its own read site.

      `.claude/skills/reimplement` exists so the next reimplementation enumerates every branch,
      field, global, call and cast before any code is written, and measures instead of asserting.

- [ ] **V6: eight textures the hardware samples are never sampled by us.** On
      `field_unknown_realm` the GS uses 31 texture bases and we use 23. The missing ones are not
      small: **tbp 11181 (6061 batches), 16216 (5355), 11612 (3094), 13321 (1071)**, all in small
      quads at y 64..192, which is the wall and ceiling detail rather than the floor. Our walk
      resolves a texture for almost every mesh (10915 found, 185 missing, 120 named none) and the
      material packets are current, so the meshes are taking a handle that names fewer distinct bases
      than the hardware switches between. That is the same shape as the battle floor decals, where
      the wrong half of the double buffer was the cause, and it is the next thing to chase: the
      set difference is cheap to compute per scene and names its own target.

      Measured the same way on **battle**, after the material-generation fix: the GS uses 27 bases
      and we use 24, with **7 never sampled**. One dominates everything else:

          tbp 12794  x7778      tbp 12380  x708       tbp 16348  x354
          tbp 15698  x354       tbp 12657  x169       tbp 15609  x118    tbp 15606  x118

      The last three are the specters' own additive glow textures, now drawn but untextured. **12794
      at 7778 batches is the single largest unsampled texture in any scene** and is the one to chase
      first; it also appears over the specters at ALPHA 0x44 with TEST 0x51801.

      **Three of those seven were the check's own blind spot.** The mesh trace row carried `tex` and
      `tbp` from the first pass only, so every multi-texture second pass read as a texture the
      hardware samples and we never do. The tell was in the counts: 12827 and 12794 both have exactly
      **7778** batches, and 12397 and 12380 both have **708**. Identical counts are a pair, not a
      coincidence, and a pair is one draw sampling two textures. The row now carries `mtex` and
      `mtbp`, and battle's list falls from **7 to 4**, losing 12794 (7778), 12380 (708) and 12657
      (169) at once.

      `dds3FirstProcess` and `dds3DrawFirstProcess` are ruled out as producers by decompilation:
      both are scene setup, building light, fog and viewport packets and initialising ordering
      tables, which is why they appear on the `[todo]` list at all.

      The same blind spot covers billboards, which carry no material TEX0 at all, so their `tbp` is
      always 0 in the row. Reading the TEX0 the cache recovered for each handle (`SDF_LOG=tex=debug`
      prints it) settles those without any plumbing: of the four left, **15606 and 15609 are
      recovered and bound**, so the specters' own glow is being sampled.

      That leaves two, and characterising them takes one more off the list:

      - **15698** is `prim 6` (SPRITE), 512x512 psm 0x14, boxes a median 238x12 wide: the **font
        sheet**, which we do recover, under a reserved handle that carries no tbp. Not a gap.
      - **16348** is `prim 3`, 128x64 psm 0x13, ALPHA 0x48 with a FIX of 0x80, a median **3x6**
        pixel box, 3 batches a frame. Additive glints beside the specters, and the one thing here
        genuinely not drawn.

      So the honest count is **one small element**, not seven textures. Wire the recovered tbp into
      the trace row before trusting any future number here, or it will drift again.

- [x] **V6b: the floor decals are drawn, and the counter that said otherwise read too early.**
      The arena floor looked short of its blood splotches, and the run summary agreed: **multi-texture
      pass: 0 meshes on, 12138 off**, every run, every scene. Two rounds of work were spent on that
      pass "never running".

      `countMultiTexture` sits in the generated-packet path, and `finishMesh` is what reads the
      material and sets `multi.on` **two lines later**. The counter was reading the field before it
      was assigned. Moved to where the value is set, the same run reports **12,036 meshes on, 102
      off**: the pass has been running the whole time, and nothing about the render changes (battle
      8.834 either way).

      This is the same class as the log level resolved at construction, and it cost more: it also
      made the largest apparent gap in the project, tbp 12794 at 7778 GS batches, look like an
      unhooked producer. See V6.

- [ ] **V2: the streaks are the bloom mask, and FBMSK is the thing to model.** The dark horizontal
      bars across the protagonist and over the save terminal's dialogue box are `aDrawBoxF_Direct`:
      `SDF_NO_SPRITES=1` renders him perfectly, and its sprites are 2 pixels tall and a third of the
      screen wide, which is what its LINE prim produces as a pair of edges.

      **It is part of a local bloom effect, not decoration.** The decompilation:
      `sdf2DMakePrimPacketHeader(hdr, 77, 2, 0x41, 4)` is a LINE prim **with ABE set**, every vertex
      gets z `0xFFFFFFFF`, and it files into `aOt2D + rsetFrameMaskOtOffset`. That table is the one
      `rsetFrameMaskRgbOn`/`Off` bracket, and they set FBMSK to `0x00FFFFFF` through
      `sdfGrSetFrameMaskRGB`, so everything in it writes alpha and no colour. The GS trace confirms
      the pass runs: **195 batches a field frame at `fbmsk 0x00FFFFFF`**, untextured blended
      triangles bounded exactly on the character. It is his silhouette drawn into the alpha channel.

      So the earlier note here ("the hardware never draws them") was wrong in its reasoning even
      where the measurement held: no *line* batch is recorded, but the pass those lines belong to is
      real and substantial. **We model no part of FBMSK**, so every draw in that pass is wrong for us:
      the triangles paint the character a second time and the lines paint bars. The producer is off
      by default (`SDF_DRAW_DIRECT_BOX` re-enables it) as a stopgap, worth 10.29 -> 10.17.

      **Now modelled, and it changes nothing here, which is itself the finding.** `sdfGsState_t`
      carries `frameMask`, the mesh pass applies it with `glColorMask`, and
      `rsetFrameMaskRgbOn`/`Off` (still `ooyTestRGBMaskOn`/`Off` in the exported map) set it as engine
      state. The pair brackets its pass in **call order**, around `mdlLink`, which is where our walk
      runs, so it needs no transmit-order settling. Every scene scores identically and **not one mesh
      of ours carries the mask**, so we are not drawing the character twice: the silhouette pass is
      not walked at all.

      `dds3ObjectBaseModelLinkOtSvs` is what draws it, and reading it is worth the space:

          rsetOtOfs(63); rsetAlpha(0); rsetFrameMaskRgbOn();
          rsetTest(1, 1, 128, 2, 0, 0, 1, 1);        // AFAIL ZB_ONLY, ZTST ALWAYS
          aDrawBoxFZ(0, 0, 512, 224, ..., 0, 0, 0, 128);   // clears alpha over the screen
          rsetOtOfs(64/65); rsetFrameMaskRgbOn();
          mdlLink(model, ...);                       // the character again, into alpha only
          rsetOtOfs(67); rsetFrameMaskRgbOff();
          aDrawBoxFZ(0, 0, 512, 224, ..., 112, 102, 88, 48);  // the tint the mask gates

      So the effect is: clear alpha, stamp the silhouette into alpha, then lay a full-screen tint
      that the alpha gates. **We draw neither the silhouette nor the tint**, which is why the mask
      has nothing to apply to. Walking `mdlLink`'s link and drawing the tint quad is the work.

      One dead end recorded so we do not repeat it: `sdfGsState_t` now carries `frameMask`, the A+D
      scan reads GS FRAME, and the mesh pass applies it with `glColorMask`. **`sdfDrawTagLinkRefGif`
      turned out to be a fourth link entry point** that nothing hooked, which is why the FBMSK packet
      was absent from the inventory; it is hooked now. The value still does not arrive, because
      `sdfGrSetRefGifPacket` points the packet at a quadword inside the draw env rather than carrying
      it, so `applyGsRegisterWrites` walks past it. Follow that reference and the mask settles like
      ALPHA and TEST already do.

- [ ] **M2: `cmpDrawBoxG` is correct and still costs 6.2 points, so it is traced rather than drawn.**
      It emits a **512x448 gouraud wash once a frame** plus the menu's row boxes and selection bar,
      all of which the reference has, and drawing them takes menu main **13.56 -> 19.76**. The
      geometry, the corner order (TL, TR, BL, BR) and the colour unpacking all match `cmpDrawBoxG`
      and `cmpExpandColor` exactly, and the blend mode makes no difference.

      **Correction: it does not cause the garbage rows.** They are still there with the producer
      traced rather than drawn; the earlier claim came from comparing against a render taken before
      other fixes. See M3, which localises them.

- [ ] **M3: eight garbage rows in the menu's empty item list are `fclDrawSpriteCore`.** They are its
      **158x21 sprites, eight a frame**, exactly where the reference has empty rows, and they read as
      small mirrored glyphs, which says the sampled rectangle is wrong rather than the geometry.

      **Rejected, so nobody repeats it**: `fclSprite_t::X` and `Y` are documented as (u0, u1) and
      (v0, v1), and the decompilation appears to say otherwise, writing `X`'s halves into the first
      UV register and `Y`'s into the second, which would make them (u0, v0) and (u1, v1). Changing to
      that reading scores **13.56 -> 25.12** (signed +21.9), so the documented per-axis reading is
      right and the decompiler's variable naming misled the derivation. Whatever is wrong with these
      sprites, it is not the axis pairing.

- [ ] **C1: fold gsdiff back into pcsx2-hook instead of keeping a third script.** Now with a third
      thing of ours worth contributing: **splitting matched pairs by whether they agree on the
      texture**. Their `drawmatch.py` filters on state fields to stop bounds pairing unrelated
      draws, which turns a state difference into a phantom missing draw; reporting the two
      populations instead keeps every batch matched and still says which pairs can be trusted. It
      deleted 1426 phantom state diffs here.

      pcsx2-hook already ships `drawmatch.py` (pairs native draws to GS batches) and `gs_diff.py` (compares the census
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

      What is left is register state, and **most of what this table used to say was the matcher, not
      the renderer.** A batch is paired to a draw on screen bounds, and the world geometry has plenty
      of meshes that merely cover the same pixels, so about half of the pairs are not a batch beside
      its own source. `gsdiff.py` now splits them: a pair that also agrees on the **texture** is the
      source draw, and the rest are reported apart. On the underground facility, 1848 batches:

      | paired by | pairs | agree on every register |
      |---|---|---|
      | texture | 857 | 81 |
      | bounds alone | 991 | 0 |

      | field | texture-agreeing | bounds only |
      |---|---|---|
      | `aref` | **730** | 887 |
      | `tme` | 137 | 342 |
      | `b` | 123 | 127 |
      | `afail` | 98 | 180 |
      | `a` | 91 | 21 |
      | `atst` | 70 | 785 |
      | `abe` | 68 | 16 |
      | `d` | 46 | 9 |
      | `fge` | 30 | 278 |
      | `prim`, `iip` | 22 | 189 |
      | `psm` | **0** | 979 |
      | `tcc` | **0** | 359 |
      | `ate` | **0** | 88 |

      **`psm`, `tcc` and `ate` are not wrong anywhere a pair can be trusted.** Every one of their
      1426 differences is a batch sitting next to a draw that is not its source, so the previous
      entry here ("psm 730, needs no attribution") was an instruction to fix a bug that does not
      exist. Any field whose `bounds only` column dwarfs its `texture` one should be read the same
      way.

      What that leaves is small and concentrated: of the 857 trustworthy pairs, **615 differ in
      `aref` and in nothing else**. Settling `aref` alone would take full agreement from 81 to about
      700, and every other field together is worth less than half of it.

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
      knowing which packet each mesh became rather than inferring it from position.

      6. **The pcsx2-hook attribution was built for that and does not work here.** `Hooks::OnGifSource`
         at the GIF DMA tag, a quadword counter on each side so the buffered halves need not pair by
         call, and `GsDraw::srcQword` naming a batch's position in that stream. The GS half counts
         correctly; the EE half **never fires**, from either `WRITERING_DMA`'s body or its entry.
         Reverted rather than left as machinery nothing reaches.

      What that says is worth more than the mechanism: **the geometry does not arrive over the GIF
      DMA.** VU1 output reaches the GS as `Transfer<3>` through the MTGS ring, so there is no DMA
      source address to attribute it to, and `GsTracker::onKick`'s `madr` names a whole chain, which
      for a frame-at-a-time engine is the whole frame. **Attribution has to come from the VU1 side.**

      7. **The ordering table does not determine it either.** Four distinct TEST values on the mesh
         batches against four ordering tables looked like a lookup, so the two were cross-tabulated
         over a matched run: **ot 0 alone maps to five of them** (0x5180B x540, 0x51801 x477,
         0x5160B x172, 0x717FB x34, 0x71801 x15). No per-table map can be right, and the last cheap
         hypothesis is gone. `aref` needs the state a draw actually carried, per draw.

      8. **Settling the state in link order, joined to the mesh row by draw sequence, is also worse.**
         This was the variant that keeps the mesh row where it is decoded, so frame alignment
         survives, and carries the settled ALPHA and TEST as their own row merged on the sequence.
         It works mechanically: 868 states settle and the largest `aref` bucket falls 479 -> 420.
         Measured over every field it is still a regression, **3768 -> 4516**, and `aref` as a field
         *rises* to 1401. The bucket counts flatter; the field totals are the truth.

      **The reason all six fail is one fact: link order is not transmit order.** `collectPackets`
      walks packets as they were linked, but the ordering tables sort by the tag's own z before they
      transmit, and `rsetTest`'s A+D packets ride in a different tag from the mesh they apply to. The
      state a batch runs under is settled by the **flush**, not by the link, so no walk of the link
      stream can produce it.

      9. **The flush cannot be modelled from `sdfSortOtAvlRegist` either.** It is the obvious way in
         and it is hooked already, but it fires **86 times against 8003 linked packets**, and on the
         busiest frame **once for 653**. Almost every tag is filed by a different `regFunc`: it is a
         function pointer on `sdfGrOt_t`, resolved per table at runtime, and none of those
         implementations is identified or hooked.

      10. **The transmit order is readable, and the link stream cannot be joined to it.** `regFunc`
          turns out not to be needed: `sdfGrOt_t` holds its tags as a list (`tagHead` -> `next`), so
          the order a table transmits them in can simply be walked. That was built. It finds the
          tables, walks **41 tags**, and maps **132 geometry packets to their meshes** — and then
          matches nothing, because every one of a frame's **648 linked packets carries only 2
          distinct tag addresses**, and neither appears in any table's list.

          So `sdfDrawTagLink`'s `$a0` is not the tag those tables hold, or the tags are replaced
          between the link and the flush. **That is the next thing to find out**, and it is a
          question about the engine rather than about our code: read `sdfPrimAllocDrawTag` and
          whatever `regFunc` files a tag, and establish what identity survives from link to flush.

      11. **The transmit-order replay works and is still a regression.** The tag identity question
          from (10) was a bug of mine, worth knowing: `LinkedPacket` already carried `isRef`, so
          adding `tag` before it turned `{addr, caller, true}` into **tag = 1**, which is why a frame
          showed exactly two distinct tags. With both initialisers fixed the model runs as designed:
          walk `sdfGrOt_t::tagHead -> next` per table, replay each tag's packets, and settle the mesh
          reached. It settles **868** of 1320 generated meshes and takes batches agreeing on every
          register from 81 to 140.

          It is still rejected. Field totals go **3768 -> 4619**, `aref` as a field *rises* to 1419,
          and the 81 -> 140 gain does not reproduce under a matcher without the texture tiebreak
          (0 fixed, 0 broken). A partially settled frame is worse than a uniformly wrong one: the
          settled meshes get varied state while the rest keep the table default, so more fields
          differ even as more batches agree completely.

          **The remaining question is coverage, not correctness**: why only 868 of 1320 geometry
          packets appear in any model table's tag list. That is where to resume.

      12. **`SDF_LOG` never reached a channel, and that is why some of the above was measured
          blind.** `LogChannel` took its level in its constructor, and every channel is a file-scope
          static, so all of them were built before `main` could read `SDF_LOG`. Every `log.trace` and
          `log.debug` in the codebase was discarded no matter what the spec said. Finding 4's "the
          hook shows `state.test` is still zero" was read off an instrument that printed nothing:
          `rsetTest` fires **170 times with `05180B`** on the field scene. Levels now resolve on use,
          against a generation counter `configureLogging` bumps.

      13. **Battle already agrees. The field is the whole of `aref`.** Cross-tabulating the GS TEST
          each batch ran against the draw it matched, restricted to generated packets (types 7 and 8,
          the `sdfCorePacketGen` output):

          | scene | GS (atst, aref, afail) on generated meshes |
          |---|---|
          | field underground | `(5, 128, 1)` on **all 479**, every frame, whatever the table |
          | field unknown realm | `(5, 128, 1)` on 3628 of 3642 |
          | battle | `(5, 127, 1)` x73 and `(0, 128, 1)` x11 per frame |

          Battle's two values are exactly `sdfGsStateForOt`'s tables 0/1 and 2/3, so **the hardcoded
          table is right and battle needs no work**. The field runs one value that is not in the
          table, uniformly, and is not per draw: **`aref` is a per-frame base state, not per-packet
          attribution.** That is a much smaller problem than findings 5-11 assumed.

      14. **What writes it, and why decode time cannot see it.** `dds3InitDrawEnvSub` builds the
          frame's standard draw env through `sdfGrSetStdDrawEnv` and writes `0517FB` at the head of
          the frame: that is `0717FB` with **ZTST 2 instead of 3**, and it is where the field's `05`
          family comes from. The remaining bit, AREF 127 -> 128, is a `rsetTest(aref=128)` whose
          packet transmits before the model tables. In link order one field frame reads:

              0    dds3InitDrawEnvSub   0517FB
              1-33 sdfModelSetDrawEnv   0717FB / 071801 / 071801, eleven groups
              34   dds3LinkZClearDrawTag
              39   rsetTest             05180B      <- what every mesh in the frame runs
              41   fldDrawFrameSprite               (the feedback blit, drawn behind the 3D)
              44+  rsetTest             05160B / 05180B alternating
              116  dds3DrawFade         031003

          The running state at mesh decode is `031003`, the **previous** frame's last write, which is
          why sampling at decode gives 2D state a frame stale. The value the meshes run is set later
          in the frame's call order and earlier in its transmit order.

          So the shape of the fix is known and unchanged: it needs transmit order, which is finding
          11's replay. What is new is the size of the target. It is **one value per frame** for the
          whole 3D pass, not one per packet, so the replay only has to place the model tables in the
          chain rather than settle 1320 packets individually. `sdfGrDrawFrame(chainAddr)` is already
          hooked and its chain names the tags in transmit order: walking that, rather than each
          table's `tagHead`, is the next thing to try.

          Worth keeping in proportion: AREF 127 against 128 is a one-level alpha threshold and is
          worth nothing visually. It is worth fixing because E1 asks for no state diffs and because,
          once the matcher stopped inventing them, it is 85% of what is left.

      15. **The type 1 chain does not carry the state either.** The DMA call chain a world-geometry
          node links alternates material packets with meshes and a 7-quadword transform block, and
          reading GS state out of it while walking would have needed no transmit-order model at all.
          Measured: **not one A+D write in any setup REF across the run.** The chain carries geometry
          and setup, never TEST or ALPHA.

      16. **Done: the transmit order is the DMA chain, and it settles the state.** Findings 10 and 11
          walked each ordering table's own `tagHead` and reached 868 of 1320 packets. The chain the
          hardware actually runs is simpler and complete: `sdfGrDrawFrame` sends
          `*(u32*)(tag + 4) & 0x0FFFFFFF` down channel 1, and `sdfGrReqDraw` is handed that same tag
          before we submit the frame. Walking it as a DMA source chain (CNT, NEXT, REF, CALL, RET,
          END) and replaying `applyGsRegisterWrites` at every tag finds **654 of 654 linked packets,
          every frame**. The coverage question that finding 11 ended on does not exist at this level.

          Two things had to be fixed for the join to be exact:

          - **`sdfDrawTagLinkCall` was never hooked.** The world geometry is linked as a DMA call
            chain rather than a packet, so three quarters of a field scene's triangles were absent
            from the frame inventory entirely. A type 1 mesh now settles by its own chain address,
            with no ordering assumption at all.
          - **A generated packet is found by the link that follows it.** The engine builds its own
            output buffer, so nothing about the source packet names the address it links; pairing
            each `sdfCorePacketGen` with the next `sdfDrawTagLink` gives it exactly. The inventory
            resets at frame end rather than at `dds3DrawProcess`, because the engine generates and
            links **before** `dds3DrawProcess` opens the frame, which was quietly clearing the list
            between the two.

          Measured on the underground facility, texture-agreeing pairs:

          | | before | after |
          |---|---|---|
          | agree on every register | 81 of 857 (9.5%) | **641 of 876 (73.2%)** |
          | `aref` wrong | 730 | **185** |

          `SDF_NO_TRANSMIT_STATE=1` turns it off and reproduces the old numbers exactly, so the
          change is attributable rather than argued. Settling TEST alone gives an identical result,
          which says the ALPHA a mesh transmits under already equals its ordering table's.

          **It costs 0.78 on the field underground image and gains 0.28 on field unknown realm**
          (10.37 -> 11.15, 11.44 -> 11.16), the other four scenes unchanged. That is a real trade and
          it is kept, because the previous state was measurably wrong and A/B against a broken
          baseline is how the fog offset was lost for a dozen runs. The lead worth following: ZTST is
          ignored by the renderer, `aref` 127 against 128 is a one-level threshold, so the cost is
          almost certainly the **26 packets a frame at `05160B`, AREF 96**, whose alpha-test
          threshold moves from 0.99 to 0.75 and which therefore write depth over far more of
          themselves than before. Whether that exposes the known 2D/3D ordering gap or a depth bug of
          our own is the next thing to measure.

          **45 of 132 generated packets a frame link nothing at all**, and we draw meshes for every
          one of them. The engine generated and discarded them; we do not, and they stay on the
          ordering table default because there is no transmitted state to settle them from. Worth its
          own entry: we are drawing geometry the hardware never saw.

      All of this should start with X2 fixed: every rejection above rests on a harness that swings
      0.6 points between identical runs, and finding 11 turns on a 59-batch difference.
      **X2 is fixed** (see below): the sweep is now byte-identical run to run.

      **MTVU is not the explanation and is already off.** `SetHardwareDependentDefaultSettings` does
      set `vuThread=true` for a host with three or more cores and the startup log still says
      "Enabling MTVU", but pcsx2-hook's override runs after it: the effective value at VM ready is
      **0**, and the thread of that name exists unused. pcsx2-hook now prints `mtvu`, `ee-rec`,
      `vu1-rec` and `renderer` as they ended up rather than as they were asked for, so this is one
      line to check rather than an afternoon. X2 stays unexplained.

      Kept from the attempt: `applyGsRegisterWrites` is now wired into `collectPackets` and tracks
      the running state across a frame's link order, and `LinkedPacket::isRef` tells a node's
      transform packet from its geometry. Both are needed by whatever finishes this.

      Corrected in the IDB along the way: `ooyTestRGBMaskOn` and `ooyTestRGBMaskOff` touch no alpha
      test at all. They call `sdfGrSetFrameMaskRGB` and `sdfGrSetFrameMaskReset`, the GS frame buffer
      write mask, and are used by `dds3ObjectBaseModelLinkOtSvs` as well as the field code, so they
      are now `rsetFrameMaskRgbOn` / `rsetFrameMaskRgbOff` with `rsetFrameMaskOtOffset`. **We do not
      model FBMSK at all**, and a draw between that pair writes no colour: worth checking against the
      `fbmsk` column the GS trace already carries.

      Still to correct: `sdfModelDrawEnv_t` is 20 bytes of pointers in the IDB, but the code strides
      **96** per set and puts the terminator at `aEnv[19].set[1]` = 96*4.

- [x] **X2: the sweep was not reproducible. Fixed: every frame is now drawn, in order.** The
      emulated shot was byte-identical across runs and only ours moved, so the nondeterminism was
      entirely on our side. Two causes, both in the frame handoff:

      1. `FrameRing::takePending` returned the *newest* pending frame and freed the rest, and
         `acquireBuild` reused the oldest pending slot when the ring was full. So the render thread
         drew a subsample of the frames the game produced, and which subsample depended on
         scheduling. **The frame feedback is what makes that visible**: `fldDrawFrameSprite` samples
         the frame before it, so frame 84 rendered differently depending on whether 81 or 84 frames
         preceded it. Now `takePending` returns the oldest and `acquireBuild` waits (bounded at 2s,
         so a stopped render thread cannot hang the emulator).
      2. The shot was only taken on an iteration where a new frame arrived, while the emulated one
         was taken synchronously in `onFrame`, so the two could be of different frames. The emulator
         now requests the native shot, waits for it, and only then writes the emulated one.

      Verified: two full sweeps of all six scenes are **byte-identical** in every native PPM, and
      five consecutive single-scene runs give 10.372 with all 84 frames drawn.

      This was worth more than a stable instrument. Two scenes had been reporting their bad mode:
      **battle 174.47 -> 67.09** and **menu loading 30.38 -> 5.06**, purely from drawing every frame
      rather than a subsample.

- [x] **X3: the renderer froze in play, and the scored sweep could never have found it.** Three
      separate faults, all in the interactive path:

      1. **A leaked ring slot, which was the freeze.** `FrameBuilder::end()` set `mBuilding` to null
         without returning the slot to the `FrameRing`, so any frame acquired and not submitted kept
         it for the run. Three of those exhaust the ring, `acquireBuild` fails from then on, nothing
         is published, and the render thread re-presents its last frame while the guest keeps running
         at 50fps. `FrameRing::abandon` returns it. **Worth 2.2 points on `menu_loading_file_1`
         (5.06 -> 2.86)**, where the same starvation was quietly costing frames.
      2. **Texture lifetime was unhandled, and my first fix made it worse.** `mUploaded`, `mAlias` and
         `Renderer::mTextures` were all insert-only, and a handle is a guest address the allocator
         reuses, so after a scene change we drew the previous scene's pixels. `sdfTexFree` is the one
         release point (`sdfTexFreeDelayed` only queues it through `sdfTexDelayedResrcCB`). Deleting
         the GL texture there was wrong: a `sdfTexDup` copy outlives its source and submitted frames
         still draw with it. The free now only forgets the cache entry; the GL object is replaced at
         its next upload.
      3. **`sdfTexFreeHook` called `getFrame()`, which acquires.** With the ring full and dropping
         off, recording a texture free stalled the guest for two seconds. `getBuilding()` never
         acquires.

      Kept from the hunt: a guest heartbeat at `SDF_LOG=run=debug`, which is what proved the emulator
      was fine and the renderer was not; `reportUnsupported("transmit chain tags", ...)` on the
      transmit walk, so a cyclic DMA chain announces itself instead of costing 400k reads a frame;
      and the interactive defaults (1x speed, drop-when-behind) against the scoring ones.

- [ ] **X4: producers read arguments off freed or reused structs.** `applyBlend` reports
      `unsupported GS ALPHA` for values that are not ALPHA registers at all: `0x3F800000` is the
      float 1.0, `0x2E746975` is the ASCII `".tiu"`, and `0xB58FFFFB`, `0x11111111` and
      `0x3F7FFFE0` are the same kind of thing. They appear only in a long run with scene changes,
      never in a savestate sweep. Same root class as the texture lifetime and a different path: some
      2D producer is being handed, or is reading, a structure the game has already released. Find
      which producer by recording the caller alongside the value.

- [ ] **S1: material packets are read one generation stale.** The walk runs at `sdfModelLinkOt`'s
      entry, deliberately, so the engine and it see the same node list; but the engine's *own* next
      call is `sdfMatListUpdatePacket`, so a material whose `duty` bit for this frame is set has a
      packet we read before it was rebuilt. Measured: **264 of 9240 on field underground facility
      (2.9%), zero on the battle and unknown-realm scenes.** The fix is narrow, because almost
      everything in the packet is copied from somewhere we already read: `sdfMatUpdatePacketBasic`
      takes ambient, diffuse, specular, refmap and constant from the material itself and the three
      TEX registers from the texture handle, and `sdfMatSetUvwParm` builds `texTm` from `uvwParm`.
      Only the uvw transform actually needs redoing.


- [ ] **B1: battle is three separate defects, and the recorded cause was wrong.** Score 67.09,
      signed +60.00. Looking at the two shots side by side rather than at the score:

      1. **The ceiling vortex saturates to near-white** where the reference is dark brown-orange.
         That is the +60, and it is the one still unexplained.
      2. **Both enemy demons are missing entirely.** Nothing is drawn where the reference has them.
      3. **All text was missing** and its panels rendered as solid grey bars. **Fixed by recovering
         the glyph sheet from GS memory.** `frTranslateUserData` builds the sheet a glyph at a time
         and never runs inside a savestate window, so there was nothing to draw with; but
         `frCreateDrawFontPacket` is handed an `frGsTex_t`, whose `Addr` and `BufWidth` name the
         sheet in GS memory, and the palettes are static data that read regardless. Reading it as
         PSMT4 at the frame boundary, the way textures are recovered, gives a 512x512 sheet and every
         string renders. **Battle 19.16 -> 16.59, menu main 35.60 -> 30.02, field unknown realm
         11.04 -> 10.03**, recovered in four of the six scenes. Cause found: the run
         reports **0 glyph uploads, font clut never seen**. The glyph sheet was uploaded before the
         savestate, so it lives only in GS memory, exactly like the textures. It needs the same
         recovery treatment, and until then every string in the scene is a blank bar.

      **Disproved, with the evidence, so nobody re-derives it:**

      - *"It is the recovered texture content: meshes sample slices of the frame buffer."* No. The GS
        trace carries `fbp`: the two frame buffers are at FBP 56 and 112 (TBP0 1792 and 3584,
        512x224), the z buffer at 0, and **every texture the hardware samples is TBP0 12380..16348**,
        with **zero** below 0x1500. Our recovered bases are in that same range and **23 of our 26 are
        exactly bases the hardware samples**. Recovery is reading the right memory.
      - *"We draw geometry the engine generated and discarded."* Measured with `SDF_NO_UNLINKED_MESHES`,
        which skips the 1583 of 8850 meshes whose packet linked nothing: **67.090 either way**, not a
        single pixel. They contribute nothing.

      **Fixed: the material's alpha never reached the shader, and additive blending multiplied the
      mistake.** `gsRgb()` takes a colour's r, g and b, so `mesh.diffuse`'s alpha was dropped. VU1
      writes that alpha as the vertex alpha, and the GS trace agrees exactly: the additive batches
      carry vertex alpha **128, 64, 38, 12 and 6**, which are precisely the materials' own
      (`80808080`, `40808080`, `26808080`, `0C808080`, `06808080`). So every additive mesh blended at
      full strength instead of between 5% and 50%, and four layers a frame of that is white.
      **Battle 67.09 -> 19.16, signed +60.0 -> +11.0**, with field unknown realm 11.16 -> 11.04 and
      nothing regressed.

      How it was found, since the route matters more than the fix: the score said nothing, the images
      said the ceiling blew out, `SDF_OT_MAX=1` said the additive and subtractive tables owned 48 of
      the 60 points, and the trace and the GS trace then agreed on a five-value list.

      **The remaining +11:** `SDF_OT_MAX=1` drops tables 2
      and 3 and battle goes **67.09 -> 18.62**, signed **+60.0 -> +6.1**. So 48 of the 60 points of
      over-brightness are those two tables, not the arena: with sprites off the floor, walls and lava
      veins already read correctly against the reference, and only the ceiling vortex blows out.
      Those meshes are ALPHA `0x48` (additive) and `0x42` (subtractive) on TEST `0x71801` (ATST
      NEVER, so every fragment takes the AFAIL path), 150 triangles over a quarter of the screen,
      four a frame. `gsAlphaToBlend` maps both correctly, including `GL_FUNC_REVERSE_SUBTRACT`, so
      the blend is not the bug: what is left is the magnitude of `Cs`, which additive layering
      multiplies. The shader inserts the material's `uAmbient` and `uDiffuse` into the light sum, and
      the VU1 image records no such term, so that is where to look first.

      **Ruled out as causes of the white:** the GS state (6163 of 6978 texture-paired batches agree on
      every register, 88%), the parser (100% triangle coverage, no rejects), and the light packet
      (read per root from `root->light`, and the shader's sum matches the VU1 image).

- [ ] **B2: recovered textures look washed out, and it is not CSA.** The battle dumps come back pale
      beige where the scene is dark. **CSA is 0 on every recovered texture in that scene** (logged
      with `cbp` and `cpsm`: all `cpsm 0`, a 32-bit CLUT read as one), so the recorded "CSA ignored in
      the CLUT read" is wrong. `readClut` is shared with the asset path and keeps the palette's alpha
      raw for the shader, so the two paths cannot differ there either. What is left to check is
      whether the pixels themselves are right: read one recovered texture back and compare it against
      the same texture decoded from its asset in a scene that has both.
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
