#include "packet_processor.h"

#include <bitset>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gif.h"
#include "gs_decoder.h"
#include "hooks_impl.h"
#include "json.h"
#include "libsdf.h"
#include "log.h"
#include "packet_type.h"
#include "trace.h"
#include "unsupported.h"

namespace sdfhook {

namespace {

LogChannel log("packets");

/// One DMA tag of the chain, as channel 1 reaches it.
struct ChainEntry {
    u32 at = 0;
    sdfDmaTag_t tag{};

    /// How many CALLs deep the entry sits.
    u32 depth = 0;

    /// Whether its transfer was A+D state and nothing else, once the register replay has read it.
    bool stateOnly = false;

    u32 getId() const { return sdfGetDmaId(tag); }
    bool isCall() const { return getId() == sdfDmaTagCall; }
    u32 getRefTarget() const { return sdfDmaIsRef(tag) ? tag.addr & 0x0FFFFFFFu : 0; }
};

/// The chain in transmit order, following NEXT, CALL and RET the way channel 1 does.
class DmaChain {
public:
    explicit DmaChain(u32 head) : mAt(head & 0x0FFFFFFFu) {}

    bool next(ChainEntry& entry);

    /// Hands the entry out again, for a span that read one past its end.
    void unread(const ChainEntry& entry) {
        mUnread = entry;
        mHasUnread = true;
    }

private:
    // A frame links a few thousand tags; beyond that the chain loops or was read out of data.
    static constexpr u32 kMaxTags = 65536;

    u32 mAt;
    u32 mReturn[16] = {};
    u32 mDepth = 0;
    u32 mSteps = 0;
    ChainEntry mUnread;
    bool mHasUnread = false;
};

bool DmaChain::next(ChainEntry& entry) {
    if (mHasUnread) {
        entry = mUnread;
        mHasUnread = false;
        return true;
    }
    if (!mAt) return false;
    if (mSteps++ == kMaxTags) {
        reportUnsupported("transmit chain tags", kMaxTags);
        return false;
    }
    const sdfDmaTagPtr_t tag{mAt};
    if (!tag) return false;
    entry = ChainEntry{mAt, *tag, mDepth};

    const u32 addr = tag->addr & 0x0FFFFFFFu;
    const u32 after = mAt + sdfQwordBytes * (1 + tag->qwc);
    switch (entry.getId()) {
        case sdfDmaTagRef:
        case sdfDmaTagRefs: mAt += sdfQwordBytes; break;
        case sdfDmaTagCnt: mAt = after; break;
        case sdfDmaTagNext: mAt = addr; break;
        case sdfDmaTagCall:
            if (mDepth == std::size(mReturn)) {
                reportUnsupported("transmit chain call depth", mDepth);
                mAt = 0;
                break;
            }
            mReturn[mDepth++] = after;
            mAt = addr;
            break;
        case sdfDmaTagRet: mAt = mDepth ? mReturn[--mDepth] : 0; break;
        case sdfDmaTagRefe:
        case sdfDmaTagEnd: mAt = 0; break;
    }
    return true;
}

/// The functions that link a model node, whose meshes the model walk has already read.
bool isModelLinker(const char* linker) {
    return std::strcmp(linker, "sdfModelLinkOt") == 0 ||
           std::strcmp(linker, "sdfModelTempPacketLinkOt") == 0 ||
           std::strcmp(linker, "sdfModelLinkShapedDrawPacket") == 0;
}

/// What a model node sends: its meshes, and the VU1 state the model walk read beside them.
bool isModelNodeType(PacketType type) {
    switch (type) {
        case PacketType::Unknown:
        case PacketType::Transform:
        case PacketType::TransformRef:
        case PacketType::Material:
        case PacketType::MaterialRef:
        case PacketType::MeshNormal:
        case PacketType::MeshSkinned:
        case PacketType::MeshShaped:
        case PacketType::MeshStream:
        case PacketType::MeshGenerated:
        case PacketType::DrawParams:
        case PacketType::VuMicrocode:
        case PacketType::PacketHeader:
        case PacketType::Terminator:
        case PacketType::VifData:
        case PacketType::VifSetup: return true;
        case PacketType::Viewport:
        case PacketType::LightParallel:
        case PacketType::Fog:
        case PacketType::PolyStream:
        case PacketType::Sprite:
        case PacketType::Glyph:
        case PacketType::Primitive2D:
        case PacketType::TestState:
        case PacketType::AlphaState:
        case PacketType::TexaState:
        case PacketType::TexBind:
        case PacketType::ZClear:
        case PacketType::Fade:
        case PacketType::BillboardBatch:
        case PacketType::BillboardTransform:
        case PacketType::Shadow:
        case PacketType::Packet2D:
        case PacketType::Primitive3D:
        case PacketType::BitBlt:
        case PacketType::DebugText:
        case PacketType::DrawEnv:
        case PacketType::FrameMask:
        case PacketType::FrameTex:
        case PacketType::DrawFrame:
        case PacketType::RefGif:
        case PacketType::TextureUpload:
        case PacketType::Clear:
        case PacketType::FrameBuffer:
        case PacketType::DirectRef:
        case PacketType::GifPrimitive:
        case PacketType::GifRegisters:
        case PacketType::Empty: return false;
        case PacketType::Count: break;
    }
    reportUnsupported("packet type", u32(type));
    return false;
}

class PacketProcessor {
public:
    PacketProcessor(HookContext& h, Frame& frame);
    void run(u32 aTag);

private:
    using Process = void (PacketProcessor::*)(DmaChain&, ChainEntry);

