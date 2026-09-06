#pragma once

#include <map>
#include <unordered_map>

#include "pcsx2hook/pcsx2hook.h"
#include "guest_ptr.h"
#include "config.h"
#include "render_state.h"
#include "libsdf.h"
#include "json.h"
#include "render/frame.h"
#include "types.h"

namespace sdfhook {

class TextureCache;
class Trace;

/// Every branch sdfModelLinkOt takes, counted. A scene that renders short names the branch that
/// swallowed it instead of leaving it to be guessed.
class ModelTrace {
public:
    enum class Node : u8 { Drawn, NoChain, Hidden, BoundsCulled, Unreadable, Count };
    enum class Packet : u8 { Generated, DmaCall, Shaped, Suppressed, Unknown, Count };
    enum class Reject : u8 { NoMaterial, Empty, BadBatch, BadElems, NoMorph, Count };

    /// A chain address that carried neither a material nor a mesh, and one that opened like a
    /// mesh and then would not decode.
    void countSetupRef() { mSetupRefs++; }
    void countStrayRef() { mStrayRefs++; }

    void countRoot(bool tempPackets);
    void countNode(Node outcome) { mNodes[usize(outcome)]++; }
    void countPacket(Packet outcome) { mPackets[usize(outcome)]++; }
    /// Bodies a generated packet carries, bucketed by the engine's own packet type. The parser
    /// dispatches on that type; how it is reported is this class's business alone.
    void countGeom(sdfCorePacketType_t type, u32 elems);
    void countMesh(sdfCorePacketType_t type, u32 elems);
    void countReject(sdfCorePacketType_t type, Reject why) { mRejects[usize(geomFor(type))][usize(why)]++; }

    /// A body inside a type 1 DMA call chain, which has no packet type of its own.
    /// A branch that is modelled but has never run, so a scene that reaches it says so rather than
    /// rendering short in silence.
    void countTempPacketWalk() { mTempPacketWalks++; }
    void countShapedWalk() { mShapedWalks++; }

    void countStreamGeom(u32 elems);
    void countStreamMesh(u32 elems);

    void countPacketType(u8 type, u16 attrBits, u16 packetBits);

    /// How many bones a mesh blends, and how many of them one vertex actually uses, so the
    /// palette size and the influence cap are measured rather than assumed.
    void countBones(u32 bones) { mBones[bones]++; }

    /// What survives the node's own attr mask, which is what the shader branches on.
    void countAttr(u16 attrBits) { mAttr[attrBits]++; }

    /// What the linked packets left in force, to be checked against the GS trace.
    void countGs(const sdfGsState_t& gs) { mGs[(gs.alpha << 32) | (gs.test & 0xFFFFFFFF)]++; }
    void countOt(u8 otIndex) { mOt[otIndex]++; }
    void countMultiTexture(bool on) { (on ? mMultiTextured : mSingleTextured)++; }
    void countTexMatrix(bool present) { (present ? mTexMatrix : mNoTexMatrix)++; }
    void countSpecular(u16 attrBits, u32 colour) {
        if (attrBits & 0x1) mSpecular[colour]++;
    }
    void countSampler(u64 tex0, u64 tex1, u64 clamp) {
        mSampler[(tex0 >> 34) & 7][(tex1 >> 5) & 0x3F][clamp & 0xF]++;
    }

    /// Our walk runs at sdfModelLinkOt's entry, before its own sdfMatListUpdatePacket, so a
    /// material whose duty bit for this frame is set has a packet we are reading one generation
    /// stale. Counted rather than assumed: for a static material the two are identical.
    void countMatDuty(u8 duty, u32 frameId) {
        ((duty >> frameId) & 1 ? mStaleMatPackets : mFreshMatPackets)++;
    }

    /// A mesh whose material names a texture the cache has nothing for draws flat, so the two are
    /// counted apart: an untextured scene reads as a lighting problem otherwise.
    void countTexture(bool named, bool found) {
        if (!named) mUntexturedMeshes++;
        else if (found) mTexturedMeshes++;
        else mMissingTextureMeshes++;
    }

