#include "hooks.h"

#include <cstring>
#include <unordered_map>

#include "log.h"
#include "json.h"

#include <cmath>
#include <cstddef>
#include <vector>

#include "guest_ptr.h"
#include "gif.h"
#include "unsupported.h"
#include "libsdf.h"
#include "render/render_thread.h"
#include "trace.h"

namespace sdfhook {


namespace {

LogChannel log("hooks");

// Reading a packet that yielded nothing, rather than guessing at it.
void dumpPacket(const LinkedPacket& packet, const char* by, const DmaTag& tag) {
    const bool byRef = tag.id == 0 || tag.id == 3 || tag.id == 4;
    const u32 base = byRef ? tag.addr : packet.addr + 16;
    JsonObject row;
    row.addString("dump", "packet")
        .addString("by", by)
        .addHexDigits("at", packet.addr, 8)
        .addUint("dmaId", tag.id)
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

// Fills no frame by design: packets are the check on the API hooks, not the renderer's input.
// Producers whose draws reach the renderer through their own entry point.
bool isHandled(const char* producer) {
    static const char* const kHandled[] = {"DrwSprite2D", "DrwSprite2DF", "nbPnlSprite",
                                          "aDrawBoxG", "aDrawBoxF_Direct",
                                          "fclDrawSpriteCore", "cmpDrawTriG",
                                          "cmpDrawBoxG",     "dds3InitDrawEnvSub",
                                          "dds3DrawFade",     "dds3LinkZClearDrawTag",
                                          "fldDrawFrameSprite",
                                          "aDrawBoxF",
                                          "frCreateDrawFontPacket_20550848", "fldDrawEATexQuad",
                                          "DispCommCursor",  "DispHelpPanel",
                                          // ModelWalk redraws everything this links.
                                          "sdfModelLinkOt"};
    for (const char* name : kHandled) {
        if (std::strcmp(producer, name) == 0) return true;
    }
    return false;
}


/// Replays the frame's GS state in the order the hardware sees it, which is the only order that
/// settles what a draw ran under: the ordering tables sort their tags before transmitting, so the
/// link order a packet was built in says nothing. sdfGrReqDraw's tag names the chain head at +4.
/// @disc SLUS-20911
/// @address 0x002AD980
/// @function sdfGrDrawFrame
std::unordered_map<u32, sdfGsState_t> replayTransmitOrder(u32 aTag) {
    std::unordered_map<u32, sdfGsState_t> settled;
    sdfGsState_t gs{};
    u32 stack[16] = {};
    u32 depth = 0;
    u32 at = u32Ptr_t{aTag + 4}.valueOr() & 0x0FFFFFFFu;

    // A frame links a few thousand tags. Anything beyond that is a chain that loops back on itself
    // or a tag read out of data, and walking it costs the guest thread the whole frame.
    constexpr u32 kMaxChainTags = 65536;
    u32 steps = 0;
    for (; at && steps < kMaxChainTags; steps++) {
        const u32* tag = u32Ptr_t{at}.slice(2);
        if (!tag) break;
        const u32 qwc = tag[0] & 0xFFFF;
        const u32 id = (tag[0] >> 28) & 7;
        const u32 addr = tag[1] & 0x0FFFFFFFu;

        applyGsRegisterWrites(at, gs);
        settled[at] = gs;

        switch (id) {
            case sdfDmaTagRef:
            case sdfDmaTagRefs: at += sdfQwordBytes; break;
            case sdfDmaTagCnt: at += sdfQwordBytes * (1 + qwc); break;
            case sdfDmaTagNext: at = addr; break;
            case sdfDmaTagCall:
                if (depth == std::size(stack)) return settled;
                stack[depth++] = at + sdfQwordBytes * (1 + qwc);
                at = addr;
                break;
            case sdfDmaTagRet: at = depth ? stack[--depth] : 0; break;
            default: at = 0; break;  // REFE and END both stop the chain
        }
    }
    if (steps == kMaxChainTags) reportUnsupported("transmit chain tags", steps);
    return settled;
}

/// Gives every mesh the ALPHA and TEST its packet actually transmitted under. The link order the
/// meshes were built in and the link order the engine files its geometry in are the same walk, so
/// the nth geometry link is the nth mesh's, however many meshes that link produced.
void settleMeshState(HookContext& h, u32 aTag) {
    Frame* frame = h.frames.getFrame();
    if (!frame || frame->meshes.empty()) return;

    const std::unordered_map<u32, sdfGsState_t> settled =
        h.config->noTransmitState ? std::unordered_map<u32, sdfGsState_t>()
                                  : replayTransmitOrder(aTag);
    if (settled.empty()) {
        h.model.traceMeshes(*frame);
        return;
    }

    const std::vector<u32>& byLink = h.frames.getGeneratedLinks();

    for (DrawMesh& mesh : frame->meshes) {
        const sdfGsState_t* state = nullptr;
        if (mesh.linkAddr) {
            const auto found = settled.find(mesh.linkAddr);
            if (found != settled.end()) state = &found->second;
        } else if (mesh.linkIndex < byLink.size()) {
            mesh.linked = byLink[mesh.linkIndex] != 0;
            const auto found = settled.find(byLink[mesh.linkIndex]);
            if (found != settled.end()) state = &found->second;
        }
        if (!state) {
            h.model.getTrace().countUnsettledMesh();
            continue;
        }
        const u64 mask = mesh.gs.frameMask;
        mesh.gs = *state;
        mesh.gs.frameMask = mask;
        h.model.getTrace().countSettledMesh();
    }
    h.model.traceMeshes(*frame);
}

void collectPackets(HookContext& h) {
    GifParser parser(&h.coverage.gif);
    sdfGsState_t& gs = h.model.getGsState();

    for (const LinkedPacket& packet : h.frames.getPackets()) {
        const GifCounts before = h.coverage.gif;
        // The state a draw runs under is whatever the packets linked before it left behind, so it
        // is tracked across the whole chain rather than looked up per draw.
        const sdfGsState_t was = gs;
        const bool stateOnly = applyGsRegisterWrites(packet.addr, gs);
        if (h.config->dumpDrawEnv && (gs.alpha != was.alpha || gs.test != was.test ||
                                      gs.frameMask != was.frameMask)) {
            log.dump(JsonObject()
                         .addString("dump", "gsState")
                         .addUint("frame", h.trace->getFrame())
                         .addString("by", h.symbols.getFunction(packet.caller))
                         .addHexDigits("alpha", gs.alpha, 2)
                         .addHexDigits("test", gs.test, 6)
                         .addHexDigits("fbmsk", gs.frameMask, 8)
                         .addUint("aref", sdfTestAref(gs.test))
                         .take());
        }
        parser.parse(packet.addr);
        if (!h.trace->isOpen()) continue;

        DmaTag tag{};
        readDmaTag(packet.addr, tag);
        u64 verts = 0;
        for (int i = 0; i < 8; i++) verts += h.coverage.gif.verts[i] - before.verts[i];
        const char* by = h.symbols.getFunction(packet.caller);
        if (!isHandled(by)) {
            if (verts) h.coverage.unhandledVerts[by] += verts;
            else if (!stateOnly) h.coverage.unhandledPackets[by]++;
        }
        h.trace->onPacket(packet, by, tag, verts);
        if (!h.config->dumpPackets.empty() && !verts && std::strstr(by, h.config->dumpPackets.c_str())) {
            dumpPacket(packet, by, tag);
        }
    }
    h.frames.getPackets().clear();
}

// Y is in half-height units, the same scale the draw space uses vertically.
void submitSprite(HookContext& h, const char* producer, f32 x, f32 y, f32 w, f32 height, f32 u, f32 v,
                f32 uw, f32 vh, Rgba colour, u32 texFileHandle) {
    Frame* frame = h.frames.getFrame();
    // A producer that names no texture is drawing with the one sdf2DLinkTexPacket bound ahead of
    // it, which is how the HUD plates and the moon icon get theirs.
    const TexHandle named{texFileHandle ? texFileHandle : h.state.texture};
    const TexHandle texHandle = h.textures.getHandle(named, frame);
    // A producer that named a texture we never decoded draws flat, which is how a whole icon comes
    // out a grey box. Counted by producer so the gap names its own source.
    if (named.addr && !texHandle.addr) h.coverage.unresolvedTextures[producer]++;
    h.coverage.apiDraws++;
    if (!frame) return;
    DrawSprite sprite = DrawSprite::fromRect(x, y * 0.5f, w, height * 0.5f, u, v, u + uw, v + vh);
    sprite.test = h.state.test;
    sprite.otOffset = h.state.otOffset;
    sprite.producer = producer;
    sprite.setColour(colour);
    sprite.texHandle = texHandle;
    sprite.alpha = h.state.alpha;
    frame->addSprite(sprite);
}

// (x, y, w, h, u, v, uw, vh, rgba, texHandle), with the last two in stack slots. nbPnlSprite has
// the same shape and differs only in the ordering table it registers into.
int spriteHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const s32 x = s32(c.getArg(0));
    const s32 y = s32(c.getArg(1));
    const s32 w = s32(c.getArg(2));
    const s32 height = s32(c.getArg(3));
    const s32 u = s32(c.getArg(4));
    const s32 v = s32(c.getArg(5));
    const s32 uw = s32(c.getArg(6));
    const s32 vh = s32(c.getArg(7));
    const u32 rgba = u32Ptr_t{c.getStackArgAddr(8)}.valueOr();
    const u32 th = u32Ptr_t{c.getStackArgAddr(9)}.valueOr();

    submitSprite(h, "DrwSprite2D", f32(x), f32(y), f32(w), f32(height), f32(u), f32(v),
                 f32(uw), f32(vh),
                 Rgba::fromPacked(rgba), th);
    return PCSX2HOOK_CONTINUE;
}

// The same call with a float rectangle, so its integer arguments start one register file over.
int spriteFloatHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const f32 x = c.getFloatArg(0);
    const f32 y = c.getFloatArg(1);
    const f32 w = c.getFloatArg(2);
    const f32 height = c.getFloatArg(3);
    const s32 u = s32(c.getArg(0));
    const s32 v = s32(c.getArg(1));
    const s32 uw = s32(c.getArg(2));
    const s32 vh = s32(c.getArg(3));
    const u32 rgba = c.getArg(4);
    const u32 th = c.getArg(5);

