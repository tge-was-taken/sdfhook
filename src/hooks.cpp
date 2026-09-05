#include "hooks.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "guest.h"
#include "gif.h"
#include "unsupported.h"
#include "libsdf.h"
#include "render/render_thread.h"
#include "trace.h"

namespace sdfhook {
namespace {

// SDF_DUMP_PACKETS names a linker whose packets are printed as raw quadwords, which is how a
// packet that yields nothing gets read rather than guessed at.
void dumpPacket(const Guest& guest, const LinkedPacket& packet, const char* by, const DmaTag& tag) {
    const bool byRef = tag.id == 0 || tag.id == 3 || tag.id == 4;
    const u32 base = byRef ? tag.addr : packet.addr + 16;
    std::fprintf(stderr, "[packet] %s 0x%08X dmaId=%u qwc=%u data=0x%08X\n", by, packet.addr,
                 tag.id, tag.qwc, base);
    const u32 count = tag.qwc < 16 ? tag.qwc : 16;
    for (u32 i = 0; i < count; i++) {
        const u8* qword = static_cast<const u8*>(guest.map(base + i * 16, 16));
        if (!qword) break;
        u64 lo = 0;
        u64 hi = 0;
        std::memcpy(&lo, qword, sizeof(lo));
        std::memcpy(&hi, qword + 8, sizeof(hi));
        std::fprintf(stderr, "   +%03u %016llX %016llX\n", i * 16, (unsigned long long)hi,
                     (unsigned long long)lo);
    }
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
                                          "DispCommCursor",  "DispHelpPanel"};
    for (const char* name : kHandled) {
        if (std::strcmp(producer, name) == 0) return true;
    }
    return false;
}

void collectPackets(HookContext& h) {
    const Guest guest(h.runtime);
    GifWalk walk(&h.coverage.gif);
    for (const LinkedPacket& packet : h.frames.getPackets()) {
        const GifCounts before = h.coverage.gif;
        walk.packet(guest, packet.addr);
        if (!h.trace->isOpen()) continue;

        DmaTag tag{};
        readDmaTag(guest, packet.addr, tag);
        u64 verts = 0;
        for (int i = 0; i < 8; i++) verts += h.coverage.gif.verts[i] - before.verts[i];
        const char* by = h.symbols.getFunction(packet.caller);
        if (verts && !isHandled(by)) h.coverage.unhandledVerts[by] += verts;
        h.trace->onPacket(packet, by, tag, verts);
        if (h.dumpCaller && !verts && std::strstr(by, h.dumpCaller)) {
            dumpPacket(guest, packet, by, tag);
        }
    }
    h.frames.getPackets().clear();
}

// Y is in half-height units, the same scale the draw space uses vertically.
void submitSprite(HookContext& h, const Guest& guest, f32 x, f32 y, f32 w, f32 height, f32 u, f32 v,
                f32 uw, f32 vh, Rgba colour, u32 texFileHandle) {
    const TexHandle texHandle = h.textures.getHandle(guest, texFileHandle);
    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;
    DrawSprite sprite = DrawSprite::fromRect(x, y * 0.5f, w, height * 0.5f, u, v, u + uw, v + vh);
    sprite.setColour(colour);
    sprite.texHandle = texHandle;
    sprite.alpha = h.state.alpha;
    frame->sprites.push_back(sprite);
}

// (x, y, w, h, u, v, uw, vh, rgba, texHandle), with the last two in stack slots. nbPnlSprite has
// the same shape and differs only in the ordering table it registers into.
int spriteHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    const Guest guest(h.runtime);

    const s32 x = s32(c.getArg(0));
    const s32 y = s32(c.getArg(1));
    const s32 w = s32(c.getArg(2));
    const s32 height = s32(c.getArg(3));
    const s32 u = s32(c.getArg(4));
    const s32 v = s32(c.getArg(5));
    const s32 uw = s32(c.getArg(6));
    const s32 vh = s32(c.getArg(7));
    const u32 rgba = guest.readU32(c.getStackArgAddr(8));
    const u32 th = guest.readU32(c.getStackArgAddr(9));

