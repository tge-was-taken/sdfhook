#include <set>
#include "hooks.h"

#include <bit>
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
#include "libsdf_impl.h"
#include "sink.h"
#include "render/render_thread.h"
#include "trace.h"

namespace sdfhook {


namespace {

LogChannel log("hooks");

/// Points the sink at this call's frame and names the producer, for the length of one hook.
struct SinkScope {
    explicit SinkScope(HookContext& aHost, const char* aProducer) {
        gSink.frame = aHost.frames.getFrame();
        gSink.textures = &aHost.textures;
        gSink.producer = aProducer;
        gSink.apiDraws = &aHost.coverage.apiDraws;
        gSink.unresolvedTextures = &aHost.coverage.unresolvedTextures;
    }

    ~SinkScope() {
        gSink.frame = nullptr;
        gSink.producer = "";
    }

    SinkScope(const SinkScope&) = delete;
    SinkScope& operator=(const SinkScope&) = delete;
};

// The chain is finished here, so this is where a frame is done being built. sdfGrOtClose walks the
// 85 tables and concatenates their tag lists; that order is the frame's.
/// @fn sdfGrOtClose
/// @note SLUS-20911 0x002AD010
void closeShadowQueue(HookContext& h) {
    Frame* frame = h.frames.getBuilding();
    std::vector<xsdfDrawTag_t*> tags;
    xsdfGrOtClose(tags);
    if (!frame) return;
    frame->order.clear();
    frame->order.reserve(tags.size());
    for (const xsdfDrawTag_t* tag : tags) {
        frame->order.push_back({tag->kind == xsdfDrawTag_t::Kind::Mesh, tag->draw});
    }
}



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


/// Replays the frame's GS state in transmit order, the only order that settles what a draw ran
/// under: the tables sort their tags first, so link order says nothing.
/// @disc SLUS-20911
/// @address 0x002AD980
/// @function sdfGrDrawFrame
std::unordered_map<u32, sdfGsState_t> replayTransmitOrder(u32 aTag) {
    std::unordered_map<u32, sdfGsState_t> settled;
    sdfGsState_t gs{};
    u32 stack[16] = {};
    u32 depth = 0;
    u32 at = u32Ptr_t{aTag + 4}.valueOr() & 0x0FFFFFFFu;

    // A frame links a few thousand tags; beyond that the chain loops or was read out of data.
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

/// Gives every mesh the ALPHA and TEST its packet transmitted under. Meshes are built in the same
/// walk the engine links in, so the nth geometry link is the nth mesh's.
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
    sdfGsState_t& gs = gModel.gsState;

    for (const LinkedPacket& packet : h.frames.getPackets()) {
        const GifCounts before = h.coverage.gif;
        // A draw runs under whatever the packets before it left behind, so it is tracked across
        // the chain rather than looked up per draw.
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
        for (const u32 tbp : parser.getParsedTbps()) h.coverage.tbpByProducer[by][tbp]++;
        // Shadows arrive as an inline VIF stream, not a REF chain, so the type 1 path misses them.
        if (h.config->drawShadows && !verts && !stateOnly && std::strstr(by, "fldSdwDraw")) {
            const u32 qwc = tag.qwc;
            if (readShadowStream(packet.addr + 16, packet.addr + 16 + 16 * qwc, gs,
                                   h.frames.getFrame())) {
                h.coverage.shadowMeshes++;
            } else {
                h.coverage.shadowRejects++;
            }
        }
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
void submitSprite(HookContext& h, const char* producer, f32 x, f32 y, f32 w, f32 height, f32 u,
                  f32 v, f32 uw, f32 vh, Rgba color, u32 texFileHandle) {
    SinkScope scope(h, producer);
    xDrwSprite2D(x, y, w, height, u, v, uw, vh, color, texFileHandle);
}

// (x, y, w, h, u, v, uw, vh, rgba, texHandle), the last two in stack slots. nbPnlSprite has the
// same shape and differs only in the ordering table.
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

int sdf2DLinkTexPacketHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    const u32 texFileHandle = c.getArg(1);
    gState.texture = texFileHandle;
    return PCSX2HOOK_CONTINUE;
}

/// Installs the registrar on all 85 tables. The guest runs this once at boot, which the
/// savestates are past, so ours installs at attach as well.
int sdfGrOtInitHook(pcsx2hook_regs*, void*) {
    xsdfGrOtInit();
    return PCSX2HOOK_CONTINUE;
}

int sdfGrOtCloseHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    h.trace->onPrimEntry(h.symbols.getFunction(c.getPc()), c.getArg(0));
    h.coverage.otCloseCalls++;
    return PCSX2HOOK_CONTINUE;
}

/// The two registrars a table can carry: FIFO, and the AVL sort sdfModelSetOtCallbacks installs.
/// Reached only through regFunc, so seeing one called is how our table learns which it is.
int sdfGrOtNormalRegHook(pcsx2hook_regs* regs, void*) {
    Context c{regs};

    const u32 index = xsdfGrOtIndexOf(c.getArg(0));
    if (xsdfGrOt_t* ot = xsdfGrOtGet(index)) ot->regFunc = &xsdfGrOtNormalReg;
    return PCSX2HOOK_CONTINUE;
}

int sdfGrSortOtRegHook(pcsx2hook_regs* regs, void*) {
    Context c{regs};

    const u32 index = xsdfGrOtIndexOf(c.getArg(0));
    if (xsdfGrOt_t* ot = xsdfGrOtGet(index)) ot->regFunc = &xsdfGrSortOtReg;
    return PCSX2HOOK_CONTINUE;
}

int rsetAlphaHook(pcsx2hook_regs* regs, void*) {
    Context c{regs};

    const u32 aAlpha = c.getArg(0);
    // No SinkScope: a state packet adds no draw, and getFrame() would acquire one.
    xrsetAlpha(aAlpha);
    return PCSX2HOOK_CONTINUE;
}

// rsetOtOfs(otofs): the table the 2D draws after it go into, which is how SDF orders them.
// There is no per-sprite depth to test.
int rsetOtOfsHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};

