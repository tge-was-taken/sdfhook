#include "hooks_impl.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "chain_census.h"
#include "chain_frame.h"
#include "gif.h"
#include "guest_ptr.h"
#include "json.h"
#include "libsdf.h"
#include "log.h"
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
        .addUint("dmaId", sdfDmaId(tag))
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

namespace {

/// Every draw of the frame by the packet that carries it. A generated mesh is looked up through
/// the link its generation was paired with.
std::unordered_map<u32, std::vector<DrawRef>> drawsByPacket(HookContext& h, const Frame& frame) {
    std::unordered_map<u32, std::vector<DrawRef>> byPacket;
    for (u32 i = 0; i < frame.sprites.size(); i++) {
        const DrawSprite& sprite = frame.sprites[i];
        if (!sprite.linkAddr) {
            h.coverage.unlinkedDraws[sprite.producer]++;
            continue;
        }
        byPacket[sprite.linkAddr].push_back({DrawRef::Kind::Sprite, i});
    }
    const std::vector<u32>& generated = h.frames.getGeneratedLinks();
    for (u32 i = 0; i < frame.meshes.size(); i++) {
        const DrawMesh& mesh = frame.meshes[i];
        u32 packet = mesh.linkAddr;
        if (!packet && mesh.linkIndex < generated.size()) packet = generated[mesh.linkIndex];
        if (!packet) {
            h.coverage.unlinkedDraws[mesh.producer]++;
            continue;
        }
        byPacket[packet].push_back({DrawRef::Kind::Mesh, i});
    }
    return byPacket;
}

const char* producerOf(const Frame& frame, DrawRef ref) {
    return ref.kind == DrawRef::Kind::Sprite ? frame.sprites[ref.index].producer
                                             : frame.meshes[ref.index].producer;
}

}  // namespace

/// Walks the frame's DMA chain the way channel 1 transmits it, replaying the GS state and putting
/// each packet's draws into the frame in that order. The tables sort their tags before this, so
/// link order says nothing about draw order.
/// @disc SLUS-20911
/// @address 0x002AD980
/// @function sdfGrDrawFrame
void placeDraws(HookContext& h, u32 aTag) {
    Frame* frame = h.frames.getFrame();
    if (!frame) return;
    std::unordered_map<u32, std::vector<DrawRef>> byPacket = drawsByPacket(h, *frame);

    // A REF names its data elsewhere, and a model stream is found by that data's address.
    const auto place = [&](u32 packet, const sdfGsState_t& gs) {
        const auto found = byPacket.find(packet);
        if (found == byPacket.end()) return;
        for (const DrawRef ref : found->second) {
            frame->order.push_back(ref);
            h.coverage.placedDraws[producerOf(*frame, ref)]++;
            if (ref.kind != DrawRef::Kind::Mesh || h.config->noTransmitState) continue;
            DrawMesh& mesh = frame->meshes[ref.index];
            const u64 mask = mesh.gs.frameMask;
            mesh.gs = gs;
            mesh.gs.frameMask = mask;
        }
        byPacket.erase(found);
    };

    sdfGsState_t gs{};
    u32 stack[16] = {};
    u32 depth = 0;
    const sdfDrawTagPtr_t drawTag{aTag};
    u32 at = drawTag ? drawTag->head.addr & 0x0FFFFFFFu : 0;

    // A frame links a few thousand tags; beyond that the chain loops or was read out of data.
    constexpr u32 kMaxChainTags = 65536;
    u32 steps = 0;
    for (; at && steps < kMaxChainTags; steps++) {
        const sdfDmaTagPtr_t tag{at};
        if (!tag) break;
        const u32 qwc = tag->qwc;
        const u32 id = sdfDmaId(*tag);
        const u32 addr = tag->addr & 0x0FFFFFFFu;

        applyGsRegisterWrites(at, gs);
        place(at, gs);
        if (sdfDmaIsRef(*tag)) place(addr, gs);

        switch (id) {
            case sdfDmaTagRef:
            case sdfDmaTagRefs: at += sdfQwordBytes; break;
            case sdfDmaTagCnt: at += sdfQwordBytes * (1 + qwc); break;
            case sdfDmaTagNext: at = addr; break;
            case sdfDmaTagCall:
                if (depth == std::size(stack)) {
                    reportUnsupported("transmit chain call depth", depth);
                    at = 0;
                    break;
                }
                stack[depth++] = at + sdfQwordBytes * (1 + qwc);
                at = addr;
                break;
            case sdfDmaTagRet: at = depth ? stack[--depth] : 0; break;
            default: at = 0; break;  // REFE and END both stop the chain
        }
    }
    if (steps == kMaxChainTags) reportUnsupported("transmit chain tags", steps);

    for (const auto& [packet, refs] : byPacket) {
        for (const DrawRef ref : refs) h.coverage.unreachedDraws[producerOf(*frame, ref)]++;
    }
    h.model.traceMeshes(*frame);
}

