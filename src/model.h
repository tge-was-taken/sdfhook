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

class ModelHooks;
class TextureCache;
class Trace;

/// Counted so a scene that renders short names the branch that swallowed it.
class ModelTrace {
public:
    enum class Node : u8 { Drawn, NoChain, Hidden, BoundsCulled, Unreadable, Count };
    enum class Packet : u8 { Generated, DmaCall, Shaped, Suppressed, Unknown, Count };
    enum class Reject : u8 { NoMaterial, Empty, BadBatch, BadElems, NoMorph, Count };

    /// Carried neither a material nor a mesh, and opened like a mesh but would not decode.
    void countSetupRef() { mSetupRefs++; }

    void countSettledMesh() { mSettledMeshes++; }
    void countUnsettledMesh() { mUnsettledMeshes++; }
    void countStrayRef() { mStrayRefs++; }

    void countRoot(bool tempPackets);
    void countNode(Node outcome) { mNodes[usize(outcome)]++; }
    void countPacket(Packet outcome) { mPackets[usize(outcome)]++; }
    /// The parser dispatches on the engine's type; the bucketing is this class's alone.
    void countGeom(sdfCorePacketType_t type, u32 elems);
    void countMesh(sdfCorePacketType_t type, u32 elems);
    void countReject(sdfCorePacketType_t type, Reject why) { mRejects[usize(geomFor(type))][usize(why)]++; }

    /// A body inside a type 1 DMA call chain, which has no packet type of its own.
    /// Modelled but never run, so a scene reaching it says so.
    void countTempPacketWalk() { mTempPacketWalks++; }
    void countShapedWalk() { mShapedWalks++; }

    void countStreamGeom(u32 elems);
    void countStreamMesh(u32 elems);

    void countPacketType(u8 type, u16 attrBits, u16 packetBits);

    /// So the palette size and the influence cap are measured rather than assumed.
    void countBones(u32 bones) { mBones[bones]++; }

    /// What survives the node's own attr mask, which is what the shader branches on.
    void countAttr(u16 attrBits) { mAttr[attrBits]++; }

    /// What the linked packets left in force, to be checked against the GS trace.
    void countGs(const sdfGsState_t& gs) { mGs[(gs.alpha << 32) | (gs.test & 0xFFFFFFFF)]++; }
    void countOt(u8 otIndex) { mOt[otIndex]++; }
    void countMultiTexture(bool on) { (on ? mMultiTextured : mSingleTextured)++; }
    void countTexMatrix(bool present) { (present ? mTexMatrix : mNoTexMatrix)++; }
    void countSpecular(u16 attrBits, u32 color) {
        if (attrBits & 0x1) mSpecular[color]++;
    }
    void countSampler(u64 tex0, u64 tex1, u64 clamp) {
        mSampler[(tex0 >> 34) & 7][(tex1 >> 5) & 0x3F][clamp & 0xF]++;
    }

    /// Our walk runs before sdfMatListUpdatePacket, so a duty bit set this frame means one
    /// generation stale. Counted, since a static material reads the same either way.
    void countMatDuty(u8 duty, u32 frameId) {
        ((duty >> frameId) & 1 ? mStaleMatPackets : mFreshMatPackets)++;
    }

    /// Counted apart from untextured, or a scene missing its textures reads as a lighting bug.
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

    /// One material packet, two meshes: disagreement means a field kept its struct default, which
    /// shades plausibly and shows only as a seam.
    void countMaterialShading(u32 packetAddr, u64 shading) {
        const auto known = mMaterialShading.find(packetAddr);
        if (known == mMaterialShading.end()) {
            mMaterialShading.emplace(packetAddr, shading);
        } else if (known->second != shading) {
            mSplitMaterials++;
        }
    }

    void countGuestGenerated() { mGuestGenerated++; }
    void countGuestShaped() { mGuestShaped++; }

    /// For a human.
    void report() const;

    /// The same counters as one JSON object so two runs diff. Zeros included: absence counts.
    void addTo(JsonObject& row) const;

private:
    /// Reporting only, never the parser's concern, so it does not leave here.
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
    u64 mSettledMeshes = 0;
    u64 mUnsettledMeshes = 0;
    u64 mGuestGenerated = 0;
    u64 mGuestShaped = 0;
    std::map<u32, u64> mMaterialShading;
    u64 mSplitMaterials = 0;
};

/// Same node list, culling and chains as sdfModelLinkOt, into meshes rather than table entries.
/// @fn sdfModelLinkOt
/// @note SLUS-20911 0x002B1DB8
class ModelParser {
public:
    /// The hook implementations run over this state; ModelParser decodes the formats they hand it.
    friend class ModelHooks;