    const s32 otofs = s32(c.getArg(0));

    xrsetOtOfs(u32(otofs));
    return PCSX2HOOK_CONTINUE;
}

/// rsetFrameMaskRgbOn/Off, still ooyTestRGBMaskOn/Off in the map. They bracket the silhouette
/// pass in call order, where our walk runs, so the flag needs no transmit-order settling.
/// @fn rsetFrameMaskRgbOn
/// @note SLUS-20911 0x0012A3F0
int frameMaskOnHook(pcsx2hook_regs*, void*) {
    // No SinkScope: a state packet adds no draw, and getFrame() would acquire one.
    xrsetFrameMaskRgbOn();
    return PCSX2HOOK_CONTINUE;
}

int frameMaskOffHook(pcsx2hook_regs*, void*) {
    xrsetFrameMaskRgbOff();
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

    xrsetTest(ate, atst, aref, afail, date, datm, ztst);
    return PCSX2HOOK_CONTINUE;
}



// aDrawBoxG(x, y, w, h, four corner colors): a trifan quad, in the draw space's own vertical
// scale rather than the doubled one the sprite calls use.
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

    const u32 rgba[16] = {r0, g0, b0, a0, r1, g1, b1, a1, r2, g2, b2, a2, r3, g3, b3, a3};

    SinkScope scope(h, "aDrawBoxG");
    xaDrawBoxG(x, y, w, height, rgba);
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

    SinkScope scope(h, "aDrawBoxF");
    xaDrawBoxF(x, y, w, height, r, g, b, a);
    return PCSX2HOOK_CONTINUE;
}

// aDrawBoxF_Direct(x0, y0, x1, y1, r, g, b, a) in raw GS coordinates, XYOFFSET included. Not a
// filled box: PRIM 0x41 is a LINE, so only the top and bottom edges draw.
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

    if (!h.config->drawDirectBox) return PCSX2HOOK_CONTINUE;

    SinkScope scope(h, "aDrawBoxF_Direct");
    xaDrawBoxF_Direct(x0, y0, x1, y1, r, g, b, a);
    return PCSX2HOOK_CONTINUE;
}

// frCreateDrawFontPacket(...): one glyph. X and Y are 12.4 in the draw space, extents whole
// pixels.
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

    f32 uv[4] = {};
    for (int i = 0; i < 4; i++) uv[i] = f32Ptr_t{pUv + u32(i) * 4}.valueOr();
    frGsTex_PTR_t gsTex{pGsTex};
    if (!gsTex) return PCSX2HOOK_CONTINUE;
    frGsTex_t tex = *gsTex;

    SinkScope scope(h, "frCreateDrawFontPacket");
    xfrCreateDrawFontPacket(X, Y, Width, Height, Col, uv, &tex);
    return PCSX2HOOK_CONTINUE;
}