    submitSprite(h, guest, f32(x), f32(y), f32(w), f32(height), f32(u), f32(v), f32(uw), f32(vh),
                 Rgba::fromPacked(rgba), th);
    return PCSX2HOOK_CONTINUE;
}

// The same call with a float rectangle, so its integer arguments start one register file over.
int spriteFloatHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    const Guest guest(h.runtime);

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

    submitSprite(h, guest, x, y, w, height, f32(u), f32(v), f32(uw), f32(vh),
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

    static_cast<HookContext*>(user)->state.test = ate | (atst << 1) | (aref << 4) | (afail << 12) |
                                                  (date << 14) | (datm << 15) | 0x10000u |
                                                  (ztst << 17);
    return PCSX2HOOK_CONTINUE;
}

Rgba regRgba(const Context& c, usize first) {
    return Rgba{u8(c.getArg(first)), u8(c.getArg(first + 1)), u8(c.getArg(first + 2)),
                u8(c.getArg(first + 3))};
}

Rgba stackRgba(const Context& c, const Guest& guest, usize first) {
    const auto at = [&](usize i) { return u8(guest.readU32(c.getStackArgAddr(i))); };
    return Rgba{at(first), at(first + 1), at(first + 2), at(first + 3)};
}

// aDrawBoxG(x, y, w, h, then four corner colours): a trifan quad, its coordinates already in the
// draw space's own vertical scale rather than the doubled one the sprite calls use.
int aDrawBoxGHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    const Guest guest(h.runtime);
    const auto stack = [&](usize i) { return guest.readU32(c.getStackArgAddr(i)); };

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
    frame->sprites.push_back(sprite);
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
    frame->sprites.push_back(sprite);
    return PCSX2HOOK_CONTINUE;
}

// aDrawBoxF_Direct(x0, y0, x1, y1, r, g, b, a) in raw GS coordinates, XYOFFSET included.
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
    const f32 top = f32(y0) / 16.0f - kOffsetY;
    DrawSprite sprite = DrawSprite::fromRect(left, top, f32(x1 - x0) / 16.0f,
                                             f32(y1 - y0) / 16.0f, 0, 0, 0, 0);
    sprite.setColour(Rgba{u8(r), u8(g), u8(b), u8(a)});
    sprite.alpha = h.state.alpha;
    frame->sprites.push_back(sprite);
    return PCSX2HOOK_CONTINUE;
}

// frCreateDrawFontPacket(X, Y, Width, Height, ClutNo, Col, Pri, Filter, pUv, pGsTex, Ot): one
// glyph. X and Y are already 12.4 relative to the draw space, while the extents are whole pixels.
int frCreateDrawFontPacketHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    const Guest guest(h.runtime);

    const s32 X = s32(c.getArg(0));
    const s32 Y = s32(c.getArg(1));
    const s32 Width = s32(c.getArg(2));
    const s32 Height = s32(c.getArg(3));
    const u32 Col = c.getArg(5);
    const u32 pUv = guest.readU32(c.getStackArgAddr(8));
    const u32 pGsTex = guest.readU32(c.getStackArgAddr(9));

    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return PCSX2HOOK_CONTINUE;

    // frSwapColor reverses the bytes, which lands on the same order a DrawSprite colour uses.
    const Rgba colour = Rgba::fromColorWord(Col);
    const auto uv = [&](usize i) { return f32(s32(guest.readU32(pUv + u32(i) * 4))) / 16.0f; };

    frGsTex_t sheet{};
    guest.read(pGsTex, sheet);
    DrawSprite sprite = DrawSprite::fromRect(f32(X) / 16.0f, f32(Y) / 16.0f, f32(Width),
                                             f32(Height), uv(0), uv(1), uv(2), uv(3));
    sprite.setColour(colour);
    sprite.texHandle = h.textures.getFontSheet(sheet.Width, frame);
    sprite.alpha = h.state.alpha;
    frame->sprites.push_back(sprite);
    return PCSX2HOOK_CONTINUE;
}

