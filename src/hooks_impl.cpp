#include "hooks_impl.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "chain_census.h"
#include "gif.h"
#include "guest_ptr.h"
#include "json.h"
#include "libsdf.h"
#include "log.h"
#include "packet_processor.h"
#include "render/render_thread.h"
#include "trace.h"
#include "unsupported.h"

namespace sdfhook {

namespace {

LogChannel log("hooks");

}  // namespace

// Reading a packet that yielded nothing, rather than guessing at it.
void dumpPacket(const LinkedPacket& packet, const char* by, const sdfDmaTag_t& tag) {
    const u32 base = sdfDmaIsRef(tag) ? tag.addr : packet.addr + sdfQwordBytes;
    JsonObject row;
    row.addString("dump", "packet")
        .addString("by", by)
        .addHexDigits("at", packet.addr, 8)
        .addUint("dmaId", sdfGetDmaId(tag))
        .addUint("qwc", tag.qwc)
        .addHexDigits("data", base, 8)
        .beginArray("qwords");
    const u32 count = tag.qwc < 16 ? tag.qwc : 16;
    for (u32 i = 0; i < count; i++) {
        const u8* qword = u8Ptr_t{base + i * 16}.slice(16);
        if (!qword) break;
        u64 lo = 0;
        u64 hi = 0;
        std::memcpy(&lo, qword, sizeof(lo));
        std::memcpy(&hi, qword + 8, sizeof(hi));
        row.beginObject().addUint("at", i * 16).addHex128("qword", hi, lo).endObject();
    }
    log.dump(row.endArray().take());
}

/// The chain is complete here, so the frame is made from it and submitted.
/// @fn sdfGrReqDraw
/// @note SLUS-20911 0x002ADA68
void sdfGrReqDraw(HookContext& h, u32 aTag) {
    // A trace-only run has no renderer and so no frame, but its packets are still accounted.
    Frame none;
    Frame* frame = h.frames.getFrame();
    readImageQueue(h);
    processCommandStream(h, aTag, frame ? *frame : none);
    if (frame) h.model.traceMeshes(*frame);
    takeFrameCensus(h, aTag);
    h.frames.submit();
}

void recordBuiltPacket(HookContext& h, const char* aBuilder, const u32 (&aArgs)[4]) {
    const PacketBuilder made = getPacketBuilder(aBuilder);
    h.builderCalls[aBuilder]++;
    if (!made.takesPacket) return;
    if (!aArgs[made.argument]) return;
    const u32 packet = aArgs[made.argument] + made.offset;
    h.packetTypes[packet] = made.type;
    if (made.trailing != PacketType::Unknown) h.packetTypes[packet + sdfQwordBytes] = made.trailing;
}

/// Records the texture the packet binds, for the chain walk.
/// @fn sdf2DMakeTexPacket
/// @note SLUS-20911 0x002B9C20
void sdf2DMakeTexPacket(HookContext& h, u32 aPacket, TexHandle aTexHndl, u32 aCtx) {
    if (aCtx) {
        reportUnsupported("2D texture packet for GS context 2", aCtx);
        return;
    }
    h.texturePackets[aPacket] = aTexHndl.addr;
}

void recordReturnedPacket(HookContext& h, const char* aBuilder, u32 aPacket) {
    const PacketBuilder made = getPacketBuilder(aBuilder);
    if (made.returnsPacket && aPacket) h.packetTypes[aPacket] = made.type;
}

void takeFrameCensus(HookContext& h, u32 aTag) {
    // Packet buffers are reused every other frame, so a type is only good for the frame that
    // recorded it.
    struct Reset {
        HookContext& h;
        ~Reset() {
            h.packetTypes.clear();
            h.texturePackets.clear();
            h.frames.getPackets().clear();
        }
    } reset{h};
    if (!h.config->dumpChainCensus) return;

    for (const char* global : {"sdf3DPrimIdentityTm", "vu1MicroPacket"}) {
        const u32 at = pcsx2hook_find_symbol(h.runtime, global);
        if (at) h.packetTypes[at] = getPacketTypeByGlobal(global);
    }
    writeChainCensus(aTag, h.frames.getPackets(), h.symbols, h.trace->getFrame(), h.packetTypes);
    for (const auto& [builder, calls] : h.builderCalls) {
        log.dump(JsonObject()
                     .addString("dump", "builder")
                     .addString("name", builder)
                     .addUint("calls", calls)
                     .take());
    }
}

void sceDmaSend(HookContext& h, u32 aRa, u32 aChan, u32 aAddr) {
    if (!h.config->dumpChainCensus) return;
    const u32 flag = pcsx2hook_find_symbol(h.runtime, "sdfDispThreadFlag");
    log.dump(JsonObject()
                 .addString("dump", "dmaSend")
                 .addUint("frame", h.trace->getFrame())
                 .addUint("chan", aChan)
                 .addHexDigits("addr", aAddr, 8)
                 .addString("by", h.symbols.getSite(aRa))
                 .addUint("dispThreadFlag", flag ? u8Ptr_t{flag}.readOr() : 0xFFu)
                 .take());
}

// Y is in half-height units, the same scale the draw space uses vertically.
void submitSprite(HookContext& h, const char* producer, f32 x, f32 y, f32 w, f32 height, f32 u, f32 v,
                f32 uw, f32 vh, Rgba color, u32 texFileHandle) {
    const ProducerDraws draws(h.frames);
    Frame* frame = h.frames.getFrame();
    const TexHandle named{texFileHandle};
    const TexHandle texHandle = h.textures.getHandle(named, frame);
    // A named texture we never decoded draws flat. Counted by producer, so the gap names it.
    if (named.addr && !texHandle.addr) h.coverage.unresolvedTextures[producer]++;
    h.coverage.apiDraws++;
    if (!frame) return;
    DrawSprite sprite = DrawSprite::makeRect(x, y * 0.5f, w, height * 0.5f, u, v, u + uw, v + vh);
    sprite.producer = producer;
    sprite.setColor(color);
    sprite.texHandle = texHandle;
    // A producer naming no texture draws with the one sdf2DLinkTexPacket bound ahead of it.
    sprite.boundTexture = !named.addr;
    frame->addSprite(sprite);
}

// The frame already presented, put back over the screen at a fixed alpha or the caller's.
void submitFrameFeedback(HookContext& h, u8 alpha) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;

