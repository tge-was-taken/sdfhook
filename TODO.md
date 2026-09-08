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

- [ ] **V7: the tbp 16216 pass is invisible in the reference. The wall is too bright for another
      reason, and that is the real defect.**
      Settled with `PCSX2HOOK_GS_SKIP`, added to pcsx2-hook for this: it takes the same grammar as
      `PCSX2HOOK_GS_ONLY` and suppresses what matches rather than keeping it, which is what isolates
      a pass. `GS_ONLY` cannot, because it does not clear the frame buffer and so leaves the previous
      frame under whatever it kept. Validated against a control before being believed: skipping the
      wall's own tbp 13459 moves **36,279 pixels, 15.8% of the frame**; skipping tbp 16216 leaves it
      **bit-identical**.

      The batch state says why. Every vertex of those 45 batches carries RGBA 0, and ALPHA 0x44 has
      C=0, so the blend is `(Cs - Cd) * As + Cd` with As = 0, which is Cd. TEST is ATST NEVER with
      AFAIL RGB_ONLY on top. The pass is issued and paints nothing.

      **So drawing it was compensating error.** It scored 5.427 -> 5.301 by darkening walls that are
      too bright for an unrelated reason, and it is now off behind `SDF_DRAW_SPECULAR_OVERLAY`. The
      honest number for the scene is 5.427 with a signed mean of **+3.898**, and that brightness bias
      is what to chase: our walls are too bright, and no overlay is missing.

      Kept from the investigation, because it is all correct and cheap to reuse: `sdf3DSpecularTex`
      at 0x005518BC is a **pointer to a texture handle** (0x005E1900) where `sdf3DRefMapParam` holds
      a raw TEX0; attrBits bit 0x0002 is now named `sdfCoreAttrSpecularPass`; and neither 16216 nor
      16348 is ever uploaded, since `sdf3DInitSpecularTex` builds both straight into GS memory (all
      68 handles in `mUploaded` were decoded to check).

      Two measurement traps this cost, both worth remembering. `PCSX2HOOK_GS_ONLY=tbp=N` shots are
      contaminated by the previous frame, so reading a colour off one measures the scene, not the
      pass. And `i=A-B` ranges cannot be checked against a trace captured at a different frame than
      the shot; the giveaway was that adding the wall batch as a control changed zero pixels.

