#include "packet_processor.h"

#include <bitset>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gif.h"
#include "gs_decoder.h"
#include "hooks_impl.h"
#include "hooks_impl_model.h"
#include "json.h"
#include "libsdf.h"
#include "log.h"
#include "packet_type.h"
#include "render/sprite.h"
#include "trace.h"
#include "unsupported.h"

namespace sdfhook {

namespace {

LogChannel log("packets");

/// One side of a GS memory transfer: a buffer, and the rectangle within it.
struct GsRegion {
    u32 base = 0;
    u32 bufferWidth = 0;
    u32 psm = 0;
    u32 x = 0;
    u32 y = 0;
    u32 width = 0;
    u32 height = 0;

    bool operator==(const GsRegion&) const = default;
};

GsRegion getTransferSource(const sdfPrimBitBltElem_t& bitblt) {
    const u64 buf = bitblt.bitbltbuf.data;
    const u64 pos = bitblt.trxpos.data;
    const u64 size = bitblt.trxreg.data;
    return {u32(buf & 0x3FFF),        u32((buf >> 16) & 0x3F), u32((buf >> 24) & 0x3F),
            u32(pos & 0x7FF),         u32((pos >> 16) & 0x7FF), u32(size & 0xFFF),
            u32((size >> 32) & 0xFFF)};
}

GsRegion getTransferDestination(const sdfPrimBitBltElem_t& bitblt) {
    const u64 buf = bitblt.bitbltbuf.data;
    const u64 pos = bitblt.trxpos.data;
    const u64 size = bitblt.trxreg.data;
    return {u32((buf >> 32) & 0x3FFF), u32((buf >> 48) & 0x3F), u32((buf >> 56) & 0x3F),
            u32((pos >> 32) & 0x7FF),  u32((pos >> 48) & 0x7FF), u32(size & 0xFFF),
            u32((size >> 32) & 0xFFF)};
}

bool isDepthFormat(u32 psm) {
    return psm == sdfPsmz32 || psm == sdfPsmz24 || psm == sdfPsmz16 || psm == sdfPsmz16s;
}

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
        case PacketType::ImageStore:
        case PacketType::ImageLoad:
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
    void processTransformPacket(const ChainEntry& entry);
    void processMaterialPacket(const ChainEntry& entry);
    void processLightPacket(const ChainEntry& entry);
    void processBillboardTransformPacket(const ChainEntry& entry);
    void placeBillboardCorners(DrawMesh& mesh) const;
    void processFogPacket(const ChainEntry& entry);
    void processViewportPacket(const ChainEntry& entry);
    void checkChainLighting(const DrawMesh& mesh);
    void processEmptyPacket(const ChainEntry& entry);
    void processImageStorePacket(const ChainEntry& entry);
    void processImageLoadPacket(const ChainEntry& entry);
    void reportUnreadPacket(u32 at, PacketType type);

    bool decodeTransfer(const ChainEntry& entry);
    void emitDecodedPrimitives(const ChainEntry& entry);
    bool convertPrimitive(const GsPrimitive& primitive, DrawSprite* sprite);
    f32 getDepth(u32 z);
    enum class Verdict { Honoured, Invisible, Unhonoured };
    Verdict judgePrimitive(const GsPrimitive& primitive, const DrawSprite& sprite,
                           const char** why) const;
    void reportUnhonoured(const GsPrimitive& primitive);
    void dumpPrimitive(const GsPrimitive& primitive, const DrawSprite& sprite) const;
    TexHandle resolveTexture(u64 tex0);
    void takeChainTexture(DrawMesh& mesh);
    void takePrimMaterial(DrawMesh& mesh);
    void scaleTexelUvs(DrawMesh& mesh);
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

    /// What sdf3DPrimSetTmPacket last put in the chain, identity until then and after a reset.
    Matrix4 mPrimTransform{};
    sdfMatPacketPtr_t mPrimMaterial;
    sdf3DLightParallelPacketPtr_t mChainLight;
    sdf3DFogPacketPtr_t mChainFog;
    f32 mBillboardTm[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};