    /// A mirrored transform reverses the winding of everything it draws.
    void countDeterminant(const Matrix4& m) {
        const f32 d = m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) -
                      m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0]) +
                      m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
        (d < 0.0f ? mMirroredNodes : mUprightNodes)++;
    }
    void countInfluences(u32 used) { mInfluences[used]++; }

    /// The engine's own calls into the two functions only sdfModelLinkOt makes, so equal counts
    /// mean our traversal reached exactly what it reached.
    void countGuestGenerated() { mGuestGenerated++; }
    void countGuestShaped() { mGuestShaped++; }

    /// The summary a human reads after a run.
    void report() const;

    /// The same counters as one JSON object, so two runs can be diffed rather than eyeballed. Every
    /// counter is written, including the zeros the text suppresses: a reader wants the absence too.
    void addTo(JsonObject& row) const;

private:
    /// How a body is bucketed for the report. Not the parser's concern, so it never leaves here.
    enum class Geom : u8 { Rigid, Weighted, BlendShape, VifStream, Unknown, Count };

    static constexpr Geom geomFor(sdfCorePacketType_t type) {
        switch (type) {
            case sdfCorePacketTypeRigid: return Geom::Rigid;
            case sdfCorePacketTypeWeighted: return Geom::Weighted;
            case sdfCorePacketTypeBlendShape: return Geom::BlendShape;
        }
        return Geom::Unknown;
    }

    static const char* geomName(Geom shape);

    void addGeom(Geom shape, u32 elems);
    void addMesh(Geom shape, u32 elems);

    struct Shape {
        u64 seen = 0;
        u64 seenElems = 0;
        u64 drawn = 0;
        u64 drawnElems = 0;
    };

    u64 mRoots = 0;
    u64 mTempPacketRoots = 0;
    u64 mNodes[usize(Node::Count)] = {};
    u64 mPackets[usize(Packet::Count)] = {};
    Shape mGeom[usize(Geom::Count)];
    u64 mRejects[usize(Geom::Count)][usize(Reject::Count)] = {};
    std::map<u8, std::map<u32, u64>> mTypes;
    std::map<u32, u64> mAttr;
    std::map<u64, u64> mGs;
    std::map<u32, u64> mOt;
    u64 mMultiTextured = 0;
    u64 mSingleTextured = 0;
    u64 mTexMatrix = 0;
    u64 mNoTexMatrix = 0;
    std::map<u32, u64> mSpecular;
    std::map<u64, std::map<u64, std::map<u64, u64>>> mSampler;
    u64 mTempPacketWalks = 0;
    u64 mShapedWalks = 0;
    u64 mStaleMatPackets = 0;
    u64 mFreshMatPackets = 0;
    u64 mTexturedMeshes = 0;
    u64 mUntexturedMeshes = 0;
    u64 mMissingTextureMeshes = 0;
    u64 mUprightNodes = 0;
    u64 mMirroredNodes = 0;
    std::map<u32, u64> mBones;
    std::map<u32, u64> mInfluences;
    u64 mSetupRefs = 0;
    u64 mStrayRefs = 0;
    u64 mGuestGenerated = 0;
    u64 mGuestShaped = 0;
};

/// The engine's own scene walk, on our side: same node list, same culling, same packet chains, so
/// what it files into an ordering table we turn into meshes instead.
/// @fn sdfModelLinkOt
/// @note SLUS-20911 0x002B1DB8
class ModelParser {
public:
    void attach(pcsx2hook_ctx* runtime, const Config& config, TextureCache* textures,
                Trace* trace, const RenderState* state);

    void sdfModelLinkOt(sdfModelRootNodePtr_t aRoot, Frame* frame);

    ModelTrace& getTrace() { return mTrace; }
    const ModelTrace& getTrace() const { return mTrace; }

    /// The GS state the linked packets have left in force, tracked across a frame's whole chain.
    sdfGsState_t& getGsState() { return mGsState; }

private:
    struct NodeDraw {
        Matrix4 model;
        Matrix4 clip;
        f32 sortZ;
        f32 zBias;
        u32 colour;
        bool reverse;
        u16 attrMask;
        u8 otIndex;
    };

    /// @fn sdfMatMakePacket
    /// @note SLUS-20911 0x002B3558
    void readMaterialPacket(const sdfMat_t& material, DrawMesh& mesh,
                            Frame* frame);

    void readLighting(sdf3DLightParallelPacketPtr_t aLight);
    void readFog(sdf3DFogPacketPtr_t aFog);
    bool readScreenTm();

    /// @fn sdfModelCheckBounds
    /// @note SLUS-20911 0x002B1838
    bool sdfModelCheckBounds(sdfModelBoundsPtr_t aBounds, const Matrix4& clip) const;

