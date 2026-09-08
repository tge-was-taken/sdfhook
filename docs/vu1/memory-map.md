# VU1 data memory

Names for the fixed addresses `vu1_image.c` loads from, with the instruction that proves each one.
VU1 addresses are quadwords. Anything not listed here is unidentified: add to it rather than
guessing at a neighbour, since the regions are not contiguous.

## The light packet, 920..928

The packet is unpacked with its 16-byte DMA and VIF header consumed rather than stored, so its
first payload quadword lands at 920 and the struct offsets follow directly:

| VU | struct | what it is |
|---|---|---|
| 920..922 | `normal[3]` (+0x10) | light directions as the matrix **columns**, so a light reads down them |
| 923 | `colorBase` (+0x40) | the directional light's color, what light 0 reaches |
| 924 | `color[0]` (+0x50) | scratch: 0x01C5 overwrites it with `colorBase - color 3` |
| 925, 926 | `color[1..2]` | lights 1 and 2, zero in every scene measured |
| 927 | `color[3]` | the ambient, the sum's base |
| 928 | `color[4]` | `colorBase + color 3`, what a mesh with no normals takes |

`color4 == color0 + color3` holds exactly in every scene dumped with `SDF_DUMP_LIGHT`, which is
what identifies 927 as the ambient rather than another light.

| 968..970 | a second direction matrix, chosen when `drawFlags & 0x800` is clear (0x013B, 0x013F). **Not modelled**: the renderer always uses 920..922. |

## Per-material, written by the unpack at 0x0090

| VU | what it is |
|---|---|
| 1002 | the material's `diffuse`, `itof0` then 1/128 |
| 1004 | its `ambient`, likewise |
| 1005 | the draw color, already multiplied by `diffuse` at 0x009E |
| 1003 | the sum's base, computed at 0x01C4 as `ambient * color 3` |
| 917.w | set to 1 when a material is unpacked (0x00A8, 0x00CE), cleared by the rebase that consumes it (0x01C1) |

Measured across all six scenes, `ambient` and `diffuse` are both 1.0 on every mesh: two lighting
models differing only by those factors produced byte-identical renders.

## Per-draw

| VU | what it is |
|---|---|
| 903.w | draw flags. `& 0x80` selects the lit path over the flat one (0x0136), `& 0x800` the direction matrix (0x013B) |
| 910.w | the flatten factor: lerps the base toward 1.0 and scales the light colors by `1 - t` (0x0146, 0x0148). Zero in every scene measured, and **not modelled** |
| 800.. | the output vertex colors (0x0196 writes four at a time) |

## Confirmed against the GS trace

The hardware's own post-VU vertex colors settle the sum, and they are in the trace already
(`rgbaq`, `r`/`g`/`b_` per batch). Over the textured triangle batches of `smt3_field_unknown_realm`,
the two dominant values are `[38, 25, 25]` (x3108) and `[128, 89, 64]` (x3539). That packet's
`color 3` is `[0.2969, 0.1953, 0.1953]`, and `x 128` it is `[38, 25, 25]` exactly; its `colorBase`
is `[1, 0.6953, 0.6953]`, or `[128, 89, 89]`. So the sum runs from the ambient up to the diffuse,
as decoded.

`smt3_field_underground_facility` agrees: `colorBase x 128` is `[115, 128, 115]` and every observed
value falls under it (`[74, 80, 70]`, `[67, 73, 64]`, `[32, 38, 32]`).

Reading `colorBase` as the base and `color[0]` as light 0's color, which is what the struct's names
suggest, instead ranges `[128, 89, 89]` to `[255, 178, 178]`. **No batch in either scene carries a
value in that range.** It scored better only because it compensated for something else.

## Per-vertex arrays

Indexed by vertex number, all three the same length. `emit.c` reads the first two and never the
texture coordinate, because that emitter writes two registers a vertex.

| VU | what it is |
|---|---|
| 704 + v | position in view space, `.w` the clip w |
| 800 + v | the vertex color, floats until emit's `FTOI0`. Filled by the lighting routines clamped to 255 (0x0183), or with a flat tint by the texture-coordinate loops (0x00F7, 0x0124) |
| 848 + v | `.xy` the texture coordinate (0x00FA, 0x0127, 0x0209), `.z` the reciprocal the projection multiplies by, `.w` a per-vertex scalar emit copies into the emitted vertex's `w` |
| 752 + v | the normal, one a vertex. `normalizeVectors` files it at 0x035C and the skinning and morph loops at 0x03A6, 0x03BC and 0x03D2. Indexed from 1, so the listing shows the offset as +751 |

## Counts and source pointers, unpacked at 0x00A9

Two to a quadword, so only `.xy` of each is kept.