    /// The one z past 24 bits this frame, which the depth clamp orders nearest of all.
    u32 mWideZ = 0;

    /// The depth buffer a store read back and no load has put back yet.
    struct SavedDepth {
        GsRegion region;
        u32 buffer = 0;
    };
    std::optional<SavedDepth> mSavedDepth;

    /// The store packet whose own transfer reads GS memory back.
    u32 mStoreAt = 0;
    u32 mIdentityTm = 0;

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
    for (int axis = 0; axis < 4; axis++) mPrimTransform.m[axis][axis] = 1.0f;
    mIdentityTm = pcsx2hook_find_symbol(h.runtime, "sdf3DPrimIdentityTm");
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
    // sdf3DPrimLinkTm writes the tag and has sdf3DPrimSetTmPacket fill the quadwords after it.
    const auto filled = h.packetTypes.find(at + sdfQwordBytes);
    if (filled != h.packetTypes.end() && filled->second == PacketType::Transform) return filled->second;
    // sdf3DPrimLinkResetTm REFs the engine's own identity matrix.
    const sdfDmaTagPtr_t tag{at};
    // A model sends its root's light and fog by a REF to the data past the packet's own tag.
    if (tag && sdfDmaIsRef(*tag)) {
        const u32 packet = (tag->addr & 0x0FFFFFFFu) - sdfQwordBytes;
        if (h.model.isLightPacket(packet)) return PacketType::LightParallel;
        if (h.model.isFogPacket(packet)) return PacketType::Fog;
    }
    if (mIdentityTm && tag && sdfDmaIsRef(*tag) && (tag->addr & 0x0FFFFFFFu) == mIdentityTm) {
        return getPacketTypeByGlobal("sdf3DPrimIdentityTm");
    }
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
        case PacketType::GifRegisters:
        case PacketType::DirectRef: processGsStatePacket(entry, type); break;
        case PacketType::Transform: processTransformPacket(entry); break;
        case PacketType::LightParallel: processLightPacket(entry); break;
        case PacketType::Fog: processFogPacket(entry); break;
        case PacketType::Viewport: processViewportPacket(entry); break;
        case PacketType::Material: processMaterialPacket(entry); break;
        // Our shaders stand in for the microcode, so there is nothing in it to read.
        case PacketType::VuMicrocode: break;
        case PacketType::Terminator: processEmptyPacket(entry); break;
        case PacketType::BillboardTransform: processBillboardTransformPacket(entry); break;
        case PacketType::TransformRef:
        case PacketType::MaterialRef:
        case PacketType::DrawParams:
        case PacketType::PacketHeader:
        case PacketType::VifSetup: processCarriedPacket(entry, type); break;
        case PacketType::Empty: processEmptyPacket(entry); break;
        case PacketType::Clear: processSpritePacket(entry); break;
        // Its transfer stales what was read from GS memory there, which decodeTransfer passes on.
        case PacketType::BitBlt: break;
        case PacketType::ImageStore: processImageStorePacket(entry); break;
        case PacketType::ImageLoad: processImageLoadPacket(entry); break;
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
    if (!mClaimed.contains(entry.at)) reportUnreadPacket(entry.at, type);
}

/// sdf3DPrimSetTmPacket's matrix, which VU1 places the polys after it by. A model node's own
/// transforms are its span's, and a producer's hook may still carry this one.
void PacketProcessor::processTransformPacket(const ChainEntry& entry) {
    const u32 target = entry.getRefTarget();
    const sdfMatrix4Ptr_t matrix{target ? target : entry.at + sdfQwordBytes};
    if (!matrix) {
        reportUnsupported("transform packet outside guest memory", entry.at);
        return;
    }
    std::memcpy(&mPrimTransform, &*matrix, sizeof(mPrimTransform));
}

/// matrixScale's axes and offset (0x05FB), which the sprite programs build every corner from.
void PacketProcessor::processBillboardTransformPacket(const ChainEntry& entry) {
    const sdfBillBoardTMPacketPtr_t packet{entry.at};
    if (!packet || entry.getRefTarget()) {
        reportUnsupported("billboard transform not sent inline", entry.getId());
        return;
    }
    std::memcpy(mBillboardTm, packet->tm, sizeof(mBillboardTm));
}

/// A corner is the centre plus axisU * x + axisV * y + the offset, in the camera's own plane.
void PacketProcessor::placeBillboardCorners(DrawMesh& mesh) const {
    static constexpr f32 kIdentity[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    if (std::memcmp(mBillboardTm, kIdentity, sizeof(kIdentity)) == 0) return;
    const DrawMesh::BillboardFrame& frame = mesh.billboardFrame;
    for (usize c = 0; c < 4 && c < mesh.vertices.size(); c++) {
        const f32 x = frame.offset[c][0];
        const f32 y = frame.offset[c][1];
        const f32 across = mBillboardTm[0] * x + mBillboardTm[2] * y + mBillboardTm[4];
        const f32 down = mBillboardTm[1] * x + mBillboardTm[3] * y + mBillboardTm[5];
        MeshInfluence& at = mesh.vertices[c].influence[0];
        at.x = frame.centre[0] + frame.right[0] * across + frame.up[0] * down;
        at.y = frame.centre[1] + frame.right[1] * across + frame.up[1] * down;
        at.z = frame.centre[2] + frame.right[2] * across + frame.up[2] * down;
    }
}

/// VU1 lights and fogs a stream with the last light and fog the chain uploaded, whoever linked
/// them; a stream mesh is checked against them where it is placed.
void PacketProcessor::processLightPacket(const ChainEntry& entry) {
    const u32 target = entry.getRefTarget();
    mChainLight = sdf3DLightParallelPacketPtr_t{target ? target - sdfQwordBytes : entry.at};
}

void PacketProcessor::processFogPacket(const ChainEntry& entry) {
    const u32 target = entry.getRefTarget();
    mChainFog = sdf3DFogPacketPtr_t{target ? target - sdfQwordBytes : entry.at};
}

/// VU1's screen mapping, which the meshes' depth is built from.
void PacketProcessor::processViewportPacket(const ChainEntry& entry) {
    const sdf3DViewportPacketPtr_t viewport{entry.getRefTarget() ? entry.getRefTarget() : entry.at};
    if (!viewport) {
        reportUnsupported("viewport packet outside guest memory", entry.at);
        return;
    }
    // A frame with no model walk never read sdf3DScreenParam, and its polys still need a depth.
    if (mFrame.depthScale == 0.0f && mFrame.depthOffset == 0.0f) {
        mFrame.depthScale = viewport->scale.z;
        mFrame.depthOffset = viewport->trans.z;
    } else if (viewport->scale.z != mFrame.depthScale || viewport->trans.z != mFrame.depthOffset) {
        reportUnsupported("viewport packet with a depth mapping other than sdf3DScreenParam's", 0);
    }
}

void PacketProcessor::checkChainLighting(const DrawMesh& mesh) {
    if (mesh.packetType != sdfPacketDmaCall) return;
    if (mChainLight) {
        const MeshLighting sent = getLighting(*mChainLight);
        if (std::memcmp(sent.direction, mesh.lighting.direction, sizeof(sent.direction)) ||
            std::memcmp(sent.color, mesh.lighting.color, sizeof(sent.color)) ||
            std::memcmp(sent.base, mesh.lighting.base, sizeof(sent.base)) ||
            std::memcmp(sent.fullColor, mesh.lighting.fullColor, sizeof(sent.fullColor))) {
            reportUnsupported("stream mesh lit other than by the chain's light", mChainLight.addr);
        }
    }
    if (mChainFog && mesh.fog.on) {
        const MeshFog sent = getFog(*mChainFog);
        if (sent.scale != mesh.fog.scale || sent.offset != mesh.fog.offset ||
            sent.minimum != mesh.fog.minimum || sent.maximum != mesh.fog.maximum ||
            std::memcmp(sent.color, mesh.fog.color, sizeof(sent.color))) {
            reportUnsupported("stream mesh fogged other than by the chain's fog", mChainFog.addr);
        }
    }
}

/// sdf3DPrimLinkMat's material, which VU1 textures the polys after it with. A model node's own
/// materials are its span's.
void PacketProcessor::processMaterialPacket(const ChainEntry& entry) {
    const u32 target = entry.getRefTarget();
    if (target) mPrimMaterial = sdfMatPacketPtr_t{target};
    else reportUnsupported("material packet with no REF", entry.getId());
}

/// fldDrwZSave's read of the depth buffer, which a load puts back after passes that overwrite it.
void PacketProcessor::processImageStorePacket(const ChainEntry& entry) {
    const sdfStoreImagePacketPtr_t packet{entry.at};
    if (!packet) {
        reportUnsupported("image store outside guest memory", entry.at);
        return;
    }
    mStoreAt = entry.at;
    const GsRegion source = getTransferSource(packet->bitblt);
    const u32 zbp = u32(mDecoder.getRegister(sdfGsAddrZbuf1) & 0x1FF) * 32;
    if (!isDepthFormat(source.psm) || source.base != zbp) {
        reportUnsupported("GS read back of other than the depth buffer", source.base);
        return;
    }
    if (packet->callback) reportUnsupported("GS read back handed to a callback", packet->callback);
    if (source.x || source.y || f32(source.width) < kGuestWidth || f32(source.height) < kGuestHeight) {
        reportUnsupported("GS read back of part of the depth buffer", source.width);
    }
    if (mSavedDepth) reportUnsupported("depth read back twice before a load", source.base);
    mSavedDepth = SavedDepth{source, packet->addr.addr & 0x0FFFFFFFu};
    mFrame.order.push_back({DrawRef::Kind::SaveDepth, 0});
}

/// fldDrwZLoad puts the depth buffer back from the buffer a store filled.
void PacketProcessor::processImageLoadPacket(const ChainEntry& entry) {
    const sdfLoadImagePacketPtr_t packet{entry.at};
    if (!packet) {
        reportUnsupported("image load outside guest memory", entry.at);
        return;
    }
    const GsRegion destination = getTransferDestination(packet->bitblt);
    const u32 buffer = packet->dmatagData.addr & 0x0FFFFFFFu;
    if (!mSavedDepth || mSavedDepth->buffer != buffer || !(mSavedDepth->region == destination)) {
        reportUnsupported("image load that restores no saved depth", destination.base);
        return;
    }
    mSavedDepth.reset();
    mFrame.order.push_back({DrawRef::Kind::RestoreDepth, 0});
}

void PacketProcessor::processEmptyPacket(const ChainEntry& entry) {
    if (entry.tag.qwc) reportUnsupported("empty packet with a payload", entry.tag.qwc);
}

void PacketProcessor::reportUnreadPacket(u32 at, PacketType type) {
    const LinkedPacket* linked = findLinked(at);
    const std::string what = std::string("unread packet: ") + getPacketTypeName(type) + " from " +
                             (linked ? getLinker(*linked) : "an unlinked tag");
    reportUnsupported(what.c_str(), u32(type));
}

/// @return Whether the transfer was state alone.
bool PacketProcessor::decodeTransfer(const ChainEntry& entry) {
    const sdfGsState_t was = mDecoder.getContext(0);
    const u32 target = entry.getRefTarget();
    mDecoder.feed(entry.tag, target ? target : entry.at + sdfQwordBytes);
    for (const GsUpload& upload : mDecoder.getUploads()) {
        h.textures.onGsUpload(upload.bitbltbuf, upload.trxreg);
        const u32 dpsm = u32((upload.bitbltbuf >> 56) & 0x3F);
        if (isDepthFormat(dpsm) && getType(entry.at) != PacketType::ImageLoad) {
            reportUnsupported("depth buffer written by an IMAGE transfer", dpsm);
        }
    }
    const bool inStore = mStoreAt && entry.at >= mStoreAt &&
                         entry.at < mStoreAt + sizeof(sdfStoreImagePacket_t);
    if (!mDecoder.getReadbacks().empty() && !inStore) reportUnsupported("GS read back to the host", 0);
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
        const std::string& wanted = h.config->dumpDecoded;
        if (!wanted.empty() && (wanted == "*" || std::strstr(sprite.producer, wanted.c_str()))) {
            dumpPrimitive(primitive, sprite);
        }
        h.coverage.decodedDraws[sprite.producer]++;
        mFrame.order.push_back({DrawRef::Kind::Sprite, u32(mFrame.sprites.size())});
        mFrame.sprites.push_back(sprite);
        mEmitted++;
    }
}

/// GS z as normalised device depth, 1 - 2z / 2^24, which the meshes write too. Exact in the float
/// depth buffer for every 24-bit z; a wider one clamps to the near plane, still exact while it is
/// the only one (the HUD's 0xFFFFFFFF).
f32 PacketProcessor::getDepth(u32 z) {
    if (z >> 24) {
        if (mWideZ && z != mWideZ) reportUnsupported("second decoded z past 24 bits in a frame", z);
        mWideZ = z;
    }
    return 1.0f - 2.0f * (f32(z) / 16777216.0f);
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
        corner.z = getDepth(vertex.z);
        corner.fog = f32(vertex.fog) / 255.0f;
        return corner;
    };