// The engine submits these as a strip: top left, top right, bottom left, bottom right. A sprite
// carries its corners as a cycle, so the third and fourth swap places on the way in.
/// sdfPrimLinkFt4(tag, colour, alpha, x0, y0, u0, v0, ... x3, y3, u3, v3, z, allocator).
int sdfPrimLinkFt4Hook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    const Guest guest(h.runtime);
    const auto stack = [&](usize i) { return guest.readU32(c.getStackArgAddr(i)); };

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
    sprite.texHandle = h.textures.getHandle(guest, h.state.texture);
    sprite.alpha = GsAlpha(aPrim);
    frame->sprites.push_back(sprite);
    return PCSX2HOOK_CONTINUE;
}

/// sdfPrimLinkG4(tag, alpha, x0, y0, colour0, ... x3, y3, colour3, z, allocator).
int sdfPrimLinkG4Hook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    const Guest guest(h.runtime);
    const auto stack = [&](usize i) { return guest.readU32(c.getStackArgAddr(i)); };

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
    sprite.alpha = GsAlpha(aPrim);
    frame->sprites.push_back(sprite);
    return PCSX2HOOK_CONTINUE;
}

// fclDrawSpriteCore(X, Y, W, H, pCol, pSprite, FlipFlag, AlphaFlag, FilterFlag, OtNo). X and Y are
// 12.4 relative to the draw space; the extents are pixels, with H in the doubled vertical scale.
int fclDrawSpriteCoreHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    const Guest guest(h.runtime);

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

    fclSprite_t sprite2d{};
    if (!guest.read(pSprite, sprite2d)) return PCSX2HOOK_CONTINUE;

    // Half a texel in, on every edge, the way the engine offsets them.
    const f32 u0 = f32(u16(sprite2d.X) + 8) / 16.0f;
    const f32 u1 = f32(u16(u32(sprite2d.X) >> 16) + 8) / 16.0f;
    const f32 v0 = f32(u16(sprite2d.Y) + 8) / 16.0f;
    const f32 v1 = f32(u16(u32(sprite2d.Y) >> 16) + 8) / 16.0f;

    const f32 left = f32(X) / 16.0f - 0.5f;
    const f32 top = f32(Y) / 16.0f - 0.25f;
    DrawSprite sprite = DrawSprite::fromRect(left, top, f32(W), f32(H) * 0.5f,
                                             (FlipFlag & 1) ? u1 : u0, (FlipFlag & 2) ? v1 : v0,
                                             (FlipFlag & 1) ? u0 : u1, (FlipFlag & 2) ? v0 : v1);
    const u32 colourWord =
        pCol ? guest.readU32(pCol) : guest.readU32(pSprite + offsetof(fclSprite_t, Col));
    sprite.setColour(Rgba::fromColorWord(colourWord));
    sprite.texHandle = h.textures.getHandle(guest, sprite2d.TexHandle.addr);
    sprite.alpha = AlphaFlag == 1   ? GsAlpha::Additive
                   : AlphaFlag == 2 ? GsAlpha::ReverseSubtract
                                    : GsAlpha::Blend;
    frame->sprites.push_back(sprite);
    return PCSX2HOOK_CONTINUE;
}