    void resolveClaims();
    void indexDrawsByPacket();
    u32 getClaimedPacket(const FrameBuilder::DrawClaim& claim) const;
    const LinkedPacket* findLinked(u32 at) const;
    const char* getLinker(const LinkedPacket& packet) const;
    PacketType getType(u32 at) const;

    void processModelSpan(DmaChain& chain, ChainEntry entry);
    void processModelPacket(DmaChain& chain, ChainEntry entry);
    void processPacket(DmaChain& chain, ChainEntry entry);
    void processCallee(DmaChain& chain, const ChainEntry& call, Process process);
    void processPacketOfType(const ChainEntry& entry, PacketType type);

    void processSpritePacket(const ChainEntry& entry);
    void processMeshPacket(const ChainEntry& entry);
    void processShadowPacket(const ChainEntry& entry, PacketType type);
    void processUnknownPacket(const ChainEntry& entry);
    void processTexBindPacket(const ChainEntry& entry);
    void processGsStatePacket(const ChainEntry& entry, PacketType type);
    void processCarriedPacket(const ChainEntry& entry, PacketType type);
    void processEmptyPacket(const ChainEntry& entry);
    void processTransferPacket(const ChainEntry& entry, PacketType type);
    void reportUnreadPacket(PacketType type);

    bool decodeTransfer(const ChainEntry& entry);
    void emitDecodedPrimitives(const ChainEntry& entry);
    bool convertPrimitive(const GsPrimitive& primitive, DrawSprite* sprite);
    enum class Verdict { Honoured, Invisible, Unhonoured };
    Verdict judgePrimitive(const GsPrimitive& primitive, const DrawSprite& sprite,
                           const char** why) const;
    void reportUnhonoured(const GsPrimitive& primitive);
    void dumpPrimitive(const GsPrimitive& primitive, const DrawSprite& sprite) const;
    TexHandle resolveTexture(u64 tex0);
    const char* getProducer(const ChainEntry& entry) const;
    void dumpStateChange(const sdfGsState_t& was, u32 at);
    void emitSprites(const ChainEntry& entry);
    void emitMeshes(const ChainEntry& entry);
    void emitSprites(u32 packet);
    void emitMeshes(u32 packet);
    void recordCoverage(const ChainEntry& entry, u64 emittedBefore);
    void countUnreached();

    HookContext& h;
    Frame& mFrame;
    GifParser mParser;
    GsDecoder mDecoder;

    /// TEX0 as the last 2D texture packet left it, which a sprite naming no texture samples.
    TexHandle mBoundTexture;

    /// The handle behind each TEX0 a 2D texture packet wrote this frame.
    std::unordered_map<u64, TexHandle> mBoundTextures;

    /// The function behind the last linked packet, which an unlinked tag after it belongs to, and
    /// whether that packet came from a producer whose hook drew it.
    const char* mLastLinker = "chain";
    bool mLastClaimed = false;

    /// The last texture looked up was tried and cannot be had, as against still on its way.
    bool mTextureMissing = false;

    std::unordered_map<u32, LinkedPacket> mLinked;
    std::unordered_map<u32, std::vector<u32>> mSpritesByPacket;
    std::unordered_map<u32, std::vector<u32>> mMeshesByPacket;