| VU | what it is |
|---|---|
| 896.w | face count |
| 897.w | vertex count |
| 898.w | the index list: `.y` and `.z` are the second and third vertex, swapped at 0x01F9 to reverse winding |
| 900.w | the source normals |
| 901.w | the source texture coordinates |
| 899.w | the output position cursor: reset to 704, the position base, at 0x03D8 and 0x0412, and walked at 0x0387 and 0x03E4. `.xy` of the same quadword is a count filed at 0x00B2, so the two never share a lane |
| 902.w | the packed vertex colors, bytes until `ITOF0` |
| 915.w | a pointer `unpackTexture` files at 0x00CB and `unpackPointers` reads back at 0x036F |
| 907.w | the GS PRIM word's flag bits, `attrBits & 0x78`, written at 0x0381. The packet builders add a primitive type to it and write it as A+D register 0 |
| 908.w | the last PRIM value a builder wrote, so packetClipped can turn it into a strip |
| 904.w | draw flags: `0x4000` draw back faces too, `0x0010` the texture-matrix path, `0x1000` a further pass, and bits 0 and 10 that readFlags hands out |
| 974..976 | `.xy` a 2x3 texture matrix, two rows and an offset, applied at 0x0205. This is `sdfMatSetUvwParm`'s six floats |
| 977..979 | **a second texture matrix**, filed `.xy` at 0x00BE..0x00C0 by the same unpack that fills 974..976 at 0x00BB..0x00BD, and applied by `texcoordSecond` at 0x02FA..0x0300 as the same 2x3 affine. Corrects the earlier reading that nothing read them: 0x02FA does |

## Fog, and the scratch the clipper and the sprites use

| VU | what it is |
|---|---|
| 919 | the fog parameters, filed by the entry stub at 0x0014: `.x` the minimum, `.y` the maximum, `.z` the offset, `.w` the scale. The transform computes `z + w/W` clamped to `[x, y]` at 0x04DC and 0x0505, and direct.c does the same at 0x071D |
| 1020.y | the VIF TOP a sprite program was entered with (0x05F2 and its four copies) |
| 1021.w | the head of the retry list the emitters thread clipped triangles onto |
| 1021.y, 1021.z | the clipper's two scratch buffers, swapped every plane (0x0572, 0x0573) |
| 973, 1006 | read by the reflection pass and the ramp; 973 is the reflection pass's own flat color |
| 980.w, 1014.x | the ramp's gain: 0x0446 adds the second to the first, 0x044A clamps the sum to at least 1, and 0x0451, 0x0454 and 0x0455 multiply it into the three light directions. `unpackTexture` files 1014.x at 0x00CF |
| 981.w, 1015.x | the ramp's bias, added at 0x0447 and scaled by the gain at 0x0452. Filed at 0x00D0 |
| 981, 982 | the GIF tag each emitter copies: 0x0236 loads 981 and 0x023B writes its `.yz` as `emitTriangles`' tag, 0x028C and 0x02DA do the same with 982 for `emitTexTriangles`. Only `.yz` is copied, and `.w` is the ramp bias above |
| 910.w | the ramp's flatten factor as well as the sum's (0x043F) |
| 914.w | 3 there makes texcoordSecond fill one color instead of relighting (0x02EE) |

## Camera and object, written by setup.c

| VU | what it is |
|---|---|
| 934 | the screen scale, also kept in vf30 |
| 964..967 | object times camera, the matrix the per-vertex transform uses |
| 968..970 | the normal rows through the light matrix |
| 971, 1007 | a tint scale and the per-draw color; their product is the flat tint the texcoord loops write. **1007 is a color, not a size**: all seven of its uses multiply a color by it and none scales a UV |
| 988..991 | filed at 0x0044..0x0047. 988..990 are read at 0x0060..0x0062 as the 3x3 that turns the object's normal rows into view normal rows at 0x0066, 0x0069 and 0x006C, so they are the camera's rotation |
| 992..995 | filed at 0x0048..0x004B. `composeMatrices` loads them at 0x0050..0x0053 and multiplies every object row by them at 0x0057, 0x005B, 0x005F and 0x0063, so they are the camera matrix proper; 0x03DB and 0x05FB reload them |
| 996..998 | the object's normal rows through 988..990, filed at 0x006E..0x0070 and read by the reflection pass at 0x05D8..0x05DB |
| 999..1001 | the object's own normal rows, filed at 0x0064..0x0066 and read at 0x010B..0x010D |
| 1011 | the reference point emit's winding test is taken against |
| 1023 | `.y` the emit output cursor, `.z` and `.w` two more buffer cursors, all reset at 0x0033 |

vi14 is the return stack, growing down from 1024 (0x0030), and vi15 the link register throughout.

## Still unidentified

`vu2c.py --names` prints these in the listing's header, ranked by how often the code reaches them,
which is the worklist for this file. As of the current image: `mem[972]`, `mem[932]`, `mem[911]`,
`mem[933]`, `mem[929..931]`, `mem[912]`, `mem[913]`, `mem[909]`, `mem[1008]`, `mem[1009]`, and the
low addresses `mem[0]`, `mem[1]`, `mem[4]`, `mem[6]`. Several are written by an unpack and never
read in this image, so a scene exercising another path is what would settle them.

## Records

`vu1_names.txt` also names the record a routine is handed a pointer to, with `!<entry> <register>
<name>`, and that record's fields. `unpackTexture(vi02)` is the one worked out so far: `+2` the
tint it files as `drawTint`, `+3` a screen-Z offset added at 0x00CC, `+4` and `+5` the ramp's two
offsets filed at 0x00CF and 0x00D0.