// fldDrawFrameSprite and fldDrawFrameSpriteAlpha put the frame already presented back over the
// screen, one at a fixed alpha and one at the caller's. The rectangle and UVs are the same fixed
// full-screen pair in both.
void submitFrameFeedback(HookContext& h, u8 alpha) {
    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;

    DrawSprite sprite = DrawSprite::fromRect(-0.5f, -0.3125f, 512.0f, 224.0f, 0.0f, 0.0f,
                                             kGuestWidth, kGuestHeight);
    sprite.setColour(Rgba{0x80, 0x80, 0x80, alpha});
    sprite.texHandle = TexHandle{kPreviousFrameHandle};
    sprite.alpha = GsAlpha::Blend;
    frame->sprites.push_back(sprite);
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

// dds3LinkZClearDrawTag(sw) links a prebuilt tag whose alpha test is NEVER with AFAIL=ZB_ONLY, so
// it writes depth and no colour. There is no depth buffer here yet, which makes it a no-op rather
// than something to draw: rendering its quad would paint the frame black.
int dds3LinkZClearDrawTagHook(pcsx2hook_regs*, void* user) {
    static_cast<HookContext*>(user)->coverage.apiDraws++;
    return PCSX2HOOK_CONTINUE;
}

// dds3DrawFade() takes no arguments: the colour and level are globals, and the quad is the whole
// screen. Alpha counts down from 128, and nothing is drawn once the fade is clear.
int dds3DrawFadeHook(pcsx2hook_regs*, void* user) {
    HookContext& h = *static_cast<HookContext*>(user);
    const Guest guest(h.runtime);

    const u32 fade = pcsx2hook_find_symbol(h.runtime, "fadergba");
    if (!fade) {
        reportUnsupported("fade globals not in the symbol map", 0);
        return PCSX2HOOK_CONTINUE;
    }
    const u8 r = guest.readOr<u8>(fade);
    const u8 g = guest.readOr<u8>(fade + 1);
    const u8 b = guest.readOr<u8>(fade + 2);
    const u8 state = guest.readOr<u8>(fade + 3);
    if (state == 0) return PCSX2HOOK_CONTINUE;

    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return PCSX2HOOK_CONTINUE;

    DrawSprite sprite = DrawSprite::fromRect(0, 0, kGuestWidth, kGuestHeight, 0, 0, 0, 0);
    sprite.setColour(Rgba{r, g, b, u8(128 - state)});
    sprite.alpha = GsAlpha::Blend;
    frame->sprites.push_back(sprite);
    return PCSX2HOOK_CONTINUE;
}

// cmpDrawBoxG(x, y, Z, w, h, pCol, Ctxt, OtNo): a gouraud quad, submitted as a strip. x and y are
// 12.4 relative to the draw space, w is pixels and h is the doubled vertical scale.
int cmpDrawBoxGHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    const Guest guest(h.runtime);

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
            Rgba::fromColorWord(guest.readU32(pCol + u32(i) * 4));
    }
    sprite.alpha = h.state.alpha;
    frame->sprites.push_back(sprite);
    return PCSX2HOOK_CONTINUE;
}

// cmpDrawTriG(pX, pY, Z, pCol, OtNo): three x, three y and three colours, read through pointers.
int cmpDrawTriGHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    const Guest guest(h.runtime);

    const u32 pX = c.getArg(0);
    const u32 pY = c.getArg(1);
    const u32 pCol = c.getArg(3);

    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return PCSX2HOOK_CONTINUE;

    DrawSprite sprite{};
    for (int i = 0; i < 3; i++) {
        const s32 x = s32(guest.readU32(pX + u32(i) * 4));
        const s32 y = s32(guest.readU32(pY + u32(i) * 4));
        const u32 colourWord = guest.readU32(pCol + u32(i) * 4);
        sprite.corner[i] =
            SpriteCorner{f32(x) / 16.0f, f32(y) / 16.0f, 0.0f, 0.0f,
                         Rgba::fromColorWord(colourWord)};
    }
    // A sprite carries four corners, so the last repeats and its triangle collapses.
    sprite.corner[3] = sprite.corner[2];
    sprite.alpha = h.state.alpha;
    frame->sprites.push_back(sprite);
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
    const Guest guest(h.runtime);

    const Context::Args in = h.calls.leave(c);
    const s32 Width = s32(in.a[0]);
    const s32 Height = s32(in.a[1]);
    const u32 pData = in.a[2];

    GsFont_t font{};
    FontMem_t slot{};
    if (!guest.read(c.getResult(), font) || !guest.read(font.font_mem, slot)) return;

    h.textures.loadFontClut(guest, pcsx2hook_find_symbol(h.runtime, "frFontClutTable"));
    h.textures.onGlyph(guest, u32(slot.uv[0]) >> 4, u32(slot.uv[1]) >> 4, u32(Width), u32(Height),
                       pData);
}