    submitSprite(h, "DrwSprite2DF", x, y, w, height, f32(u), f32(v), f32(uw), f32(vh),
                 Rgba::fromPacked(rgba), th);
    return PCSX2HOOK_CONTINUE;
}

// rsetAlpha takes a mode rather than a register, and resolves it through this table.
GsAlpha alphaForMode(u32 mode) {
    switch (mode) {
        case 1: return GsAlpha::Additive;
        case 2:
        case 6: return GsAlpha::ReverseSubtract;
        case 3: return GsAlpha(0x84);
        case 4: return GsAlpha(0x06);
        case 5: return GsAlpha(0x89);
        case 10: return GsAlpha(0x2A);
        case 20: return GsAlpha(0x4A);
        case 0:
        case 7:
        case 8:
        case 9:
        case 11:
        case 12:
        case 13:
        case 14:
        case 15:
        case 16:
        case 17:
        case 18:
        case 19: return GsAlpha::Blend;
        default: reportUnsupported("rsetAlpha mode", mode); return GsAlpha::Blend;
    }
}

int sdf2DLinkTexPacketHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    const u32 texFileHandle = c.getArg(1);
    static_cast<HookContext*>(user)->state.texture = texFileHandle;
    return PCSX2HOOK_CONTINUE;
}

int rsetAlphaHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    const u32 mode = c.getArg(0);
    static_cast<HookContext*>(user)->state.alpha = alphaForMode(mode);
    return PCSX2HOOK_CONTINUE;
}

// rsetOtOfs(otofs): which ordering table the 2D draws after it go into, which is how SDF orders
// them against each other. There is no per-sprite depth to test.
int rsetOtOfsHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};

    const s32 otofs = s32(c.getArg(0));

    static_cast<HookContext*>(user)->state.otOffset = otofs;
    return PCSX2HOOK_CONTINUE;
}

/// rsetFrameMaskRgbOn/Off, still ooyTestRGBMaskOn/Off in the map. They bracket the silhouette pass
/// in call order, which is where our walk runs, so the flag needs no transmit-order settling.
/// @fn rsetFrameMaskRgbOn
/// @note SLUS-20911 0x0012A3F0
int frameMaskOnHook(pcsx2hook_regs*, void* user) {
    static_cast<HookContext*>(user)->state.frameMask = sdfFrameMaskRgb;
    return PCSX2HOOK_CONTINUE;
}

int frameMaskOffHook(pcsx2hook_regs*, void* user) {
    static_cast<HookContext*>(user)->state.frameMask = 0;
    return PCSX2HOOK_CONTINUE;
}

// rsetTest(ate, atst, aref, afail, date, datm, zte, ztst), assembled the way the engine does.
int rsetTestHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    const u32 ate = c.getArg(0);
    const u32 atst = c.getArg(1);
    const u32 aref = c.getArg(2);
    const u32 afail = c.getArg(3);
    const u32 date = c.getArg(4);
    const u32 datm = c.getArg(5);
    const u32 ztst = c.getArg(7);

    const u32 test = ate | (atst << 1) | (aref << 4) | (afail << 12) | (date << 14) |
                     (datm << 15) | 0x10000u | (ztst << 17);
    static_cast<HookContext*>(user)->state.test = test;
    return PCSX2HOOK_CONTINUE;
}