    DrawSprite sprite = DrawSprite::makeRect(-0.5f, -0.3125f, 512.0f, 224.0f, 0.0f, 0.0f,
                                             kGuestWidth, kGuestHeight);
    sprite.setColor(Rgba{0x80, 0x80, 0x80, alpha});
    sprite.texHandle = TexHandle{kPreviousFrameHandle};
    sprite.producer = "submitFrameFeedback";
    frame->addSprite(sprite);
}

// aDrawBoxG(x, y, w, h, four corner colors): a trifan quad, in the draw space's own vertical
// scale rather than the doubled one the sprite calls use.
void aDrawBoxG(HookContext& h, const s32 x, const s32 y, const s32 w, const s32 height,
               const u32 r0, const u32 g0, const u32 b0, const u32 a0, const u32 r1, const u32 g1,
               const u32 b1, const u32 a1, const u32 r2, const u32 g2, const u32 b2, const u32 a2,
               const u32 r3, const u32 g3, const u32 b3, const u32 a3) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;

    DrawSprite sprite = DrawSprite::makeRect(f32(x), f32(y), f32(w), f32(height), 0, 0, 0, 0);
    sprite.corner[0].color = Rgba{u8(r0), u8(g0), u8(b0), u8(a0)};
    sprite.corner[1].color = Rgba{u8(r1), u8(g1), u8(b1), u8(a1)};
    sprite.corner[2].color = Rgba{u8(r2), u8(g2), u8(b2), u8(a2)};
    sprite.corner[3].color = Rgba{u8(r3), u8(g3), u8(b3), u8(a3)};
    sprite.producer = "aDrawBoxG";
    frame->addSprite(sprite);
}