    void setHooks(ModelHooks* hooks) { mHooks = hooks; }
    void attach(pcsx2hook_ctx* runtime, const Config& config, TextureCache* textures,
                Trace* trace, const RenderState* state);

    /// Says whether a producer hands world or projected coordinates. Magnitude alone cannot.
    /// @todo TODO.md V19: nothing calls it while that entry's remaining producers are unhooked.
    bool isWorldSpace(const sdfVector3_t& point, f32 out[3]) const;

    /// Spans the frame, not one root: every root links into the chain a mesh is matched against.
    void beginFrame(u32 frame) {
        mLinkIndex = 0;
        mDecodeFrame = frame;
    }

    /// Needs the frame's chain: a row recorded at decode carries the table's default instead.
    void traceMeshes(const Frame& frame);

    ModelTrace& getTrace() { return mTrace; }
    const ModelTrace& getTrace() const { return mTrace; }

    void setBillboardTexture(sdfTexHandlePtr_t aTex) { mBillboardTexture = aTex; }

    /// The GS state the linked packets have left in force, tracked across a frame's whole chain.
    sdfGsState_t& getGsState() { return mGsState; }

private:
    struct NodeDraw {
        Matrix4 model;
        Matrix4 clip;
        f32 sortZ;
        f32 faceDivSoften;
        f32 faceDivBias;
        u32 color;
        bool reverse;
        u16 attrMask;
        u8 otIndex;
    };

    bool readScreenTm();

    void dumpWords(const char* what, u32 at);
    void dumpPayload(sdfCorePacketPtr_t aPacket);

    /// A batch caps at VU0 memory, so a larger mesh arrives split, triangles indexing the whole.
    bool readRigidBatches(u32 at, const sdfCorePacket_t& packet, DrawMesh& mesh);

    /// Plain arrays, not VIF-tagged. Shapes past the first are deltas, weighted by the root's
    /// morph controller.
    bool readBlendShapes(u32 at, const sdfCorePacket_t& packet,
                         guest_ref<sdfModelRootNode_t> root, DrawMesh& mesh);

    /// Over the same nodes. Unseen in every sample scene, which is about the savestates.
    /// @fn sdfModelTempPacketLinkOt
    /// @note SLUS-20911 0x002B1950
    void reportFaceDiv(const char* what, f32 value);

    bool readVifStreamMesh(u32 at, u32 end, const NodeDraw& draw, sdfMatPtr_t aMat,
                           Frame* frame);

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
    guest_ptr<u32> mSdf3DSpecularTex;
    guest_ptr<sdfVector4_t> mSdf3DScreenParam;
    f32 mProjectionScale[2] = {1.0f, 1.0f};
    u32Ptr_t mSdfFrameId;
    f32Ptr_t mSdfFaceDivBias;
    f32Ptr_t mSdfFaceDivSoften;
    u8Ptr_t mCorePktSoftenBias;
    u64Ptr_t mSdf3DRefMapParam;
    sdf3DLightDefaultPtr_t mSdf3DLightDefault;
    sdf3DFogDefaultPtr_t mSdf3DFogDefault;

    // $f12 and $f13 into sdfCorePacketGen. The walk runs ahead of generation, so a frame uses
    // the previous frame's pair and only the first falls back to the fitted config values.
    f32 mFaceDivSoftenTerm = 0.0f;
    f32 mFaceDivBiasTerm = 0.0f;
    bool mFaceDivTermsSeen = false;
    bool mFaceDivParmSeen = false;
    // Per material packet, so the second pass interpolates the engine's endpoints, not ours.
    std::unordered_map<u32, MeshRamp> mFaceDivRamps;

    sdfModelNodeListPtr_t mNodeList;
    MeshLighting mLighting{};
    MeshFog mFog{};
    sdfGsState_t mGsState{};
    /// sdfBillBoardMakeN gets the geometry alone, so the handle comes from the call that fills.
    sdfTexHandlePtr_t mBillboardTexture;
    dds3BillboardTagPtr_t mDds3BillboardTag;
    std::unordered_map<u32, u32> mBillboardOt;
    u32 mLinkIndex = 0;
    u32 mCurrentChain = 0;
    u32 mDecodeFrame = 0;
    u32 mCurrentLink = 0;

    /// So a chain address names its material rather than being guessed from its size.
    std::unordered_map<u32, u32> mMatByPacket;
    Matrix4 mScreenTm{};
    ModelTrace mTrace;
    ModelHooks* mHooks = nullptr;
};

}  // namespace sdfhook