    DrawSprite sprite{};
    const GsVertex* vertex = primitive.vertex;
    switch (sdfGetPrimType(prim)) {
        case sdfPrimSprite: {
            const SpriteCorner a = getCorner(vertex[0]);
            const SpriteCorner b = getCorner(vertex[1]);
            sprite = DrawSprite::makeRect(a.x, a.y, b.x - a.x, b.y - a.y, a.u, a.v, b.u, b.v);
            // A sprite takes the color and depth of the vertex that kicked it.
            sprite.setColor(b.color);
            sprite.setDepth(b.z);
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
            sprite.setDepth(b.z);
            break;
        }
        case sdfPrimPoint: {
            const SpriteCorner a = getCorner(vertex[0]);
            sprite = DrawSprite::makeRect(a.x, a.y, 1.0f, 1.0f, a.u, a.v, a.u, a.v);
            sprite.setColor(a.color);
            sprite.setDepth(a.z);
            break;
        }
        default: reportUnsupported("decoded PRIM type", sdfGetPrimType(prim)); return false;
    }
    sprite.blends = sdfGetPrimAbe(prim) != 0;
    sprite.alpha = state.alpha;
    sprite.test = u32(state.test);
    sprite.hasDepth = true;
    sprite.zbuf = state.zbuf;
    sprite.fogs = sdfGetPrimFge(prim) != 0;
    sprite.fogColor = Rgba::unpack(u32(state.fogcol & 0xFFFFFF));
    sprite.frameMask = sdfGetFrameMask(state.frame);
    mTextureMissing = false;
    if (sdfGetPrimTme(prim)) {
        sprite.texHandle = resolveTexture(state.tex0);
        sprite.hasSampler = true;
        sprite.textureFunction = sdfGetTex0Tfx(state.tex0);
        sprite.textureAlpha = sdfGetTex0Tcc(state.tex0) != 0;
        // AEM would make a black texel transparent instead; no scene sets it with a 24-bit texture.
        if (tex0.psm == sdfPsmct24) {
            if ((state.texa >> 15) & 1) reportUnsupported("24-bit texture under AEM", state.texa);
            sprite.texelAlpha = f32(state.texa & 0xFF) / 255.0f;
        }
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

    // Every fragment fails and keeps nothing.
    const u32 atst = sdfGetTestAtst(test);
    const u32 afail = sdfGetTestAfail(test);
    if (sdfGetTestAte(test) && atst == sdfAtstNever && afail == sdfAfailKeep) {
        return Verdict::Invisible;
    }
    if (sdfGetTestZte(test) && sdfGetTestZtst(test) == sdfZtstNever) return Verdict::Invisible;
    if (sdfGetPrimTme(prim)) {
        if (!sprite.texHandle.addr && mTextureMissing) return unhonoured("decoded texture not found");
    }
    if (state.pabe & 1) return unhonoured("decoded primitive under PABE");
    if (state.fba & 1) return unhonoured("decoded primitive under FBA");
    if (!(state.colclamp & 1)) return unhonoured("decoded primitive with COLCLAMP off");
    return Verdict::Honoured;
}

/// Differences reported but drawn as they are.
void PacketProcessor::reportUnhonoured(const GsPrimitive& primitive) {
    const GsDrawState& state = primitive.state;
    if (sdfGetPrimAa1(state.prim)) reportUnsupported("decoded primitive with antialiasing", state.prim);
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
/// VU1 emits a poly's TEX0, TEX1 and CLAMP out of the material packet sdf3DPrimLinkMat links
/// ahead of it, so the GIF path never carries them.
void PacketProcessor::takePrimMaterial(DrawMesh& mesh) {
    if (!mPrimMaterial) {
        reportUnsupported("textured poly with no material ahead of it in the chain", 0);
        return;
    }
    mesh.tex0 = mPrimMaterial->tex0Data;
    mesh.tex1 = mPrimMaterial->tex1Data;
    mesh.clamp = mPrimMaterial->clampData;
    mesh.texHandle = resolveTexture(mesh.tex0);
}

/// VU1 draws a billboard batch under the chain's TEX0; the hook sees only the last resource
/// tagged before the batches are made.
void PacketProcessor::takeChainTexture(DrawMesh& mesh) {
    mesh.tex0 = mDecoder.getRegister(sdfGsAddrTex01);
    mesh.tex1 = mDecoder.getRegister(sdfGsAddrTex11);
    mesh.clamp = mDecoder.getRegister(sdfGsAddrClamp1);
    mesh.texHandle = resolveTexture(mesh.tex0);
}

/// A billboard batch stores texel coordinates, which the GS takes over TW and TH.
void PacketProcessor::scaleTexelUvs(DrawMesh& mesh) {
    const GsTex0 tex0 = GsTex0::decode(mesh.tex0);
    const f32 uScale = 1.0f / f32(1u << tex0.tw);
    const f32 vScale = 1.0f / f32(1u << tex0.th);
    for (MeshVertex& vertex : mesh.vertices) {
        vertex.u *= uScale;
        vertex.v *= vScale;
    }
}

TexHandle PacketProcessor::resolveTexture(u64 tex0) {
    const u32 tbp0 = GsTex0::decode(tex0).tbp0;
    if (h.textures.isFrameBuffer(tbp0)) {
        h.textures.wantFrameSeed(tex0);
        return TexHandle{kPreviousFrameHandle};
    }
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
        if (mesh.billboard) {
            takeChainTexture(mesh);
            scaleTexelUvs(mesh);
            placeBillboardCorners(mesh);
        }
        if (mesh.chainTransform && (mesh.attrBits & sdfCoreAttrTextured)) takePrimMaterial(mesh);
        // VU1 places a poly by the transform in force at its packet.
        if (mesh.chainTransform && mesh.bones.size() == 1) mesh.bones[0] = mPrimTransform;
        checkChainLighting(mesh);
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
void readImageQueue(HookContext& h) {
    const sdfImageSetCbPtr_t cb{pcsx2hook_find_symbol(h.runtime, "sdfImageSetCb")};
    if (!cb || !cb->tagHead) return;
    const u32 tail = cb->tagTail.addr & 0x0FFFFFFFu;
    GsDecoder decoder;
    DmaChain chain(cb->tagHead.addr);
    ChainEntry entry;
    while (chain.next(entry) && entry.at != tail) {
        const u32 target = entry.getRefTarget();
        decoder.feed(entry.tag, target ? target : entry.at + sdfQwordBytes);
        for (const GsUpload& upload : decoder.getUploads()) {
            h.textures.onGsUpload(upload.bitbltbuf, upload.trxreg);
        }
    }
}

void processCommandStream(HookContext& h, u32 aTag, Frame& frame) {
    PacketProcessor(h, frame).run(aTag);
}

}  // namespace sdfhook