void recordBuiltPacket(HookContext& h, const char* aBuilder, const u32 (&aArgs)[4]) {
    const PacketBuilder made = packetBuilderFor(aBuilder);
    h.builderCalls[aBuilder]++;
    if (!made.takesPacket) return;
    const u32 packet = aArgs[made.argument];
    if (!packet) return;
    h.packetTypes[packet] = made.type;
    if (made.trailing != PacketType::Unknown) h.packetTypes[packet + sdfQwordBytes] = made.trailing;
}

void recordReturnedPacket(HookContext& h, const char* aBuilder, u32 aPacket) {
    const PacketBuilder made = packetBuilderFor(aBuilder);
    if (made.returnsPacket && aPacket) h.packetTypes[aPacket] = made.type;
}

void censusFrame(HookContext& h, u32 aTag) {
    // Packet buffers are reused every other frame, so a type is only good for the frame that
    // recorded it.
    struct Reset {
        std::unordered_map<u32, PacketType>& types;
        ~Reset() { types.clear(); }
    } reset{h.packetTypes};
    if (!h.config->dumpChainCensus) return;

    for (const char* global : {"sdf3DPrimIdentityTm", "vu1MicroPacket"}) {
        const u32 at = pcsx2hook_find_symbol(h.runtime, global);
        if (at) h.packetTypes[at] = packetTypeForGlobal(global);
    }
    // sdfCorePacketGen builds these, and the frame builder already pairs each with its link.
    for (const u32 link : h.frames.getGeneratedLinks()) {
        if (link) h.packetTypes[link] = PacketType::MeshGenerated;
    }
    censusChain(aTag, h.frames.getPackets(), h.symbols, h.trace->getFrame(), h.packetTypes);
    for (const auto& [builder, calls] : h.builderCalls) {
        log.dump(JsonObject()
                     .addString("dump", "builder")
                     .addString("name", builder)
                     .addUint("calls", calls)
                     .take());
    }
}

void chainFrameSend(HookContext& h, u32 aRa, u32 aAddr) {
    if (!h.config->chainFrame) return;
    // sceDmaSend has three call sites: sdfImageSetTrans, and sdfGrDrawFrame's own send and its
    // resume after a GS readback. Only the first of those carries the frame.
    static const char* const kFrameSend = "sdfGrDrawFrame+0x12C";
    const char* site = h.symbols.getSite(aRa);
    if (std::strcmp(site, kFrameSend) != 0) {
        log.debug("chain send from {}, not the frame send", site);
        return;
    }
    // The engine draws one chain twice, at mode 0 and mode 1, so the second send repeats it.
    if (aAddr == h.lastChainHead) return;
    h.lastChainHead = aAddr;

    // sdfGrDrawFrame runs after dds3DrawProcessPost has closed the API path's frame, so the walk
    // builds its own rather than borrowing one. Frame boundaries belong to the chain here.
    Frame chain;
    const ChainFrameCounts counts = buildFrameFromChain(aAddr, h, &chain);
    log.info("chain frame: {} entries, {} sprites, {} regs, {} opaque, {} dropped; api {} sprites",
             counts.entries, counts.sprites, counts.registerWrites, counts.opaque,
             counts.verticesDropped, h.coverage.apiDraws - h.lastApiDraws);
    log.info("chain frame: x {:.1f}..{:.1f} y {:.1f}..{:.1f}", counts.minX, counts.maxX,
             counts.minY, counts.maxY);
    h.lastApiDraws = h.coverage.apiDraws;
}