// aDrawBoxG(x, y, w, h, then four corner colours): a trifan quad, its coordinates already in the
// draw space's own vertical scale rather than the doubled one the sprite calls use.
int aDrawBoxGHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    const auto stack = [&](usize i) { return u32Ptr_t{c.getStackArgAddr(i)}.valueOr(); };

    const s32 x = s32(c.getArg(0));
    const s32 y = s32(c.getArg(1));
    const s32 w = s32(c.getArg(2));
    const s32 height = s32(c.getArg(3));
    const u32 r0 = c.getArg(4);
    const u32 g0 = c.getArg(5);
    const u32 b0 = c.getArg(6);
    const u32 a0 = c.getArg(7);
    const u32 r1 = stack(8);
    const u32 g1 = stack(9);
    const u32 b1 = stack(10);
    const u32 a1 = stack(11);
    const u32 r2 = stack(12);
    const u32 g2 = stack(13);
    const u32 b2 = stack(14);
    const u32 a2 = stack(15);
    const u32 r3 = stack(16);
    const u32 g3 = stack(17);
    const u32 b3 = stack(18);
    const u32 a3 = stack(19);

    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return PCSX2HOOK_CONTINUE;

    DrawSprite sprite = DrawSprite::fromRect(f32(x), f32(y), f32(w), f32(height), 0, 0, 0, 0);
    sprite.corner[0].colour = Rgba{u8(r0), u8(g0), u8(b0), u8(a0)};
    sprite.corner[1].colour = Rgba{u8(r1), u8(g1), u8(b1), u8(a1)};
    sprite.corner[2].colour = Rgba{u8(r2), u8(g2), u8(b2), u8(a2)};
    sprite.corner[3].colour = Rgba{u8(r3), u8(g3), u8(b3), u8(a3)};
    sprite.alpha = h.state.alpha;
    sprite.test = h.state.test;
    sprite.otOffset = h.state.otOffset;
    sprite.producer = "aDrawBoxG";
    frame->addSprite(sprite);
    return PCSX2HOOK_CONTINUE;
}

// aDrawBoxF(x, y, w, h, r, g, b, a) in pixels.
int aDrawBoxFHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const s32 x = s32(c.getArg(0));
    const s32 y = s32(c.getArg(1));
    const s32 w = s32(c.getArg(2));
    const s32 height = s32(c.getArg(3));
    const u32 r = c.getArg(4);
    const u32 g = c.getArg(5);
    const u32 b = c.getArg(6);
    const u32 a = c.getArg(7);

    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return PCSX2HOOK_CONTINUE;

    DrawSprite sprite = DrawSprite::fromRect(f32(x), f32(y), f32(w), f32(height), 0, 0, 0, 0);
    sprite.setColour(Rgba{u8(r), u8(g), u8(b), u8(a)});
    sprite.alpha = h.state.alpha;
    sprite.producer = "aDrawBoxF";
    frame->addSprite(sprite);
    return PCSX2HOOK_CONTINUE;
}

// aDrawBoxF_Direct(x0, y0, x1, y1, r, g, b, a) in raw GS coordinates, XYOFFSET included. It is not
// a filled box: it builds PRIM 0x41, a LINE, so the hardware draws the top and bottom edges only.
int aDrawBoxF_DirectHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const s32 x0 = s32(c.getArg(0));
    const s32 y0 = s32(c.getArg(1));
    const s32 x1 = s32(c.getArg(2));
    const s32 y1 = s32(c.getArg(3));
    const u32 r = c.getArg(4);
    const u32 g = c.getArg(5);
    const u32 b = c.getArg(6);
    const u32 a = c.getArg(7);

    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return PCSX2HOOK_CONTINUE;

    const f32 left = f32(x0) / 16.0f - kOffsetX;
    const f32 right = f32(x1) / 16.0f - kOffsetX;
    const f32 edge[2] = {f32(y0) / 16.0f - kOffsetY, f32(y1) / 16.0f - kOffsetY};
    if (!h.config->drawDirectBox) return PCSX2HOOK_CONTINUE;
    for (f32 y : edge) {
        DrawSprite sprite = DrawSprite::fromRect(left, y, right - left, 1.0f, 0, 0, 0, 0);
        sprite.setColour(Rgba{u8(r), u8(g), u8(b), u8(a)});
        sprite.alpha = h.state.alpha;
        sprite.test = h.state.test;
        sprite.otOffset = h.state.otOffset;
        sprite.producer = "aDrawBoxF_Direct";
        frame->addSprite(sprite);
    }
    return PCSX2HOOK_CONTINUE;
}

// frCreateDrawFontPacket(X, Y, Width, Height, ClutNo, Col, Pri, Filter, pUv, pGsTex, Ot): one
// glyph. X and Y are already 12.4 relative to the draw space, while the extents are whole pixels.
int frCreateDrawFontPacketHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const s32 X = s32(c.getArg(0));
    const s32 Y = s32(c.getArg(1));
    const s32 Width = s32(c.getArg(2));
    const s32 Height = s32(c.getArg(3));
    const u32 Col = c.getArg(5);
    const u32 pUv = u32Ptr_t{c.getStackArgAddr(8)}.valueOr();
    const u32 pGsTex = u32Ptr_t{c.getStackArgAddr(9)}.valueOr();

    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return PCSX2HOOK_CONTINUE;

    // frSwapColor reverses the bytes, which lands on the same order a DrawSprite colour uses.
    const Rgba colour = Rgba::fromColorWord(Col);
    const auto uv = [&](usize i) { return f32(s32(u32Ptr_t{pUv + u32(i) * 4}.valueOr())) / 16.0f; };

    const frGsTex_t sheet = frGsTex_PTR_t{pGsTex}.valueOr();
    // The palettes are static data, so they read whether or not the glyph uploads ran in the window.
    h.textures.loadFontClut(pcsx2hook_find_symbol(h.runtime, "frFontClutTable"));
    h.textures.wantFontSheet(sheet.Addr, sheet.BufWidth, sheet.Width, sheet.Height);
    DrawSprite sprite = DrawSprite::fromRect(f32(X) / 16.0f, f32(Y) / 16.0f, f32(Width),
                                             f32(Height), uv(0), uv(1), uv(2), uv(3));
    sprite.setColour(colour);
    sprite.texHandle = h.textures.getFontSheet(sheet.Width, frame);
    sprite.alpha = h.state.alpha;
    sprite.producer = "frCreateDrawFontPacket";
    frame->addSprite(sprite);
    return PCSX2HOOK_CONTINUE;
}