// fclDrawSpriteCore(X, Y, W, H, pCol, pSprite, FlipFlag, AlphaFlag, FilterFlag, OtNo).
int fclDrawSpriteCoreHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const s32 X = s32(c.getArg(0));
    const s32 Y = s32(c.getArg(1));
    const s32 W = s32(c.getArg(2));
    const s32 H = s32(c.getArg(3));
    const u32 pCol = c.getArg(4);
    const fclSprite_PTR_t pSprite{c.getArg(5)};
    const u32 FlipFlag = c.getArg(6);
    const u32 AlphaFlag = c.getArg(7);

    SinkScope scope(h, "fclDrawSpriteCore");
    xfclDrawSpriteCore(X, Y, W, H, pCol, pSprite, FlipFlag, AlphaFlag);
    return PCSX2HOOK_CONTINUE;
}

// The previous frame, drawn back over this one. Sampling it is what the feedback pass is for.
int fldDrawFrameSpriteHook(pcsx2hook_regs*, void* user) {
    HookContext& h = *static_cast<HookContext*>(user);

    SinkScope scope(h, "fldDrawFrameSprite");
    xfldDrawFrameSprite(0x80);
    return PCSX2HOOK_CONTINUE;
}

int fldDrawFrameSpriteAlphaHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const u32 Alpha = c.getArg(0);

    SinkScope scope(h, "fldDrawFrameSpriteAlpha");
    xfldDrawFrameSprite(u8(Alpha));
    return PCSX2HOOK_CONTINUE;
}

// dds3DrawFade(R, G, B, State): a full screen quad at the fade's own alpha.
int dds3DrawFadeHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const u32 R = c.getArg(0);
    const u32 G = c.getArg(1);
    const u32 B = c.getArg(2);
    const u32 State = c.getArg(3);

    SinkScope scope(h, "dds3DrawFade");
    xdds3DrawFade(u8(R), u8(G), u8(B), u8(State));
    return PCSX2HOOK_CONTINUE;
}

/// Watched only: the z clear is a GS state the renderer does itself.
int dds3LinkZClearDrawTagHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    h.coverage.graph.record(h.symbols.getFunction(c.getPc()), h.symbols.getSite(c.getRa()));
    return PCSX2HOOK_CONTINUE;
}

// sdfPrimLinkFt4(aColor, aPrim, aVertex, aAllocator): argument 2 is a GS PRIM word, not an ALPHA.
int sdfPrimLinkFt4Hook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const u32 aColor = c.getArg(0);
    const u32 aPrim = c.getArg(1);
    const u32 pVertex = c.getArg(2);

    PrimVertex vertex[4] = {};
    for (int i = 0; i < 4; i++) {
        const guest_ptr<PrimVertex> at{pVertex + u32(i) * u32(sizeof(PrimVertex))};
        if (at) vertex[i] = *at;
    }

    SinkScope scope(h, "sdfPrimLinkFt4");
    xsdfPrimLinkFt4(aColor, aPrim, vertex, nullptr);
    return PCSX2HOOK_CONTINUE;
}

// sdfPrimLinkG4(aPrim, aVertex, aAllocator): argument 1 is the PRIM word here.
int sdfPrimLinkG4Hook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const u32 aPrim = c.getArg(0);
    const u32 pVertex = c.getArg(1);

    PrimVertex vertex[4] = {};
    for (int i = 0; i < 4; i++) {
        const guest_ptr<PrimVertex> at{pVertex + u32(i) * u32(sizeof(PrimVertex))};
        if (at) vertex[i] = *at;
    }

    SinkScope scope(h, "sdfPrimLinkG4");
    xsdfPrimLinkG4(aPrim, vertex, nullptr);
    return PCSX2HOOK_CONTINUE;
}