    /// Packets linked by a producer that recorded draws.
    std::unordered_set<u32> mClaimed;
    u64 mEmitted = 0;
};

PacketProcessor::PacketProcessor(HookContext& h, Frame& frame)
    : h(h), mFrame(frame), mParser(&h.coverage.gif) {
    for (const LinkedPacket& packet : h.frames.getPackets()) mLinked.emplace(packet.addr, packet);
    resolveClaims();
    indexDrawsByPacket();
}

const LinkedPacket* PacketProcessor::findLinked(u32 at) const {
    const auto found = mLinked.find(at);
    return found == mLinked.end() ? nullptr : &found->second;
}

const char* PacketProcessor::getLinker(const LinkedPacket& packet) const {
    return h.symbols.getFunction(packet.caller);
}

/// What its builder recorded, else what the function that linked it is known to link.
PacketType PacketProcessor::getType(u32 at) const {
    const auto built = h.packetTypes.find(at);
    if (built != h.packetTypes.end()) return built->second;
    const LinkedPacket* linked = findLinked(at);
    return linked ? getPacketTypeByLinker(getLinker(*linked)) : PacketType::Unknown;
}

/// The first packet the producer linked that draws: a texture or A+D state packet linked ahead of
/// the geometry is passed over, now that its contents are written.
u32 PacketProcessor::getClaimedPacket(const FrameBuilder::DrawClaim& claim) const {
    for (const LinkedPacket& packet : claim.packets) {
        if (isDrawType(getType(packet.addr)) && !isRegisterWrites(packet.addr)) return packet.addr;
    }
    return 0;
}

void PacketProcessor::resolveClaims() {
    for (const FrameBuilder::DrawClaim& claim : h.frames.getClaims()) {
        for (const LinkedPacket& packet : claim.packets) mClaimed.insert(packet.addr);
        const u32 packet = getClaimedPacket(claim);
        for (usize i = claim.from.sprites; i < claim.to.sprites; i++) {
            mFrame.sprites[i].linkAddr = packet;
        }
        for (usize i = claim.from.meshes; i < claim.to.meshes; i++) {
            DrawMesh& mesh = mFrame.meshes[i];
            if (!mesh.linkAddr && mesh.linkIndex == kNoLinkIndex) mesh.linkAddr = packet;
        }
    }
}

/// A generated mesh is found through the link its generation returned.
void PacketProcessor::indexDrawsByPacket() {
    for (u32 i = 0; h.config->spriteHooks && i < mFrame.sprites.size(); i++) {
        const DrawSprite& sprite = mFrame.sprites[i];
        if (sprite.linkAddr) mSpritesByPacket[sprite.linkAddr].push_back(i);
        else h.coverage.unlinkedDraws[sprite.producer]++;
    }
    const std::vector<u32>& generated = h.frames.getGeneratedLinks();
    for (u32 i = 0; i < mFrame.meshes.size(); i++) {
        const DrawMesh& mesh = mFrame.meshes[i];
        u32 packet = mesh.linkAddr;
        if (!packet && mesh.linkIndex < generated.size()) packet = generated[mesh.linkIndex];
        if (packet) mMeshesByPacket[packet].push_back(i);
        else h.coverage.unlinkedDraws[mesh.producer]++;
    }
}

void PacketProcessor::run(u32 aTag) {
    const sdfDrawTagPtr_t drawTag{aTag};
    DmaChain chain(drawTag ? drawTag->head.addr : 0);
    ChainEntry entry;
    while (chain.next(entry)) {
        const LinkedPacket* linked = findLinked(entry.at);
        if (linked && isModelLinker(getLinker(*linked))) processModelSpan(chain, entry);
        else processPacket(chain, entry);
    }
    countUnreached();
}

/// A model node's packets, from its first link to its draw tag's tail.
void PacketProcessor::processModelSpan(DmaChain& chain, ChainEntry entry) {
    const sdfDrawTagPtr_t tag = findLinked(entry.at)->tag;
    const u32 tail = tag ? tag->tail.addr & 0x0FFFFFFFu : 0;
    for (;;) {
        processModelPacket(chain, entry);
        if (entry.at == tail || !chain.next(entry)) return;
        // Another producer linked into the same tag.
        const LinkedPacket* linked = findLinked(entry.at);
        if (linked && !isModelLinker(getLinker(*linked))) {
            chain.unread(entry);
            return;
        }
    }
}

void PacketProcessor::processModelPacket(DmaChain& chain, ChainEntry entry) {
    entry.stateOnly = decodeTransfer(entry);
    const u64 before = mEmitted;
    const PacketType type = getType(entry.at);
    if (isModelNodeType(type)) emitMeshes(entry);
    else processPacketOfType(entry, type);
    if (mEmitted == before) emitDecodedPrimitives(entry);
    if (entry.isCall()) processCallee(chain, entry, &PacketProcessor::processModelPacket);
    recordCoverage(entry, before);
}

void PacketProcessor::processPacket(DmaChain& chain, ChainEntry entry) {
    entry.stateOnly = decodeTransfer(entry);
    const u64 before = mEmitted;
    processPacketOfType(entry, getType(entry.at));
    // A hook's draws stand for the packet; anything else it carries is drawn as decoded.
    if (mEmitted == before) emitDecodedPrimitives(entry);
    if (entry.isCall()) processCallee(chain, entry, &PacketProcessor::processPacket);
    recordCoverage(entry, before);
}

/// Everything a CALL reaches before the chain returns past it.
void PacketProcessor::processCallee(DmaChain& chain, const ChainEntry& call, Process process) {
    ChainEntry entry;
    while (chain.next(entry)) {
        if (entry.depth <= call.depth) {
            chain.unread(entry);
            return;
        }
        (this->*process)(chain, entry);
    }
}

void PacketProcessor::processPacketOfType(const ChainEntry& entry, PacketType type) {
    switch (type) {
        case PacketType::Sprite:
        case PacketType::Packet2D:
        case PacketType::Primitive2D:
        case PacketType::Glyph:
        case PacketType::Fade:
        case PacketType::ZClear:
        case PacketType::DebugText:
        case PacketType::GifPrimitive: processSpritePacket(entry); break;
        case PacketType::MeshNormal:
        case PacketType::MeshSkinned:
        case PacketType::MeshShaped:
        case PacketType::MeshStream:
        case PacketType::MeshGenerated:
        case PacketType::PolyStream:
        case PacketType::BillboardBatch:
        case PacketType::Primitive3D:
        case PacketType::VifData: processMeshPacket(entry); break;
        case PacketType::Shadow: processShadowPacket(entry, type); break;
        case PacketType::Unknown: processUnknownPacket(entry); break;
        case PacketType::TexBind: processTexBindPacket(entry); break;
        case PacketType::TestState:
        case PacketType::AlphaState:
        case PacketType::TexaState:
        case PacketType::DrawEnv:
        case PacketType::FrameMask:
        case PacketType::FrameTex:
        case PacketType::DrawFrame:
        case PacketType::FrameBuffer:
        case PacketType::RefGif:
        case PacketType::GifRegisters: processGsStatePacket(entry, type); break;
        case PacketType::Transform:
        case PacketType::TransformRef:
        case PacketType::Viewport:
        case PacketType::LightParallel:
        case PacketType::Fog:
        case PacketType::Material:
        case PacketType::MaterialRef:
        case PacketType::DrawParams:
        case PacketType::VuMicrocode:
        case PacketType::BillboardTransform:
        case PacketType::DirectRef:
        case PacketType::PacketHeader:
        case PacketType::Terminator:
        case PacketType::VifSetup: processCarriedPacket(entry, type); break;
        case PacketType::Empty: processEmptyPacket(entry); break;
        case PacketType::Clear: processSpritePacket(entry); break;
        case PacketType::BitBlt:
        case PacketType::TextureUpload: processTransferPacket(entry, type); break;
        case PacketType::Count: reportUnsupported("packet type", u32(type)); break;
    }
}

void PacketProcessor::processSpritePacket(const ChainEntry& entry) {
    if (h.config->spriteHooks) emitSprites(entry);
}

void PacketProcessor::processMeshPacket(const ChainEntry& entry) {
    emitMeshes(entry);
}

/// fldSdwDraw links four packets and only its inline VIF stream is the shadow, read where the chain
/// sends it, under the state in force there.
void PacketProcessor::processShadowPacket(const ChainEntry& entry, PacketType type) {
    if (entry.stateOnly) {
        processGsStatePacket(entry, type);
        return;
    }
    if (entry.getId() != sdfDmaTagCnt) {
        reportUnsupported("shadow packet that is not an inline VIF stream", entry.getId());
        return;
    }
    if (!h.config->drawShadows) return;
    const u32 data = entry.at + sdfQwordBytes;
    if (!h.modelHooks.fldSdwDraw(data, data + sdfQwordBytes * entry.tag.qwc, mDecoder.getContext(0),
                                 &mFrame)) {
        h.coverage.shadowRejects++;
        return;
    }
    h.coverage.shadowMeshes++;
    mFrame.order.push_back({DrawRef::Kind::Mesh, u32(mFrame.meshes.size() - 1)});
    mEmitted++;
    h.coverage.placedDraws["fldSdwDraw"]++;
}

/// Untyped, so it carries whatever a producer claimed on it: the feedback sprite's linker has
/// three sites.
void PacketProcessor::processUnknownPacket(const ChainEntry& entry) {
    if (h.config->spriteHooks) emitSprites(entry);
    emitMeshes(entry);
}

void PacketProcessor::processTexBindPacket(const ChainEntry& entry) {
    const auto binds = h.texturePackets.find(entry.at);
    if (binds == h.texturePackets.end()) {
        reportUnsupported("2D texture packet with no recorded texture", entry.tag.qwc);
        return;
    }
    mBoundTexture = TexHandle{binds->second};
    mBoundTextures[mDecoder.getRegister(sdfGsAddrTex01)] = mBoundTexture;
}

/// The decoder has stored every register; what a draw cannot honour is reported where it is
/// converted, so nothing is dropped here.
void PacketProcessor::processGsStatePacket(const ChainEntry&, PacketType) {}

/// Taken from the producer's call rather than the packet, so one that no producer with recorded
/// draws linked is unread.
void PacketProcessor::processCarriedPacket(const ChainEntry& entry, PacketType type) {
    if (!mClaimed.contains(entry.at)) reportUnreadPacket(type);
}

/// An upload stales what was read from GS memory there, which the decoder has passed on; a
/// copy within GS memory the decoder reports.
void PacketProcessor::processTransferPacket(const ChainEntry& entry, PacketType type) {
    if (type == PacketType::TextureUpload && mDecoder.getUploads().empty() && !entry.stateOnly) {
        reportUnsupported("texture upload packet with no IMAGE transfer", entry.tag.qwc);
    }
}

void PacketProcessor::processEmptyPacket(const ChainEntry& entry) {
    if (entry.tag.qwc) reportUnsupported("empty packet with a payload", entry.tag.qwc);
}

void PacketProcessor::reportUnreadPacket(PacketType type) {
    const std::string what = std::string("unread packet: ") + getPacketTypeName(type);
    reportUnsupported(what.c_str(), u32(type));
}

/// @return Whether the transfer was state alone.
bool PacketProcessor::decodeTransfer(const ChainEntry& entry) {
    const sdfGsState_t was = mDecoder.getContext(0);
    const u32 target = entry.getRefTarget();
    mDecoder.feed(entry.tag, target ? target : entry.at + sdfQwordBytes);
    for (const GsUpload& upload : mDecoder.getUploads()) {
        h.textures.onGsUpload(upload.bitbltbuf, upload.trxreg);
    }
    // FBP counts 2048-word pages, TBP0 64-word blocks.
    for (const u32 frame : {sdfGsAddrFrame1, sdfGsAddrFrame2}) {
        if (mDecoder.getWritten()[frame]) {
            h.textures.noteFrameBuffer(u32(mDecoder.getRegister(frame) & 0x1FF) * 32);
        }
    }
    if (const LinkedPacket* linked = findLinked(entry.at)) {
        mLastLinker = getLinker(*linked);
        mLastClaimed = mClaimed.contains(entry.at);
    }
    if (h.config->dumpDrawEnv) dumpStateChange(was, entry.at);
    return mDecoder.isStateOnly();
}

const char* PacketProcessor::getProducer(const ChainEntry& entry) const {
    const LinkedPacket* linked = findLinked(entry.at);
    return linked ? getLinker(*linked) : mLastLinker;
}

void PacketProcessor::emitDecodedPrimitives(const ChainEntry& entry) {
    // With the hooks on, a hooked producer's draws stand for every packet it linked.
    if (h.config->spriteHooks && mLastClaimed) return;
    for (const GsPrimitive& primitive : mDecoder.getPrimitives()) {
        const char* producer = getProducer(entry);
        DrawSprite sprite;
        if (!convertPrimitive(primitive, &sprite)) continue;
        sprite.linkAddr = entry.at;
        sprite.producer = producer;
        reportUnhonoured(primitive);
        const char* why = nullptr;
        const Verdict verdict = judgePrimitive(primitive, sprite, &why);
        if (verdict == Verdict::Invisible) continue;
        if (verdict == Verdict::Unhonoured) {
            reportUnsupported(why, sdfGetPrimType(primitive.state.prim));
            h.coverage.unhonouredDraws[producer]++;
            sprite.unhandled = true;
        }
        if (!h.config->dumpDecoded.empty() && std::strstr(sprite.producer, h.config->dumpDecoded.c_str())) {
            dumpPrimitive(primitive, sprite);
        }
        h.coverage.decodedDraws[sprite.producer]++;
        mFrame.order.push_back({DrawRef::Kind::Sprite, u32(mFrame.sprites.size())});
        mFrame.sprites.push_back(sprite);
        mEmitted++;
    }
}

/// A primitive as the 2D renderer's four corners: a triangle repeats its last, a line is a
/// pixel wide. XYOFFSET comes off, and STQ becomes texels the way FST already is.
bool PacketProcessor::convertPrimitive(const GsPrimitive& primitive, DrawSprite* out) {
    const GsDrawState& state = primitive.state;
    const u32 prim = state.prim;
    const GsTex0 tex0 = GsTex0::decode(state.tex0);
    const u32 ofx = u32(state.xyoffset & 0xFFFF);
    const u32 ofy = u32((state.xyoffset >> 32) & 0xFFFF);
    // The odd field draws half a line down, which the display takes back; a progressive frame
    // drops it.
    constexpr u32 kFieldHalfLine = 8;
    if (ofx & 0xF || (ofy & 0xF) & ~kFieldHalfLine) reportUnsupported("XYOFFSET fraction", state.xyoffset);
    const f32 offsetX = f32(ofx) / 16.0f;
    const f32 offsetY = f32(ofy & ~0xFu) / 16.0f;
    const auto getCorner = [&](const GsVertex& vertex) {
        SpriteCorner corner{};
        corner.x = f32(vertex.x) / 16.0f - offsetX;
        corner.y = f32(vertex.y) / 16.0f - offsetY;
        if (sdfGetPrimFst(prim)) {
            corner.u = f32(vertex.u) / 16.0f;
            corner.v = f32(vertex.v) / 16.0f;
        } else {
            const f32 q = vertex.q != 0.0f ? vertex.q : 1.0f;
            corner.u = vertex.s / q * f32(1u << tex0.tw);
            corner.v = vertex.t / q * f32(1u << tex0.th);
        }
        corner.color = Rgba::unpack(vertex.rgba);
        return corner;
    };

    DrawSprite sprite{};
    const GsVertex* vertex = primitive.vertex;
    switch (sdfGetPrimType(prim)) {
        case sdfPrimSprite: {
            const SpriteCorner a = getCorner(vertex[0]);
            const SpriteCorner b = getCorner(vertex[1]);
            sprite = DrawSprite::makeRect(a.x, a.y, b.x - a.x, b.y - a.y, a.u, a.v, b.u, b.v);
            // A sprite takes the color of the vertex that kicked it.
            sprite.setColor(b.color);
            break;
        }
        case sdfPrimTriangle:
        case sdfPrimTriStrip:
        case sdfPrimTriFan:
            for (int i = 0; i < 3; i++) sprite.corner[i] = getCorner(vertex[i]);
            sprite.corner[3] = sprite.corner[2];
            if (!sdfGetPrimIip(prim)) sprite.setColor(sprite.corner[2].color);
            break;
        case sdfPrimLine:
        case sdfPrimLineStrip: {
            const SpriteCorner a = getCorner(vertex[0]);
            const SpriteCorner b = getCorner(vertex[1]);
            if (a.x != b.x && a.y != b.y) {
                reportUnsupported("decoded diagonal line", prim);
                return false;
            }
            const f32 left = std::min(a.x, b.x);
            const f32 top = std::min(a.y, b.y);
            sprite = DrawSprite::makeRect(left, top, std::max(std::abs(b.x - a.x), 1.0f),
                                          std::max(std::abs(b.y - a.y), 1.0f), 0, 0, 0, 0);
            sprite.setColor(b.color);
            break;
        }
        case sdfPrimPoint: {
            const SpriteCorner a = getCorner(vertex[0]);
            sprite = DrawSprite::makeRect(a.x, a.y, 1.0f, 1.0f, a.u, a.v, a.u, a.v);
            sprite.setColor(a.color);
            break;
        }
        default: reportUnsupported("decoded PRIM type", sdfGetPrimType(prim)); return false;
    }
    sprite.blends = sdfGetPrimAbe(prim) != 0;
    sprite.alpha = state.alpha;
    sprite.test = u32(state.test);
    sprite.frameMask = sdfGetFrameMask(state.frame);
    mTextureMissing = false;
    if (sdfGetPrimTme(prim)) {
        sprite.texHandle = resolveTexture(state.tex0);
        sprite.hasSampler = true;
        sprite.tex1 = state.tex1;
        sprite.clamp = state.clamp;
    }
    *out = sprite;
    return true;
}

/// Whether the 2D renderer draws the primitive as the GS would. One it cannot is painted
/// magenta, so the gap shows on screen; one whose tests pass no color is not drawn.
PacketProcessor::Verdict PacketProcessor::judgePrimitive(const GsPrimitive& primitive,
                                                         const DrawSprite& sprite,
                                                         const char** why) const {
    const GsDrawState& state = primitive.state;
    const u32 prim = state.prim;
    const u64 test = state.test;
    const auto unhonoured = [&](const char* reason) {
        *why = reason;
        return Verdict::Unhonoured;
    };

    // A failing fragment that keeps nothing, or only depth, adds no color at all.
    const u32 atst = sdfGetTestAtst(test);
    const u32 afail = sdfGetTestAfail(test);
    if (sdfGetTestAte(test) && atst == sdfAtstNever &&
        (afail == sdfAfailKeep || afail == sdfAfailZbOnly)) {
        return Verdict::Invisible;
    }
    if (sdfGetTestZte(test) && sdfGetTestZtst(test) == sdfZtstNever) return Verdict::Invisible;
    if (sdfGetTestDate(test)) return unhonoured("decoded primitive destination alpha test");
    if (sdfGetPrimTme(prim)) {
        if (!sprite.texHandle.addr && mTextureMissing) return unhonoured("decoded texture not found");
        if (sdfGetTex0Tfx(state.tex0) != sdfTexModulate) return unhonoured("decoded primitive TFX");
        if (!sdfGetTex0Tcc(state.tex0)) return unhonoured("decoded primitive without texture alpha");
    }
    if (sdfGetPrimFge(prim)) return unhonoured("decoded primitive with fog");
    if (state.pabe & 1) return unhonoured("decoded primitive under PABE");
    if (state.fba & 1) return unhonoured("decoded primitive under FBA");
    if (!(state.colclamp & 1)) return unhonoured("decoded primitive with COLCLAMP off");
    return Verdict::Honoured;
}

/// Differences reported but drawn as they are. Depth is one: 2D here neither tests nor writes it,
/// and the chain's own order already layers what the menus depth-test against each other.
void PacketProcessor::reportUnhonoured(const GsPrimitive& primitive) {
    const GsDrawState& state = primitive.state;
    if (sdfGetTestZte(state.test) && sdfGetTestZtst(state.test) != sdfZtstAlways) {
        reportUnsupported("decoded primitive depth test", sdfGetTestZtst(state.test));
    }
    if (sdfGetPrimAa1(state.prim)) reportUnsupported("decoded primitive with antialiasing", state.prim);
    if (sdfGetTestAte(state.test) && sdfGetTestAfail(state.test) == sdfAfailRgbOnly) {
        reportUnsupported("decoded primitive alpha test keeping alpha", sdfGetTestAtst(state.test));
    }
    if (!((state.zbuf >> 32) & 1)) reportUnsupported("decoded primitive writes depth", 0);
}

void PacketProcessor::dumpPrimitive(const GsPrimitive& primitive, const DrawSprite& sprite) const {
    const GsDrawState& state = primitive.state;
    JsonObject row;
    row.addString("dump", "decoded")
        .addUint("frame", h.trace->getFrame())
        .addString("by", sprite.producer)
        .addHexDigits("at", sprite.linkAddr, 8)
        .addHexDigits("prim", state.prim, 3)
        .addHexDigits("tex0", state.tex0, 16)
        .addHexDigits("tex1", state.tex1, 16)
        .addHexDigits("clamp", state.clamp, 16)
        .addHexDigits("texHandle", sprite.texHandle.addr, 8)
        .addHexDigits("alpha", state.alpha, 16)
        .addHexDigits("test", state.test, 8)
        .addHexDigits("frame", state.frame, 16)
        .addHexDigits("zbuf", state.zbuf, 16)
        .addHexDigits("xyoffset", state.xyoffset, 16)
        .addHexDigits("scissor", state.scissor, 16)
        .beginArray("vertices");
    for (u32 i = 0; i < primitive.count; i++) {
        const GsVertex& v = primitive.vertex[i];
        row.beginObject()
            .addUint("x", v.x)
            .addUint("y", v.y)
            .addUint("z", v.z)
            .addHexDigits("rgba", v.rgba, 8)
            .addUint("u", v.u)
            .addUint("v", v.v)
            .addReal("s", v.s, 6)
            .addReal("t", v.t, 6)
            .addReal("q", v.q, 6)
            .endObject();
    }
    log.dump(row.endArray().take());
}

/// A frame buffer is the previous frame and the font is the sheet the glyph uploads built; a 2D
/// texture packet names its handle; anything else is found by its TEX0.
TexHandle PacketProcessor::resolveTexture(u64 tex0) {
    const u32 tbp0 = GsTex0::decode(tex0).tbp0;
    if (h.textures.isFrameBuffer(tbp0)) return TexHandle{kPreviousFrameHandle};
    if (h.textures.isFontSheet(tbp0)) return h.textures.getFontSheet(&mFrame);
    const auto bound = mBoundTextures.find(tex0);
    TexHandle handle;
    if (bound != mBoundTextures.end()) {
        handle = h.textures.getHandle(bound->second, &mFrame);
        mTextureMissing = !handle.addr && h.textures.isMissing(bound->second);
    } else {
        handle = h.textures.getHandleForGsTex0(tex0, &mFrame);
        mTextureMissing = !handle.addr && h.textures.isGsTex0Missing(tex0);
    }
    return handle;
}

void PacketProcessor::dumpStateChange(const sdfGsState_t& was, u32 at) {
    const sdfGsState_t& gs = mDecoder.getContext(0);
    if (gs.alpha == was.alpha && gs.test == was.test && gs.frameMask == was.frameMask) return;
    const LinkedPacket* linked = findLinked(at);
    log.dump(JsonObject()
                 .addString("dump", "gsState")
                 .addUint("frame", h.trace->getFrame())
                 .addString("by", linked ? getLinker(*linked) : "-")
                 .addHexDigits("alpha", gs.alpha, 2)
                 .addHexDigits("test", gs.test, 6)
                 .addHexDigits("fbmsk", gs.frameMask, 8)
                 .addHexDigits("frame1", mDecoder.getRegister(sdfGsAddrFrame1), 16)
                 .addUint("aref", sdfGetTestAref(gs.test))
                 .take());
}

/// A REF names its data elsewhere, and a model stream is keyed by that data's address.
void PacketProcessor::emitSprites(const ChainEntry& entry) {
    emitSprites(entry.at);
    if (const u32 target = entry.getRefTarget()) emitSprites(target);
}

void PacketProcessor::emitMeshes(const ChainEntry& entry) {
    emitMeshes(entry.at);
    if (const u32 target = entry.getRefTarget()) emitMeshes(target);
}

/// Every draw here runs in context 1.
void PacketProcessor::emitSprites(u32 packet) {
    const auto found = mSpritesByPacket.find(packet);
    if (found == mSpritesByPacket.end()) return;
    const sdfGsState_t& gs = mDecoder.getContext(0);
    for (const u32 index : found->second) {
        DrawSprite& sprite = mFrame.sprites[index];
        if (sprite.boundTexture) {
            sprite.texHandle = h.textures.getHandle(mBoundTexture, &mFrame);
            if (mBoundTexture.addr && !sprite.texHandle.addr) {
                h.coverage.unresolvedTextures[sprite.producer]++;
            }
        }
        sprite.alpha = gs.alpha;
        sprite.test = u32(gs.test);
        sprite.frameMask = gs.frameMask;
        mFrame.order.push_back({DrawRef::Kind::Sprite, index});
        h.coverage.placedDraws[sprite.producer]++;
        mEmitted++;
    }
    mSpritesByPacket.erase(found);
}

/// The second texture pass carries its own ALPHA.
void PacketProcessor::emitMeshes(u32 packet) {
    const auto found = mMeshesByPacket.find(packet);
    if (found == mMeshesByPacket.end()) return;
    for (const u32 index : found->second) {
        DrawMesh& mesh = mFrame.meshes[index];
        if (!h.config->noTransmitState) mesh.gs = mDecoder.getContext(0);
        mFrame.order.push_back({DrawRef::Kind::Mesh, index});
        h.coverage.placedDraws[mesh.producer]++;
        mEmitted++;
    }
    mMeshesByPacket.erase(found);
}

/// A linked packet that should draw and had nothing emitted, at it or in what it calls, is one the
/// renderer does not handle, ranked by its GIF vertices where it has any.
void PacketProcessor::recordCoverage(const ChainEntry& entry, u64 emittedBefore) {
    const LinkedPacket* packet = findLinked(entry.at);
    if (!packet) return;
    const bool drew = mEmitted > emittedBefore;
    const GifCounts before = h.coverage.gif;
    mParser.parse(packet->addr);
    u64 verts = 0;
    for (int i = 0; i < 8; i++) verts += h.coverage.gif.verts[i] - before.verts[i];

    const char* by = getLinker(*packet);
    for (const u32 tbp : mParser.getParsedTbps()) h.coverage.tbpByProducer[by][tbp]++;
    if (!drew && !entry.stateOnly && isDrawType(getType(entry.at))) {
        if (verts) h.coverage.unhandledVerts[by] += verts;
        else h.coverage.unhandledPackets[by]++;
    }

    if (h.trace->isOpen()) h.trace->onPacket(*packet, by, entry.tag, verts);
    if (!drew && !h.config->dumpPackets.empty() && std::strstr(by, h.config->dumpPackets.c_str())) {
        dumpPacket(*packet, by, entry.tag);
    }
}

void PacketProcessor::countUnreached() {
    for (const auto& [packet, indices] : mSpritesByPacket) {
        for (const u32 index : indices) h.coverage.unreachedDraws[mFrame.sprites[index].producer]++;
    }
    for (const auto& [packet, indices] : mMeshesByPacket) {
        for (const u32 index : indices) h.coverage.unreachedDraws[mFrame.meshes[index].producer]++;
    }
}

}  // namespace

/// @disc SLUS-20911
/// @address 0x002AD980
/// @function sdfGrDrawFrame
void processCommandStream(HookContext& h, u32 aTag, Frame& frame) {
    PacketProcessor(h, frame).run(aTag);
}

}  // namespace sdfhook