/// frCopyFontClutToVram(gsAddr, pData): the sixteen entries a glyph sheet is drawn through.
int frCopyFontClutToVramHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const u32 pData = c.getArg(1);

    h.textures.onFontClut(Guest(h.runtime), pData);
    return PCSX2HOOK_CONTINUE;
}

int sdfModelLinkOtHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    static_cast<HookContext*>(user)->trace->onSdfModelLinkOt(c.getArg(0), c.getArg(1));
    return PCSX2HOOK_CONTINUE;
}

int sdfCorePacketGenHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    static_cast<HookContext*>(user)->trace->onSdfCorePacketGen(c.getArg(0), c.getArg(1), c.getArg(2), c.getArg(3));
    return PCSX2HOOK_CONTINUE;
}

int sdfModelLinkShapedDrawPacketHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    static_cast<HookContext*>(user)->trace->onSdfModelLinkShapedDrawPacket(c.getArg(0), c.getArg(1));
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
    h.textures.add(Guest(h.runtime), h.calls.leave(c).a[0], c.getResult(), h.frames.getFrame());
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
    h.trace->onSdfGrReqDraw(c.getArg(0), c.getArg(1));
    collectPackets(h);
    h.frames.submit();
    return PCSX2HOOK_CONTINUE;
}

int dds3DrawProcessHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    h.trace->onDds3DrawProcess(c.getArg(0));
    h.frames.begin();
    // The clear colour is a global the game may change between frames, so it is read per frame.
    const u32 backColor = pcsx2hook_find_symbol(h.runtime, "dds3BackColor");
    if (backColor) h.state.background = Rgba::fromColorWord(Guest(h.runtime).readU32(backColor));
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
    if (c.getArg(1)) h.frames.link(LinkedPacket{c.getArg(1), c.getRa()});
    return PCSX2HOOK_CONTINUE;
}

// Texture uploads are linked by reference rather than inline, so they arrive through their own
// entry point: the packet is a REF tag pointing at data the game already holds.
int sdfDrawTagLinkRefHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    if (c.getArg(1)) h.frames.link(LinkedPacket{c.getArg(1), c.getRa()});
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
    h.textures.onDuplicate(Guest(h.runtime), h.calls.leave(c).a[0], c.getResult());
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

// SDF's own entry points. These carry over to another game on the same engine.
// SDF's own entry points. These carry over to another game on the same engine.
// SDF and the dds3 kernel above it. These carry over to another game on the same engine.
constexpr Entry kEngineEntries[] = {
    {"sdf3DSetCamera", &sdf3DSetCameraHook, nullptr},
    {"sdfModelLinkOt", &sdfModelLinkOtHook, nullptr},
    {"sdfCorePacketGen", &sdfCorePacketGenHook, nullptr},
    {"sdfModelLinkShapedDrawPacket", &sdfModelLinkShapedDrawPacketHook, nullptr},
    {"sdfSortOtAvlRegist", &sdfSortOtAvlRegistHook, nullptr},
    {"sdfGrReqDraw", &sdfGrReqDrawHook, nullptr},
    {"sdfGrDrawFrame", &sdfGrDrawFrameHook, nullptr},
    {"sdfModelDrawEnvLinkOt", &traceEntryHook, nullptr},
    {"sdfGrOtClose", &traceEntryHook, nullptr},
    {"sdfPrimLinkSprite", &observeHook, nullptr},
    {"sdfPrimLinkRect", &observeHook, nullptr},
    {"sdfPrimLinkG4", &sdfPrimLinkG4Hook, nullptr},
    {"sdfPrimLinkGt3", &observeHook, nullptr},
    {"sdfPrimLinkGt4", &observeHook, nullptr},
    {"sdfPrimLinkLine", &observeHook, nullptr},
    {"sdfPrimLinkSimplePacket", &observeHook, nullptr},
    {"sdfDrawTagLink", &sdfDrawTagLinkHook, nullptr},
    {"sdfDrawTagLinkRef", &sdfDrawTagLinkRefHook, nullptr},
    {"sdfMatSet", &sdfMatSetHook, nullptr},
    {"sdfTexSetEx", &sdfTexSetExHook, &sdfTexSetExPost},
    {"sdfTexDup", &sdfTexDupHook, &sdfTexDupPost},
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
    {"sdfGsAllocFrameBuff", &observeHook, nullptr},
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
    {"sdfBillBoardMakeN", &traceEntryHook, nullptr},
    {"sdfGrLinkDispFrameTex", &traceEntryHook, nullptr},
    {"dds3DrawProcess", &dds3DrawProcessHook, &dds3DrawProcessPost},
};