// A strip (TL, TR, BL, BR) into a cycle, so the third and fourth corners swap.
/// sdfPrimLinkFt4(tag, colour, prim, x0, y0, u0, v0, ... x3, y3, u3, v3, z, allocator).
int sdfPrimLinkFt4Hook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    const auto stack = [&](usize i) { return u32Ptr_t{c.getStackArgAddr(i)}.valueOr(); };

    const u32 aColor = c.getArg(1);
    const u32 aPrim = c.getArg(2);
    const s32 aX0 = s32(c.getArg(3));
    const s32 aY0 = s32(c.getArg(4));
    const s32 aU0 = s32(c.getArg(5));
    const s32 aV0 = s32(c.getArg(6));
    const s32 aX1 = s32(c.getArg(7));
    const s32 aY1 = s32(stack(8));
    const s32 aU1 = s32(stack(9));
    const s32 aV1 = s32(stack(10));
    const s32 aX2 = s32(stack(11));
    const s32 aY2 = s32(stack(12));
    const s32 aU2 = s32(stack(13));
    const s32 aV2 = s32(stack(14));
    const s32 aX3 = s32(stack(15));
    const s32 aY3 = s32(stack(16));
    const s32 aU3 = s32(stack(17));
    const s32 aV3 = s32(stack(18));

    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return PCSX2HOOK_CONTINUE;

    const Rgba colour = Rgba::fromPacked(aColor);
    DrawSprite sprite{};
    const auto place = [&](int corner, s32 x, s32 y, s32 u, s32 v) {
        sprite.corner[corner] = SpriteCorner{f32(x) / 16.0f - kOffsetX, f32(y) / 16.0f - kOffsetY,
                                             f32(u) / 16.0f, f32(v) / 16.0f, colour};
    };
    place(0, aX0, aY0, aU0, aV0);
    place(1, aX1, aY1, aU1, aV1);
    place(3, aX2, aY2, aU2, aV2);
    place(2, aX3, aY3, aU3, aV3);
    sprite.texHandle = h.textures.getHandle(TexHandle{h.state.texture}, frame);
    sprite.alpha = sdfPrimAbe(aPrim) ? h.state.alpha : GsAlpha::Opaque;
    sprite.producer = "sdfPrimLinkFt4";
    frame->addSprite(sprite);
    return PCSX2HOOK_CONTINUE;
}

/// sdfPrimLinkG4(tag, prim, x0, y0, colour0, ... x3, y3, colour3, z, allocator).
int sdfPrimLinkG4Hook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    const auto stack = [&](usize i) { return u32Ptr_t{c.getStackArgAddr(i)}.valueOr(); };

    const u32 aPrim = c.getArg(1);
    const s32 aX0 = s32(c.getArg(2));
    const s32 aY0 = s32(c.getArg(3));
    const u32 aColor0 = c.getArg(4);
    const s32 aX1 = s32(c.getArg(5));
    const s32 aY1 = s32(c.getArg(6));
    const u32 aColor1 = c.getArg(7);
    const s32 aX2 = s32(stack(8));
    const s32 aY2 = s32(stack(9));
    const u32 aColor2 = stack(10);
    const s32 aX3 = s32(stack(11));
    const s32 aY3 = s32(stack(12));
    const u32 aColor3 = stack(13);

    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return PCSX2HOOK_CONTINUE;

    DrawSprite sprite{};
    const auto place = [&](int corner, s32 x, s32 y, u32 rgba) {
        sprite.corner[corner] = SpriteCorner{f32(x) / 16.0f - kOffsetX, f32(y) / 16.0f - kOffsetY,
                                             0.0f, 0.0f, Rgba::fromPacked(rgba)};
    };
    place(0, aX0, aY0, aColor0);
    place(1, aX1, aY1, aColor1);
    place(3, aX2, aY2, aColor2);
    place(2, aX3, aY3, aColor3);
    sprite.alpha = sdfPrimAbe(aPrim) ? h.state.alpha : GsAlpha::Opaque;
    sprite.producer = "sdfPrimLinkG4";
    frame->addSprite(sprite);
    return PCSX2HOOK_CONTINUE;
}

// fclDrawSpriteCore(X, Y, W, H, pCol, pSprite, FlipFlag, AlphaFlag, FilterFlag, OtNo). X and Y are
// 12.4 relative to the draw space; the extents are pixels, with H in the doubled vertical scale.
int fclDrawSpriteCoreHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const s32 X = s32(c.getArg(0));
    const s32 Y = s32(c.getArg(1));
    const s32 W = s32(c.getArg(2));
    const s32 H = s32(c.getArg(3));
    const u32 pCol = c.getArg(4);
    const u32 pSprite = c.getArg(5);
    const u32 FlipFlag = c.getArg(6);
    const u32 AlphaFlag = c.getArg(7);

    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return PCSX2HOOK_CONTINUE;

    const fclSprite_PTR_t sprite2d{pSprite};
    if (!sprite2d) return PCSX2HOOK_CONTINUE;

    // Half a texel in, on every edge, the way the engine offsets them.
    const f32 u0 = f32(u16(sprite2d->X) + 8) / 16.0f;
    const f32 u1 = f32(u16(u32(sprite2d->X) >> 16) + 8) / 16.0f;
    const f32 v0 = f32(u16(sprite2d->Y) + 8) / 16.0f;
    const f32 v1 = f32(u16(u32(sprite2d->Y) >> 16) + 8) / 16.0f;

    const f32 left = f32(X) / 16.0f - 0.5f;
    const f32 top = f32(Y) / 16.0f - 0.25f;
    DrawSprite sprite = DrawSprite::fromRect(left, top, f32(W), f32(H) * 0.5f,
                                             (FlipFlag & 1) ? u1 : u0, (FlipFlag & 2) ? v1 : v0,
                                             (FlipFlag & 1) ? u0 : u1, (FlipFlag & 2) ? v0 : v1);
    const u32 colourWord =
        pCol ? u32Ptr_t{pCol}.valueOr() : u32Ptr_t{pSprite + u32(offsetof(fclSprite_t, Col))}.valueOr();
    sprite.setColour(Rgba::fromColorWord(colourWord));
    sprite.texHandle = h.textures.getHandle(sprite2d->TexHandle, frame);
    sprite.alpha = AlphaFlag == 1   ? GsAlpha::Additive
                   : AlphaFlag == 2 ? GsAlpha::ReverseSubtract
                                    : GsAlpha::Blend;
    sprite.producer = "fclDrawSpriteCore";
    frame->addSprite(sprite);
    return PCSX2HOOK_CONTINUE;
}

// The frame already presented, put back over the screen at a fixed alpha or the caller's.
void submitFrameFeedback(HookContext& h, u8 alpha) {
    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;

    DrawSprite sprite = DrawSprite::fromRect(-0.5f, -0.3125f, 512.0f, 224.0f, 0.0f, 0.0f,
                                             kGuestWidth, kGuestHeight);
    sprite.setColour(Rgba{0x80, 0x80, 0x80, alpha});
    sprite.texHandle = TexHandle{kPreviousFrameHandle};
    sprite.alpha = GsAlpha::Blend;
    sprite.producer = "submitFrameFeedback";
    frame->addSprite(sprite);
}

int fldDrawFrameSpriteHook(pcsx2hook_regs*, void* user) {
    submitFrameFeedback(*static_cast<HookContext*>(user), 48);
    return PCSX2HOOK_CONTINUE;
}

int fldDrawFrameSpriteAlphaHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    const u32 alpha = c.getArg(0);
    submitFrameFeedback(*static_cast<HookContext*>(user), u8(alpha));
    return PCSX2HOOK_CONTINUE;
}

// Its tag tests NEVER with AFAIL=ZB_ONLY, so it writes depth and no colour. Drawing the quad
// would paint the frame black.
int dds3LinkZClearDrawTagHook(pcsx2hook_regs*, void* user) {
    static_cast<HookContext*>(user)->coverage.apiDraws++;
    return PCSX2HOOK_CONTINUE;
}