// aDrawBoxF(x, y, w, h, r, g, b, a) in pixels.
void aDrawBoxF(HookContext& h, const s32 x, const s32 y, const s32 w, const s32 height,
               const u32 r, const u32 g, const u32 b, const u32 a) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;

    DrawSprite sprite = DrawSprite::makeRect(f32(x), f32(y), f32(w), f32(height), 0, 0, 0, 0);
    sprite.setColor(Rgba{u8(r), u8(g), u8(b), u8(a)});
    sprite.producer = "aDrawBoxF";
    frame->addSprite(sprite);
}

// aDrawBoxF_Direct(x0, y0, x1, y1, r, g, b, a) in raw GS coordinates, XYOFFSET included. Not a
// filled box: PRIM 0x41 is a LINE, so only the top and bottom edges draw.
void aDrawBoxF_Direct(HookContext& h, const s32 x0, const s32 y0, const s32 x1, const s32 y1,
                      const u32 r, const u32 g, const u32 b, const u32 a) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;

    const f32 left = f32(x0) / 16.0f - kOffsetX;
    const f32 right = f32(x1) / 16.0f - kOffsetX;
    const f32 edge[2] = {f32(y0) / 16.0f - kOffsetY, f32(y1) / 16.0f - kOffsetY};
    if (!h.config->drawDirectBox) return;
    for (f32 y : edge) {
        DrawSprite sprite = DrawSprite::makeRect(left, y, right - left, 1.0f, 0, 0, 0, 0);
        sprite.setColor(Rgba{u8(r), u8(g), u8(b), u8(a)});
                    sprite.producer = "aDrawBoxF_Direct";
        frame->addSprite(sprite);
    }
}

// frCreateDrawFontPacket(...): one glyph. X and Y are 12.4 in the draw space, extents whole
// pixels.
void frCreateDrawFontPacket(HookContext& h, const s32 X, const s32 Y, const s32 Width,
                            const s32 Height, const u32 Col, const u32 pUv, const u32 pGsTex) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;

    // frSwapColor reverses the bytes, which lands on the same order a DrawSprite color uses.
    const Rgba color = Rgba::unpackColorWord(Col);
    const auto uv = [&](usize i) { return f32(s32(u32Ptr_t{pUv + u32(i) * 4}.readOr())) / 16.0f; };

    const frGsTex_t sheet = frGsTex_PTR_t{pGsTex}.readOr();
    // The palettes are static data, so they read whether or not the glyph uploads ran in the window.
    h.textures.loadFontClut(pcsx2hook_find_symbol(h.runtime, "frFontClutTable"));
    h.textures.wantFontSheet(sheet.Addr, sheet.BufWidth, sheet.Width, sheet.Height);
    DrawSprite sprite = DrawSprite::makeRect(f32(X) / 16.0f, f32(Y) / 16.0f, f32(Width),
                                             f32(Height), uv(0), uv(1), uv(2), uv(3));
    sprite.setColor(color);
    sprite.texHandle = h.textures.getFontSheet(sheet.Width, frame);
    sprite.producer = "frCreateDrawFontPacket";
    frame->addSprite(sprite);
}

