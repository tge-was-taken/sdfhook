# Packet overview

What each packet in the frame's DMA chain is, what handles it, and where a hook still earns its
place. Counts are summed over the six curated scenes, 240 frames each, at `834ee11`.

## How a packet is handled

`processCommandStream` walks the chain and feeds every transfer to `GsDecoder` first, so the GS
register file is current before any handler runs. Then, by recorded type:

| Types | Handler | Consumed by |
|---|---|---|
| Sprite, Packet2D, Primitive2D, Glyph, Fade, ZClear, DebugText, GifPrimitive, Clear | `processSpritePacket` | the decoded primitives; the hook's draws only with `SDF_SPRITE_HOOKS` |
| MeshNormal, MeshSkinned, MeshShaped, MeshStream, MeshGenerated, PolyStream, BillboardBatch, Primitive3D, VifData | `processMeshPacket` | the meshes a hook recorded at the packet or its REF target |
| Shadow | `processShadowPacket` | `fldSdwDraw`'s inline VIF stream; its other links are reported |
| Unknown | `processUnknownPacket` | whatever a hook claimed on it, else the decoded primitives |
| TexBind | `processTexBindPacket` | the handle behind the TEX0 it writes |
| TestState, AlphaState, TexaState, DrawEnv, FrameMask, FrameTex, DrawFrame, FrameBuffer, RefGif, GifRegisters | `processGsStatePacket` | the decoder's register file |
| Transform, TransformRef, Viewport, LightParallel, Fog, Material, MaterialRef, DrawParams, VuMicrocode, BillboardTransform, DirectRef, PacketHeader, Terminator, VifSetup | `processCarriedPacket` | the hook that recorded the draw; reported when no hook did |
| TextureUpload, BitBlt | `processTransferPacket` | the decoder: an IMAGE transfer stales GS-memory textures, a local copy is reported |
| Empty | `processEmptyPacket` | nothing, and a payload is reported |

A model node is checked first and consumed as a span, from its first link to its draw tag's tail.

## Producers by path

**Decoded from the packet (GIF, PATH2).** Every 2D producer. Primitives drawn, all scenes:

| Producer | Decoded | Scenes |
|---|---|---|
| `aDrawBoxG` | 38054 | all but the main menu |
| `frCreateDrawFontPacket` | 29124 | all but the title |
| `cmpDrawTriG` | 18921 | main menu |
| `fclDrawSpriteCore` | 7735 | main menu |
| `DrwSprite2D`, `DrwSprite2DF`, `nbPnlSprite` | 10615 | all |
| `dds3LinkZClearDrawTag`, `dds3InitDrawEnvSub` | 2744 | all; depth barriers and the clear |
| `cmpDrawBoxG` | 1378 | main menu, correct now where the hook cost 6.2 points (M2) |
| `fldDrawEATexQuad`, `aDrawBoxF`, `DispCommCursor`, `DispHelpPanel` | 2026 | field and battle |
| `fldDrawFrameSprite` | 190 | the feedback tap |
| `release_movieobj2` | 120 | title; no hook ever drew it |
| `dds3DrawFade` | 44 | underground |

**Refused.** The shadows `aDrawBoxF_Direct`'s `$ra` names: 6460 fans that test destination alpha,
which the 2D renderer cannot (TODO P7).

**Hooked, because VU1 builds the draw from what the chain carries (spans).**

| Producer | Meshes | Why the packet is not enough |
|---|---|---|
| `sdfBillBoardMakeN` | 34460 | VU1 expands centres into quads |
| `sdfCorePacketGen` | 25280 | lit on the EE at generation (`corePktCurrLight`) |
| `sdf3DPrimMakePoly` | 16326 | VU1 transforms and lights the arrays |
| `sdfModelLinkOt` | 16205 | bones from VU0, the type 1 streams run on VU1 |
| `drawSprite3D` | 120 | the rotation reaches the transform through VU0 |

**Unhandled.** `fldDrawPoly3` (25704 packets in battle, 8640 and 7488 on the fields; its hook is
off unless `SDF_POLY`), `sdfModelLinkOt`'s battle call chains (3689), `fldSdwDraw` (the VIF shadow
is off unless `SDF_DRAW_SHADOWS`), `dds3TrackPolyLinkOt` (120). All VU1.

## Span candidates

A set of packet types only one function links is a span: consumed whole, its draws from that
function's hook.

- **Model node**, already a span: Transform, TransformRef, DrawParams, MaterialRef, the mesh
  types, PacketHeader, Terminator and VuMicrocode, from `sdfModelLinkOt`,
  `sdfModelTempPacketLinkOt` and `sdfModelLinkShapedDrawPacket`.
- **Billboard**: BillboardTransform with its BillboardBatch, from `drawSprite3D` (three links) and
  `dds3BillboardTagLink`. Today the batch is keyed and the transform only checked as carried; a span
  would make the pair one unit and catch a transform with no batch.
- **Shadow**: `fldSdwDraw`'s four links. One is the inline VIF stream, the rest are the REF links
  P6 lists; the DATE fans the renderer refuses are probably the same shadow sent the other way.
  Worth a span once the renderer has a destination alpha test.
- **Poly**: `sdf3DPrimMakePoly` and `fldDrawPoly3`, PolyStream behind a transform. One packet
  each, so a span adds nothing over the type.

## Hook or decode, for the 2D

The decoder matches the hooks pixel for pixel in battle and loading and beats them on the menu and
title, so the question is only what a hook adds on top.

- **Widescreen.** Anchoring a HUD element needs to know which element it is. The decoded draw
  carries its linker, the same `$ra` a hook is keyed by, and its final 12.4 coordinates, which are
  what a hook's arguments turn into. No gain from the hook, except for a producer that draws
  several elements in one packet, which none of the 2D here does.
- **Upscaling.** The packet has the full 12.4 subpixel position, the same as a hook. A sharper font
  needs the glyph, and that is known at upload (`frTranslateUserData`), not at the draw: keep the
  upload hook, not the draw hook.
- **Frame interpolation.** Needs a draw's identity across frames. For 2D, linker and chain order are
  as stable as the hook's call order. For 3D the hooks hold what interpolates (bones, matrices), and
  those stay.
- **Performance.** A hook is a trap into the host per call: the glyph hook alone fires 29124 times
  over these runs. Decoding reads quadwords the chain already holds. Not measured yet.

So: the 2D draw hooks (`DrwSprite2D`, `DrwSprite2DF`, `nbPnlSprite`, `fclDrawSpriteCore`,
`aDrawBoxG`, `aDrawBoxF`, `aDrawBoxF_Direct`, `cmpDrawTriG`, `cmpDrawBoxG`, `sdfPrimLinkFt4`,
`sdfPrimLinkG4`, `dds3DrawFade`, `fldDrawFrameSprite`) can come out of the default hook table.
`frCreateDrawFontPacket` stays until the font sheet it asks for is found some other way, and the
texture hooks (`sdfTexSetEx`, `frTranslateUserData`) stay: they supply texels, not draws.