// dds3DrawFade() takes no arguments: the colour and level are globals, and the quad is the whole
// screen. Alpha counts down from 128, and nothing is drawn once the fade is clear.
int dds3DrawFadeHook(pcsx2hook_regs*, void* user) {
    HookContext& h = *static_cast<HookContext*>(user);

    const u32 fade = pcsx2hook_find_symbol(h.runtime, "fadergba");
    if (!fade) {
        reportUnsupported("fade globals not in the symbol map", 0);
        return PCSX2HOOK_CONTINUE;
    }
    const u8 r = u8Ptr_t{fade}.valueOr();
    const u8 g = u8Ptr_t{fade + 1}.valueOr();
    const u8 b = u8Ptr_t{fade + 2}.valueOr();
    const u8 state = u8Ptr_t{fade + 3}.valueOr();
    if (state == 0) return PCSX2HOOK_CONTINUE;

    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return PCSX2HOOK_CONTINUE;

    DrawSprite sprite = DrawSprite::fromRect(0, 0, kGuestWidth, kGuestHeight, 0, 0, 0, 0);
    sprite.setColour(Rgba{r, g, b, u8(128 - state)});
    sprite.alpha = GsAlpha::Blend;
    sprite.producer = "dds3DrawFade";
    frame->addSprite(sprite);
    return PCSX2HOOK_CONTINUE;
}

// cmpDrawBoxG(x, y, Z, w, h, pCol, Ctxt, OtNo): a gouraud quad, submitted as a strip. x and y are
// 12.4 relative to the draw space, w is pixels and h is the doubled vertical scale.
/// @todo TODO.md M2: correct and still costs 6.2 points, so the table traces cmpDrawBoxG rather
/// than installing this.
// cppcheck-suppress unusedFunction
int cmpDrawBoxGHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const s32 x = s32(c.getArg(0));
    const s32 y = s32(c.getArg(1));
    const s32 w = s32(c.getArg(3));
    const s32 height = s32(c.getArg(4));
    const u32 pCol = c.getArg(5);

    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return PCSX2HOOK_CONTINUE;

    DrawSprite sprite =
        DrawSprite::fromRect(f32(x) / 16.0f, f32(y) / 16.0f, f32(w), f32(height) * 0.5f, 0, 0, 0, 0);
    // The engine's order is top left, top right, bottom left, bottom right.
    static constexpr int kCorner[4] = {0, 1, 3, 2};
    for (int i = 0; i < 4; i++) {
        sprite.corner[kCorner[i]].colour =
            Rgba::fromColorWord(u32Ptr_t{pCol + u32(i) * 4}.valueOr());
    }
    sprite.alpha = h.state.alpha;
    sprite.producer = "cmpDrawBoxG";
    frame->addSprite(sprite);
    return PCSX2HOOK_CONTINUE;
}

// cmpDrawTriG(pX, pY, Z, pCol, OtNo): three x, three y and three colours, read through pointers.
int cmpDrawTriGHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const u32 pX = c.getArg(0);
    const u32 pY = c.getArg(1);
    const u32 pCol = c.getArg(3);

    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return PCSX2HOOK_CONTINUE;

    DrawSprite sprite{};
    for (int i = 0; i < 3; i++) {
        const s32 x = s32(u32Ptr_t{pX + u32(i) * 4}.valueOr());
        const s32 y = s32(u32Ptr_t{pY + u32(i) * 4}.valueOr());
        const u32 colourWord = u32Ptr_t{pCol + u32(i) * 4}.valueOr();
        sprite.corner[i] =
            SpriteCorner{f32(x) / 16.0f, f32(y) / 16.0f, 0.0f, 0.0f,
                         Rgba::fromColorWord(colourWord)};
    }
    // A sprite carries four corners, so the last repeats and its triangle collapses.
    sprite.corner[3] = sprite.corner[2];
    sprite.alpha = h.state.alpha;
    sprite.producer = "cmpDrawTriG";
    frame->addSprite(sprite);
    return PCSX2HOOK_CONTINUE;
}

/// frTranslateUserData(Width, Height, pData, DataFreeMode) uploads one glyph into the sheet. The
/// slot it lands in is only known on the way out, through the GsFont_t it returns.
int frTranslateUserDataHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    static_cast<HookContext*>(user)->calls.enter(c);
    return PCSX2HOOK_CONTINUE;
}

void frTranslateUserDataPost(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const Context::Args in = h.calls.leave(c);
    const s32 Width = s32(in.a[0]);
    const s32 Height = s32(in.a[1]);
    const u32 pData = in.a[2];

    const GsFont_PTR_t font{c.getResult()};
    if (!font) return;
    const FontMem_PTR_t slot = font->font_mem;
    if (!slot) return;

    h.textures.loadFontClut(pcsx2hook_find_symbol(h.runtime, "frFontClutTable"));
    h.textures.onGlyph(u32(slot->uv[0]) >> 4, u32(slot->uv[1]) >> 4, u32(Width), u32(Height),
                       pData);
}

/// frCopyFontClutToVram(gsAddr, pData): the sixteen entries a glyph sheet is drawn through.
int frCopyFontClutToVramHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const u32 pData = c.getArg(1);

    h.textures.onFontClut(pData);
    return PCSX2HOOK_CONTINUE;
}

// sdfModelDrawEnvLinkOt(aOt, aEnv) links one env set per ordering table, striding 96 bytes: that
// layer's frame buffer, clear, alpha test and blend for both GS contexts.
int sdfModelDrawEnvLinkOtHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const u32 aOt = c.getArg(0);
    const u32 aEnv = c.getArg(1);
    h.trace->onPrimEntry("sdfModelDrawEnvLinkOt", aOt);

    static bool shown = false;
    if (h.config->dumpDrawEnv && !shown) {
        shown = true;
        for (u32 ot = 0; ot < sdfModelOrderingTables; ot++) {
            const u32 at = aEnv + sdfModelDrawEnvStride * ot;
            JsonObject row;
            row.addString("dump", "drawEnv").addUint("ot", ot).addHexDigits("at", at, 8);
            row.beginArray("qwords");
            for (u32 q = 0; q < sdfModelDrawEnvStride / 16; q++) {
                const u64* pair = u64Ptr_t{at + 16 * q}.slice(2);
                if (!pair) break;
                row.beginObject().addUint("at", 16 * q).addHex128("qword", pair[1], pair[0]).endObject();
            }
            log.dump(row.endArray().take());
        }
    }
    return PCSX2HOOK_CONTINUE;
}

// The scene walk. Ours runs at entry, over the same node list the engine is about to walk, so the
// two see identical state and the packet counts either side must agree.
int sdfModelLinkOtHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const u32 aOt = c.getArg(0);
    const sdfModelRootNodePtr_t aRoot{c.getArg(1)};

    h.trace->onSdfModelLinkOt(aOt, aRoot.addr);
    h.model.sdfModelLinkOt(aRoot, h.frames.getFrame());
    return PCSX2HOOK_CONTINUE;
}

int sdfCorePacketGenHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    h.trace->onSdfCorePacketGen(c.getArg(0), c.getArg(1), c.getArg(2), c.getArg(3));
    h.frames.beginGenerated();
    h.model.getTrace().countGuestGenerated();
    return PCSX2HOOK_CONTINUE;
}

int sdfModelLinkShapedDrawPacketHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    h.trace->onSdfModelLinkShapedDrawPacket(c.getArg(0), c.getArg(1));
    h.model.getTrace().countGuestShaped();
    return PCSX2HOOK_CONTINUE;
}

int sdf3DSetCameraHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    static_cast<HookContext*>(user)->trace->onSdf3DSetCamera(c.getArg(0), c.getArg(1));
    return PCSX2HOOK_CONTINUE;
}

int sdfGrDrawFrameHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    static_cast<HookContext*>(user)->trace->onSdfGrDrawFrame(c.getArg(0));
    return PCSX2HOOK_CONTINUE;
}

int sdfMatSetHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    static_cast<HookContext*>(user)->trace->onSdfMatSet(c.getArg(0));
    return PCSX2HOOK_CONTINUE;
}

// sdfTexSetEx(asset, memMode) returns the handle it filled in, so the post-hook has both the asset
// still in main memory and the description of what is in it.
int sdfTexSetExHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    h.trace->onSdfTexSetEx(c.getArg(0));
    h.calls.enter(c);
    return PCSX2HOOK_CONTINUE;
}

void sdfTexSetExPost(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    h.textures.add(TMX_HEADER_PTR_t{h.calls.leave(c).a[0]}, TexHandle{c.getResult()},
                   h.frames.getFrame());
}

int sdfGrSetClearPacketHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    static_cast<HookContext*>(user)->trace->onSdfGrSetClearPacket(c.getArg(0));
    return PCSX2HOOK_CONTINUE;
}

int sdfSortOtAvlRegistHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    static_cast<HookContext*>(user)->trace->onSdfSortOtAvlRegist(c.getArg(0), c.getArg(1), c.getFloatArg(0));
    return PCSX2HOOK_CONTINUE;
}

// The chain is finished here, so this is where a frame is done being built.
int sdfGrReqDrawHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    const u32 aTag = c.getArg(0);
    h.trace->onSdfGrReqDraw(aTag, c.getArg(1));
    settleMeshState(h, aTag);
    collectPackets(h);
    h.frames.submit();
    return PCSX2HOOK_CONTINUE;
}

int dds3DrawProcessHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    h.trace->onDds3DrawProcess(c.getArg(0));
    h.frames.begin();
    h.model.beginFrame(h.trace->getFrame());
    // The clear colour is a global the game may change between frames, so it is read per frame.
    const u32 backColor = pcsx2hook_find_symbol(h.runtime, "dds3BackColor");
    if (backColor) h.state.background = Rgba::fromColorWord(u32Ptr_t{backColor}.valueOr());
    return PCSX2HOOK_CONTINUE;
}

// No submission means dds3DrawFlag's 0x2000000 bit discarded the frame; leave the last one up.
void dds3DrawProcessPost(pcsx2hook_regs*, void* user) {
    HookContext& h = *static_cast<HookContext*>(user);
    h.frames.end();
}

// Every render function links its packet through here, so this is a complete frame inventory.
int sdfDrawTagLinkHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    if (c.getArg(1)) {
        h.frames.link(LinkedPacket{.addr = c.getArg(1), .caller = c.getRa()});
    }
    return PCSX2HOOK_CONTINUE;
}

// Texture uploads are linked by reference rather than inline, so they arrive through their own
// entry point: the packet is a REF tag pointing at data the game already holds.
int sdfDrawTagLinkRefHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    if (c.getArg(1)) {
        h.frames.link(LinkedPacket{.addr = c.getArg(1), .caller = c.getRa(),
                                   .kind = LinkKind::Ref});
    }
    return PCSX2HOOK_CONTINUE;
}

// The world geometry is linked as a DMA call chain rather than a packet, so it arrives here and
// nowhere else: without this the type 1 streams, three quarters of a field scene's triangles, are
// absent from the frame inventory entirely.
int sdfDrawTagLinkCallHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    if (c.getArg(1)) {
        h.frames.link(LinkedPacket{.addr = c.getArg(1), .caller = c.getRa(),
                                   .kind = LinkKind::Call});
    }
    return PCSX2HOOK_CONTINUE;
}

void sdfGsAllocFrameBuffPost(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    static_cast<HookContext*>(user)->textures.onFrameBuffer(sdfGsMemSlicePtr_t{c.getResult()});
}

// The one release point for a texture: sdfTexFreeDelayed only queues this through
// sdfTexDelayedResrcCB, and the 22 game-side callers all land here.
int sdfTexFreeHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const TexHandle aHandle{c.getArg(0)};
    h.textures.onFree(aHandle);
    return PCSX2HOOK_CONTINUE;
}

// A fourth way in: rsetFrameMaskRgbOn links the FBMSK packet through here, so without this the
// frame-mask pass is absent from the inventory and its write mask never reaches a draw.
int sdfDrawTagLinkRefGifHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    if (c.getArg(1)) {
        h.frames.link(LinkedPacket{.addr = c.getArg(1), .caller = c.getRa(),
                                   .kind = LinkKind::Ref});
    }
    return PCSX2HOOK_CONTINUE;
}

/// dds3BillboardTagLink(hBillboard, hResource, use_mesh_idx): fills a billboard batch one quad at a
/// time. Only the resource names the texture, and sdfBillBoardMakeN is handed the geometry alone,
/// so the handle has to be taken here.
/// @fn dds3BillboardTagLink
/// @note SLUS-20911 0x0015B320
int dds3BillboardTagLinkHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const dds3BillResourcePtr_t hResource{c.getArg(1)};
    if (hResource) h.model.setBillboardTexture(hResource->hTex);
    return PCSX2HOOK_CONTINUE;
}

/// dds3BillboardLinkOt(): links every pending billboard batch into the ordering table its own node
/// names. Read at entry, since the engine's loop rewrites the list as it consumes it.
/// @fn dds3BillboardLinkOt
/// @note SLUS-20911 0x0022E2E8
int dds3BillboardLinkOtHook(pcsx2hook_regs* regs, void* user) {
    (void)regs;
    static_cast<HookContext*>(user)->model.readBillboardTables();
    return PCSX2HOOK_CONTINUE;
}

/// sdfBillBoardMakeN(aCenter, aColor, aUVList, aXYOffsetList, aNum, aAllocator): one batch of
/// camera-facing quads, which is what the battle specters are drawn as. The four pointers are one
/// allocation, so the batch is taken as the struct it is.
/// @fn sdfBillBoardMakeN
/// @note SLUS-20911 0x002BB0F0
int sdfBillBoardMakeNHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const sdfBillBoardBatchPtr_t aCenter{c.getArg(0)};
    const u32 aNum = c.getArg(4);

    h.coverage.apiDraws++;
    if (h.config->noBillboards) return PCSX2HOOK_CONTINUE;
    h.model.sdfBillBoardMakeN(aCenter, aNum, h.frames.getFrame());
    return PCSX2HOOK_CONTINUE;
}