// A strip (TL, TR, BL, BR) into a cycle, so the third and fourth corners swap.
/// sdfPrimLinkFt4(tag, color, prim, x0, y0, u0, v0, ... x3, y3, u3, v3, z, allocator).
void sdfPrimLinkFt4(HookContext& h, const u32 aColor, const u32 aPrim, const s32 aX0,
                    const s32 aY0, const s32 aU0, const s32 aV0, const s32 aX1, const s32 aY1,
                    const s32 aU1, const s32 aV1, const s32 aX2, const s32 aY2, const s32 aU2,
                    const s32 aV2, const s32 aX3, const s32 aY3, const s32 aU3, const s32 aV3) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;

    const Rgba color = Rgba::unpack(aColor);
    DrawSprite sprite{};
    const auto place = [&](int corner, s32 x, s32 y, s32 u, s32 v) {
        sprite.corner[corner] = SpriteCorner{f32(x) / 16.0f - kOffsetX, f32(y) / 16.0f - kOffsetY,
                                             f32(u) / 16.0f, f32(v) / 16.0f, color};
    };
    place(0, aX0, aY0, aU0, aV0);
    place(1, aX1, aY1, aU1, aV1);
    place(3, aX2, aY2, aU2, aV2);
    place(2, aX3, aY3, aU3, aV3);
    sprite.boundTexture = true;
    sprite.blends = sdfGetPrimAbe(aPrim) != 0;
    sprite.producer = "sdfPrimLinkFt4";
    frame->addSprite(sprite);
}

/// sdfPrimLinkG4(tag, prim, x0, y0, color0, ... x3, y3, color3, z, allocator).
void sdfPrimLinkG4(HookContext& h, const u32 aPrim, const s32 aX0, const s32 aY0,
                   const u32 aColor0, const s32 aX1, const s32 aY1, const u32 aColor1,
                   const s32 aX2, const s32 aY2, const u32 aColor2, const s32 aX3, const s32 aY3,
                   const u32 aColor3) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;

    DrawSprite sprite{};
    const auto place = [&](int corner, s32 x, s32 y, u32 rgba) {
        sprite.corner[corner] = SpriteCorner{f32(x) / 16.0f - kOffsetX, f32(y) / 16.0f - kOffsetY,
                                             0.0f, 0.0f, Rgba::unpack(rgba)};
    };
    place(0, aX0, aY0, aColor0);
    place(1, aX1, aY1, aColor1);
    place(3, aX2, aY2, aColor2);
    place(2, aX3, aY3, aColor3);
    sprite.blends = sdfGetPrimAbe(aPrim) != 0;
    sprite.producer = "sdfPrimLinkG4";
    frame->addSprite(sprite);
}

// fclDrawSpriteCore(...). X and Y are 12.4 in the draw space; extents are pixels, H in the
// doubled vertical scale.
void fclDrawSpriteCore(HookContext& h, const s32 X, const s32 Y, const s32 W, const s32 H,
                       const u32 pCol, const u32 pSprite, const u32 FlipFlag) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;

    const fclSprite_PTR_t sprite2d{pSprite};
    if (!sprite2d) return;

    // Half a texel in, on every edge, the way the engine offsets them.
    const f32 u0 = f32(u16(sprite2d->X) + 8) / 16.0f;
    const f32 u1 = f32(u16(u32(sprite2d->X) >> 16) + 8) / 16.0f;
    const f32 v0 = f32(u16(sprite2d->Y) + 8) / 16.0f;
    const f32 v1 = f32(u16(u32(sprite2d->Y) >> 16) + 8) / 16.0f;

    const f32 left = f32(X) / 16.0f - 0.5f;
    const f32 top = f32(Y) / 16.0f - 0.25f;
    DrawSprite sprite = DrawSprite::makeRect(left, top, f32(W), f32(H) * 0.5f,
                                             (FlipFlag & 1) ? u1 : u0, (FlipFlag & 2) ? v1 : v0,
                                             (FlipFlag & 1) ? u0 : u1, (FlipFlag & 2) ? v0 : v1);
    const u32 colorWord =
        pCol ? u32Ptr_t{pCol}.readOr() : u32Ptr_t{pSprite + u32(offsetof(fclSprite_t, Col))}.readOr();
    sprite.setColor(Rgba::unpackColorWord(colorWord));
    sprite.texHandle = h.textures.getHandle(sprite2d->TexHandle, frame);
    sprite.producer = "fclDrawSpriteCore";
    frame->addSprite(sprite);
}