// cmpDrawBoxG(X, Y, Z, W, H, pCol, OtNo): four corner colors read through a pointer.
int cmpDrawBoxGHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const s32 x = s32(c.getArg(0));
    const s32 y = s32(c.getArg(1));
    const s32 w = s32(c.getArg(3));
    const s32 height = s32(c.getArg(4));
    const u32 pCol = c.getArg(5);

    SinkScope scope(h, "cmpDrawBoxG");
    xcmpDrawBoxG(x, y, w, height, pCol);
    return PCSX2HOOK_CONTINUE;
}

// cmpDrawTriG(pX, pY, Z, pCol, OtNo): three x, three y and three colors, read through pointers.
int cmpDrawTriGHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const u32 pX = c.getArg(0);
    const u32 pY = c.getArg(1);
    const u32 pCol = c.getArg(3);

    SinkScope scope(h, "cmpDrawTriG");
    xcmpDrawTriG(pX, pY, pCol);
    return PCSX2HOOK_CONTINUE;
}

/// frTranslateUserData(...) uploads one glyph. Its slot is only known on the way out, through
/// the GsFont_t it returns.
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

    xfrCopyFontClutToVram(pData);
    return PCSX2HOOK_CONTINUE;
}

// sdfModelDrawEnvLinkOt(aOt, aEnv): one env set per table, striding 96 bytes, holding that
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

// Ours runs at entry over the node list the engine is about to walk, so the two see identical
// state and the packet counts must agree.
int sdfModelLinkOtHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const sdfModelOtPtr_t aOt{c.getArg(0)};
    const sdfModelRootNodePtr_t aRoot{c.getArg(1)};

    h.trace->onSdfModelLinkOt(aOt.addr, aRoot.addr);
    xsdfModelLinkOt(aOt, aRoot, h.frames.getFrame());
    return PCSX2HOOK_CONTINUE;
}

int sdfCorePacketGenHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    h.trace->onSdfCorePacketGen(c.getArg(0), c.getArg(1), c.getArg(2), c.getArg(3));
    // 0x2B91A8/0x2B91B0: the face-division terms arrive here as $f12 and $f13.
    xcorePktSetFaceDivParm(regs->fpr[12], regs->fpr[13]);
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

// sdfTexSetEx(asset, memMode) returns the handle, so the post-hook has the asset still in main
// memory and the description of what is in it.
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
    const sdfTexFilePtr_t aTex{h.calls.leave(c).a[0]};
    const sdfTexHandlePtr_t handle{c.getResult()};
    xsdfTexSetEx(aTex, handle, h.frames.getFrame());
}

/// The ramp reaches here on the caller's stack, the only point where its endpoints exist.
int corePktApplyFaceDivColorHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    const corePktCbPtr_t cb{c.getArg(0)};
    const sdfVector4Ptr_t aParm{c.getArg(1)};
    xcorePktApplyFaceDivColor(cb, aParm);
    return PCSX2HOOK_CONTINUE;
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
    // sdfGrOtClose is not reached in a savestate run, so the queue closes here, where the chain is
    // handed to the DMA. The order it yields is the frame's.
    closeShadowQueue(h);
    xsdfGrOtReset();
    h.frames.submit();
    return PCSX2HOOK_CONTINUE;
}

int dds3DrawProcessHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    h.trace->onDds3DrawProcess(c.getArg(0));
    h.frames.begin();
    h.model.beginFrame(h.trace->getFrame());
    // The clear color is a global the game may change between frames, so it is read per frame.
    const u32 backColor = pcsx2hook_find_symbol(h.runtime, "dds3BackColor");
    if (backColor) gState.background = Rgba::fromColorWord(u32Ptr_t{backColor}.valueOr());
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

// Texture uploads link by reference, so the packet is a REF tag at data the game already holds.
int sdfDrawTagLinkRefHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    if (c.getArg(1)) {
        h.frames.link(LinkedPacket{.addr = c.getArg(1), .caller = c.getRa(),
                                   .kind = LinkKind::Ref});
    }
    return PCSX2HOOK_CONTINUE;
}

// World geometry links as a DMA call chain, not a packet, so it arrives here and nowhere else:
// the type 1 streams are three quarters of a field scene's triangles.
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
    xsdfGsAllocFrameBuff(sdfGsMemSlicePtr_t{c.getResult()});
}

// The one release point: sdfTexFreeDelayed only queues this, and every game-side caller lands
// here.
int sdfTexFreeHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const sdfTexHandlePtr_t aHandle{c.getArg(0)};
    xsdfTexFree(aHandle);
    return PCSX2HOOK_CONTINUE;
}

