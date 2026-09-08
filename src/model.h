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

class ModelParser {
public:
    void attach(pcsx2hook_ctx* runtime, const Config& config, TextureCache* textures,
                Trace* trace);

    /// Says whether a producer hands world or projected coordinates. Magnitude alone cannot.
    /// @todo TODO.md V19: nothing calls it while that entry's remaining producers are unhooked.
    bool isWorldSpace(const sdfVector3_t& point, f32 out[3]) const;

    /// Spans the frame, not one root: every root links into the chain a mesh is matched against.
    void beginFrame(u32 frame);

    /// Needs the frame's chain: a row recorded at decode carries the table's default instead.
    void traceMeshes(const Frame& frame);

    ModelTrace& getTrace() { return mTrace; }
    const Config* getConfig() const { return mConfig; }
    TextureCache* getTextures() const { return mTextures; }
    const ModelTrace& getTrace() const { return mTrace; }

    /// sdfBillBoardMakeN is handed the batch alone, and the table index lives on the node above.
    void readBillboardTables();

private:

    /// A batch caps at VU0 memory, so a larger mesh arrives split, triangles indexing the whole.

    /// Plain arrays, not VIF-tagged. Shapes past the first are deltas, weighted by the root's
    /// morph controller.

    /// Over the same nodes. Unseen in every sample scene, which is about the savestates.
    /// @fn sdfModelTempPacketLinkOt
    /// @note SLUS-20911 0x002B1950

public:
    bool readBlendShapes(u32 at, const sdfCorePacket_t& packet,
                         guest_ref<sdfModelRootNode_t> root, DrawMesh& mesh);
    void dumpWords(const char* what, u32 at);
    void reportFaceDiv(const char* what, f32 value);
    void dumpPayload(sdfCorePacketPtr_t aPacket);
    bool readRigidBatches(u32 at, const sdfCorePacket_t& packet, DrawMesh& mesh);
    bool readVifStreamMesh(u32 at, u32 end, const NodeDraw& draw, sdfMatPtr_t aMat,
                           Frame* frame);
    
    bool finishMesh(guest_ref<sdfModelRootNode_t> root, const sdfCorePacket_t& packet,
                    sdfCorePacketType_t type, DrawMesh& mesh, Frame* frame);
    void readSecondUv(u32 at, const sdfCorePacket_t& packet, DrawMesh& mesh);

private:

    /// @fn corePktEmitTexTris
    /// @note SLUS-20911 0x002B85D8

    pcsx2hook_ctx* mRuntime = nullptr;
    TextureCache* mTextures = nullptr;
    const Config* mConfig = nullptr;
    Trace* mEvents = nullptr;

    dds3BillboardNodePtr_t mDds3BillboardTag;
    u32 mDecodeFrame = 0;

    ModelTrace mTrace;
};

}  // namespace sdfhook