// cmpDrawBoxG(x, y, Z, w, h, pCol, Ctxt, OtNo): a gouraud quad. x and y are 12.4 in draw space.
/// @todo TODO.md M2: correct and still costs 6.2 points, so the table traces it instead.
// cppcheck-suppress unusedFunction
void cmpDrawBoxG(HookContext& h, const s32 x, const s32 y, const s32 w, const s32 height,
                 const u32 pCol) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;

    DrawSprite sprite =
        DrawSprite::makeRect(f32(x) / 16.0f, f32(y) / 16.0f, f32(w), f32(height) * 0.5f, 0, 0, 0, 0);
    // The engine's order is top left, top right, bottom left, bottom right.
    static constexpr int kCorner[4] = {0, 1, 3, 2};
    for (int i = 0; i < 4; i++) {
        sprite.corner[kCorner[i]].color =
            Rgba::unpackColorWord(u32Ptr_t{pCol + u32(i) * 4}.readOr());
    }
    sprite.producer = "cmpDrawBoxG";
    frame->addSprite(sprite);
}

/// cmpDrawTriG(pX, pY, Z, pCol, OtNo): three x, three y and three colors, read through
/// pointers.
/// @fn cmpDrawTriG
/// @note SLUS-20911 0x00239FD0
void cmpDrawTriG(HookContext& h, const u32 pX, const u32 pY, const u32 pCol) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;

    DrawSprite sprite{};
    for (int i = 0; i < 3; i++) {
        const s32 x = s32(u32Ptr_t{pX + u32(i) * 4}.readOr());
        const s32 y = s32(u32Ptr_t{pY + u32(i) * 4}.readOr());
        const u32 colorWord = u32Ptr_t{pCol + u32(i) * 4}.readOr();
        sprite.corner[i] =
            SpriteCorner{f32(x) / 16.0f, f32(y) / 16.0f, 0.0f, 0.0f,
                         Rgba::unpackColorWord(colorWord)};
    }
    // A sprite carries four corners, so the last repeats and its triangle collapses.
    sprite.corner[3] = sprite.corner[2];
    sprite.producer = "cmpDrawTriG";
    frame->addSprite(sprite);
}

/// One env set per table, striding 96 bytes, holding that layer's frame buffer, clear, alpha
/// test and blend for both GS contexts.
/// @fn sdfModelDrawEnvLinkOt
/// @note SLUS-20911 0x002B2590
void sdfModelDrawEnvLinkOt(HookContext& h, const u32 aOt, const sdfModelDrawEnvPtr_t aEnv) {
    h.trace->onPrimEntry("sdfModelDrawEnvLinkOt", aOt);

    static bool shown = false;
    if (h.config->dumpDrawEnv && !shown) {
        shown = true;
        constexpr u32 kSetBytes = sizeof(sdfModelDrawEnvSet_t);
        for (u32 ot = 0; ot < std::size(sdfModelDrawEnv_t{}.set); ot++) {
            const u32 at = aEnv.addr + kSetBytes * ot;
            JsonObject row;
            row.addString("dump", "drawEnv").addUint("ot", ot).addHexDigits("at", at, 8);
            row.beginArray("qwords");
            for (u32 q = 0; q < kSetBytes / sdfQwordBytes; q++) {
                const u64* pair = u64Ptr_t{at + 16 * q}.slice(2);
                if (!pair) break;
                row.beginObject().addUint("at", 16 * q).addHex128("qword", pair[1], pair[0]).endObject();
            }
            log.dump(row.endArray().take());
        }
    }
}

/// sdfBillBoardMakeN(aCenter, aColor, aUVList, aXYOffsetList, aNum, aAllocator): one batch of
/// camera-facing quads. The four pointers are one allocation, so it is taken as the struct it is.
/// @fn sdfBillBoardMakeN
/// @note SLUS-20911 0x002BB0F0
void sdfBillBoardMakeN(HookContext& h, const sdfBillBoardBatchPtr_t aCenter, const u32 aNum) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    if (h.config->noBillboards) return;
    h.modelHooks.sdfBillBoardMakeN(aCenter, aNum, h.frames.getFrame());
}