// rsetFrameMaskRgbOn links the FBMSK packet here, so without it the write mask reaches no draw.
int sdfDrawTagLinkRefGifHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    if (c.getArg(1)) {
        h.frames.link(LinkedPacket{.addr = c.getArg(1), .caller = c.getRa(),
                                   .kind = LinkKind::Ref});
    }
    return PCSX2HOOK_CONTINUE;
}

/// dds3BillboardTagLink(hBillboard, hResource, use_mesh_idx): only the resource names the texture,
/// and sdfBillBoardMakeN is handed the geometry alone, so the handle is taken here.
/// @fn dds3BillboardTagLink
/// @note SLUS-20911 0x0015B320
int dds3BillboardTagLinkHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const dds3BillResourcePtr_t hResource{c.getArg(1)};
    if (hResource) gModel.billboardTexture = hResource->hTex;
    return PCSX2HOOK_CONTINUE;
}

/// dds3BillboardLinkOt(): each pending batch into the table its node names. Read at entry, since
/// the engine's loop rewrites the list as it consumes it.
/// @fn dds3BillboardLinkOt
/// @note SLUS-20911 0x0022E2E8
int dds3BillboardLinkOtHook(pcsx2hook_regs* regs, void* user) {
    (void)regs;
    static_cast<HookContext*>(user)->model.readBillboardTables();
    return PCSX2HOOK_CONTINUE;
}

/// sdfBillBoardMakeN(aCenter, aColor, aUVList, aXYOffsetList, aNum, aAllocator): one batch of
/// camera-facing quads. The four pointers are one allocation, so it is taken as the struct it is.
/// @fn sdfBillBoardMakeN
/// @note SLUS-20911 0x002BB0F0
int sdfBillBoardMakeNHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const sdfBillBoardBatchPtr_t aCenter{c.getArg(0)};
    const u32 aNum = c.getArg(4);

    h.coverage.apiDraws++;
    if (h.config->noBillboards) return PCSX2HOOK_CONTINUE;
    xsdfBillBoardMakeN(aCenter, aNum, h.frames.getFrame());
    return PCSX2HOOK_CONTINUE;
}

/// drawSprite3D(aTexHandle, aSprite): the level rather than sdfBillBoardMake1, whose rotation
/// only reaches the transform packet through VU0.
/// @fn drawSprite3D
/// @note SLUS-20911 0x0012A2D0
int drawSprite3DHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    const TexHandle aTexHandle{c.getArg(0)};
    const sdfSprite3DPtr_t aSprite{c.getArg(1)};

    h.coverage.apiDraws++;
    if (h.config->noBillboards) return PCSX2HOOK_CONTINUE;
    xdrawSprite3D(aTexHandle, aSprite, h.frames.getFrame());
    return PCSX2HOOK_CONTINUE;
}

/// sdf3DPrimMakePoly(aPoly): counts, a PRIM word and one array per attribute. Every sdf3DPrim
/// producer builds through it.
/// @fn sdf3DPrimMakePoly
/// @note SLUS-20911 0x002BAAB8
int sdf3DPrimMakePolyHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    const sdf3DPolyPtr_t aPoly{c.getArg(0)};
    h.coverage.apiDraws++;
    if (!aPoly) return PCSX2HOOK_CONTINUE;

    if (!h.config->drawPrimPoly) return PCSX2HOOK_CONTINUE;
    xsdf3DPrimMakePoly(aPoly, h.frames.getFrame());
    return PCSX2HOOK_CONTINUE;
}

/// fldDrawPoly3(...): one gouraud triangle. The largest unhandled producer by packets, and
/// invisible to a vertex ranking since its geometry only exists after VU1 runs.
/// @fn fldDrawPoly3
/// @note SLUS-20911 0x0012A5E0
int fldDrawPoly3Hook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);

    // Eight floats reach the FPRs and the ninth the stack, while the colors take a0 to a2.
    const sdfVector3_t point[3] = {
        {c.getFloatArg(0), c.getFloatArg(1), c.getFloatArg(2)},
        {c.getFloatArg(3), c.getFloatArg(4), c.getFloatArg(5)},
        {c.getFloatArg(6), c.getFloatArg(7), f32Ptr_t{c.getStackArgAddr(8)}.valueOr()},
    };
    const u32 color[3] = {c.getArg(0), c.getArg(1), c.getArg(2)};

    h.coverage.apiDraws++;
    if (!h.config->poly) return PCSX2HOOK_CONTINUE;
    xfldDrawPoly3(point, color, h.frames.getFrame());
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
    const sdfTexHandlePtr_t aSource{h.calls.leave(c).a[0]};
    const sdfTexHandlePtr_t copy{c.getResult()};
    xsdfTexDup(aSource, copy);
}

