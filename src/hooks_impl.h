#pragma once

#include "hooks.h"

// What each hooked engine function does once hooks.cpp has read its arguments out of the
// registers. hooks.cpp is the transport: it names the arguments and calls in here.
namespace sdfhook {

/// Reading a packet that yielded nothing, rather than guessing at it.
void dumpPacket(const LinkedPacket& packet, const char* by, const sdfDmaTag_t& tag);

/// sdfGrReqDraw(aTag, ...): the frame's chain is complete, so its command stream is processed into
/// the frame and the frame submitted.
void sdfGrReqDraw(HookContext& h, u32 aTag);

/// An inventory of the frame's DMA chain, for judging what is left untyped. Runs after the command
/// stream is processed and forgets the frame's linked packets and their types either way.
void takeFrameCensus(HookContext& h, u32 aTag);

/// A builder ran, handed its four argument registers. Records the packet it makes by address.
void recordBuiltPacket(HookContext& h, const char* aBuilder, const u32 (&aArgs)[4]);

/// sdf2DMakeTexPacket(aPacket, aTexHndl, aCtx): the texture a 2D texture packet binds.
void sdf2DMakeTexPacket(HookContext& h, u32 aPacket, TexHandle aTexHndl, u32 aCtx);

/// A builder returned its packet.
void recordReturnedPacket(HookContext& h, const char* aBuilder, u32 aPacket);

/// One sceDmaSend, with its caller and the engine's display-thread flag, for the census.
void sceDmaSend(HookContext& h, u32 aRa, u32 aChan, u32 aAddr);

/// Y is in half-height units, the same scale the draw space uses vertically.
void submitSprite(HookContext& h, const char* producer, f32 x, f32 y, f32 w, f32 height, f32 u,
                  f32 v, f32 uw, f32 vh, Rgba color, u32 texFileHandle);

/// The frame already presented, put back over the screen at a fixed alpha or the caller's.
void submitFrameFeedback(HookContext& h, u8 alpha);

/// dds3DrawFade(): the color and level are globals and the quad is the whole screen.
void dds3DrawFade(HookContext& h);

/// frTranslateUserData uploads one glyph; its slot is only known through the GsFont_t it returns.
void frTranslateUserData(HookContext& h, s32 Width, s32 Height, u32 pData, u32 aResult);

// aDrawBoxG(x, y, w, h, four corner colors): a trifan quad, in the draw space's own vertical
// scale rather than the doubled one the sprite calls use.
void aDrawBoxG(HookContext& h, const s32 x, const s32 y, const s32 w, const s32 height,
               const u32 r0, const u32 g0, const u32 b0, const u32 a0, const u32 r1, const u32 g1,
               const u32 b1, const u32 a1, const u32 r2, const u32 g2, const u32 b2, const u32 a2,
               const u32 r3, const u32 g3, const u32 b3, const u32 a3);

// aDrawBoxF(x, y, w, h, r, g, b, a) in pixels.
void aDrawBoxF(HookContext& h, const s32 x, const s32 y, const s32 w, const s32 height,
               const u32 r, const u32 g, const u32 b, const u32 a);

// aDrawBoxF_Direct(x0, y0, x1, y1, r, g, b, a) in raw GS coordinates, XYOFFSET included. Not a
// filled box: PRIM 0x41 is a LINE, so only the top and bottom edges draw.
void aDrawBoxF_Direct(HookContext& h, const s32 x0, const s32 y0, const s32 x1, const s32 y1,
                      const u32 r, const u32 g, const u32 b, const u32 a);

// frCreateDrawFontPacket(...): one glyph. X and Y are 12.4 in the draw space, extents whole
// pixels.
void frCreateDrawFontPacket(HookContext& h, const s32 X, const s32 Y, const s32 Width,
                            const s32 Height, const u32 Col, const u32 pUv, const u32 pGsTex);

// A strip (TL, TR, BL, BR) into a cycle, so the third and fourth corners swap.
/// sdfPrimLinkFt4(tag, color, prim, x0, y0, u0, v0, ... x3, y3, u3, v3, z, allocator).
void sdfPrimLinkFt4(HookContext& h, const u32 aColor, const u32 aPrim, const s32 aX0,
                    const s32 aY0, const s32 aU0, const s32 aV0, const s32 aX1, const s32 aY1,
                    const s32 aU1, const s32 aV1, const s32 aX2, const s32 aY2, const s32 aU2,
                    const s32 aV2, const s32 aX3, const s32 aY3, const s32 aU3, const s32 aV3);

/// sdfPrimLinkG4(tag, prim, x0, y0, color0, ... x3, y3, color3, z, allocator).
void sdfPrimLinkG4(HookContext& h, const u32 aPrim, const s32 aX0, const s32 aY0,
                   const u32 aColor0, const s32 aX1, const s32 aY1, const u32 aColor1,
                   const s32 aX2, const s32 aY2, const u32 aColor2, const s32 aX3, const s32 aY3,
                   const u32 aColor3);

// fclDrawSpriteCore(...). X and Y are 12.4 in the draw space; extents are pixels, H in the
// doubled vertical scale.
void fclDrawSpriteCore(HookContext& h, const s32 X, const s32 Y, const s32 W, const s32 H,
                       const u32 pCol, const u32 pSprite, const u32 FlipFlag);

// cmpDrawBoxG(x, y, Z, w, h, pCol, Ctxt, OtNo): a gouraud quad. x and y are 12.4 in draw space.
/// @todo TODO.md M2: correct and still costs 6.2 points, so the table traces it instead.
// cppcheck-suppress unusedFunction
void cmpDrawBoxG(HookContext& h, const s32 x, const s32 y, const s32 w, const s32 height,
                 const u32 pCol);

// cmpDrawTriG(pX, pY, Z, pCol, OtNo): three x, three y and three colors, read through pointers.
void cmpDrawTriG(HookContext& h, const u32 pX, const u32 pY, const u32 pCol);

// sdfModelDrawEnvLinkOt(aOt, aEnv): one env set per table, holding that
// layer's frame buffer, clear, alpha test and blend for both GS contexts.
void sdfModelDrawEnvLinkOt(HookContext& h, const u32 aOt, const sdfModelDrawEnvPtr_t aEnv);

/// sdfBillBoardMakeN(aCenter, aColor, aUVList, aXYOffsetList, aNum, aAllocator): one batch of
/// camera-facing quads. The four pointers are one allocation, so it is taken as the struct it is.
/// @fn sdfBillBoardMakeN
/// @note SLUS-20911 0x002BB0F0
void sdfBillBoardMakeN(HookContext& h, const sdfBillBoardBatchPtr_t aCenter, const u32 aNum);

/// drawSprite3D(aTexHandle, aSprite): the level rather than sdfBillBoardMake1, whose rotation
/// only reaches the transform packet through VU0.
/// @fn drawSprite3D
/// @note SLUS-20911 0x0012A2D0
void drawSprite3D(HookContext& h, const TexHandle aTexHandle, const Sprite3DPtr_t aSprite);

/// sdf3DPrimMakePoly(aPoly): counts, a PRIM word and one array per attribute. Every sdf3DPrim
/// producer builds through it.
/// @fn sdf3DPrimMakePoly
/// @note SLUS-20911 0x002BAAB8
void sdf3DPrimMakePoly(HookContext& h, const sdf3DPrimPolyParmPtr_t aPoly);

/// fldDrawPoly3(...): one gouraud triangle. The largest unhandled producer by packets, and
/// invisible to a vertex ranking since its geometry only exists after VU1 runs.
/// @fn fldDrawPoly3
/// @note SLUS-20911 0x0012A5E0
void fldDrawPoly3(HookContext& h, const sdfVector3_t point[3], const u32 color[3]);

}  // namespace sdfhook
