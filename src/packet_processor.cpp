#include "packet_processor.h"

#include <bitset>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gif.h"
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

/// The registers GsRegisterReplay tracks.
bool isReplayedRegister(u32 reg) {
    switch (reg) {
        case sdfGsAddrAlpha1:
        case sdfGsAddrAlpha2:
        case sdfGsAddrTest1:
        case sdfGsAddrTest2:
        case sdfGsAddrFrame1:
        case sdfGsAddrFrame2: return true;
        default: return false;
    }
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
    void reportUnreadPacket(PacketType type);

    bool replayRegisters(const ChainEntry& entry);
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
    GsRegisterReplay mRegs;

    /// TEX0 as the last 2D texture packet left it, which a sprite naming no texture samples.
    TexHandle mBoundTexture;

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
    for (u32 i = 0; i < mFrame.sprites.size(); i++) {
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
    entry.stateOnly = replayRegisters(entry);
    const u64 before = mEmitted;
    const PacketType type = getType(entry.at);
    if (isModelNodeType(type)) emitMeshes(entry);
    else processPacketOfType(entry, type);
    if (entry.isCall()) processCallee(chain, entry, &PacketProcessor::processModelPacket);
    recordCoverage(entry, before);
}

void PacketProcessor::processPacket(DmaChain& chain, ChainEntry entry) {
    entry.stateOnly = replayRegisters(entry);
    const u64 before = mEmitted;
    processPacketOfType(entry, getType(entry.at));
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
        // The frame's clear comes from dds3BackColor, and GS memory is not modelled.
        case PacketType::Clear:
        case PacketType::BitBlt:
        case PacketType::TextureUpload: reportUnreadPacket(type); break;
        case PacketType::Count: reportUnsupported("packet type", u32(type)); break;
    }
}

void PacketProcessor::processSpritePacket(const ChainEntry& entry) {
    emitSprites(entry);
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
    if (!h.modelHooks.fldSdwDraw(data, data + sdfQwordBytes * entry.tag.qwc, mRegs.getContext(0),
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
    emitSprites(entry);
    emitMeshes(entry);
}

void PacketProcessor::processTexBindPacket(const ChainEntry& entry) {
    const auto binds = h.texturePackets.find(entry.at);
    if (binds == h.texturePackets.end()) {
        reportUnsupported("2D texture packet with no recorded texture", entry.tag.qwc);
        return;
    }
    mBoundTexture = TexHandle{binds->second};
}

/// The replay has applied the registers the renderer takes from the chain; the rest are named.
void PacketProcessor::processGsStatePacket(const ChainEntry& entry, PacketType type) {
    if (!entry.stateOnly) {
        reportUnsupported("GS state packet that is more than A+D writes", u32(type));
    }
    const std::bitset<128>& untracked = mRegs.getUntracked();
    for (u32 reg = 0; reg < untracked.size(); reg++) {
        if (untracked[reg] && !isReplayedRegister(reg)) {
            reportUnsupported("GS register a state packet sets and the replay drops", reg);
        }
    }
}

/// Taken from the producer's call rather than the packet, so one that no producer with recorded
/// draws linked is unread.
void PacketProcessor::processCarriedPacket(const ChainEntry& entry, PacketType type) {
    if (!mClaimed.contains(entry.at)) reportUnreadPacket(type);
}

void PacketProcessor::processEmptyPacket(const ChainEntry& entry) {
    if (entry.tag.qwc) reportUnsupported("empty packet with a payload", entry.tag.qwc);
}

void PacketProcessor::reportUnreadPacket(PacketType type) {
    const std::string what = std::string("unread packet: ") + getPacketTypeName(type);
    reportUnsupported(what.c_str(), u32(type));
}

/// @return Whether the transfer was state alone.
bool PacketProcessor::replayRegisters(const ChainEntry& entry) {
    const sdfGsState_t was = mRegs.getContext(0);
    const bool stateOnly = mRegs.apply(entry.at);
    if (h.config->dumpDrawEnv) dumpStateChange(was, entry.at);
    return stateOnly;
}

void PacketProcessor::dumpStateChange(const sdfGsState_t& was, u32 at) {
    const sdfGsState_t& gs = mRegs.getContext(0);
    if (gs.alpha == was.alpha && gs.test == was.test && gs.frameMask == was.frameMask) return;
    const LinkedPacket* linked = findLinked(at);
    log.dump(JsonObject()
                 .addString("dump", "gsState")
                 .addUint("frame", h.trace->getFrame())
                 .addString("by", linked ? getLinker(*linked) : "-")
                 .addHexDigits("alpha", gs.alpha, 2)
                 .addHexDigits("test", gs.test, 6)
                 .addHexDigits("fbmsk", gs.frameMask, 8)
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
    const sdfGsState_t& gs = mRegs.getContext(0);
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
        if (!h.config->noTransmitState) mesh.gs = mRegs.getContext(0);
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