// This game's own drawing code. SDF publishes a semantic 2D API that Nocturne barely uses,
// so the producers that carry the meaning are here rather than in the engine, and are per
// disc.
constexpr Entry kGameEntries[] = {
    {"frTranslateUserData", &frTranslateUserDataHook, &frTranslateUserDataPost},
    {"frCopyFontClutToVram", &frCopyFontClutToVramHook, nullptr},
    {"DrwSprite2D", &spriteHook, nullptr},
    {"rsetAlpha", &rsetAlphaHook, nullptr},
    {"rsetTest", &rsetTestHook, nullptr},
    {"rsetTexa", &traceEntryHook, nullptr},
    {"rsetOtOfs", &traceEntryHook, nullptr},
    {"frCreateDrawFontPacket_20550848", &frCreateDrawFontPacketHook, nullptr},
    {"aDrawBoxG", &aDrawBoxGHook, nullptr},
    {"fclDrawSpriteCore", &fclDrawSpriteCoreHook, nullptr},
    {"cmpDrawTriG", &cmpDrawTriGHook, nullptr},
    {"cmpDrawBoxG", &cmpDrawBoxGHook, nullptr},
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
    {"cmpDrawBoxG", &traceEntryHook, nullptr},
    {"cmpDrawSprite", &traceEntryHook, nullptr},
    {"fclDrawSpriteCore", &traceEntryHook, nullptr},
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
    if (!mBuilding && mRenderer) mBuilding = mRenderer->beginFrame();
    return mBuilding;
}

void FrameBuilder::submit() {
    mSubmitted = true;
    mSubmittedFrames++;
    if (!mBuilding || !mRenderer) return;
    mRenderer->submit(mBuilding);
    mBuilding = nullptr;
}

void FrameBuilder::end() {
    if (!mSubmitted && (mBuilding || !mPending.empty())) mDiscardedFrames++;
    mPending.clear();
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
    context.frames.attach(render);

    usize installed = 0;
    usize unnamed = 0;
    const auto install = [&](const Entry& entry) {
        const u32 address = pcsx2hook_find_symbol(runtime, entry.name);
        if (!address) {
            std::fprintf(stderr, "[sdf] %s: not in the symbol map\n", entry.name);
            unnamed++;
            return;
        }
        if (!pcsx2hook_add_hook(runtime, address, entry.pre, entry.post, &context)) {
            std::fprintf(stderr, "[sdf] %s at 0x%08X: %s\n", entry.name, address,
                         pcsx2hook_get_last_error(runtime));
            return;
        }
        installed++;
    };
    for (const Entry& entry : kEngineEntries) install(entry);
    const usize engineHooks = installed;
    for (const Entry& entry : kGameEntries) install(entry);

    const char* serial = pcsx2hook_get_disc_serial(runtime);
    std::fprintf(stderr, "[sdf] %s: hooked %zu engine and %zu game entry points of %zu%s\n",
                 serial && *serial ? serial : "?", engineHooks, installed - engineHooks,
                 std::size(kEngineEntries) + std::size(kGameEntries),
                 unnamed ? " (some unnamed: is the symbol map for this disc?)" : "");
}

}  // namespace sdfhook