/// fldDrawPoly3(x0, y0, z0, x1, y1, z1, x2, y2, z2, col0, col1, col2): one gouraud triangle. The
/// largest unhandled producer in the game by packets, and invisible to a ranking by vertices because
/// its geometry only exists after VU1 runs.
/// @fn fldDrawPoly3
/// @note SLUS-20911 0x0012A5E0
int fldDrawPoly3Hook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    // Eight floats reach the FPRs and the ninth the stack, while the colours take a0 to a2.
    const sdfVector3_t point[3] = {
        {c.getFloatArg(0), c.getFloatArg(1), c.getFloatArg(2)},
        {c.getFloatArg(3), c.getFloatArg(4), c.getFloatArg(5)},
        {c.getFloatArg(6), c.getFloatArg(7), f32Ptr_t{c.getStackArgAddr(8)}.valueOr()},
    };
    const u32 colour[3] = {c.getArg(0), c.getArg(1), c.getArg(2)};

    h.coverage.apiDraws++;
    if (!h.config->poly) return PCSX2HOOK_CONTINUE;
    h.model.fldDrawPoly3(point, colour, h.frames.getFrame());
    return PCSX2HOOK_CONTINUE;
}

int sdfTexDupHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    static_cast<HookContext*>(user)->calls.enter(c);
    return PCSX2HOOK_CONTINUE;
}

void sdfTexDupPost(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    h.textures.onDuplicate(TexHandle{h.calls.leave(c).a[0]}, TexHandle{c.getResult()});
}

int observeHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    h.coverage.graph.record(h.symbols.getFunction(c.getPc()), h.symbols.getSite(c.getRa()));
    return PCSX2HOOK_CONTINUE;
}

// Only for a function nothing tail-calls: installing a post-handler replaces $ra with the return
// sentinel, which a tail call then passes to its callee as a bogus caller.
void observePost(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    h.coverage.graph.recordReturn(h.symbols.getFunction(c.getPc()), c.getResult());
}

int traceEntryHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    h.trace->onPrimEntry(h.symbols.getFunction(c.getPc()), c.getArg(0));
    return PCSX2HOOK_CONTINUE;
}

struct Entry {
    const char* name;
    pcsx2hook_pre_fn pre;
    pcsx2hook_post_fn post;
};

// SDF and the dds3 kernel above it. These carry over to another game on the same engine.
constexpr Entry kEngineEntries[] = {
    {"sdf3DSetCamera", &sdf3DSetCameraHook, nullptr},
    {"sdfModelLinkOt", &sdfModelLinkOtHook, nullptr},
    {"sdfCorePacketGen", &sdfCorePacketGenHook, nullptr},
    {"sdfModelLinkShapedDrawPacket", &sdfModelLinkShapedDrawPacketHook, nullptr},
    {"sdfSortOtAvlRegist", &sdfSortOtAvlRegistHook, nullptr},
    {"sdfGrReqDraw", &sdfGrReqDrawHook, nullptr},
    {"sdfGrDrawFrame", &sdfGrDrawFrameHook, nullptr},
    {"sdfModelDrawEnvLinkOt", &sdfModelDrawEnvLinkOtHook, nullptr},
    // Watched only, to find out whether any sample scene reaches a render target or a movie.
    {"dds3AllocRenderTexture", &observeHook, nullptr},
    {"sdfIpuSetChainSource", &observeHook, nullptr},
    {"sdfGrOtClose", &traceEntryHook, nullptr},
    {"sdfPrimLinkSprite", &observeHook, nullptr},
    {"sdfPrimLinkRect", &observeHook, nullptr},
    {"sdfPrimLinkG4", &sdfPrimLinkG4Hook, nullptr},
    {"sdfPrimLinkGt3", &observeHook, nullptr},
    {"sdfPrimLinkGt4", &observeHook, nullptr},
    {"sdfPrimLinkLine", &observeHook, nullptr},
    {"sdfPrimLinkSimplePacket", &observeHook, nullptr},
    // The whole family, so the caller list below is closed: nothing reaches a draw tag any other
    // way, and a producer missing from it is a producer that cannot draw.
    {"sdfDrawTagLink", &sdfDrawTagLinkHook, nullptr},
    {"sdfDrawTagLink2", &sdfDrawTagLinkHook, nullptr},
    {"sdfDrawTagLinkRef", &sdfDrawTagLinkRefHook, nullptr},
    {"sdfDrawTagLinkCall", &sdfDrawTagLinkCallHook, nullptr},
    {"sdfDrawTagLinkRefGif", &sdfDrawTagLinkRefGifHook, nullptr},
    {"sdfMatSet", &sdfMatSetHook, nullptr},
    {"sdfTexSetEx", &sdfTexSetExHook, &sdfTexSetExPost},
    {"sdfTexDup", &sdfTexDupHook, &sdfTexDupPost},
    {"sdfTexFree", &sdfTexFreeHook, nullptr},
    {"sdfGsAllocFrameBuff", &observeHook, &sdfGsAllocFrameBuffPost},
    {"sdfTexTransData", &observeHook, nullptr},
    {"sdfTexTransCLUT", &observeHook, nullptr},
    {"sdfTexAllocClutBuff", &observeHook, nullptr},
    {"sdfTexCopyClutData", &observeHook, nullptr},
    {"sdfTexGetPixelAddr", &observeHook, nullptr},
    {"sdfTexGetTexelBitCount", &observeHook, nullptr},
    {"sdfTexClutToGs", &observeHook, nullptr},
    {"sdfTexAlloc", &observeHook, nullptr},
    {"sdfTexMakeGIFPacket", &observeHook, &observePost},
    {"sdfTexNew", &observeHook, nullptr},
    {"sdfGrSetClearPacket", &sdfGrSetClearPacketHook, nullptr},
    {"sdf2DAllocSprite", &observeHook, nullptr},
    {"sdf2DMakeStdPacket", &traceEntryHook, nullptr},
    {"sdf2DMakeTexPacket", &traceEntryHook, nullptr},
    {"sdf2DLinkTexPacket", &sdf2DLinkTexPacketHook, nullptr},
    {"sdfPrimLinkF3", &observeHook, nullptr},
    {"sdfPrimLinkF4", &observeHook, nullptr},
    {"sdfPrimLinkFt3", &observeHook, nullptr},
    {"sdfPrimLinkFt4", &sdfPrimLinkFt4Hook, nullptr},
    {"sdfPrimLinkG3", &observeHook, nullptr},
    {"sdfPrimLinkBoxLine", &observeHook, nullptr},
    {"sdfPrimLinkBitBltPacket", &observeHook, nullptr},
    {"sdfBillBoardMake1", &traceEntryHook, nullptr},
    {"sdfBillBoardMakeN", &sdfBillBoardMakeNHook, nullptr},
    {"dds3BillboardTagLink", &dds3BillboardTagLinkHook, nullptr},
    {"dds3BillboardLinkOt", &dds3BillboardLinkOtHook, nullptr},
    {"sdfGrLinkDispFrameTex", &traceEntryHook, nullptr},
    {"dds3DrawProcess", &dds3DrawProcessHook, &dds3DrawProcessPost},
};