- [ ] **V8: a near-uniform +3.7 brightness offset frame-wide, plus extra red on the walls.**
      Measured without any batch-to-mesh pairing, which is what makes it trustworthy. Take the
      emulator frame twice, once plain and once with `PCSX2HOOK_GS_SKIP=tbp=13459`; the pixels that
      differ are exactly the wall, 36,279 of them or 15.8% of the frame. Then compare emulator
      against ours on that mask and on its complement:

          region                emulator             ours                 delta
          wall pixels (15.8%)   [58.6,  4.3,  3.0]   [68.2,  7.4,  5.3]   +9.6, +3.1, +2.3
          everywhere else       [49.6, 22.9, 16.5]   [53.3, 26.6, 20.2]   +3.7, +3.7, +3.7

      So there is a **near-uniform +3.7 on all three channels across the whole frame**, and the wall
      carries roughly +6 of extra red on top of it. Binning that delta by the emulator's own level
      gives the shape, and the shape is the useful part:

          emu level    mean(ours - emu)   pixels
            0- 15          +3.30          307934
           32- 47          +4.06          105119
           80- 95          +5.00           15272
          144-159         +10.52            3091
          176-191         +12.71            2306
          208-223          +5.47            1158
          240-255          +0.24            1532

      Zero at black, zero at white, a maximum in the upper mid-tones. That is a transfer or
      precision curve, not a missing draw and not a wrong formula: a lighting error would scale with
      the light term rather than vanish at both ends. Where the emulator writes 0 we write 1 or 2
      (17,343 pixels at 1, 9,171 at 2, 9,748 correct), so there is also a small additive floor, but
      it is not what dominates the mean.

      **It is 3D-specific, so it is not an emulation floor and it is worth chasing.** Binning the
      same way on smt3_menu_loading_file_1, which is 2D with no 3D shading, gives a completely
      different profile: +1.37 and +1.82 on the two dark bins and then large *negative* spikes on
      tiny populations, -8.77 on 8,778 pixels and -48.80 on 2,207. That is a handful of mismatched
      UI elements, not a transfer curve. The 3D scene instead lifts uniformly, +3.47 across 470,967
      dark samples, rising to +11.45 at 160..191. Whatever it is, the 2D path does not share it, so
      an argument of the form "the 2D scenes sit at 2.080, therefore this is the floor" does not
      hold -- I made that argument earlier and it was wrong.

      Ruled out so far. **Fog is not it**: `SDF_NO_FOG=1` takes the dark-pixel delta from +3.47 to
      +6.72 and the score from 5.199 to 8.943, so fog is doing real work in the right direction, and
      its parameters read [0, 255, -2.576, 257575.77] with a black fog colour, where dividing all
      four by 255 is algebraically what VU1 does in its own units. **Blend truncation is
      unsupported**: the GS truncating `((A-B)*C >> 7) + D` where GL rounds would show on the 2D
      path too, and it does not.

      **Narrowed to the AFAIL second pass.** Suppressing it takes the signed mean from +3.527 to
      **-0.200**, so that one draw carries essentially the whole brightness bias. It is not simply
      surplus, though: suppressing it costs real content and the score goes 5.199 -> 7.309. So the
      pass belongs there and something about how we issue it puts too much light on the frame.

      What is already right, so start elsewhere: the alpha test is quantised as
      `floor(min(a * 128, 255) + 0.5)` against the raw AREF, which is the form CLAUDE.md prescribes;
      the two passes are complementary (`atestFails(testA) != uAtestInvert`); and the colour and
      depth masks match AFAIL FB_ONLY, ZB_ONLY and RGB_ONLY.

      **Ordering is ruled out.** Drawing the failing half before the passing one instead of after
      leaves every score bit-identical, so the depth interleaving the GS does and we do not makes no
      visible difference here.

      **The alpha units are the live suspect, and the obvious fix is wrong.** In the fragment stage
      `testA = min(texA * color.a * kPs2Alpha * uDiffuseAlpha, 1.0)` looks like a double
      application: the vertex stage already does `vColor.a = aColor.a * kPs2Alpha`, `texA` is scaled
      just above, and `uDiffuseAlpha` is uploaded as `mesh.diffuse.a / sdfGsUnit`, so all three are
      already bytes over 128, and `readClut` keeps the PS2 alpha byte so `texA`'s own scale checks
      out. Three variants measured, and the form that looks wrong wins:

          texA * color.a * kPs2Alpha * uDiffuseAlpha   5.199 / 5.263   (what we ship)
          texA * uDiffuseAlpha                         5.470 / 5.263
          texA * color.a                               9.100 / 22.655

      The middle one is motivated -- CLAUDE.md says the material alpha *is* the vertex alpha VU1
      writes, so multiplying both double-counts it and the stray kPs2Alpha roughly undoes the
      double-count -- but it loses 0.27 on unknown realm, so `color.a` is carrying something
      `uDiffuseAlpha` does not.

      What the GS actually emits, checked rather than inferred: **alpha is constant within every
      batch** -- 128, 128, 128, 104, 128, 104 across the first batches of frame 1, one value each and
      never varying per vertex over all 341. So it is a per-mesh constant, not something the
      lighting sum computes, and `lightVertex` operating on xyzw does not mean the alpha lane
      carries a per-vertex result. Do not chase that; I did, on the strength of the *aggregate*
      alpha histogram showing values 49, 64, 70 and 15, which turned out to be variation between
      meshes rather than within one.

      **`uDiffuseAlpha` is the wrong source, not the wrong scale.** Per-batch alpha for frame 1,
      one value each since it never varies within a batch:

          GS               128 (235), 0 (54), 70 (26), 20 (13), 104 (2), 49 (2), 25 (2), 108 (1)
          our mesh.diffuse 128 (312), 47 (3), 25 (1), 108 (1), 32 (1)

      They agree on 128, 25 and 108 and disagree on **97 of 335 batches**: the GS's 0, 70, 20, 104
      and 49 appear nowhere in our material alphas, and our 47 and 32 appear nowhere in the GS's.

      The transcription says what it should be: VU1 writes the vertex colour on **xyzw**, so the
      alpha is the lighting sum's w lane. For the flat path that is `min(color4_flattened *
      drawColour, 255)` with w carried through, which makes it a product of `drawTint.w`,
      `matDiffuse.w` and `color4.w` rather than the material's diffuse alpha alone. That is why the
      odd-looking `texA * color.a * kPs2Alpha * uDiffuseAlpha` beats both tidy single-alpha forms:
      being a product of several alphas, it is structurally closer to the truth than either.

      Two candidates measured against that table, and **neither matches**. `drawColor` is now in the
      trace so this check can be run at all:

          GS                            128(235) 0(54) 70(26) 20(13) 104(2) 49(2) 25(2) 108(1)
          ours diffuse.a                128(312) 47(3) 25(1) 108(1) 32(1)
          ours drawColor.a              128(226) 64(39) 45(24) 58(9) 40(5) 9(3)
          ours drawColor.a * diffuse.a  128(220) 64(39) 45(24) 58(9) 40(5) 47(3)

      One group is explained: the GS's 54 batches at alpha 0 are mostly the 45 invisible
      sdf3DSpecularTex batches from V7, which we do not draw. The rest is not. The 128 group lines
      up (235 against 226) but our 64/45/58 have no counterpart in the GS's 70/20/104/49, and the
      product distribution is barely different from drawColor.a alone, so multiplying the two is not
      the answer either.

      **The sum does not need widening to vec4.** The light packet's alpha lanes are
      `colorBase.w = 0`, `color[0..2].w = 0`, `color[3].w = 1`, `color[4].w = 1`, so every light term
      contributes nothing to w and the flat path's `color4.w` is exactly 1. The vertex alpha
      therefore reduces to **`drawColour.w`** on its own, which 0x009E makes
      `drawTint.w * matDiffuse.w` -- and both of those come from the **material packet**, where
      `uDiffuseAlpha` supplies only the second and `color.a` is the node's draw colour, a different
      quantity entirely. That is why neither candidate matched the table.

      That prediction is refuted too. `PCSX2HOOK_VU_WATCH` on the three lanes, in raw 0..255 units:

          mem[1002].w  matDiffuse      128, 38, 108, 32, 25, 49, 47
          mem[1007].w  drawTint        128, 64, 58, 1, 57, 52, 34, 40
          mem[1005].w  drawColour      128, 64, 38, 108, 58, 32, 25, 40
          GS per-batch alpha           128, 0, 70, 20, 104, 49, 25, 108

      `drawColour.w` overlaps the GS only on 128, 108 and 25; its 64, 38, 58, 40 have no counterpart
      and the GS's 70, 20, 104 have none in it. (Watch samples are per microprogram run rather than
      per batch, so the counts are not comparable, but the value *sets* should still meet and do
      not.)

      **Four candidates now refuted against the table**: `diffuse.a`, the node's `drawColor.a`,
      their product, and `drawColour.w` from VU memory. The alpha source is genuinely unidentified,
      and it is not any of the obvious per-mesh colours. Next idea worth testing: the GS's 0 and 70
      groups are large (54 and 26 batches), so pick one batch of each, find the mesh by position with
      the wall-mask method from the top of this entry, and read every alpha the engine has for it.
      Stop testing whole distributions -- four have failed and each cost a full capture.

      **This refutes V8's earlier vertdiff lead, and vertdiff was wrong.** It claimed our walls were
      [218.5, 154.2, 102.8] against the hardware's [81, 73, 66], a factor of 2.7. If that were true
      the wall mask above could not be off by only +9.6 out of 58.6. The 10 batches it flagged are
      tbp 13459, the wall, so the mesh class was right and the colour comparison was a mispair --
      exactly the failure mode CLAUDE.md records for bounds matching, and centroid matching has not
      cured it. Prefer the mask method above for any question of the form "are we too bright here".

      Fixed on the way to this, and kept: the vertex colour scale was 2.0 where 0x0174 and 0x019F
      scale by exactly 1/128 against a colour ITOF0 leaves in 0..255, so it is 255/128. Worth
      unknown realm 5.427 -> 5.360, underground 5.411 -> 5.262 and battle 6.682 -> 6.620.

- [ ] **V6: the parity list against the transcribed VU1. Four of five done; the fifth is measured
      as worth 88 pixels.** Between them these took unknown realm 5.541 -> 5.199 and underground
      5.988 -> 5.263. The list is now exhausted for these scenes: what is left in the frame score is
      the precision curve in V8, not a missing or mismodelled draw.
      `docs/vu1/vu1_annotated.c` covers all 1884 instructions, so every one of these is a read
      difference rather than a guess. Fixed already, for the record: fog was computed as
      `min(fogParm[0] + fogParm[3]/w, fogParm[1])` where transformVertices 0x04DC, 0x04DF and 0x04E3
      compute `clamp(fogParm[2] + fogParm[3]/w, fogParm[0], fogParm[1])`, and nothing read
      `fogParm[2]` at all; correcting it took battle 6.904 to 6.682, underground 5.988 to 5.411 and
      unknown realm 5.541 to 5.427.

      1. **The reflection pass samples one column of its texture -- and it is worth almost nothing
         here.** `PCSX2HOOK_GS_SKIP=tbp=16348` changes **88 pixels, 0.0% of the frame**, at a mean
         delta of 7. Fix it for correctness if a scene leans on it, not for the score on this one.
         The defect itself: Its UV is
         `dot(normalize(facing), uSpecularAxis)`, and `MeshLighting::specularAxis` is declared in
         frame.h, read in renderer.cpp and trace.cpp, and **assigned nowhere**, so it is (0, 0, 0)
         on every mesh and the dot is always 0. reflectPass 0x05DF..0x05E6 builds a two-dimensional
         sphere map instead, `0.5 + 0.5 * (M * normal)` divided by w at 0x05E8, with M taken from
         mem[996..998] or mem[988..990] as drawFlags bit 11 selects at 0x05D6. Its flat colour is
         `reflectColour(mem[973]) * drawTint(mem[1007])` at 0x05E2 where we pass `mesh.specular`
         alone. The pass does fire (5,143 meshes in unknown realm, 25,855 in battle), so this is
         wrong output rather than an absent one.

         The matrix is recoverable without hooking loadCamera. `sdf3DScreenParam` is not the packet
         loadCamera consumes, but its quadwords 3..6 are the projection: measured on
         smt3_field_unknown_realm they are diag(1.9078, 2.5401, -1.0005) with w 1.0 and 32.0078 in
         the third row. mScreenTm is projection x view, and a sphere map needs only the view
         normal's .xy, so `viewN.xy = (dot(screenTmRow0.xyz, n) / 1.9078,
         dot(screenTmRow1.xyz, n) / 2.5401)` gets there with two uniforms and no new hook. Check it
         against the GS trace's own ST values for a refmap batch before trusting it.
      2. ~~Two-sided meshes are shaded with front-facing normals.~~ Done: the back faces are now a
         second draw with `glCullFace(GL_FRONT)` and the normal negated, as drawBatch 0x01D0 and
         0x01D2 do, instead of simply disabling culling. Exercised on 1,524 meshes in unknown realm
         and none in battle, and it measures neutral -- correct, but those back faces are evidently
         covered. Keep it: the old form would show as soon as a scene exposes one.
      3. **Partly done.** `uStreamColors` is no longer dead: lightFlatColor 0x019D..0x01B0 has no
         MINIi anywhere, unlike every other path, so a stream-colour mesh on the flat term is now
         left unclamped. Measures neutral, because once `flatten` is applied the product stays under
         255 anyway. What is still unchecked is lightVertexColour 0x0173..0x0187: working the algebra
         through, `vertexColour * drawColour / 128` against our `vColor * 255/128 * lit` comes out
         equivalent, so there may be nothing to do -- but confirm it rather than trust the algebra.
         Note 0x01AE, the odd-vertex tail, stores the converted colour **without** multiplying by
         the flat term, which looks like an engine bug and is not modelled.
      4. ~~`flatten` is unparsed.~~ Done, and the note that it measures zero everywhere was wrong:
         `PCSX2HOOK_VU_WATCH=910` on smt3_field_unknown_realm gives 0.0 (89,703 samples), 1.0
         (52,108), 0.5 (51,770) and 0.7627 (6,419). It is the material packet's `constant`, which we
         already read but only inside the multi-texture guard, so meshes without a second UV set
         never saw it -- the same shape of bug as the faceDiv seam. Worth unknown realm 5.360 ->
         5.199 with the signed mean +3.810 -> +3.527; underground unchanged and battle +0.034.
      5. ~~The vertex-colour scale is 255/128, not 2.~~ Done, and worth more than the 0.4% it looks
         like: unknown realm 5.427 -> 5.360, underground 5.411 -> 5.262, battle 6.682 -> 6.620.

      Two things that looked like divergences and are not. The specular term removed from
      `sdfMeshLit` was dead: its axis is never assigned, so it contributed exactly zero. And the
      ramp on light 0 has no counterpart in lightVertex, whose dots are plain `max(dot, 0)` at
      0x0156 and 0x0157 — but it stands in for corePktApplyFaceDivColor, which runs on the EE over
      the points the face subdivision adds and which VU1 therefore never sees. Removing it costs
      0.84 points on unknown realm (5.427 to 6.268), so the comment was wrong rather than the code.

- [ ] **T1: verification tooling, so a shading question is answered by a number and not by the score.**
      A frame mean cannot say which mesh is wrong, and chasing it produced a dozen rounds of
      hypotheses that measurement then refuted. What replaced it:

      **The lighting is one text compiled twice.** `src/render/shading.inc` holds the sum;
      `render/shading_cpu.h` gives it the handful of GLSL names it needs and compiles it as C++;
      CMake turns the same file into `shading_glsl.h`, which `compileMesh` prepends to both mesh
      shaders. So a CPU check and the GPU run one implementation and cannot disagree. This is not
      theoretical: the first version of the checker was a hand copy and had a unit bug (the vertex
      color is a normalised attribute over 255, the rest are over 128) within an hour of being
      written, which made its whole ranking wrong.

      Editing `shading.inc` needs a **CMake reconfigure**, not just a build: the generated header is
      a configure-time artefact. `cmake --preset sdfhook` then build, or ninja reports no work and
      the old shader keeps running, which reads as "the change did nothing".

      **`scripts/lightdiff.py <trace.jsonl> <gs.jsonl>`** joins the two: the mesh row carries
      `litLo`/`litHi`, the range our shaders give a mesh's vertices, and the GS trace carries each
      batch's post-VU `rgbaq`, which is the same quantity. A batch color outside our range is a
      mesh we shade wrong; inside is consistent, not proof.

      **What it says on field unknown realm**, first run, where the score had said nothing for a
      dozen rounds:

          tbp    type attr batches mean miss   hardware vs ours
          13459     1 0078   16143     335.3   (96, 68, 45) vs [218.5, 154.2, 102.8] flat
          11630     1 0058     480     351.1   (0, 0, 0)    vs [166.7, 114.4, 114.4] flat
          13525     1 0878     360     310.4   (1, 254, 209) vs [0, 0, 0]..[129.7, 91.6, 61.0]

      The first row is world geometry, three quarters of the scene, and it is **flat**: `litLo`
      equals `litHi`, so every vertex gets one color. 96/218.5, 68/154.2 and 45/102.8 are 0.439,
      0.441 and 0.438, **one scalar across all three channels**, so it is a single factor wrong and
      not a color error. Flat with `lit` true means `t` saturates: at soften 7 the ramp clamps to 1
      for nearly every normal. That is the same mechanism as the dark shoulders, seen from the other
      end.

      **A measured dead end**, so nobody repeats it: `docs/vu1` reads 0x0188..0x019B as taking
      `color[4]` outright, and dropping `diffuse` and `draw` from the no-normals path costs battle
      **2.5 points** (6.896 to 9.359) while changing field not at all. The material terms belong
      somewhere in that path and the transcription does not yet say where.

      **The inputs are now a fixture, and there are only 14 of them.** `litcase` rows carry each
      distinct set of shading inputs once, and every mesh row names the case it used, so a run holds
      the inputs beside the hardware's own answer. Fourteen cases cover the whole field scene.

      **`build/bin/shadecheck` + `scripts/shadeconform.py` ask the match-free question.** shadecheck
      sweeps `shading.inc` over 8192 Fibonacci normals per case and prints the reachable color
      range; the script feeds it the run's cases and compares against the hardware's `rgbaq`. A
      color our lighting cannot produce for **any** normal is a wrong model whichever vertex it came
      from, so this survives every objection about batch-to-draw matching. It needs no emulator, no
      GPU and no run once a trace exists.

      **What it found, and the contradiction to resolve next.** Both dominant cases are `lit = 0`:

          case 2   18363 batches   ours 218.5, 154.2, 102.8 flat   hardware 162 distinct colors
          case 0    1320 batches   ours 166.7, 114.4, 114.4 flat   hardware  14 distinct colors

      Our value is one color per case; the hardware's varies. And **our constant is the hardware's
      observed maximum**: case 0 tops out at (217, 153, 102), which is case 2's constant, and case 2
      reaches (166, 114, 114), which is case 0's. So `lightFull` is the fully-lit endpoint of a ramp
      and we pin every vertex at it.

      Yet `lit = 0` is not obviously wrong: with `mesh.packetBits` now stored for streams (it was
      never assigned, so the trace read 0000 and hid this), type 1 world geometry carries **0x0360,
      no 0x80, so no normals**. Something else varies those colors. The candidates are the stream's
      own per-vertex colors under `attrBits & 0x800`, which `readVifStreamMesh` does read, and
      VU1's vertex-color variant at 0x0173 and 0x019D, which the shader does not model. Note the
      largest single population, `tbp 13459 attr 0078`, has **neither** normals nor vertex colors
      and is still 218.5 against the hardware's (96, 68, 45), so at least that part is not explained
      by either.

      **The inputs are verified against the oracle and are correct.** A `PCSX2HOOK_VU_WATCH` run on
      923, 928, 1003 and 1004 gives, for the program that runs 16221 of the samples:

          colorBase 923 = (1.5, 1.0, 0.8)      ours lightBase   (1.5, 1, 0.8)
          color[4]  928 = (1.7, 1.2, 0.8)      ours lightFull   (1.7, 1.2, 0.8)
          base     1003 = (0.2, 0.2, 0.0)      ours lightAmbient (0.2, 0.2, 0)
          matAmbient 1004 = (1, 1, 1)          ours ambient     (1, 1, 1)

      and the second program matches case 0 the same way. So the light terms we feed the shader are
      right, and the error is in the computation. **Note `batch` in `vu1_watch.jsonl` is always 0**,
      so it is one sample per microprogram run and cannot be joined to a GS batch index; group by the
      `vu` program hash instead.

      **Two cautions about the oracle field, both learned by getting them wrong.**

      `rgbaq` is the last RGBAQ written before the batch, not a clean per-batch lighting sample. On
      texture-agreeing pairs, 600 case 2 batches report `(0, 0, 0)` and 540 report `(128, 240, 128)`,
      which is `0x80F080`, a shadow material's own color unpack. Filter those before ranking.

      **Bounds-only matching inflates a spread enormously.** Distinct hardware colors per case:

          case 0    bounds-only 14   texture-agreeing  1
          case 2    bounds-only 162  texture-agreeing 20

      An earlier note here read the 162 as "the hardware varies where we are flat". Most of it was
      the matcher. `shadeconform.py` still matches on bounds and takes the first hit, which is why
      its headline says 100%: **treat its ranking as a lead, not a measurement**, until it filters
      the way the table above does.

      **Where the pairing is trustworthy, the lit path is right.** On the character (`tbp 11214`),
      type 7 `attr 1078` gives ours `(38,25,25)..(128,89,64)` against hardware `(128,89,64)` x300 and
      `(38,25,25)` x60: **our two range endpoints are exactly its two dominant colors.** The light
      directions are confirmed too, independently of any score: `d0 = m * n0` at 0x0150 is column
      major, so light i's direction is component i across the three registers, which is what
      `readLighting` reads.

      `type 8 attr 0078` meshes look short (ours tops out at 79 against 128), but they share tbp
      11214 and overlapping bounds with the `attr 1078` ones, so bounds-plus-texture cannot tell the
      two apart and that shortfall is not yet evidence. **A stronger join is the next tooling need**:
      vertex counts, or a tighter per-mesh screen box, or draw order.

      **But the streams carry no normals, tested rather than assumed.** `readArray` is now asked for
      a 12-wide array of `pointCount` after the positions regardless of what `packetBits` claims,
      which is unambiguous since only positions and normals are 12 wide and positions are consumed.
      It finds none, `mesh.lit` stays false, and no scene moves. So the interpolant is neither a
      normal we skipped nor the vertex colors (this population is `attr 0078`, without `0x800`).

      What it is, is the open question. Candidates worth testing in this order: a face normal VU1
      derives from the triangle's own positions; a per-vertex scalar the stream carries in an array
      we currently walk past; or the second UV set being read as something else. Use
      `PCSX2HOOK_VU_EXEC_TRACE` with the `690365AB` prefix to see which loads the lit path issues.

      **The end-to-end capture is in, and it is what finally pairs a batch to its own mesh.**

      - `Hooks::GsVertexOut` and `GsDraw::readVertex` hand out the vertices a batch draws, which is
        VU1's output and the GS's input: one buffer, nothing between it and our vertex stage.
      - **`rgbaLo`/`rgbaHi` are on every batch row, always.** `hookDraw.rgbaq` is **vertex 0's**
        color, not a batch property, and one sample cannot say whether VU1 shaded a batch flat.
      - `PCSX2HOOK_GS_VERTS=1` writes the per-vertex stream (x, y, z, rgba, u, v, s, t, fog) as
        parallel arrays: 20179 rows and 159MB over 60 frames, so it is opt-in.
      - `scripts/vertdiff.py` pairs a batch to the mesh whose **projected centroid** is nearest, the
        texture as a filter, and diffs colors. `ndcMid` on the mesh row is that centroid.

      Four traps, one build each:

      - rapidjson refuses a non-finite double and a GS vertex carries them: an untextured batch
        leaves Q at zero so its S/T are infinite. Guard before writing.
      - **A second `fwrite` on the GS thread segfaulted the CPU thread inside `recExecute`**, the same
        corrupt-stack signature this file records for a stray GS-side read. One write per batch.
      - The renderer is built only when `cfg.window` or `nativeShotPath` is set, so a trace run with
        neither produces no mesh rows at all.
      - Vertex XY are 12.4 with XYOFFSET still applied; the batch's `bbox` is already corrected, so
        the difference between them is the offset.

      **What position matching says, against the box match it replaces:**

          type attr case vcol  ours                       hardware
             1 0078    2   no  218.5, 154.2, 102.8 flat   81, 73, 66
           8/7 0078    7   no  max 79.2, 54.3, 42.9       128, 89, 64
             8 1078    8   no  128.5, 89.3, 64.2          128, 89, 64   (matches)

      The hardware's `(81, 73, 66)` is near-grey where ours is warm, so **no scalar times `lightFull`
      reaches it**: that population is not a mis-scaled lighting sum. Case 8 matching exactly says the
      lit path is right.

      Kept from getting it wrong: the ramp midpoint `(base + colorBase)/2` fits case 2's sampled
      color to 2% and **costs underground 12.8 points and field 1.6**. A formula that fits one
      bounds-matched sample and breaks two scenes means the sample was attributed wrong.

      **Draws can now be suppressed on both sides with one grammar, which removes matching from the
      problem entirely.** `PCSX2HOOK_GS_ONLY` and `SDF_DRAW_ONLY` both take comma separated `i=N`,
      `i=A-B` and `tbp=N` (the GS side also takes `prim=N`), and anything matching a term is kept.
      Cut a frame to one draw on each side and the two shots diff directly, with no centroid, no
      bounding box and no texture tiebreak.

      The GS side works through `Hooks::SetSkipCurrentDraw`, read and cleared by `GSState` right
      after `OnGsDraw`, so it is per batch and cannot leak into the next. Two things to know:
      **the filter has to sit above `onDraw`'s `if (!mRecording) return`** or it only works when a
      trace file is open, and **suppressing a draw does not clear the frame buffer**, so the
      emulator's shot keeps whatever the untouched pixels held. Include the clear batch in the spec
      for a clean isolate.

      **tbp 16216 is a real missing pass over the walls, and here is everything measured about it.**
      It is the only texture in the scene the GS samples and we never bind: 191544 vertices, zero
      ours. What is known:

      - **1:1 with the walls.** Every one of its 5355 batches sits directly after a `tbp 13459`
        batch and 5117 are followed by one, so it is a second pass over the same geometry.
      - **It writes color only**: ALPHA 0x44, TEST 0x53001 (ATST NEVER with AFAIL RGB_ONLY), PSMT8
        128x128, TFX MODULATE, TCC 1. Isolating it with `PCSX2HOOK_GS_ONLY=tbp=16216` renders black,
        because over a cleared buffer 0x44 gives `Cs * As` and its alpha is low. It is a modulation,
        not a layer.
      - **It is not the material's second pass.** Every wall mesh reports `mtbp 0` and none carries
        `attr & 0x1000`.
      - **It is not a second material in the wall's own chain.** Dumping the type 1 call chains gives
        `Mm` every time, one material and one mesh, and the run reports zero setup or stray refs, so
        nothing is being mis-assigned or skipped there.
      - **It is not visible to the GIF walker.** `Coverage::tbpByProducer` now records every texture
        a producer's packets bind, and only five producers appear: world geometry's TEX0 is written
        by VU1 rather than by a packet we can walk, so this pass leaves no trace there.

      So the packets that carry it are linked by a path our chain walk does not reach. Check the
      other link entry points first (`sdfDrawTagLinkRef` and `sdfDrawTagLinkRefGif` against
      `sdfDrawTagLinkCall`), since the frame-mask pass was found hiding behind exactly that.

      Next, in order:
      - Fabricate inputs for the GS and VIF paths and assert the decode, so a translation error fails
        loudly at its own layer instead of as a color ten stages later.
      - An on-demand VIF1 interpreter through pcsx2-hook, to run a captured packet and compare our
        decode against it directly.

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

      **The cause was found later and is fixed at the source, see X6.** Quarantining treated the
      symptom: F9 kept writing states that could not resume, two more of them into `captures/`.

- [x] **X6: F9 wrote a savestate with the hook layer's return sentinel baked into it, and resuming
      one spun forever.** `captures/capture_000.p2s`, `capture_001.p2s` and the quarantined
      `smt3_save_terminal.p2s` all hang on load; `states/*.p2s` do not.

      A post-handler is implemented by stealing `$ra`: `Hooks::Dispatch` puts the return sentinel
      (0x000FFF00) in the guest's `$ra` and keeps the real address in `s_returnStack`, on the
      **host**. The callee then spills that sentinel into its own stack frame. A savestate carries
      the guest stack but not `s_returnStack`, so a state written while a hooked call is in flight
      resumes into a process where the return address it is holding means nothing. When the guest
      returns to the sentinel, the sp match finds no entry and the fallback ran
      `cpuRegs.pc = cpuRegs.GPR.n.ra.UL[0]` -- which is the sentinel -- so the pc never moved, no
      cycles were spent, and nothing ever reported.

      Proved by moving the sentinel: `PCSX2HOOK_SENTINEL_BASE=0x000FFE00` makes
      `captures/capture_000.p2s` resume to completion where the default hangs. A probe on
      `Hooks::PendingReturns()` at save time then showed **one call in flight on 3 of 5 captures**,
      which is why F9 was unreliable rather than always broken.

      Three fixes:

      - `Hooks::PendingReturns()` reports how many hooked calls are between their pre-handler and
        their post-handler.
      - `pcsx2hook_save_state` retries on later CPU-queue drains until that count is zero (64
        attempts, then it refuses and says so rather than writing an unresumable file). Each drain
        runs the guest a further slice, so the calls in flight return; the measured cost is **2 VM
        slices**, and a wait is reported.
      - The unmatched-sentinel fallback reports once and asks the VM to stop. It returns 0 rather
        than setting the pc, so the guest runs the stub's own `jr $ra` and burns cycles: a stop is
        only noticed at a frame boundary, and a frame boundary needs cycles. `Cpu->ExitExecution()`
        from inside `Dispatch` does not unwind, which the backtrace showed.

      Captures at frames 60, 100 and 134 all waited 2 slices and all resume with no sentinel error.
      The three states already written stay broken (the real return address is not recoverable), but
      they now fail in about a second with a named cause instead of hanging.

- [ ] **V23: `flatten` reaches only light 0 in the lighting sum, and the comment said otherwise.**
      Found while trimming comments, not by a render defect. `shading.inc` computed
      `keep = 1.0 - flatten` under a comment saying "every term below is scaled by it while the base
      is lerped toward 1.0", but the lit path applies `keep` to `light0` alone: the base is added
      raw and `color1` and `color2` carry no factor. One of the two is wrong and the comment has
      been corrected to describe the code, not the other way round, since changing the sum changes
      every scene.

      The fuller form the comment described is

          vec3 sum = vec3(flatten) + base * keep
                   + clamp(soften * (bias + dot(n, dir0)), 0.0, 1.0) * light0
                   + max(dot(n, dir1), 0.0) * color1 * keep
                   + max(dot(n, dir2), 0.0) * color2 * keep;

      Settle it against lightVertex 0x0132..0x0172 rather than against a frame score: 0x0139 builds
      the term and 0x0146, 0x0148 and 0x0149..0x014B are the lines that say which products it
      multiplies. `PCSX2HOOK_VU_WATCH` on mem[910].w gives the values in play (0, 0.5, 0.7627 and
      1.0 were measured on smt3_field_unknown_realm), so a scene where it is non-zero exists to
      test against.

- [x] **V20: depth was rebuilt from w, which dropped the arena's red overlay behind its floor.**
      Reported from the battle scene, where the missing layer covers the whole floor.

      `emitTexTriangles` projects with the **perspective-divided z**: 0x02A0 multiplies the view
      position by the reciprocal the transform stored, then 0x02BD scales it by `screenScale` and
      adds `screenOffset`. So the GS depth is affine in `clip.z / clip.w`, and ordering by
      `clip.z` reproduces it. The shader instead built depth linearly from `clip.w`, which orders
      surfaces by distance alone.

      The arena's additive overlay (tbp 13315, one 1179-vertex full-screen strip on ordering table
      2, ALPHA 0x48 with ATST NEVER and AFAIL FB_ONLY) is drawn over the floor (tbp 13812) and is
      nearer on hardware by a margin the GS trace measures per screen band: +8759, +1790 and +382 z
      units. Our reconstruction put it 4 to 13 units of window depth **behind**, so ZTST GREATER
      rejected every fragment of it.

      Isolated three ways before changing anything: `SDF_DRAW_ONLY=tbp=13315` draws the overlay
      correctly on its own; forcing `glDepthFunc(GL_ALWAYS)` restores it in the full scene; and the
      emulator's own contribution, measured as full minus `PCSX2HOOK_GS_SKIP=tbp=13315`, is the same
      splotch pattern in the same places as ours.

      `gl_Position.z = -clip.z`, negated because sdf3DScreenParam's z scale is negative. The other
      sign scores 8.443 on battle against 6.634, so it is measured rather than assumed.

      | scene | before | after |
      |---|---|---|
      | battle unknown realm | 6.654 | **6.634** |
      | field underground facility | 5.263 | **4.272** |
      | field unknown realm | 5.199 | **5.125** |

      `root->zBias` was the only other term in the old formula and it measures **zero in every
      savestate**, so it now goes through `reportRootDepthBias` instead of being silently dropped.
      VU1 adds the real bias to `screenScale.z` at 0x00CC, out of the material packet's fourth
      quadword, which scales the projected depth rather than offsetting it; nothing needs it yet.

- [x] **V21: the battle specters were drawn twice as bright, clipping their cores to white.**
      Their batches are additive (ALPHA 0x48, `Cs * As + Cd`), so an alpha error shows up directly
      as brightness instead of being hidden by a clamp to opaque.

      The fragment alpha was `texA * color.a * kPs2Alpha * uDiffuseAlpha`, and the vertex stage
      already scales `vColor.a` by `kPs2Alpha`. For a billboard that second scale is a pure
      duplicate: `sdfBillBoardMakeN` never sets `mesh.diffuse`, so `uDiffuseAlpha` is 1.0, and VU1
      draws these through `spriteOne..spriteFour`, which never run `unpackTexture` and so never put
      a material alpha into the vertex color at all.

      `DrawMesh::billboard` now selects `texA * color.a`. Measured on the specters alone, as full
      minus `SDF_NO_BILLBOARDS` against full minus `PCSX2HOOK_GS_SKIP=tbp=15606,tbp=15609`:

      | | mean rgb over the specters |
      |---|---|
      | emulator | 18.0, 13.9, 27.4 |
      | before | 32.1, 26.4, 37.6 |
      | after | 24.9, 16.3, 28.2 |

      Battle goes 6.634 to **5.354**; no other scene moves.

      **Still 1.38x on red, and our quads read rounder than the emulator's**, which has a visible
      figure inside the halo where ours is a smooth blob. That is geometry, not alpha: the next
      thing to check is the `right`/`up` axes `sdfBillBoardMakeN` takes from `mScreenTm`'s columns.

      The same duplicate is **not** removable for the mesh path: doing so costs underground 4.272 to
      22.681. That is V8's knot, and the depth fix above did not loosen it.

- [ ] **V22: the player's hair, trousers and shoes are missing. Their material draws nothing.**
      Reported from the battle scene and confirmed: the emulator's silhouette has a full head of
      hair, ours shows the background through it.

      The material is **tbp 13107**. At frame 230 the GS draws it as two batches, i=167 with 48
      vertices and i=168 with 336, the second spanning the head box exactly (bbox x 265..298,
      y 114.7..134.9 in field coordinates). Against that we emit **75 triangle-vertices to the GS's
      384**, and `SDF_DRAW_ONLY=tbp=13107` with `SDF_DEBUG_DEPTH=1` renders an **empty depth
      buffer** over the head, so it is absent rather than drawn black on black. The full-scene depth
      pass shows the collar spikes and the torso with no hair mass above them.

      What is already ruled out:

      - **Not a traversal miss.** `ModelLedger` reports 12138 packets walked against 12138 the
        engine generated, and `chain refs: 5950 setup, 0 looked like a mesh and failed`, so no mesh
        stream was abandoned by `readVifStreamMesh`'s error paths.
      - **Not the character's own meshes.** Isolating tbp 12662 and 12827 on both sides gives the
        same headless torso, so the hair was never part of those.
      - **Not `sdf3DPrimMakePoly`.** Its 12852 "without geometry" reports are `fldSdwDraw`'s
        shadows; the refs it skips as setup hold floats (0x3E.., 0xBE..), not VIF codes.
      - The three tbp 13107 meshes we do emit are **type 1 with `worldGeometry` set**, so they are
        arena geometry sharing the texture, not the character.

      **The missing meshes are the single-bind ones**, which the GS trace confirms and quantifies.
      Pairing each ctxt 0 batch with the ctxt 1 batch of equal vertex count that follows it, frame
      230 has 206 batches: **148 in a two-pass pair and 18 single-bind textured triangle batches,
      916 triangles**, over tbp 13812, 13746, 12397, 12380, 13107, 13177, 13444 and 13186. Those are
      the same textures the per-texture vertex comparison measures short, and the two badly short
      ones are **13107 at 0.20 and 13177 at 0.06**. Our own run summary says
      `multiTexturedMeshes: 12036` against `singleTexturedMeshes: 102`, under one a frame.

      Every generated packet we see carries `attrBits 0x1078`, so the second-UV bit is set on all of
      them; the hardware nonetheless emits 18 batches a frame with no second pass.

      Four more causes ruled out by measurement, none of them it:

      - **Not the bounds cull.** Letting all 1904 culled nodes through leaves the head empty.
      - **Not the draw-packet buffer parity.** `node->drawPacket[0]` and `[1]` give the same
        silhouette, so the double buffer is not being read on the wrong side.
      - **Not the mesh/setup classifier.** Instrumenting the rejected chain refs shows **none of the
        5950 begins with a VIF unpack at all**; they are raw float data, so no mesh stream is being
        discarded as setup.
      - **Not a decode failure.** Every reject counter in the summary is zero, in all five geometry
        buckets.

      So the hair's packet is not reached by either walk we do: `sdfCorePacketGen` (12138 walked,
      12138 generated, ledger agrees) or the DMA-call chain (7616). **Next: run the producer net.**
      CLAUDE.md's closed producer set is the five draw-tag linkers -- `sdfDrawTagLink`,
      `sdfDrawTagLink2`, `sdfDrawTagLinkRef`, `sdfDrawTagLinkRefGif`, `sdfDrawTagLinkCall`. Hook all
      five, resolve `$ra`, and find which one links the packet that binds tbp 13107. That names the
      producer we are missing, and the same run answers it for 13177.

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

- [x] **V5: light 0's color is a difference, and the base is `color[3]`.** The character was evenly
      bright where the reference lights him hard from the front and leaves his back in shadow. Four
      candidates had already been ruled out by measurement (scaling the base, scaling the whole sum,
      the specular term, the sign of the dot), which left the inputs.

      The VU1 image says what no struct in the game does. At 0x01BB..0x01C5, run once per material
      under a flag set when one is unpacked (0x00A8) and cleared by the rebase that consumes it:

          mem[1003] = mem[1004] * mem[927]                        // base    = ambient * color 3
          mem[924]  = mem[1002] * mem[923] - mem[1004] * mem[927] // light 0 = diffuse * colorBase - base

      `mem[923]` is `colorBase` and `mem[924..928]` are `color[0..4]`, which independently confirms
      the struct offsets. So **light 0's color is a difference, not a color**: a vertex facing it
      reaches `colorBase` and one facing away falls to `color[3]`, the ambient. The packet's own
      `color[0]` is scratch and never survives. `color[4]` is `colorBase + color[3]`, exactly, in
      every scene dumped, and 0x0188 shows a mesh with **no normals takes it outright** rather than
      running the sum or taking the base.

      Reading the struct's names at face value instead gave a range of `colorBase` to `2 * colorBase`,
      which is why the base looked twice too large and why no scalar on it fitted every scene.

      **Confirmed against the hardware, not the picture.** The GS trace already carries each batch's
      post-VU vertex color. On field unknown realm the two dominant values over 34,586 textured
      triangle batches are `[38, 25, 25]` and `[128, 89, 64]`; that packet's `color[3] * 128` is
      `[38, 25, 25]` exactly and its `colorBase * 128` is `[128, 89, 89]`. Underground agrees, every
      value falling under its `[115, 128, 115]`. **No batch in either scene carries a value in the
      old model's range**, so the old reading was wrong everywhere and merely compensating.

      The materials are measured, not assumed: `ambient` is `0x80808080` on all 16,954 meshes and
      `diffuse` varies only in its alpha byte, so both coefficients are 1.0 and the two forms are
      byte-identical renders.

      `docs/vu1/` now holds the image, the disassembly and annotated copies of these routines.

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

      - The sum's two ends match the hardware's own vertex colors exactly. His root resolves to
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
      the pass writes color and never depth. Table 2 it is, and battle goes **9.408 -> 9.209** with
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

      **The coordinate is `scale * dot + offset`, and our soften/bias are exactly that pair.** An
      earlier note here claimed the coordinate carried no soften, on the strength of
      `corePktApplyFaceDivColor` showing a bare `0.5 + 0.5x`. That was reading one half of the
      calculation: the scale and offset are applied upstream, when `x` is produced.

          0x2B6DF4  vf24.w = vf19.w * vf24.x   the light direction is scaled by vf19.w
          0x2B6E30  ACC.w  = vf0.w * vf19.y    and an offset vf19.y is added
                    s = vf19.y + vf19.w * dot(n, dir)   ==   soften * (bias + dot)

      So the shader's form is right and the fitted values stand in for `vf19.w` and `vf19.y`.
      Dropping them costs **0.49 on field and 0.13 on battle**, which is the measurement that
      refuted the first reading.

      **They cannot be read live.** `vf19` comes from the global `corePktFaceDivParm` (0x0058F030),
      which `sdfCorePacketGen` copies into the cb at +0xE30/+0xE40/+0xE50 during generation. The walk
      runs before the engine touches any of it, so a read at walk time returns the previous packet's
      values. Getting the real pair means capturing it at generation, not resolving the symbol.

      **What was wrong and is now fixed: the material terms were counted twice.** VU1 at
      0x01C1..0x01C5 folds them into both endpoints (`lo = matAmbient * color[3]`,
      `delta = matDiffuse * colorBase - lo`), and the shader then multiplied the sum by `uDiffuse`
      again. Removing it is score-neutral on all six scenes because every material term measures 1.0
      here, so this is a correctness fix that no scene can currently detect.

      **The ramp's endpoint colors are captured but reach nothing, and that is now reported rather
      than silent.** `corePktSetFaceDivParm` tail-calls `corePktMakeFaceDivParm`, so a post-handler
      on the first reads the block the second filled: on field unknown realm the coordinate is
      **scale 7.01, offset 0** (the root's 7 plus the material's own 0.01, which confirms `vf19.w`
      and `vf19.y` directly) and the endpoints are **lo (38, 25, 25), delta (90, 64, 39)**, so hi is
      (128, 89, 64) with 128 as one.

      Wiring those into the second pass changed **no scene by any amount**, and the reason is worth
      keeping: cross-tabulating the lookup against the pass that consumes it gives

          152 found   ramp=false algo=3      the constant-color pass, which never interpolates
           76 missing ramp=false algo=3
           81 missing ramp=true  algo=1      the ramp pass, and it has no captured endpoints
            6 missing ramp=true  algo=2

      **The two sets are disjoint, and `corePktEmitMultiTexPass` (0x002B8C30) says why: the ramp's
      endpoints are built on the stack, not in the global.**

          0x2B8C68  s0 = cb[84]              the material packet
          0x2B8C6C  a0 = s0[36]              mtexAlgo: < 3 takes the ramp, == 3 corePktSetConstColor
          0x2B8C90  s0[8] == s0[12]          the two diffuse colors equal, so no ramp
          0x2B8CA0  a0 = sp                  <- corePktMakeFaceDivParm writes a **stack** block
          0x2B8CC0  jal corePktMakeFaceDivParm
          0x2B8CD0  a1 = sp                  <- and corePktApplyFaceDivColor reads that same block

      So `corePktSetFaceDivParm`, which fills the global `corePktFaceDivParm`, serves a different
      consumer. Hooking it can never see the ramp pass's endpoints, however it is keyed. The two
      branch conditions also confirm our `multi.algo` and `multi.ramp` derivations exactly.

      **That capture is now in (`corePktApplyFaceDivColor`, `a1` the endpoints and `a0->matPacketPtr`
      the key), and it proves the pass never runs here: the hook fires zero times in these scenes.**
      `sdfCorePacketCb_t` went into `libsdf.h` for it, and the struct is a finding of its own: the
      reconstructed HD header maps onto this build with a **constant +60 shift**, `matPacketPtr` at
      0x54 against HD's 0x18, `fdSoften`/`fdBias` at 0x5C/0x60, `pointBuff` at 0x64, `pointCount` at
      0x42. Every read in both functions fits, `lw a2, 88` included, which is `color`.

      **It also found a real bug in our ramp condition.** 0x2B8C90 compares the material packet's own
      two colors, `+8 diffuse` against `+12 mtexDiffuse`; we compared `sdfMat_t`'s `diffuse` against
      the packet's `mtexDiffuse`, which is the same trap this file already records for the fd terms.
      Fixed, and it took the meshes claiming a ramp from **87 a frame to 58**, score-neutral on all
      six scenes.

      **58 is still not 0, and the hardware's count is 0.** Watching the caller settles why:

          corePktEmitMultiTexPass <- corePktEmitAllPasses+0x2C   x152
          corePktSetConstColor    <- corePktEmitMultiTexPass+0xB8 x152

      **Every one of the engine's 152 second passes takes the constant-color branch. The ramp is
      never taken in this scene**, and 152 is exactly the algo 3 count from the cross-tab above, so
      the two agree. Our 58 ramp meshes are therefore wrong, not merely unmeasured.

      **Resolved, and the stale-packet theory was wrong.** Cross-tabulating at the point a mesh is
      actually added to the frame gives, over 5 frames:

          152 ramp=false algo=3 stale=0     exactly the engine's 152 corePktSetConstColor
          112 ramp=false algo=0 stale=0

      **The drawn set never ramps, and no material is stale**, so the duty bit explains nothing here.
      The 58 came from `readMaterialPacket`, which runs for materials whose mesh is never drawn, so
      the report was counting a set that does not reach the screen. It now sits beside the mesh's own
      `addMesh` and reads 0.

      So our drawn output already agrees with the hardware on this pass: the ramp is dead code in
      these scenes, exercised by neither side. Leave it modelled and reported rather than deleted, on
      the usual grounds, but stop attributing anything in the picture to it.

      **The count difference is real, and it is a texture problem, not a gating one.** Three
      measurements over the same 5 frames line up exactly:

          corePktEmitMultiTexPass                     x152   the engine's own calls
          meshes with attrBits & 0x1000 (2+116+34)     152   sdfCoreAttrMultiTexPass
          multi-texture pass: meshes on                 76   ours, drawn

      **Those three numbers come from a 5 frame run and two of them are startup artefacts. Do not
      use them.** Over the standard 120 frame run the drawn set reads:

          6300 mesh rows carry attrBits & 0x1000
          6195 of them (98.3%) have a resolved second-pass texture
           105 do not, and every one uses a tbp that resolves on other meshes
               (11181 x64, 11612 x26, 10888 x8, 10883 x4, 13321 x2, 13459 x1)

      So the second pass is **not** substantially missing: the 105 are the first frames, before
      recovered textures land, which is expected since a recovery reaches the frame after the lookup.
      A short run makes this look like a systematic failure because recovery has not settled.

      Two readings to avoid, both mine and both wrong when checked:

      - The 2x between 152 and 76 is **not** the back-face repeat. No mesh in the scene sets
        `attr & 0x4000` (the histogram is 0078, 0079 and 1078 only), so `corePktEmitAllPasses` runs
        each pass once. The ratio was a coincidence of a 5 frame sample.
      - "About 30 a frame against our roughly 52" compared engine calls against every mesh reaching
        the code rather than the ones with the pass enabled. Wrong denominator.

      **Measure this class of thing over a full run.** Anything downstream of texture recovery is
      wrong for the first frames by construction, so a short run reports a defect that does not
      exist.

      `reportUnsupported` now fires on a ramp mesh with no capture, so the run summary counts it
      instead of the path quietly measuring zero.

      The three bodies, for anyone re-checking the above:

          corePktXfmLightClip     0x002B6DE0  s = vf19.y + vf19.w * dot(n, dir)   per vertex
                                  0x002B6E50  stores 2s-1 at vertex +16
                                  0x002B6E80  color = vf20 + vf21 * clamp(s,0,1) at vertex +32
          corePktApplyFaceDivColor 0x002B72D8 t = clamp(0.5 + 0.5*(2s-1), 0, 1) = clamp(s, 0, 1)
                                              color = param[16] + param[32] * t at vertex +32
          corePktSetFaceDivParm   0x002B6A30  adds its two floats into material [40] and [44],
          corePktMakeFaceDivParm  0x002B6968  which scale the endpoint **colors** (vf5.x, vf6.x,
                                              vf7.x) and are never applied to t

      So the base pass is itself a ramp, `lo + delta * clamp(s, 0, 1)`, with the same coordinate the
      second pass reads back; soften and bias belong to the endpoints. Our fragment shader instead
      uses `clamp(uFaceDivSoften * (uFaceDivBias + dot))`, a terminator 7x too steep, which reads on
      screen as **shoulders far too dark with hard banding on the arms while the back looks right**.

      **Removing the soften alone costs 0.49** (field 5.891 to 6.382, battle 6.896 to 7.022), though
      the signed mean improves (+3.400 to +2.535). So the endpoints are wrong too and the steep ramp
      was compensating by crushing more pixels onto the dark one. Ours is
      `(ambient + t * (diffuse * base - ambient)) * diffuse * drawColor`, where the hardware is
      `vf20 + vf21 * t` with no ambient term and no second diffuse multiply. **Fix the endpoints and
      the coordinate in one change**, and judge it on the character's own box (reference L/R 1.163)
      rather than the frame mean, which is the same trap the negated dot set.

      The character shades smoothly where the reference has hard two-tone bands. Measured on the
      character's own box, the reference spans 143.6 luminance against our 90.6, with sharper
      transitions (mean |dx| 3.41 against 2.07).

      **It is not the lighting sum.** `PCSX2HOOK_VU_WATCH` on the light slots confirms every input
      live: `base(1003) = matAmbient * color[3]` and `color0(924) = matDiffuse * colorBase - base`
      hold exactly, both material terms are 1.0, one light is active and its direction matches ours.
      It is not a ramp texture either: the scene has none. It is not subdivision density, since
      lighting per fragment scores 8.358/8.724/8.834 against 8.347/8.778/8.834.

      It is a second coloring pass we do not model at all. `corePktEmitMultiTexPass` recolors
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
      x3103 and `mtexAlgo` 2 x230 take the ramp, `mtexAlgo` 3 x8665 takes the constant color, and 2
      keep their vertex colors. So about 3,300 second passes a run are ramped and 8,700 are flat,
      and we draw all of them with the first pass's smooth `vLit`. `mtexAlgo` is now reported.

      **Implemented.** The mesh carries `mtexAlgo` and `constant`, and the second pass takes its own
      vertex color in the shader rather than reusing the first pass's `vLit`:

          lit = mtexDiffuse * colorBase
          lo  = mix(ambient * color3, lit, constant) * drawColor
          hi  = min(lit * drawColor, 2.0)
          col = lo + (hi - lo) * clamp(0.5 + 0.5 * dot(n, lightDir0), 0, 1)

      with `mtexAlgo == 3` taking `mtexDiffuse` flat and anything above it keeping the first pass's
      colors. The value it moves is the one the defect was stated in, the sharpness of the
      transition across the character's box:

      | | mean abs diff | edge sharpness (mean abs dx) | range |
      |---|---|---|---|
      | before | 8.347 | 2.07 | 90.6 |
      | with the ramp | **8.306** | **6.24** | 95.9 |
      | reference | | 10.28 | 143.6 |

      Underground 8.778 -> 8.739, battle unchanged. Three times the transition sharpness, and the
      tattoos read as crisp cyan rather than soft teal.

      **The coordinate is now exact too.** `corePktXfmLightClip` stores `2v - 1` at `point[+0x10].w`
      where `v = fdSoften * (fdBias + dot(lightDir0, n))`, so the ramp's `clamp(0.5 + 0.5*(2v-1))`
      is `clamp(v, 0, 1)`. `fdSoften` is the material's plus the root's, clamped to at least 1, and
      measures **0 + 7 = 7** live: it sharpens rather than softens, which is what the name hides.

      **The ramp terms are no longer fitted: they are read from the guest.** This entry used to say
      `sdfCorePacketGen` takes them in `$f16`/`$f17` and that neither could be read. Both halves were
      wrong. 0x2B91A8 and 0x2B91B0 save **`$f12` and `$f13`** into `$f20`/`$f21` and 0x2B9230/0x2B923C
      hand them straight back to `corePktSetFaceDivParm`, so a pre-hook at the entry reads them out
      of `regs->fpr`. `setFaceDivTerms` does that; Config is now only the first-frame fallback.

      The guest's values on field unknown realm are **soften 7, bias 0**, which is what the sweep
      below had fitted, so that scene does not move. `smt3_field_underground_facility` does, 6.338 to
      6.329, which is the measurement that the two are not the same everywhere. The fit that stood:

      | soften | 1 | 2 | 3 | 4 | **7** | 12 |
      |---|---|---|---|---|---|---|
      | | 8.309 | 8.128 | 8.037 | 7.991 | **7.948** | 7.961 |

      | bias | -0.2 | -0.1 | **0** | +0.1 |
      |---|---|---|---|---|
      | | 6.249 | 6.068 | **5.925** | 6.210 |

      Both minima land exactly on the globals `sdfFaceDivSoften` (7) and `sdfFaceDivBias` (0), which
      is independent confirmation of two values the disassembly can only be guessed at for. They are
      `Config::faceDivSoften` and `faceDivBias` (`SDF_FACEDIV_SOFTEN`, `SDF_FACEDIV_BIAS`), each
      documented as fitted so the next reader can re-derive rather than trust them.

      Scores do not move from the placeholder `0.5 + 0.5*dot`, and the reason is measured rather
      than assumed: the second pass shows only through a mask that is 72% transparent, and on the
      texels that do show, `t` saturates to 1 under both forms. Forcing the branch magenta moves the
      score (8.306 -> 8.260), which is what proves it runs at all.

      **The constant path was missing a term.** `corePktSetConstColor` (0x2B7370) is
      `cb[0x58] * mtexDiffuse / 128`, a product of two colors, where this took `mtexDiffuse` alone.
      In normalised units that is `uMtexDiffuse * uDrawColor`. It covers **8,665 of the ~12,000**
      second passes, the majority, and the scores do not move only because the draw color is
      `0x80808080` on every mesh in these scenes; a tinted draw would have been wrong.

      **The ramp belongs to the base pass too, and that was the rest of it.** `corePktEmitAllPasses`
      runs base, then multitex, then refmap, and `corePktXfmLightClip` writes the vertex color
      *before all three*: its `vf8 = parm[0x10] + parm[0x20] * clamp(v, 0, 1)` is the same ramp. So
      the base pass's color is the face-division ramp, not a plain clamped dot.

      The GS trace said so all along and I read it as a curiosity: 3062 batches at exactly
      `colorBase`, 2829 at exactly `color[3]`, only 535 in between. That is a **saturating** ramp,
      not a smooth dot, and `soften = 7` puts the whole transition inside `dot` of 0 to 0.14.

      The endpoints were already right, so the fix is the interpolant alone, `max(dot, 0)` becoming
      `clamp(soften * (bias + dot), 0, 1)`:

      | | field / underground / battle | edge sharpness | range |
      |---|---|---|---|
      | before | 8.306 / 8.739 / 8.834 | 6.24 | 95.9 |
      | base pass ramped, per vertex | 8.008 / 8.428 / 8.795 | 8.67 | 137.0 |
      | ramp sampled per fragment | **7.948 / 8.395 / 8.743** | | |
      | reference | | 10.28 | 143.6 |

      **A step function cannot be interpolated.** Ramping the base pass per vertex faceted the
      character: the clamp lands at triangle corners and the mesh topology shows through the skin,
      which the reference does not have. Face *division* is what makes the hardware smooth, since it
      subdivides before applying the ramp, so the step is sampled far more densely than the original
      vertices. Sampling it per fragment is the equivalent and removes the faceting.

      This is the opposite of the earlier per-fragment measurement, which was byte-identical: that
      one moved only the **second** pass, where the mask is 72% transparent and the ramp saturates
      on every visible texel. On the base pass the same change is worth 0.06 to 0.05 a scene and
      removes a visible artefact.

      **The fragment override has to be gated to the base pass.** Applying it to every pass
      overwrites the second pass's own color, which took the tattoos' cyan away entirely and cost
      0.14; `uMtexPass == 0` is the guard.

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

- [x] **V15: the shaders had two silent wrong answers, and now fail loudly instead.**
      Prompted by the rule that an unhandled state paints magenta rather than something plausible.

      - **ATST 4 (EQUAL) was missing** from `atestFails`, so it fell through to "never fails". The
        GS's eight comparisons are now all present, with 1 (ALWAYS) correctly never failing.
      - **TFX 2 and 3 added the vertex color where the GS adds the fragment alpha**, and were
        lumped together though HIGHLIGHT and HIGHLIGHT2 differ in the alpha they output. Rather than
        render an alpha path never measured, both now paint magenta and `reportUnsupported` names
        the value.

      Both measure **zero** in every scene: TFX is MODULATE on all 36,609 field and 23,336 battle
      textured batches, and ATST is only ever 0, 1 or 5. Scores are unchanged (8.306 and 8.834) and
      the unsupported report stays empty of TFX, which is what makes the zero a measurement rather
      than an assumption.

      `kUnhandled` is in both shader stages now, and covers `mtexAlgo` above 3 as well.

- [x] **V16: the whole frame was half a pixel off, and it was worth more than every shading fix
      put together.** Chasing a zero diff on field unknown realm, the largest single error turned
      out not to be shading at all.

      Shifting the *reference* against ours and re-scoring finds a clear minimum at one pixel down
      and right (8.021 at (0,0) against 6.568 at (+1,+1)). Splitting the frame into regions shows
      **every** one of them offset by the same amount:

          HUD text 'Unknown Realm'   20.81 -> 15.33 at (+1,+1)
          HUD compass                 9.28 ->  7.59 at (+1,+1)
          3D ceiling                 12.41 -> 10.54 at (+1,+1)
          3D character               14.71 -> 10.33 at (+1,+1)

      The HUD is drawn by the sprite pass from explicit pixel coordinates and the meshes by the
      engine's own screen matrix, so a common offset is neither path's arithmetic: it is the raster
      convention. GL samples at pixel centres where the GS covers the pixel a primitive's edge
      reaches, so the frame needs half a pixel.

      **Give the two axes their own offset.** The guest is 512x224 and the target is twice as tall,
      so a "guest pixel" is one rendered pixel across and two down; a single shared offset finds a
      compromise (best 6.581 at 0.25) and hides the answer. In rendered pixels:

      | x, y | 0, 0 | 0.5, 0.5 | 0.5, 1.0 | 1.0, 0.5 |
      |---|---|---|---|---|
      | field unknown realm | 7.948 | **5.925** | 6.701 | 6.935 |

      Half a rendered pixel on each axis, which is the principled value, is also the measured
      minimum. `Config::pixelOffsetX` and `pixelOffsetY` (`SDF_PIXEL_OFFSET_X`, `_Y`) apply it in
      NDC to both the sprite and the mesh vertex shaders, so the whole frame moves together.

      **Measure alignment before attributing error to shading.** The worst tiles off the character
      had *matching mean colors* and large per-pixel diffs, which is the signature of a shift
      rather than a wrong color, and 75% of the frame's error was outside the character box.

- [ ] **V17: the ceiling is too bright, and it is VU1's vertex-color variant at 0x0173.** With the
      half-pixel offset in (V16), the worst tiles split into two populations: the character, now
      consistently too **dark** (102.4 against 118.0 red), and the top strip, too **bright** (78.3
      against 60.0).

      The pass switches attribute the ceiling to nothing we model. Only fog moves it at all, and in
      the right direction:

          baseline 78.3   NO_FOG 91.4   NO_SPECULAR 78.3
          NO_FEEDBACK 77.8   NO_BILLBOARDS 78.3   NO_SPRITES 78.8   (reference 60.0)

      So the geometry's own color is too bright and fog is partly masking it. **6,240 of the 6,600
      type 1 stream meshes carry vertex colors** (`attrBits & 0x800`), which the decoder reads, and
      the shader then multiplies by the unlit `color[4]` term as well.

      Dropping that term for those meshes **overshoots**: 45.9 against the reference's 60.0, and the
      scene goes 5.925 -> 6.860. So the hardware lights them, just not the way the subtraction
      assumed.

      **Transcribing the two vertex-color branches rules both out.** They were the obvious suspects
      and neither is wrong here:

      - `0x0173..0x0186`, the lit variant: `drawColor /= 128` at 0x0174, `vf05 = vertexColor *
        drawColor` at 0x017D, then `min((base + sum) * vf05, 255)`. Plain `max(dot, 0)`, no
        face-division ramp.
      - `0x019D..0x01AF`, the **flat** variant these meshes actually take, since they carry no
        normals: `vf03 = color[4] * drawColor / 128` at 0x01A1, then `vertexColor * vf03` per
        vertex, two at a time.

      So the hardware computes `vertexColor * color[4] * drawColor / 128`, and the shader's
      `vColor * 2 * (uLightFull * uDiffuse * uDrawColor)` is the same expression. **This path is
      already right**, and the ceiling's brightness is something else.

      **It is a missing texture.** The environment covers the screen with two of them, and only one
      arrives: 13459 recovers as handle `005EE700` and we sample it as a second pass, while **16216
      is never recovered at all**: no handle in the run resolves to it, so nothing ever queues it.
      5355 batches a run, 128x128 PSMT8, spanning x 0..512 y 0..196.

      **It is not the reflection pass, though that pass is real and missing.** Reading
      `sdf3DRefMapParam` at runtime gives `tex0=8007FF859D30BFDC`, **tbp 16348**, 128x64 PSMT8,
      CLUT at 16380, and 16348 draws **119 batches, one a frame**. So the refmap is a small
      contributor here and 16216's 45 a frame are something else. Inferring which pass owned a tbp
      from batch counts was wrong twice; the global says it outright.

      **The reflection pass is still unmodelled**, and the note that called it a highlight was
      wrong:

      `corePktEmitRefMapPass` ends in `corePktEmitTexTrisCtx2(cb, 0, sdf3DRefMapParam, 5, 72, 32)`:
      a **textured** pass, TEX0 from the global `sdf3DRefMapParam`, ALPHA 72 which is additive. Its
      per-vertex loop stores the scalar to `+0x30`, the texcoord slot, not to a color, so it is a
      sphere map indexed by the normal dotted against an axis from the world light matrix, tinted by
      `specularColor * cb[0x58] / 128` written to `+0x40`. CLAUDE.md said this pass was a highlight
      rather than a lookup; that is corrected.

      **Implemented.** `sdf3DRefMapParam` reads `tbp 16348`, 128x64 PSMT8 with its CLUT at 16380,
      and V18's TEX0 recovery brings it in (65 textures to 66). The pass draws third, additive,
      with `uRefMapPass` giving it its own UV: **u is the clamped dot against `specularAxis`** (which
      is `TM.row[3]`, the `light[+208]` the routine loads) and **v selects a row from the material's
      alpha**, since the loop writes `vf5.x` from the dot and `vf5.y` from `specularAlpha / 128`.
      CLAMP 5 is both axes clamped, so it is a ramp lookup rather than a sphere map.

      **It measures zero on both unknown realm scenes**, byte identical. 5,023 meshes carry the attr
      bit but `specularColor` is zero on nearly all of them, and the hardware agrees: only **one GS
      batch a frame** samples 16348. So the pass is modelled and inert here rather than missing.
      Judge it on a scene where it draws more.

      **What 16216 actually is, is still open, and the gap is bigger than one texture.** Comparing
      vertices per texture, ours submitted against the GS's drawn:

          tbp 11893   1232700 GS   822360 ours
          tbp 13525   1069182 GS   900240 ours
          tbp 11214    426372 GS   212760 ours
          tbp 16216    191544 GS        0 ours
          total      4452756 GS  2802052 ours

      (`tbp 11181` reads as 0 only because a mesh row records its first pass; the second pass is
      drawn.) Ours is what we submit and the GS's is what survived VU1, so the two are not the same
      quantity and the totals are not directly comparable, but **being under on every single texture
      is the wrong direction** for that difference, and 16216 has no counterpart at all.

      **Re-measured with every producer hooked**, our total is 3,458,062 against the GS's 4,452,756,
      and the shape of the gap points somewhere specific:

          tbp 11893   GS 1232700   ours 822360   short 410340
          tbp 11214   GS  426372   ours 212760   short 213612
          tbp 16216   GS  191544   ours      0   short 191544
          tbp      0                ours 383280   <- ours, drawn with no texture at all

      **Our untextured vertices are not 16216's, and that hypothesis is closed.** They are
      `attrBits 0x0868`, which is **TME=0**: untextured by design, gouraud with vertex colors
      (`0x800`) and blending. A mesh drawn with no texture because the packet asked for none is not a
      texture lookup that failed.

      Two traps in making that comparison, both hit once here:

      - **A mesh row's `x0`/`y0`/`x1`/`y1` are model space; the screen bounds are `ndc`.** The
        character's own meshes read x -5..31 where the GS puts that texture at x 193..317. `onMesh`
        already projects through the bone and screen matrices and emits the result as `ndc`, so use
        that field. Screen pixels are `(ndc + 1) / 2 * [512, 224]`: the scene renders **224 tall**
        (scissor `0,511,0,223`), not 448.
      - **`fbp` 56 and 112 are the two halves of a double buffer**, not a render target. Both carry
        about half the frame's vertices, and reading the split as an offscreen pass invents one.

      **16216 is a blended, depth-suppressed, single-texture quad pass**, and the ranking says whose:

          5355 batches / 119 frames  = 45 a frame, every one prim 3, nverts a multiple of 6 (quads)
          ALPHA 0x44, TEST 0x53001   = ATST NEVER + AFAIL RGB_ONLY: color only, no depth, no alpha
          PSMT8 128x128, TFX MODULATE, union bbox x 0..512 y -0.5..196

      `fldSdwDraw` heads the `[todo]` list at **12,840 packets, 108 a frame** against those 45 merged
      batches, and a field shadow pass is exactly a small texture drawn as many blended quads that
      must not write depth. That is the candidate to hook next, and the check is whether its texture
      resolves to tbp 16216. It is 6584 bytes (0x001193E8..0x0011ADA0) and wedged IDA once; read it
      with objdump.

      **Do not hook it at `sdf3DPrimMakePoly`, which is already hooked and is not the path.**
      `fldSdwDraw` does call it, at 4 sites, which makes it look covered. It is not: the hook
      produces **zero draws in this scene** and reports nothing unsupported, so those sites do not
      run here. Its call census says where the packets come from instead:

          4x sdfDrawTagLink   4x sdf3DPrimMakePoly   4x fldPoint3To2_B   5x rsetTest   3x rsetAlpha
          2x sdf3DPrimLinkMat  2x sdfDrawTagInit  2x sdfGrAllocPacketBuff  11x memset
          1x aDrawBoxF   1x aDrawBoxF_Direct   1x ooyTestRGBMaskOn/Off

      The `[todo]` count attributes to the caller at `sdfDrawTagLink`, so those 12,840 packets are
      the **4 direct link sites**, hand-built packets alongside the primitive calls. `ooyTestRGBMaskOn`
      and `aDrawBoxF_Direct` also put part of this function in the FBMSK bloom-mask pass we keep off
      deliberately, so expect some of its packets to be correctly undrawn.

      **`SDF_DUMP_PACKETS=fldSdwDraw` settles what it links.**
      It emits a repeating triple, 68 of each over 3 frames (~23 shadows a frame against the 45 GS
      batches, VU1 merging the difference):

          dmaId 3, qwc  8   REF   an identity matrix, so shadows arrive already in world space
          dmaId 3, qwc 10   REF   material: `6E05C000` then 12 bytes of 0x80, and ALPHA **0x44**
          dmaId 2, qwc N    NEXT  inline VIF geometry, N = 14..44

      **`fldSdwDraw` is not tbp 16216's producer, and reading ALPHA 0x44 as a signature is what said
      it was.** Decoding all 78 geometry packets gives `attrBits` **0x68 on every one**, which is
      IIP+FGE+ABE with **TME clear**: untextured. tbp 16216's batches run `tme=1`. 0x44 is simply the
      scene's most common blend, so matching on it identifies nothing. Two separate gaps, not one:
      `fldSdwDraw` is undrawn *untextured* geometry, and **16216's producer is still unknown**.

      These are also not the untextured meshes we already draw, which are `0x868`: the same bits plus
      `0x800`, vertex colors. So the geometry really is missing.

      **The batch header is there but not where our reader looks.** `readVifStreamMesh` is entered
      only when the **first** VIF code at the target is a count-1 width-8 unpack. Here the first code
      is `6E01C002`, and `unpackWidth(0x6E)` is **4**, not 8: it is a V4_8 color unpack (`8080F080`).
      The real header sits at offset 32 as `6D01C000`, count 1 and width 8, and decodes to
      `[faceCount, pointCount, attrBits, packetBits]` exactly as world geometry does, followed by a
      `6E10C001` V4_8 index array (3 bytes plus a pad per triangle) and then float positions.

          6E01C002  V4_8  x1  -> color 8080F080
          6003C003  S_32  x3  -> settings, then ITOP 04000004 and MSCAL 14000000
          6D01C000  V4_16 x1  -> faceCount, pointCount, attrBits, packetBits
          6E10C001  V4_8  xN  -> byte triangle indices, 3 + pad
                                 then float positions

      Header counts over the sample: faceCount 4..16, and pointCount is **3x faceCount on some and 2x
      on others** (16/48 and 12/36, but 12/24 and 16/32), so the two are not one primitive type and
      that has to be settled before decoding rather than assumed.

      **Written, and the geometry decodes: `ModelParser::fldSdwDraw` reads 5700 streams a run with
      sane world bounds** (x -14.5..104.8, y -112.2..85.9, z 357.4..410.6). The remaining 7140
      "refused" are the transform and material packets, which are not geometry, so that number is
      expected rather than a shortfall.

      The layout, since two guesses about it were wrong before it read:

          6E01C002  V4_8  x1   color, so the batch header is NOT at the target
          6003C003  S_32  x3   settings, then ITOP and MSCAL
          6D01C000  V4_16 x1   faceCount, pointCount, attrBits (0x68), packetBits
          6E10C001  V4_8  xN   triangle indices, 3 bytes and a pad per face
          6830C011  V3_32 xP   **the positions get their own unpack**; reading them at the end of
                               the index array takes this code word itself as the first x, which
                               shows up as a bound of 3.3e24 rather than as a failure

      **It is off by default (`SDF_DRAW_SHADOWS`), because drawn it costs 24 points**: field unknown
      realm goes 5.891 to 29.875 and underground 6.329 to 18.701, both with the signed mean swinging
      positive, so we paint bright grey where the hardware darkens.

      **Reading the material packet does not fix that, which is worth knowing before trying it.**
      Decoded, the qwc-10 REF beside each stream holds

           0: 6E05C000  V4_8 x5   ->  4/8/12: 0x80808080, then two zero words
          24: 6005C005  S_32 x5   -> 28: 1.0, **32: 0x00000044**, then zeros
          52: 640CC00A  V4_16 x12

      so its color is grey at 128 and its ALPHA is 0x44, **exactly the defaults the pass already
      draws with**. The 24 points are not a color or blend gap.

      **The masked pass is theirs, and taking the chain's running state instead of the ordering
      table's default is worth 1.3 and 3.9 points of the cost** (field 29.875 to 28.609, underground
      18.701 to 14.765). `collectPackets` already carries that state per packet, so `fldSdwDraw`
      takes it now rather than `sdfGsStateForOt(0)` plus a stale `mState->frameMask`.

      The GS trace says the profile is theirs: **every** masked batch in the scene is
      `fbmsk 0x00FFFFFF, prim 3, tme 0, abe 1, iip 1`, which is `attrBits 0x68` exactly, 1547 batches
      over 119 frames bounded x 190.7..393.0, y 78.7..224.0. FBMSK is modelled (`glColorMask` in the
      mesh pass), so a shadow that carried the mask would cost nothing.

      **It still costs ~22 points, so most streams are not picking the mask up.** `ooyTestRGBMaskOn`
      and `Off` are called once each in `fldSdwDraw`, so only the span between them is masked, and
      the running state at the geometry packet evidently has the mask off for the rest. Either those
      A+D writes land outside the chain we replay, or the unmasked shadows are genuinely drawn and
      their shading is wrong in some other way: the stream carries no vertex colors (`packetBits`
      0x60 has no color array) so every vertex takes the material's grey at 128, which over a whole
      ground plane is exactly the washing-out the signed mean shows. Settle which before turning the
      flag on: log the `fbmsk` each stream receives and compare the count against the 13 masked
      batches a frame.

      A second thing to weigh before reading the shortfalls as missing geometry: `corePktEmitAllPasses`
      runs every pass **a second time for back faces** when `attr & 0x4000`, so a two-sided mesh is
      two GS batches and one submission of ours. That alone would put the GS above us everywhere,
      which is what the table shows.

      `DrawMesh::streamColors` marks the vertex-colored meshes if it is wanted later.

- [x] **V18: a material can name a texture with no handle, and those meshes drew untextured.** The
      one environment mesh a frame carried `tex 00000000` and `mtex 0` while its material packet's
      TEX0 registers named tbp 13492 and 13459 outright. **468 meshes a run** were in that state.

      `recoverFromGs` goes handle to upload packet to TEX0 to pixels, which cannot serve these:
      there is no handle and the register is already in hand. It is now split at the register, and
      `getHandleForTex0` recovers straight from a TEX0, filed under the material packet's own
      address since there is no texture handle to key by.

      **Textures recovered went 26 to 65** and meshes with no handle **468 to 409**, all 65
      recoveries succeeding. The score does not move: the meshes it reaches carry 2,499 vertices of
      2.8M, under a tenth of a percent. Correct, and small here.

- [ ] **V19: the biggest gap on this scene is geometry we never draw at all.** With V18 in,
      tbp 16216 still carries **zero** of our vertices against the GS's 191,544, so it is not a
      texture-resolution problem. The run's own producer worklist says why:

          fldSdwDraw            25680 packets, not drawn natively
          ooyTestPoly3          12960
          dds3TrackPolyLinkOt    1644
          dds3FirstProcess       1440
          dds3BillboardTagLink    480
          drawSprite3D            240

      `fldSdwDraw` alone is 216 packets a frame. Rank by area rather than packets before picking
      one, per the note on `ooyTestPoly3` being worth 0.010 of a point.

      **Half this list is not missing geometry.** Reading each producer's `jal` targets through the
      map, which costs one objdump each:

          dds3TrackPolyLinkOt   effPkt3DPrimMakePoly, sdf3DPrimLinkResetTm, sdf3DPrimLinkMat
          dds3FirstProcess      dds3InitDrawEnvSub, sdfGrInitNewFrame, sdfGrOtInit,
                                sdfModelSetDrawEnv        <- per-frame setup, draws nothing
          fldDrwZSave           sdfLinkStoreImage         <- a frame buffer store, not geometry
          drawSprite3D          sdfBillBoardMakeTMPacket, sdfBillBoardMake1, SetRotateMatrixZ
                                                          <- the billboard path, partly handled

      So `dds3FirstProcess`, `dds3DrawFirstProcess` and `fldDrwZSave` are state and transfers rather
      than draws, and the ranking should exclude them the way it already excludes pure A+D writes.

      **The billboard path is the one real producer left, and `drawSprite3D` is not hooked.**
      `sdfBillBoardMake1` is in the table with `traceEntryHook` only, which CLAUDE.md already
      explains: the rotation reaches the transform packet through VU0 from `SetRotateMatrixZ` and
      never passes through that call, so hooking it alone gives an unoriented billboard.

      `drawSprite3D(aTexHandle, aSprite)` is the floor, and its whole descriptor is now read. `a0`
      goes to `sdfTexGetPacket` and `sdfTexGetPacketLength`, so that is the texture. `a1` is the
      sprite, and 0x12A388 sets up the `sdfBillBoardMake1(packet, centre, color, uvList,
      xyOffsetList)` call straight out of it:

          +0x00  f32[3]  centre       (a1 itself; Make1 reads only 0, 4 and 8 off it)
          +0x10  uv list              (a3 = a1 + 16)
          +0x20  xy offset list       (t0 = a1 + 32)
          +0x40  u32     color       (a2 = lw 64(a1))
          +0x44  f32     rotation     (lwc1 68, into SetRotateMatrixZ at 0x2B6060)

      A 0x48-byte struct holding position, size, color and orientation together, which is exactly
      why this is the level to hook rather than `sdfBillBoardMake1`, whose rotation only ever
      reaches it through VU0.

      **Implemented.** `sdfSprite3D_t` carries the layout with all four offsets asserted, and
      `ModelParser::drawSprite3D` builds the quad off the screen matrix's columns the way
      `sdfBillBoardMakeN` does, turning the offsets by the rotation first.

      **It draws nothing on either unknown realm scene**, both byte identical with the hook in. The
      `[todo]` ranking put 240 packets a frame under this name on the field scene, so either those
      packets reach the tag through another path or they carry no geometry. Find which before
      assuming the hook is wrong: the struct is measured and its asserts hold.
      `effPkt3DPrimMakePoly` reads **the same descriptor field for field** as `sdf3DPrimMakePoly`
      (offsets 0, 2, 4, 6, 8, 12, 16, 20, 24, 28, 32, 36 in both), so it is hooked to the same
      handler. It changes nothing on the field scenes, so whatever it draws is elsewhere.

      **`fldDrawPoly3` is already served by the `sdf3DPrimMakePoly` hook**, which is the payoff the
      boundary rule promises. Its calls are `sdfGrAllocPacketBuff`, `sdfDrawTagInit`,
      `sdf3DPrimLinkResetTm`, `sdf3DPrimLinkMat`, **`sdf3DPrimMakePoly`** and `sdfDrawTagLink`, with
      no `rset*` among them, so it is one more producer feeding the same primitive. Its own hook
      (`SDF_POLY`) adds nothing on top: the scene reads 5.891 with it on or off, which is V9's
      "implemented and does not rasterise" seen from the other side, and that entry can be closed
      against this rather than chased.

      Note the trap that caught this: the state fix that was worth 0.034 on `sdf3DPrimMakePoly` was
      applied to `fldDrawPoly3` by analogy, measured zero, and turned out to be unjustified because
      that producer never calls `rset*`. Check the calls before carrying a fix across.

      **The hook point is `sdf3DPrimMakePoly` (0x002BAAB8), not these producers.** `fldSdwDraw` is
      6584 bytes and reimplementing it would be the wrong level; its `jal` targets show it builds
      geometry through `sdf3DPrimMakePoly` four times, with `sdf3DPrimLinkMat`,
      `sdf3DPrimLinkResetTm` and `sdfDrawTagLink` around it. That is the `sdf*` primitive family the
      boundary rule prefers, so one hook serves every producer that uses it rather than one per
      disc.

      It takes **a single pointer to a polygon descriptor**, and every field it reads off that
      pointer is enumerated (`grep 'l[whb]u\?.*(s3)'` over the disassembly is the whole list):

          +0x00  s16  a count, added to the qword total
          +0x02  s16  vertex count
          +0x04  u16  prim word, or'd with 0x68
          +0x06  u16  written to the packet at +12, inverted (a mask)
          +0x08  u32  written to the packet at +20
          +0x0C  u32
          +0x10  u32
          +0x14  ptr  positions; non-null takes the stride from 96 to 224 and the prim from 18 to 19
          +0x18  ptr  UVs, and sets attr 0x10 (TME)
          +0x1C  ptr  second UVs, and sets attr 0x1000
          +0x20  ptr  colors, and sets attr 0x800
          +0x24  ptr  an allocator to call, defaulting to sdfGrAllocPacketBuff when null
          +0x28  f32  converted to fixed point into the packet

      Those three attr flags are the attrBits the mesh path already understands. The body builds a
      GIF packet (`0x6102C000`, `0x6E01C002`, `trunc.w.s` into 12.4), so **read the descriptor and
      ignore what it writes**, which is the boundary rule exactly.

      **The hook is in and reading real descriptors, and it falsifies half of that reading.** A
      probe over field unknown realm gives:

          verts 48  extraQwords 16  prim 0000  colorWord 8080F080  unk0C 00556000  unk10 00555D00
          verts 12  extraQwords  4  prim 0000  colorWord 8080F080  unk0C 00556000  unk10 00555D00
          verts 28  extraQwords 14  prim 0000  colorWord 8080F080  unk0C 00556040  unk10 005558D0
          verts 48  extraQwords 16  prim 0000  colorWord 408080F0  unk0C 00556000  unk10 00555D00

      So **+0x14 through +0x20 are null on every call**: the arrays whose non-null branches set TME,
      the second UV set and the vertex-color bit are the *extended* mode, and this producer does not
      use it. The data is at **+0x0C and +0x10**, both guest pointers, and +0x08 is a packed color
      rather than the pointer its packet write suggested. `vertexCount` and `extraQwords` read
      sensibly, so the header half of the struct is right.

      **Both pointers are now read, from the body rather than the prologue:**

      - **+0x0C** is a block of `extraQwords` words `memcpy`'d into the packet ahead of the geometry
        (0x2BACFC), defaulting to a global at `0x5297A0` when null. Prepended state.
      - **+0x10 is the vertex array**: 0x2BAD68 reads one quadword per vertex and writes three
        words, advancing 16 in and 12 out, with no conversion.

      **The vertices are float XYZ.** Dumping the raw words gives `42D1A5D2`, `42A7808C`,
      `43C55F62` = **(104.8, 83.75, 394.7)**, and one at `C2B771AF` = -91.7.

      **They are world space, so they need the screen matrix.** The magnitudes suit either reading:
      `fldSdwDraw` calls `fldPoint3To2_A`/`_B` first, which suggests screen, but the character's own
      mesh rows carry world bounds of `x0 -645, y0 -410`. Pushing the first vertex through
      `sdf3DScreenTm` settles it:

          verts 48  ndc (0.83, 2.5)     w 240
          verts 12  ndc (0.73, 2.2)     w 260
          verts 28  ndc (-0.0002, 0.78) w 200

      `w` is positive and in a sensible view range on every call, and the NDC stays inside a **±4**
      box, where re-projecting an already-projected point lands far outside it. The first version of
      `ModelParser::isWorldSpace` accepted anything within 512 units, which an NDC of 2.5 passes
      trivially and proves nothing; the box is what makes the verdict mean something.

      So the mesh path takes these, not the sprite path, and the remaining unknown is the topology
      alone.

      **The word it writes is the attrBits/packetBits pair.** At 0x2BACCC it builds
      `(s5 << 16) | (s1 & 0xFFFF)`: the low half is `0x68 | descriptor+4`, gaining 0x10, 0x1000 and
      0x800 as the arrays turn out non-null, and the high half is the stride, 96 or 224, gaining
      0x100 for UVs and 0x200 for colors. That is the same pairing the mesh path already reads.
      `a0`, seeded 3 or 6, is **words per vertex**, which is why the loop writes 12 bytes.

      **The topology is not in the descriptor.** `0x68` is IIP, FGE and ABE with PRI left at 0, and
      the measured `descriptor+4` is 0, so the primitive is implied by whatever consumes the packet
      rather than stated here. **The GS trace cannot supply it either**: these packets are on the
      no-GIF-geometry ranking precisely because VU1 builds their primitives after link time, so
      matching batches by vertex count finds only the ordinary textured triangles (48 verts, prim 3,
      tme 1 dominates at thousands of batches) and says nothing about this producer.

      **The vertex ordering says it is not a triangle list.** Stepping through a 48-vertex polygon
      and measuring the distance between consecutive vertices, the steps that would cross a triangle
      boundary in a list are:

          15.4  11.5  15.8  211.9  30.7  23.6  21.4  207.2  23.5  15.4  21.0  226.8  14.7  14.7  14.4

      Only **4 of 15** exceed 30, and the other eleven sit in the same 11 to 30 range as the steps
      *inside* a triple, so triples are not discrete. The three huge jumps jump at vertices 11-12,
      23-24 and 35-36, splitting the 48 into **four contiguous runs of 12**, each continuous within
      itself. Strips, fans or closed outlines, which is what a projected shadow silhouette would be.

      Comparing means alone says "list" (15.97 within against 57.87 across) and is wrong: three
      outliers carry it. Read the distribution.

      **+0x0C is the index list, and that settles the shape.** Dumping the block it `memcpy`s:

          48 verts, 16 words: 00020100 00050403 00080706 ...    (0,1,2)(3,4,5)(6,7,8)
          12 verts,  4 words: 00020100 00050403 00080706 000B0A09
          28 verts, 14 words: 00020100 00030200 00060504 ...    (0,1,2)(0,2,3), quads

      One byte index per lane, three to a word, and **+0x00 is the triangle count**. So it is an
      ordinary indexed triangle list and the two readings below, taken from the order the vertices
      happen to sit in, are both artefacts. Read the indices, not the vertex order.

      **Implemented and drawing, worth 0.034.** `ModelParser::sdf3DPrimMakePoly` builds the mesh and
      the hook draws it; `SDF_NO_PRIM_POLY` turns it off.

      The GS state was the whole difference. Under a guessed `sdfGsStateForOt(0)` the scene went
      5.925 to **5.987**, worse: `fldSdwDraw` sets its own blend and test through `rsetAlpha` (x3)
      and `rsetTest` (x5) rather than taking an ordering table's pair. Both were already hooked into
      `RenderState`, which `ModelParser` already held, so taking the state in force at the call
      gives **5.891**. A producer whose geometry is right can still score worse than not drawing it.

      **The 48-vertex call reads as four closed loops of 12** (last-to-first 15.43, 23.62, 23.46,
      14.37 against mean steps of 14.18, 20.05, 16.45, 15.99, and the spread from each run's first
      vertex rules out a fan). **The other calls do not**, so that is one sample rather than the
      shape of the producer.

      The 28-vertex call is the informative one. Its steps are bimodal, roughly 15 or 200 with
      nothing between, and the vertices alternate between two clusters: y about +83 (`42A7...`) and
      y about -91 (`C2B7...`). **Every vertex appears exactly twice**, 14 repeats in 28. That is a
      quad strip zig-zagging between two contours, which is a shadow volume's skirt: one contour on
      the caster, one on the ground.

      So the producer carries more than one shape and the reading has to come from the descriptor
      rather than from measuring gaps, which threw up three different answers across three samples.
      The count at +0x00 (16 for 48 vertices, 4 for 12, 14 for 28) and the state block at +0x0C are
      where the structure is stated; the block is `memcpy`'d whole, so dumping it is the next step.

      The probe in `sdf3DPrimMakePolyHook` prints the descriptor and the first vertices under
      `SDF_LOG=hooks=debug`.

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
      `sdfGrSetFrameMaskRGB`, so everything in it writes alpha and no color. The GS trace confirms
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
      geometry, the corner order (TL, TR, BL, BR) and the color unpacking all match `cmpDrawBoxG`
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

      **The current ranking, and it says the ordering table is picked wrong far more often than
      anything else.** 20582 batches, all matched, 11967 paired by texture and 6370 of those clean:

          b     4437    aref  3487    atst  3480    tme  1814    fge  900    afail  240

          x2700   b: gs=1 ours=2 | atst: gs=5 ours=0 | aref: gs=96 ours=128
          x780    tme: gs=0 ours=1 | fge: gs=0 ours=1 | b: gs=1 ours=2 | atst: gs=1 ours=0
          x727    aref: gs=128 ours=127

      The first signature is one fault, not three: ALPHA `b=2` with ATST NEVER **is table 2**
      (0x48 / 0x71801), and the hardware ran table 0/1's blend instead. So 2700 batches are being
      given the wrong ordering table's state by the transmit-order replay, and `b`, `atst` and `aref`
      all move together when that is fixed. Chase the table assignment, not the three fields.

      Note `applyBlend` takes a coarse `GsAlpha` enum, so `Additive` **is** 0x48: the enum cannot
      express a table mix-up, which is why this shows up as a state diff rather than a bad blend.

      **Localised, and it is the generated-packet link pairing.** Our own trace is enough to see it
      without the GS: cross-tabbing `ot` against the registers we assign gives **2995 meshes with
      `ot=0` carrying table 2's state** (`b=2`, ATST NEVER), which the documented mapping says is
      impossible. The mesh row now carries `link`, `linkIndex` and `linked`, and splitting on those:

          ot0 with table 2 state = True    matched by link index   x2995
          ot0 with table 2 state = False   matched by link index  x10319
          ot0 with table 2 state = False   matched by chain addr   x3300

      **Every wrong one is matched by index; not one is matched by chain address.** World geometry,
      which names its own chain address, is never wrong. So the fault is in pairing a generated
      packet to the `sdfDrawTagLink` that follows its `sdfCorePacketGen` by position.

      It is not a traversal gap: the ledger reads **7920 generated walked / 7920 by the engine**, so
      we visit exactly the packets the engine does.

      **Measured, and the pairing is not broken at all. The cause is the hardcoded table.** At
      `sdfGrReqDraw`, over a full run:

          generations 132   unlinked 44   meshes 324   pastEnd 0   maxIndex 131   sentinel 137

      `maxIndex` is exactly `generations - 1`, so every index-matched mesh lands inside the array.
      The 137 are `sentinel`: `linkIndex == 0xFFFFFFFF`, never assigned, because **they are not
      model-walk meshes**. Splitting the trace by packet type says which:

          orphan  type=3 (sdfPacketGenerated)  8047 rows, every one of them
          paired  type=1 (sdfPacketDmaCall)    3300 rows, exactly the chain-matched count

      Type 3 here is the *link* packet type, and those rows come from the producer hooks
      (`drawSprite3D`, `sdf3DPrimMakePoly`, `fldDrawPoly3`, `sdfBillBoardMakeN`), which set
      `mesh.gs = sdfGsStateForOt(N)` from the **hardcoded table** by design. `sdfGsStateForOt(2)` is
      ALPHA 0x48 with ATST NEVER, which is precisely the `b=2 atst=0` signature gsdiff reports.

      **And chasing it further showed the "impossible" combination was the instrument.** Those meshes
      were reporting `ot=0` because the producer paths never stamped `otIndex`, while taking their
      registers from `sdfGsStateForOt(2)`. The table is not a guess: `readBillboardTables` reads it
      off the engine's own `dds3BillboardTag` node list, a miss is reported (2 in a full run), and
      CLAUDE.md's GS-trace note says the specters' additive batches do run ALPHA 0x48 with TEST
      0x71801, which is table 2. So **the state was right and the label was missing.** `otIndex` is
      now stamped beside it, and the cross-tab reads `type=3 ot=2 b=2 atst=0 x2575` instead of an
      `ot=0` that looked like a fault.

      That also means `applyGsRegisterWrites` and `getGsState` are **not** unwired any more, whatever
      CLAUDE.md still says: `collectPackets` tracks the running state across the linked chain and
      `sdfGrReqDraw` settles each mesh against it. Only the producer-built meshes take the table, and
      for the billboards that table comes from the guest.

      **Comparing the ALPHA distributions instead of the pairs says the `b` column overstates it.**
      Shares, since mesh and batch counts are different quantities (VU1 merges before the GS sees
      anything) and only proportions are comparable:

          ALPHA (a,b,c,d)      GS      ours
          (0,1,0,1)  0x44    84.6%    81.7%
          (0,2,0,1)  0x48    12.2%    18.0%
          (2,0,0,1)  0x42     3.1%     0.3%   <- 10x under-produced
          (1,2,0,2)           0.15%    0%     <- never produced at all

      So the mix is close on the two common blends and gsdiff's 4437 `b` diffs are mostly its pairing,
      not 4437 wrong draws.

      **The 0x42 row in that table is an artefact of the same blind spot, not a gap.** A mesh row's
      `a`/`b`/`c`/`d` are the **first pass only**, while every GS batch is counted, second passes
      included. Adding `malpha` to the row shows we do emit subtractive, exactly where the hardware
      does:

          our second passes        tbp 13321  ALPHA 0x42  x118
          GS subtractive batches   tbp 13321  prim 3 tme 1 iip 1 fge 1  x1071  (of 1190 total)

      The hardware's 0x42 population is overwhelmingly textured, gouraud and fogged at one tbp, not
      untextured shadow quads, so `fldSdwDraw` is not its producer either. Counts still differ
      because ours are meshes and the GS's are post-VU batches; the blend itself is modelled.

      What survived the table was the last row: `(1,2,0,2)` is **ALPHA 0x89**, `Cd * As`, 60 untextured
      triangle batches, and `GsAlpha` had no case for it. Added as `DestScale`
      (`glBlendFunc(GL_ZERO, GL_SRC_ALPHA)`).

      It **measures zero on every scene**, and no draw of ours carries 0x89, so
      `reportUnsupported("ALPHA", ...)` never fired for it either.

      **Closed: the geometry is negligible and there is nothing further to chase.** One batch a frame,
      36 verts, gouraud, fogged, blended, ZTST GEQUAL, and a union bounding box over all 60 frames of

          x 183.7 .. 195.5   y 121.8 .. 122.1

      which is a sliver 12 pixels wide and 0.3 tall, about **4 pixels of coverage**. The blend is
      modelled so the state is drawn rather than guessed if it ever reaches a larger draw, and that is
      the right place to stop: this is the case CLAUDE.md's "rank by the area they cover, not by
      packets" warns about, arrived at from the register side instead of the packet side.

      **Third time this exact denominator has bitten: `mtex`, `mtbp`, now `malpha`.** Any per-mesh
      register comparison must count both passes on our side or it invents a deficit.

      Use this comparison, not the per-pair one, whenever a register looks wrong across a whole
      population: a share that matches says the model is right and the matcher is guessing.

      Two things tried and measured as no-ops, so nobody repeats them: moving the `mLinkIndex` reset
      from `dds3DrawProcess` entry to where `mGeneratedLinks` is cleared changes nothing (the counts
      are identical either way, and no `sdfModelLinkOt` runs between the two points), and there is no
      indexing arithmetic to fix.

      **And tbp 16216 is not missing geometry.** Every one of its batches matches a draw, so we do
      draw it, with a different texture resolved. Earlier notes here reading "0 of ours at tbp 16216"
      as absent geometry were wrong: it is a texture-assignment difference on draws we already make.

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
      model FBMSK at all**, and a draw between that pair writes no color: worth checking against the
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
      mistake.** `gsRgb()` takes a color's r, g and b, so `mesh.diffuse`'s alpha was dropped. VU1
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

- [ ] **The shadow refactor lives on `shadow-refactor`, not on master.** Rendering was broken by the
      refactor and is recovered: battle 5.933, field 5.283, underground 4.422, loading 2.080,
      title 5.574 against master's 5.354, 5.125, 4.267, 2.080, 5.574. Two match exactly and three
      sit within 0.6.

      **Three bugs, all the same mistake: a hook rebuilt from scratch that guessed where a value
      comes from.** `hooks.cpp` was destroyed by a `git checkout` and rewritten against the
      shadow's header alone, so nothing checked the guest.

      - The fade took R, G, B and state from a0..a3; the guest keeps them in `fadergba`. A garbage
        state of 0 makes the alpha `128 - 0`, so it drew as a solid opaque quad. 42.133 to 9.274.
      - The font read its uv array as floats where the guest stores 12.4, and never loaded
        `frFontClutTable`, so every glyph sampled a garbage rectangle with no palette. 9.274 to
        5.933.
      - `fldDrawFrameSprite` passed a hardcoded 0x80 where the engine feeds the previous frame back
        at 48, painting it opaquely over the field. 202.604 to 5.283.

      **The menu is 17.774 against master's 11.785 and I did not find the cause.** What it is
      not, each measured rather than argued:

      - not the sprite stream: all 119 frames have byte identical sprite records between the two
        builds, every field including corner colours and submission order,
      - not the meshes: the menu draws none, and SDF_ORDER q/b/n and SDF_OT_MAX=84 all score the
        same 17.774,
      - not the composite, clear or post pass: with SDF_NO_SPRITES the two renders are bit
        identical, 0.000 mean difference,
      - not the textures: every texture SDF_DUMP_TEXTURES writes is byte identical,
      - not the shot frame: pinned to the same SDF_SHOT_FRAME the two still split 11.9 to 17.5,
      - not the feedback pass, the rsetAlpha table, otOffset or the GS state, all compared equal.

      So identical sprites drawn over an identical background with identical textures produce
      renders 9.318 apart, spread across the frame rather than concentrated. The remaining
      suspects are the font sheet, which is fetched through getFontSheet and recovered from GS
      asynchronously so its arrival could differ by a frame, and the sprite pass's own GL state.
      **An earlier note here claimed a whole frame composite difference; SDF_NO_SPRITES disproves
      that and it is withdrawn.**

      Battle is 0.58 over master, also unexplained, and likely the same cause.

- [ ] **Moving the rest of the hooks into the shadow.** The 2D producers, the ordering-table queue
      and the engine state setters are done: `rsetAlpha` (12/12 blocks, its mode table moved out of
      hooks.cpp), `rsetOtOfs`, `rsetTest`, `rsetFrameMaskRgbOn`/`Off`, `sdf2DLinkTexPacket`, all
      with every fact accounted and no opt-outs. 39 functions attributed.

      **Re-exporting the symbol map silently unhooked four functions.** The IDB had renamed
      `ooyTestRGBMaskOn`/`Off` to `rsetFrameMaskRgbOn`/`Off`, `ooyTestPoly3` to `fldDrawPoly3` and
      `sdfIpuSetChainSource` to `sdfIpuMakeToIpuDmaTag`, but `kEngineEntries` still named the old
      ones, so they installed nothing: 63 engine hooks became 62 and 36 game became 33. The only
      symptom was field unknown realm drifting 5.127 to 5.163 and one extra word in the run log.
      `attrcheck.py` now fails on a hook name the map does not carry, which is the guard that was
      missing.

      **The 3D reimplementations have left `ModelParser`.** 21 functions and the engine globals
      they read moved to `libsdf_impl.cpp` as free functions over `gModel`; the class keeps the
      format decoders and the trace, which the shadow reaches through `gConvert`. `NodeDraw` and
      the VIF readers came out of the class with them. model.cpp fell 1960 to 754 lines, the
      shadow grew 856 to 2089, and every scene scored identically before and after, which is what
      says the move changed nothing. `sdfMatMakePacket`, `corePktSkinVertFirst`,
      `corePktSetFaceDivParm` and `corePktApplyFaceDivColor` gained their `x` names and
      attribution; the first three are fully accounted.

      `corePktCb_t` is mirrored whole (0x140), under the database's own name rather than our
      `sdfCorePacketCb_t`. **It is not `sdfCorePacket_t`**: the packet is 0x18 with `prim` at 0x10,
      the cb is the 0x140 control block the generator fills for VU0, and confusing the two cost a
      detour. `xsdfCorePacketGen` now builds one the way `corePktInitCb` does, which is what let
      that function and the three below it be accounted at all.

      **Everything is moved and accounted except two.** 50 functions carry `@fn`/`@note`, lint and
      attrcheck are clean, and every scene scores as it did before the work started, which is what
      says none of it changed behaviour. Fully accounted this stretch: `sdfCorePacketGen` (66/66,
      74/74), `sdfModelLinkOt` (50/50, 43/43), `sdfModelTempPacketLinkOt` (44/44, 37/37),
      `sdfModelLinkShapedDrawPacket` (13/13, 18/18), `corePktBuildFaceList` (20/20, 48/48),
      `corePktEmitMultiTexPass` (24/24, 8/8), `drawSprite3D`, `sdfBillBoardMakeN`, the whole
      texture layer, and about twenty packet and prim helpers.

      **`walkChain` was merged back into `xsdfModelLinkOt`.** It is that function's inner loop, so
      as a separate function the checker never saw its facts: the packet header reads and the
      three-way type dispatch counted against `sdfModelLinkOt` and had nothing to satisfy them.

      **Two remain, both blocked on the same disagreement.** `fldDrawPoly3` (8 of 24 facts) and
      `sdf3DPrimMakePoly` (0 of 23) read `sdf3DPrimPolyParm_t`, and our `sdf3DPoly_t` disagrees
      with the database's typing: ours is 0x2C with pointers at 0x14 and up and carries measured
      values in its comments, IDA's is 0x4C with inline one-element arrays at 0x10, 0x20, 0x30.
      **`sdf3DPrimMakePoly` fires in none of the savestates**, so the layout cannot be re-measured
      now; a scene that reaches it is what settles which is right. Mirroring IDA's guess over
      measured evidence was tried and backed out.

      **`fldSdwDraw` is no longer claimed.** `xfldSdwDraw` never reimplemented it: the guest builds
      the shadow geometry and our function reads the VIF stream it linked, the way
      `readVifStreamMesh` reads a model's. It is now `readShadowStream`, says so, and the symbol
      left `ida_facts.py`'s targets, since nothing reimplements it.

- [x] **A1 stage 3: every scene draws from the shadow queue.** `SDF_ORDER` defaults to `q`, and
      the order is what `xsdfGrOtClose` concatenates out of the 85 tables rather than a sort of
      ours. The queue is **exact**: ordered draws equal frame draws in every scene measured
      (field 8109, battle 9795, menu 5586), so nothing is missed and nothing is counted twice.

      | scene | sort | queue |
      |---|---|---|
      | field underground facility | 4.272 | **4.267** |
      | field unknown realm | 5.125 | 5.127 |
      | battle unknown realm | 5.354 | 5.354 |
      | the other three | unchanged | unchanged |

      **What the earlier boundary problem actually was.** Not a boundary: every draw was being
      registered twice, once by the producer's own tag and once by the sink's. Menu main gave it
      away at exactly 2x (11172 against 5586). The sink now stamps the producer's tag with the draw
      index and the producer registers it, which is what the guest does; the sink registers nothing.

      **The registration hooks pay for themselves.** `sdfGrOtInit` is called only from
      `dds3FirstProcess`, which runs at boot, so a savestate run never observes it: the tables had
      no registrar and every shadow registration was silently a no-op. We install them ourselves
      now. `sdfGrOtClose` is called from `dds3DrawProcess` per the database and **never reaches our
      hook in any sample scene**, so the queue is closed at `sdfGrReqDraw` instead, which is where
      the frame is submitted anyway.

      Hooking `sdfGrOtNormalReg` and `sdfGrSortOtReg` gives A1 stage 1's check, now a row of the run
      summary: 29 tables take normal registrations and exactly one (table 40) is sorted, matching
      `sdfModelSetOtCallbacks` overriding one table per model group. The base the game passes is
      **0x2F5F18**, confirming the IDB correction from the running game rather than from the
      disassembly.

      Two more index bugs found by measuring rather than by eye: `sdfBillBoardMakeN` used the node's
      0..3 as a global table index, which cost battle 5.354 to 7.462; and `otMax` (default 3) met a
      global index of 34 and blanked every 3D scene.

- [x] **A1 stages 1-2 landed as a shadow of SDF, and the hooks are now transport only.**
      `src/libsdf_impl.{h,cpp}` reimplements the engine's own functions with an `x` prefix, so
      `xsdfPrimLinkFt4` and `sdfPrimLinkFt4` never read as the same symbol. The hooks extract
      arguments and call; `SinkScope` names the producer and counts the draw, so a reimplementation
      counts nothing, traces nothing and never sees a `Frame`.

      **18 functions, every fact accounted, zero opt-outs.** Allocation is modelled rather than
      excused, though the model differs (a pool of `xsdfPacket_t` against the guest's bump allocator
      over a recycled arena). The last two opt-outs were `dds3DrawEnv_t+0x1300`, closed by mirroring
      `sdfGrStdDrawEnv_t` (44 bytes, all packet pointers) and `dds3DrawEnv_t` (stride 7136, only
      `stdDrawEnv` modelled) rather than excusing them, which is what the skill asks for a missing
      struct.

      The queue is the engine's: `xsdfGrOtTable[85]`, `xsdfGrOtNormalReg` FIFO, `xsdfGrSortOtReg`
      diverting the 0xFFFF sentinel to `noSortTag`, `xsdfDrawTagInit`, `xsdfGrJoinDrawTag`. It is
      registered into but not yet emitted from: the renderer still draws `frame->sprites` in its own
      order, which is why the score did not move. **Stage 3 is emitting from the tables.**

      **Five checker defects this exposed**, each of which had been under-counting silently:

      - `assets/SLUS-20911.facts` was **0 bytes**, so `guestcheck.py` passed vacuously for every
        function in the tree. `ida_facts.py` was being run through `ida.py py`, which execs inside a
        function scope, so its top-level `def`s became locals and `emit` died with a `NameError`,
        truncating the file it holds open. The skill's own `mcp__ida__py_exec_file` is the fix.
      - `guestcheck` bound a symbol to the **call site** in hooks.cpp rather than the definition,
        because its `define` regex let leading whitespace through.
      - `body()` collected from the brace, so a multi-line signature's parameters were invisible and
        every field read through them went unaccounted.
      - `DECL` did not recognise `T* name`, only `guest_ptr<T>` and bare `T name`.
      - `ALIAS` did not allow `guest_ptr<struct T>`, which is how **20** of libsdf.h's aliases are
        written, so those types resolved to nothing.

      `attrcheck.py` now checks the `@note` address against the map. It caught **11** addresses I
      had invented while writing these, one off by 0x11990.

      **Four database corrections**, all with the evidence on the item:

      - `dds3LightParam2` was declared 64 bytes and owned 56, overrunning `sdfGrOtTable`. This is
        what made seven producers report a phantom `read sdf3DLight_t+0x30`; that count is now zero.
      - `sdfGrOtTable` created as `sdfGrOt_t[85]` at **0x2F5F18**, which is `sdfGrOtInit`'s actual
        argument and ends exactly at `pModelOt` (0x2F69B8). A1's note of 0x2F5F00 is wrong.
      - `unk_515151` was never an address: it is the immediate 0x515151, the GIF REGS nibble list
        `sdf2DMakePrimPacketHeader` takes with `aNumOfRegs` 6. Retyped as a number.
      - `unk_5570C0`/`qword_5570C8` named `dds3FadePacket`, 0x160 bytes to `dds3LightColor`, with
        `dds3DrawFade` its only referrer.

      Sweep unchanged throughout, which is what stage 2 requires: battle 5.354, underground 4.272,
      unknown realm 5.125, loading 2.080, menu 11.785, title 5.574.

- [ ] **A1: the ordering tables are the engine's own command queue, and our two draw lists are an
      approximation of it.** Surveyed against the IDB. SDF already has the structure, so the work is
      to stop reconstructing it.

      **The queue.** `dds3FirstProcess` calls `sdfGrOtInit(0x2F5F00, 0x55)` every frame: **85
      ordering tables**, 32 bytes each, `next`-linked in address order. A `sdfDrawTag_t` is the
      command list itself, a `head`/`tail` DMA chain plus a `z` key and side slots for
      `lightPacket`, `fogPacket` and `texPacket[2]`. `sdfGrOtClose` walks all 85 in order and
      concatenates every tag list into one chain; `sdfGrReqDraw` pushes it into the three-slot ring
      and `sdfGrDrawFrame` sends it. `sdfModelOt_t` (11 groups at `0x2F69B8`) only names four of the
      85 apiece.

      **Registration is a closed set of two functions.** `sdfGrOtInit` installs `sdfGrOtNormalReg`
      on all 85 (FIFO append); `sdfModelSetOtCallbacks`, reached only from `sdfModelOtInit`,
      overrides `ot[1]` of each model group with `sdfGrSortOtReg` (AVL on `tag->z`, sentinel
      `0xFFFF` diverted to `noSortTag`). Each is written from exactly one place, so hooking the two
      catches every registration in the game with `(ot, tag)` in the arguments, and the global table
      index is `(ot - 0x2F5F00) / 32`. This is the piece we have no equivalent of, and it is two
      hooks rather than one per producer.

      **`otIndex` and `otOffset` are the same number, and we hold two.** `DrwSprite2D` at
      `0x1289E0` does `ot = 0x2F5F00 + (rsetFrameMaskOtOffset << 5)` then calls `regFunc` at `+0x10`,
      so the 2D `otOffset` indexes the same 85. Ours is a group-local 0..3 on meshes and a separate
      offset on sprites, which is why `sdfGsStateForOt` has to guess ALPHA and TEST from a
      four-valued index with a `default:`.

      **Emit order inside a sorted table is not a plain sort.** `sdfGrSortOtSetup` emits `noSortTag`
      first, and that list is built by prepending, so it comes out in **reverse registration order**;
      then the AVL walk, which by the comparison in `sdfSortOtAvlRegist` (`v10 >= z` goes right,
      traversal is left-node-right) is **descending z**, with equal keys chained LIFO. The other 81
      tables do not sort at all. The comment at `renderer.cpp:678` says the tables sort by the tag's
      z, and that is true of eleven of them.

      **Light and fog are a state diff, not a per-draw property.** `sdfGrJoinDrawTag` splices a light
      or fog packet at a tag boundary only where the two tags disagree. `DrawMesh` carries a full
      `MeshLighting` and `MeshFog` copy per mesh instead.

      **Producers, for scale.** 247 static callers of the five `sdfDrawTagLink*` entry points, 25 of
      them in `sdf*`; 213 static callers of `sdfGrAllocPacketBuff`; **29 producers observed** in
      `dumps/trace.jsonl`. The queue rework does not touch that list, which is the point: producers
      keep handing over arguments and the tables stop being ours to invent.

      **Staging.** Each stage is measurable on its own and the first two must not move the score.

      1. **Observe.** Hook `sdfGrOtNormalReg` and `sdfGrSortOtReg`, record `(table, tag, z, mode)`
         per frame, add the counts to the summary row. Check the reconstructed concatenation against
         the DMA chain `gif.cpp` already walks, which resolves 654 of 654 linked packets a frame. A
         mismatch here says the model is wrong before anything depends on it. No render change.
      2. **Key by the real table.** Stamp every `DrawMesh` and `DrawSprite` with the global 0..84
         index and the tag address, replacing `otIndex` and `otOffset`. Keep the existing sort.
         **A score change at this stage is a bug**, since nothing about drawing has changed yet.
      3. **Adopt the engine's emit order.** Tables in index order; within a sorted table the
         `noSortTag` run reversed, then descending z with LIFO ties; the other tables in
         registration order. First stage that can move the score, and the first that can regress
         2D against 3D interleaving.
      4. **Key GS state off the table.** `sdfModelSetDrawEnv` builds one ALPHA/TEST pair per table,
         so `sdfGsStateForOt` reads the env rather than switching on 0..3 with a default.
      5. **Light and fog as state commands**, spliced by the `sdfGrJoinDrawTag` rule instead of
         copied per mesh. Only after 1 through 4, since it depends on tag adjacency being right.

      **What it costs.** The arena recycles: `sdfGrSetupPacketBuff` resets the bump pointer per frame
      id and alternates two buffers, so a tag address is unique only within a frame and must be
      scoped that way. A tag we build from a producer hook has no guest address until it is linked,
      which is the problem `DrawMesh::linked` and `linkIndex` already work around; the new structure
      inherits it rather than solving it.

      **What not to do.** The tables must be read from the registration hooks, never rebuilt by
      walking the DMA chain. That is `gif.cpp`'s job and it stays a coverage check.

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
  the depth mechanism: FB_ONLY means a failing fragment still writes color and only its depth is
  suppressed, which is two draws back to back. Blend translator ported from SkyGL's
  `gsAlphaToBlend`, unit scale from its `3d.frag`.
- **R5 culling and R6 the two-sided pass.** `corePktBuildFaceList` drops a triangle whose
  screen-space signed area disagrees with `cb->backfaceSign`, which `sdfCorePacketGen` takes from
  `root->flags & 0x08`. On by default; a two-sided draw turns it off for itself. It looked
  catastrophic for six measurements because it leaked into the sprite pass and deleted half of every
  2D quad. It is what stops the character's face showing through the back of his head.
- **Lighting**, from the VU1 image at 0x0150-0x0194: three parallel directions, one color each and
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
- **M7 the multi-texture pass**, through the second UV set with its own texture, color and blend.
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