void collectPackets(HookContext& h) {
    GifParser parser(&h.coverage.gif);
    sdfGsState_t& gs = h.model.getGsState();

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

        const sdfDmaTag_t tag = sdfDmaTagPtr_t{packet.addr}.valueOr();
        u64 verts = 0;
        for (int i = 0; i < 8; i++) verts += h.coverage.gif.verts[i] - before.verts[i];
        const char* by = h.symbols.getFunction(packet.caller);
        for (const u32 tbp : parser.getParsedTbps()) h.coverage.tbpByProducer[by][tbp]++;
        // Shadows arrive as an inline VIF stream, not a REF chain, so the type 1 path misses them.
        if (h.config->drawShadows && !verts && !stateOnly && std::strstr(by, "fldSdwDraw")) {
            const u32 qwc = tag.qwc;
            if (h.modelHooks.fldSdwDraw(packet.addr + 16, packet.addr + 16 + 16 * qwc, gs,
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
void submitSprite(HookContext& h, const char* producer, f32 x, f32 y, f32 w, f32 height, f32 u, f32 v,
                f32 uw, f32 vh, Rgba color, u32 texFileHandle) {
    const ProducerDraws draws(h.frames);
    Frame* frame = h.frames.getFrame();
    // A producer naming no texture draws with the one sdf2DLinkTexPacket bound ahead of it.
    const TexHandle named{texFileHandle ? texFileHandle : h.state.texture};
    const TexHandle texHandle = h.textures.getHandle(named, frame);
    // A named texture we never decoded draws flat. Counted by producer, so the gap names it.
    if (named.addr && !texHandle.addr) h.coverage.unresolvedTextures[producer]++;
    h.coverage.apiDraws++;
    if (!frame) return;
    DrawSprite sprite = DrawSprite::fromRect(x, y * 0.5f, w, height * 0.5f, u, v, u + uw, v + vh);
    sprite.test = h.state.test;
    sprite.producer = producer;
    sprite.setColor(color);
    sprite.texHandle = texHandle;
    sprite.alpha = h.state.alpha;
    frame->addSprite(sprite);
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

// The frame already presented, put back over the screen at a fixed alpha or the caller's.
void submitFrameFeedback(HookContext& h, u8 alpha) {
    const ProducerDraws draws(h.frames);
    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;

    DrawSprite sprite = DrawSprite::fromRect(-0.5f, -0.3125f, 512.0f, 224.0f, 0.0f, 0.0f,
                                             kGuestWidth, kGuestHeight);
    sprite.setColor(Rgba{0x80, 0x80, 0x80, alpha});
    sprite.texHandle = TexHandle{kPreviousFrameHandle};
    sprite.alpha = GsAlpha::Blend;
    sprite.producer = "submitFrameFeedback";
    frame->addSprite(sprite);
}

// rsetTest(ate, atst, aref, afail, date, datm, zte, ztst), assembled the way the engine does.
void rsetTest(HookContext& h, const u32 ate, const u32 atst, const u32 aref, const u32 afail,
              const u32 date, const u32 datm, const u32 ztst) {
    const u32 test = ate | (atst << 1) | (aref << 4) | (afail << 12) | (date << 14) |
                     (datm << 15) | 0x10000u | (ztst << 17);
    h.state.test = test;
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

    DrawSprite sprite = DrawSprite::fromRect(f32(x), f32(y), f32(w), f32(height), 0, 0, 0, 0);
    sprite.corner[0].color = Rgba{u8(r0), u8(g0), u8(b0), u8(a0)};
    sprite.corner[1].color = Rgba{u8(r1), u8(g1), u8(b1), u8(a1)};
    sprite.corner[2].color = Rgba{u8(r2), u8(g2), u8(b2), u8(a2)};
    sprite.corner[3].color = Rgba{u8(r3), u8(g3), u8(b3), u8(a3)};
    sprite.alpha = h.state.alpha;
    sprite.test = h.state.test;
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

    DrawSprite sprite = DrawSprite::fromRect(f32(x), f32(y), f32(w), f32(height), 0, 0, 0, 0);
    sprite.setColor(Rgba{u8(r), u8(g), u8(b), u8(a)});
    sprite.alpha = h.state.alpha;
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
        DrawSprite sprite = DrawSprite::fromRect(left, y, right - left, 1.0f, 0, 0, 0, 0);
        sprite.setColor(Rgba{u8(r), u8(g), u8(b), u8(a)});
        sprite.alpha = h.state.alpha;
        sprite.test = h.state.test;
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
    const Rgba color = Rgba::fromColorWord(Col);
    const auto uv = [&](usize i) { return f32(s32(u32Ptr_t{pUv + u32(i) * 4}.valueOr())) / 16.0f; };

    const frGsTex_t sheet = frGsTex_PTR_t{pGsTex}.valueOr();
    // The palettes are static data, so they read whether or not the glyph uploads ran in the window.
    h.textures.loadFontClut(pcsx2hook_find_symbol(h.runtime, "frFontClutTable"));
    h.textures.wantFontSheet(sheet.Addr, sheet.BufWidth, sheet.Width, sheet.Height);
    DrawSprite sprite = DrawSprite::fromRect(f32(X) / 16.0f, f32(Y) / 16.0f, f32(Width),
                                             f32(Height), uv(0), uv(1), uv(2), uv(3));
    sprite.setColor(color);
    sprite.texHandle = h.textures.getFontSheet(sheet.Width, frame);
    sprite.alpha = h.state.alpha;
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

    const Rgba color = Rgba::fromPacked(aColor);
    DrawSprite sprite{};
    const auto place = [&](int corner, s32 x, s32 y, s32 u, s32 v) {
        sprite.corner[corner] = SpriteCorner{f32(x) / 16.0f - kOffsetX, f32(y) / 16.0f - kOffsetY,
                                             f32(u) / 16.0f, f32(v) / 16.0f, color};
    };
    place(0, aX0, aY0, aU0, aV0);
    place(1, aX1, aY1, aU1, aV1);
    place(3, aX2, aY2, aU2, aV2);
    place(2, aX3, aY3, aU3, aV3);
    sprite.texHandle = h.textures.getHandle(TexHandle{h.state.texture}, frame);
    sprite.alpha = sdfPrimAbe(aPrim) ? h.state.alpha : GsAlpha::Opaque;
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
                                             0.0f, 0.0f, Rgba::fromPacked(rgba)};
    };
    place(0, aX0, aY0, aColor0);
    place(1, aX1, aY1, aColor1);
    place(3, aX2, aY2, aColor2);
    place(2, aX3, aY3, aColor3);
    sprite.alpha = sdfPrimAbe(aPrim) ? h.state.alpha : GsAlpha::Opaque;
    sprite.producer = "sdfPrimLinkG4";
    frame->addSprite(sprite);
}

// fclDrawSpriteCore(...). X and Y are 12.4 in the draw space; extents are pixels, H in the
// doubled vertical scale.
void fclDrawSpriteCore(HookContext& h, const s32 X, const s32 Y, const s32 W, const s32 H,
                       const u32 pCol, const u32 pSprite, const u32 FlipFlag, const u32 AlphaFlag) {
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
    DrawSprite sprite = DrawSprite::fromRect(left, top, f32(W), f32(H) * 0.5f,
                                             (FlipFlag & 1) ? u1 : u0, (FlipFlag & 2) ? v1 : v0,
                                             (FlipFlag & 1) ? u0 : u1, (FlipFlag & 2) ? v0 : v1);
    const u32 colorWord =
        pCol ? u32Ptr_t{pCol}.valueOr() : u32Ptr_t{pSprite + u32(offsetof(fclSprite_t, Col))}.valueOr();
    sprite.setColor(Rgba::fromColorWord(colorWord));
    sprite.texHandle = h.textures.getHandle(sprite2d->TexHandle, frame);
    sprite.alpha = AlphaFlag == 1   ? GsAlpha::Additive
                   : AlphaFlag == 2 ? GsAlpha::ReverseSubtract
                                    : GsAlpha::Blend;
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
        DrawSprite::fromRect(f32(x) / 16.0f, f32(y) / 16.0f, f32(w), f32(height) * 0.5f, 0, 0, 0, 0);
    // The engine's order is top left, top right, bottom left, bottom right.
    static constexpr int kCorner[4] = {0, 1, 3, 2};
    for (int i = 0; i < 4; i++) {
        sprite.corner[kCorner[i]].color =
            Rgba::fromColorWord(u32Ptr_t{pCol + u32(i) * 4}.valueOr());
    }
    sprite.alpha = h.state.alpha;
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
        const s32 x = s32(u32Ptr_t{pX + u32(i) * 4}.valueOr());
        const s32 y = s32(u32Ptr_t{pY + u32(i) * 4}.valueOr());
        const u32 colorWord = u32Ptr_t{pCol + u32(i) * 4}.valueOr();
        sprite.corner[i] =
            SpriteCorner{f32(x) / 16.0f, f32(y) / 16.0f, 0.0f, 0.0f,
                         Rgba::fromColorWord(colorWord)};
    }
    // A sprite carries four corners, so the last repeats and its triangle collapses.
    sprite.corner[3] = sprite.corner[2];
    sprite.alpha = h.state.alpha;
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
    const u8 r = u8Ptr_t{fade}.valueOr();
    const u8 g = u8Ptr_t{fade + 1}.valueOr();
    const u8 b = u8Ptr_t{fade + 2}.valueOr();
    const u8 state = u8Ptr_t{fade + 3}.valueOr();
    if (state == 0) return;

    h.coverage.apiDraws++;
    Frame* frame = h.frames.getFrame();
    if (!frame) return;

    DrawSprite sprite = DrawSprite::fromRect(0, 0, kGuestWidth, kGuestHeight, 0, 0, 0, 0);
    sprite.setColor(Rgba{r, g, b, u8(128 - state)});
    sprite.alpha = GsAlpha::Blend;
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