// This game's own drawing code. Nocturne barely uses SDF's semantic 2D API, so the producers that
// carry the meaning are here rather than in the engine, and are per disc.
constexpr Entry kGameEntries[] = {
    {"frTranslateUserData", &frTranslateUserDataHook, &frTranslateUserDataPost},
    {"frCopyFontClutToVram", &frCopyFontClutToVramHook, nullptr},
    {"DrwSprite2D", &spriteHook, nullptr},
    {"rsetAlpha", &rsetAlphaHook, nullptr},
    {"rsetTest", &rsetTestHook, nullptr},
    {"ooyTestPoly3", &fldDrawPoly3Hook, nullptr},
    {"ooyTestRGBMaskOn", &frameMaskOnHook, nullptr},
    {"ooyTestRGBMaskOff", &frameMaskOffHook, nullptr},
    {"rsetOtOfs", &rsetOtOfsHook, nullptr},
    {"rsetTexa", &traceEntryHook, nullptr},
    {"frCreateDrawFontPacket_20550848", &frCreateDrawFontPacketHook, nullptr},
    {"aDrawBoxG", &aDrawBoxGHook, nullptr},
    {"fclDrawSpriteCore", &fclDrawSpriteCoreHook, nullptr},
    {"cmpDrawTriG", &cmpDrawTriGHook, nullptr},
    // Traced, not drawn: its full-screen gouraud wash and row boxes are real draws, but drawing
    // them costs 6.2 points on the menu. See TODO M2.
    {"cmpDrawBoxG", &traceEntryHook, nullptr},
    {"dds3DrawFade", &dds3DrawFadeHook, nullptr},
    {"dds3LinkZClearDrawTag", &dds3LinkZClearDrawTagHook, nullptr},
    {"aDrawBoxF", &aDrawBoxFHook, nullptr},
    {"aDrawBoxF_Direct", &aDrawBoxF_DirectHook, nullptr},
    {"nbPnlSprite", &spriteHook, nullptr},
    {"DrwSprite2DF", &spriteFloatHook, nullptr},
    {"cmpDrawSprite2", &traceEntryHook, nullptr},
    {"cmpDrawSprite", &traceEntryHook, nullptr},
    {"fldDrawFrameSprite", &fldDrawFrameSpriteHook, nullptr},
    {"fldDrawFrameSpriteAlpha", &fldDrawFrameSpriteAlphaHook, nullptr},
    {"fclDrawSpriteAngleCore", &traceEntryHook, nullptr},
    {"itfMiscDrawSpritePrim", &traceEntryHook, nullptr},
    {"itfMiscDrawSpritePrim2", &traceEntryHook, nullptr},
    {"itfMiscDrawSpritePlanePrim", &traceEntryHook, nullptr},
    {"itfMiscDrawRectanglePrim", &traceEntryHook, nullptr},
    {"itfMiscDrawTexPoly", &traceEntryHook, nullptr},
    {"effPkt2DAllocTriStrip", &traceEntryHook, nullptr},
    {"eglLinkBox", &traceEntryHook, nullptr},
};

}  // namespace

// The frame a draw call lands in, opened by whichever call arrives first: the engine builds its
// content across several tasks before dds3DrawProcess submits it, so no call reliably precedes all.
Frame* FrameBuilder::getFrame() {
    if (!mBuilding && mRenderer && !mAcquireFailed) {
        mBuilding = mRenderer->beginFrame();
        if (mBuilding && mTextures) mTextures->drainRecovered(*mBuilding);
        // Every producer asks for the frame, so retrying here is hundreds of waits for one frame.
        mAcquireFailed = mBuilding == nullptr;
    }
    return mBuilding;
}

void FrameBuilder::submit() {
    mSubmitted = true;
    mSubmittedFrames++;
    if (!mBuilding || !mRenderer) return;
    // Every producer reaches the frame through addSprite, so one place stamps them all.
    if (mTrace && mTrace->isOpen()) {
        for (const DrawSprite& sprite : mBuilding->sprites) mTrace->onSprite(sprite);
    }
    mRenderer->submit(mBuilding);
    mBuilding = nullptr;
}

void FrameBuilder::end() {
    if (!mSubmitted && (mBuilding || !mPending.empty())) mDiscardedFrames++;
    if (mBuilding && mRenderer) mRenderer->abandon(mBuilding);
    mPending.clear();
    mAcquireFailed = false;
    mGeneratedLinks.clear();
    mGenerating = false;
    mBuilding = nullptr;
}

const char* SymbolCache::lookup(u32 address, bool trimOffset) {
    const auto known = mNames.find(address);
    if (known != mNames.end()) return known->second.c_str();
    char name[128];
    pcsx2hook_resolve_symbol(mRuntime, address, name, sizeof(name));
    if (trimOffset) {
        if (char* offset = std::strrchr(name, '+')) *offset = '\0';
    }
    return mNames.emplace(address, name).first->second.c_str();
}

void installHooks(HookContext& context, Trace& trace, RenderThread* render) {
    pcsx2hook_ctx* runtime = context.runtime;
    context.trace = &trace;
    context.symbols.attach(runtime);
    context.frames.attach(render, &context.textures, &trace);
    context.textures.attach(runtime, *context.config);
    context.model.attach(runtime, *context.config, &context.textures, &trace, &context.state);

    usize installed = 0;
    usize unnamed = 0;
    std::unordered_map<u32, const char*> seen;
    const auto install = [&](const Entry& entry) {
        const u32 address = pcsx2hook_find_symbol(runtime, entry.name);
        if (!address) {
            log.warn("{}: not in the symbol map", entry.name);
            unnamed++;
            return;
        }
        if (!pcsx2hook_add_hook(runtime, address, entry.pre, entry.post, &context)) {
            log.error("{} at 0x{:08X}: {}", entry.name, address,
                pcsx2hook_get_last_error(runtime));
            return;
        }
        // A second entry for an address the table already covers replaces the first, which is how
        // a producer quietly became a bare trace hook and its draws stopped reaching the frame.
        if (!seen.emplace(address, entry.name).second) {
            log.error("{} and {} are the same address {:08X}: the later hook wins",
                seen[address], entry.name, address);
        }
        installed++;
    };
    for (const Entry& entry : kEngineEntries) install(entry);
    const usize engineHooks = installed;
    for (const Entry& entry : kGameEntries) install(entry);

    const char* serial = pcsx2hook_get_disc_serial(runtime);
    log.info("{}: hooked {} engine and {} game entry points of {}{}",
        serial && *serial ? serial : "?", engineHooks, installed - engineHooks,
        std::size(kEngineEntries) + std::size(kGameEntries),
        unnamed ? " (some unnamed: is the symbol map for this disc?)" : "");
}

}  // namespace sdfhook