/// drawSprite3D(aTexHandle, aSprite): the level rather than sdfBillBoardMake1, whose rotation
/// only reaches the transform packet through VU0.
/// @fn drawSprite3D
/// @note SLUS-20911 0x0012A2D0
void drawSprite3D(HookContext& h, const TexHandle aTexHandle, const Sprite3DPtr_t aSprite) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    if (h.config->noBillboards) return;
    h.modelHooks.drawSprite3D(aTexHandle, aSprite, h.frames.getFrame());
}

/// sdf3DPrimMakePoly(aPoly): counts, a PRIM word and one array per attribute. Every sdf3DPrim
/// producer builds through it.
/// @fn sdf3DPrimMakePoly
/// @note SLUS-20911 0x002BAAB8
void sdf3DPrimMakePoly(HookContext& h, const sdf3DPrimPolyParmPtr_t aPoly) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    if (!aPoly) return;

    if (!h.config->drawPrimPoly) return;
    h.modelHooks.sdf3DPrimMakePoly(aPoly, h.frames.getFrame());
}

/// effPkt3DPrimMakePoly(aPoly): the game's effect copy of sdf3DPrimMakePoly.
/// @fn effPkt3DPrimMakePoly
/// @note SLUS-20911 0x0016A120
void effPkt3DPrimMakePoly(HookContext& h, const sdf3DPrimPolyParmPtr_t aPoly) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    if (!aPoly) return;

    if (!h.config->drawPrimPoly) return;
    h.modelHooks.effPkt3DPrimMakePoly(aPoly, h.frames.getFrame());
}

/// fldDrawPoly3(...): one gouraud triangle. The largest unhandled producer by packets, and
/// invisible to a vertex ranking since its geometry only exists after VU1 runs.
/// @fn fldDrawPoly3
/// @note SLUS-20911 0x0012A5E0
void fldDrawPoly3(HookContext& h, const sdfVector3_t point[3], const u32 color[3]) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    if (!h.config->poly) return;
    h.modelHooks.fldDrawPoly3(point, color, h.frames.getFrame());
}

/// The color and level are globals and the quad is the whole screen. Alpha counts down from
/// 128, and nothing draws once the fade is clear.
/// @fn dds3DrawFade
/// @note SLUS-20911 0x001052D8
void dds3DrawFade(HookContext& h) {
    const ProducerDraws draws(h.frames);
    const u32 fade = pcsx2hook_find_symbol(h.runtime, "fadergba");
    if (!fade) {
        reportUnsupported("fade globals not in the symbol map", 0);
        return;
    }
    const u8 r = u8Ptr_t{fade}.readOr();
    const u8 g = u8Ptr_t{fade + 1}.readOr();
    const u8 b = u8Ptr_t{fade + 2}.readOr();
    const u8 state = u8Ptr_t{fade + 3}.readOr();
    if (state == 0) return;

    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;

    DrawSprite sprite = DrawSprite::makeRect(0, 0, kGuestWidth, kGuestHeight, 0, 0, 0, 0);
    sprite.setColor(Rgba{r, g, b, u8(128 - state)});
    sprite.producer = "dds3DrawFade";
    frame->addSprite(sprite);
}

/// One glyph uploaded. Its slot in the sheet is only known through the GsFont_t it returns.
/// @fn frTranslateUserData
/// @note SLUS-20911 0x001C7408
void frTranslateUserData(HookContext& h, s32 Width, s32 Height, u32 pData, u32 aResult) {
    const GsFont_PTR_t font{aResult};
    if (!font) return;
    const FontMem_PTR_t slot = font->font_mem;
    if (!slot) return;

    h.textures.loadFontClut(pcsx2hook_find_symbol(h.runtime, "frFontClutTable"));
    h.textures.onGlyph(u32(slot->uv[0]) >> 4, u32(slot->uv[1]) >> 4, u32(Width), u32(Height),
                       pData);
}

}  // namespace sdfhook