int observeHook(pcsx2hook_regs* regs, void* user) {
    Context c{regs};
    HookContext& h = *static_cast<HookContext*>(user);
    h.coverage.graph.record(h.symbols.getFunction(c.getPc()), h.symbols.getSite(c.getRa()));
    return PCSX2HOOK_CONTINUE;
}

// Only for a function nothing tail-calls: a post-handler replaces $ra with the return sentinel,
// which a tail call passes on as a bogus caller.
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
    {"corePktApplyFaceDivColor", &corePktApplyFaceDivColorHook, nullptr},
    // The ramp's caller and its sibling, to say whether the second pass recolors at
    // all: corePktApplyFaceDivColor never fires, and this is what tells the two reasons apart.
    {"corePktEmitMultiTexPass", &observeHook, nullptr},
    {"corePktSetConstColor", &observeHook, nullptr},
    {"sdfModelLinkShapedDrawPacket", &sdfModelLinkShapedDrawPacketHook, nullptr},
    {"sdfSortOtAvlRegist", &sdfSortOtAvlRegistHook, nullptr},
    {"sdfGrReqDraw", &sdfGrReqDrawHook, nullptr},
    {"sdfGrDrawFrame", &sdfGrDrawFrameHook, nullptr},
    {"sdfModelDrawEnvLinkOt", &sdfModelDrawEnvLinkOtHook, nullptr},
    // Watched only, to find out whether any sample scene reaches a render target or a movie.
    {"dds3AllocRenderTexture", &observeHook, nullptr},
    {"sdfIpuMakeToIpuDmaTag", &observeHook, nullptr},
    {"sdfGrOtInit", &sdfGrOtInitHook, nullptr},
    {"sdfGrOtClose", &sdfGrOtCloseHook, nullptr},
    {"sdfGrOtNormalReg", &sdfGrOtNormalRegHook, nullptr},
    {"sdfGrSortOtReg", &sdfGrSortOtRegHook, nullptr},
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
    {"fldDrawPoly3", &fldDrawPoly3Hook, nullptr},
    {"rsetFrameMaskRgbOn", &frameMaskOnHook, nullptr},
    {"rsetFrameMaskRgbOff", &frameMaskOffHook, nullptr},
    {"rsetOtOfs", &rsetOtOfsHook, nullptr},
    {"rsetTexa", &traceEntryHook, nullptr},
    {"frCreateDrawFontPacket_20550848", &frCreateDrawFontPacketHook, nullptr},
    {"aDrawBoxG", &aDrawBoxGHook, nullptr},
    {"fclDrawSpriteCore", &fclDrawSpriteCoreHook, nullptr},
    {"cmpDrawTriG", &cmpDrawTriGHook, nullptr},
    // Traced, not drawn: its full-screen gouraud wash and row boxes are real draws, but drawing
    // them costs 6.2 points on the menu. See TODO M2.
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
    {"fclDrawSpriteAngleCore", &traceEntryHook, nullptr},
    {"itfMiscDrawSpritePrim", &traceEntryHook, nullptr},
    {"drawSprite3D", &drawSprite3DHook, nullptr},
    {"sdf3DPrimMakePoly", &sdf3DPrimMakePolyHook, nullptr},
    // The effect build of the same function, reading the same descriptor field for field.
    {"effPkt3DPrimMakePoly", &sdf3DPrimMakePolyHook, nullptr},
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
    log.debug("submit sprites={} meshes={} order={}", mBuilding ? mBuilding->sprites.size() : 0,
              mBuilding ? mBuilding->meshes.size() : 0, mBuilding ? mBuilding->order.size() : 0);
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
    context.model.attach(runtime, *context.config, &context.textures, &trace);

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