    void walkChain(guest_ref<sdfModelRootNode_t> root, const NodeDraw& draw,
                   u32 chainAddr, Frame* frame);

    /// @fn sdfCorePacketGen
    /// @note SLUS-20911 0x002B9178
    void sdfCorePacketGen(guest_ref<sdfModelRootNode_t> root, const NodeDraw& draw,
                         sdfCorePacketPtr_t aPacket, Frame* frame);

    void dumpWords(const char* what, u32 at);
    void dumpPayload(sdfCorePacketPtr_t aPacket);

    /// One batch at a time, until the mesh's own vertex count is met. A batch caps at what fits
    /// in VU0 memory, so anything larger arrives split and the triangles index the whole.
    bool readRigidBatches(u32 at, const sdfCorePacket_t& packet, DrawMesh& mesh);

    /// @fn corePktSkinVertFirst
    /// @note SLUS-20911 0x002B6BC0
    bool readWeightedBatches(u32 at, const sdfCorePacket_t& packet,
                             u32 nodesAddr, DrawMesh& mesh);

    /// Blend shapes are plain arrays rather than VIF-tagged, and the shapes past the first are
    /// deltas onto it, weighted by the root's own morph controller.
    bool readBlendShapes(u32 at, const sdfCorePacket_t& packet,
                         guest_ref<sdfModelRootNode_t> root, DrawMesh& mesh);

    /// A DMA call packet holds a chain of material and mesh pairs, each mesh a VIF stream the
    /// hardware would run directly. Its arrays are tagged the same way a batch's are.
    /// The root's temporary packet list, walked over the same nodes. Never seen in any sample
    /// scene, which is a fact about the savestates, not about the engine.
    /// @fn sdfModelTempPacketLinkOt
    /// @note SLUS-20911 0x002B1950
    void sdfModelTempPacketLinkOt(guest_ref<sdfModelRootNode_t> root, Frame* frame);

    /// The type 2 path: the morph list picks up to two blend targets and their streams are chained
    /// behind the mesh's own. Also unseen in every sample scene.
    /// @fn sdfModelLinkShapedDrawPacket
    /// @note SLUS-20911 0x002B0CB0
    void sdfModelLinkShapedDrawPacket(guest_ref<sdfModelRootNode_t> root, const NodeDraw& draw,
                                      u32 dataAddr, Frame* frame);

    /// @fn sdfModelSetNormalDrawPacket
    /// @note SLUS-20911 0x002B0AC0
    void sdfModelSetNormalDrawPacket(const NodeDraw& draw, u32 chainAddr, Frame* frame);

    bool readVifStreamMesh(u32 at, u32 end, const NodeDraw& draw, sdfMatPtr_t aMat,
                           Frame* frame);

    /// @fn corePktBuildFaceList
    /// @note SLUS-20911 0x002B7A80
    bool corePktBuildFaceList(u32 at, const sdfCorePacket_t& packet, DrawMesh& mesh);

    void readSecondUv(u32 at, const sdfCorePacket_t& packet, DrawMesh& mesh);

    /// @fn corePktEmitTexTris
    /// @note SLUS-20911 0x002B85D8
    bool finishMesh(guest_ref<sdfModelRootNode_t> root, const sdfCorePacket_t& packet,
                    sdfCorePacketType_t type, DrawMesh& mesh, Frame* frame);

    pcsx2hook_ctx* mRuntime = nullptr;
    TextureCache* mTextures = nullptr;
    const Config* mConfig = nullptr;
    Trace* mEvents = nullptr;
    const RenderState* mState = nullptr;
    // The engine's own globals, resolved once from the symbol map and named for it.
    Matrix4Ptr_t mSdf3DScreenTm;
    u32Ptr_t mSdfFrameId;
    f32Ptr_t mSdfFaceDivBias;
    f32Ptr_t mSdfFaceDivSoften;
    u8Ptr_t mCorePktSoftenBias;
    sdf3DLightDefaultPtr_t mSdf3DLightDefault;
    sdf3DFogDefaultPtr_t mSdf3DFogDefault;

    u32 mNodeListAddr = 0;
    MeshLighting mLighting{};
    MeshFog mFog{};
    sdfGsState_t mGsState{};

    /// Every material's own DMA packet this frame, so a chain address names the material that
    /// built it rather than being guessed at from its size.
    std::unordered_map<u32, u32> mMatByPacket;
    Matrix4 mScreenTm{};
    ModelTrace mTrace;
};

}  // namespace sdfhook
