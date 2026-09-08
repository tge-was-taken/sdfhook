#include "model.h"

#include <bit>
#include <cmath>

#include <cstddef>
#include <cstring>
#include <vector>

#include "log.h"
#include "texture.h"
#include "trace.h"
#include "json.h"
#include "unsupported.h"

namespace sdfhook {


namespace {

LogChannel log("model");

// The palette is indexed by a byte in the vertex, and no measured mesh comes near this.
constexpr u16 kMaxBones = 64;

// unpackTexture scales screenScale.z by this at 0x00CC; the shader has no such term and every
// savestate measures it zero.
void reportRootDepthBias(f32 bias) {
    if (bias != 0.0f) reportUnsupported("root depth bias", std::bit_cast<u32>(bias));
}

/// The one influence a body without weights has, so every mesh reaches the shader alike.
void setRigidInfluence(MeshVertex& vertex, const sdfVector3_t& position,
                       const sdfVector3_t* normal) {
    static constexpr sdfVector3_t kNoNormal{};
    const sdfVector3_t& n = normal ? *normal : kNoNormal;
    MeshInfluence& only = vertex.influence[0];
    only.x = position.x;
    only.y = position.y;
    only.z = position.z;
    only.nx = n.x;
    only.ny = n.y;
    only.nz = n.z;
    only.weight = 1.0f;
    only.bone = 0.0f;
}

constexpr f32 kZero3[3] = {};

// A packet's payload advances quadword by quadword, whatever a stream's own element size is.
u32 align16(u32 address) { return (address + 15) & ~15u; }

/// The engine keeps the left operand in VU0's VF28-31 and passes only the right, which is what
/// "Rev" names.
/// @fn MultMatrixRev
/// @note SLUS-20911 0x002B66B8
Matrix4 MultMatrixRev(const Matrix4& aLeft, const Matrix4& aMatrix) {
    Matrix4 out{};
    for (int i = 0; i < 4; i++) {
        for (int c = 0; c < 4; c++) {
            out.m[i][c] = aLeft.m[i][0] * aMatrix.m[0][c] + aLeft.m[i][1] * aMatrix.m[1][c] +
                          aLeft.m[i][2] * aMatrix.m[2][c] + aLeft.m[i][3] * aMatrix.m[3][c];
        }
    }
    return out;
}

// The six bits VCLIP sets. sdfModelCheckBounds ANDs them across the corners, so only agreement
// matters, not which plane each names.
u32 clipCode(const f32 v[4]) {
    const f32 w = v[3];
    u32 code = 0;
    for (int axis = 0; axis < 3; axis++) {
        if (v[axis] > w) code |= 1u << (2 * axis);
        if (v[axis] < -w) code |= 1u << (2 * axis + 1);
    }
    return code;
}

void transformPoint(const Matrix4& m, f32 x, f32 y, f32 z, f32 out[4]) {
    for (int c = 0; c < 4; c++) {
        out[c] = x * m.m[0][c] + y * m.m[1][c] + z * m.m[2][c] + m.m[3][c];
    }
}

// Both lists keep a pointer to their item array at sdfListItems, so an item costs two loads.
u32 readListItem(u32 listAddr, u32 index) {
    const u32 items = u32Ptr_t{listAddr + sdfListItems}.valueOr();
    return items ? u32Ptr_t{items + 4 * index}.valueOr() : 0;
}

/// A VIF code, as the mesh body carries it: an immediate, a count, then the command.
struct VifCode {
    u32 immediate = 0;
    u32 count = 0;
    u32 command = 0;
    u32 next = 0;
};

bool readVifCode(u32 at, VifCode& out) {
    const u32Ptr_t at32{at};
    if (!at32) return false;
    const u32 word = *at32;
    out.immediate = word & 0xFFFF;
    out.count = (word >> 16) & 0xFF;
    out.command = (word >> 24) & 0xFF;
    out.next = at + 4;
    return true;
}

bool isUnpack(const VifCode& code) { return (code.command & sdfVifUnpack) == sdfVifUnpack; }

/// Bytes one unpacked element takes, from the command's own component count and element size.
u32 unpackWidth(const VifCode& code) {
    static const u32 kElementBytes[4] = {4, 2, 1, 0};
    return kElementBytes[code.command & 3] * (((code.command >> 2) & 3) + 1);
}

/// The UNPACK naming one vertex array, checked against the caller's width and count so a misread
/// offset is refused rather than decoded into plausible nonsense.
bool readArray(u32 at, u32 width, u32 count, u32& data, u32& next) {
    VifCode code;
    if (!readVifCode(at, code) || !isUnpack(code)) return false;
    if (unpackWidth(code) != width || code.count != count) return false;
    data = code.next;
    next = data + width * count;
    return true;
}

/// Vertices in the batch, and for a weighted mesh the nodes beyond the first blending into them.
bool readBatchHeader(u32 at, u16& first, u16& second, u32& next) {
    VifCode code;
    if (!readVifCode(at, code) || !isUnpack(code) || code.count != 1) return false;
    const u16* pair = u16Ptr_t{code.next}.slice(2);
    if (!pair) return false;
    first = pair[0];
    second = pair[1];
    next = code.next + 4;
    return true;
}

/// Past the microprogram kick and the flush that close a batch, to the next quadword.
u32 skipBatchEnd(u32 at) {
    for (int code = 0; code < 4; code++) {
        VifCode tag;
        if (!readVifCode(at, tag)) return 0;
        at = tag.next;
        if (tag.command == sdfVifFlushEnd) return align16(at);
    }
    return 0;
}

const char* nodeName(ModelTrace::Node outcome) {
    switch (outcome) {
        case ModelTrace::Node::Drawn: return "drawn";
        case ModelTrace::Node::NoChain: return "no packet chain";
        case ModelTrace::Node::Hidden: return "hidden by flags";
        case ModelTrace::Node::BoundsCulled: return "bounds culled";
        case ModelTrace::Node::Unreadable: return "unreadable";
        case ModelTrace::Node::Count: break;
    }
    return "?";
}

const char* packetName(ModelTrace::Packet outcome) {
    switch (outcome) {
        case ModelTrace::Packet::Generated: return "generated";
        case ModelTrace::Packet::DmaCall: return "dma call";
        case ModelTrace::Packet::Shaped: return "shaped";
        case ModelTrace::Packet::Suppressed: return "suppressed by root flags";
        case ModelTrace::Packet::Unknown: return "unknown type";
        case ModelTrace::Packet::Count: break;
    }
    return "?";
}

const char* rejectName(ModelTrace::Reject why) {
    switch (why) {
        case ModelTrace::Reject::NoMaterial: return "no material";
        case ModelTrace::Reject::Empty: return "empty";
        case ModelTrace::Reject::BadBatch: return "batch did not decode";
        case ModelTrace::Reject::BadElems: return "triangles out of range";
        case ModelTrace::Reject::NoMorph: return "no morph controller";
        case ModelTrace::Reject::Count: break;
    }
    return "?";
}

}  // namespace

void ModelTrace::countRoot(bool tempPackets) {
    mRoots++;
    if (tempPackets) mTempPacketRoots++;
}

void ModelTrace::countPacketType(u8 type, u16 attrBits, u16 packetBits) {
    mTypes[type][(u32(packetBits) << 16) | attrBits]++;
}

const char* ModelTrace::geomName(Geom shape) {
    switch (shape) {
        case Geom::Rigid: return "rigid";
        case Geom::Weighted: return "weighted";
        case Geom::BlendShape: return "blend shape";
        case Geom::VifStream: return "vif stream";
        case Geom::Unknown: return "unknown body";
        case Geom::Count: break;
    }
    return "?";
}

void ModelTrace::countGeom(sdfCorePacketType_t type, u32 elems) { addGeom(geomFor(type), elems); }

void ModelTrace::countMesh(sdfCorePacketType_t type, u32 elems) { addMesh(geomFor(type), elems); }

void ModelTrace::countStreamGeom(u32 elems) { addGeom(Geom::VifStream, elems); }

void ModelTrace::countStreamMesh(u32 elems) { addMesh(Geom::VifStream, elems); }

void ModelTrace::addGeom(Geom shape, u32 elems) {
    Shape& entry = mGeom[usize(shape)];
    entry.seen++;
    entry.seenElems += elems;
}

void ModelTrace::addMesh(Geom shape, u32 elems) {
    Shape& entry = mGeom[usize(shape)];
    entry.drawn++;
    entry.drawnElems += elems;
}

void ModelTrace::addTo(JsonObject& row) const {
    row.beginObject("model");
    row.addUint("roots", mRoots).addUint("tempPacketRoots", mTempPacketRoots);
    row.addUint("splitMaterials", mSplitMaterials);

    row.beginObject("nodes");
    for (usize i = 0; i < usize(Node::Count); i++) row.addUint(nodeName(Node(i)), mNodes[i]);
    row.endObject();

    row.beginObject("packets");
    for (usize i = 0; i < usize(Packet::Count); i++) row.addUint(packetName(Packet(i)), mPackets[i]);
    row.endObject();

    u64 seenElems = 0;
    u64 drawnElems = 0;
    row.beginObject("geom");
    for (usize i = 0; i < usize(Geom::Count); i++) {
        const Shape& entry = mGeom[i];
        seenElems += entry.seenElems;
        drawnElems += entry.drawnElems;
        row.beginObject(geomName(Geom(i)))
            .addUint("seen", entry.seen)
            .addUint("seenTris", entry.seenElems)
            .addUint("drawn", entry.drawn)
            .addUint("drawnTris", entry.drawnElems);
        row.beginObject("rejected");
        for (usize r = 0; r < usize(Reject::Count); r++) {
            row.addUint(rejectName(Reject(r)), mRejects[i][r]);
        }
        row.endObject().endObject();
    }
    row.endObject();
    row.addUint("seenTris", seenElems).addUint("drawnTris", drawnElems);

    row.beginArray("bodyTypes");
    for (const auto& [type, byBits] : mTypes) {
        for (const auto& [bits, count] : byBits) {
            row.beginObject()
                .addUint("type", type)
                .addHexDigits("attrBits", bits & 0xFFFF, 4)
                .addHexDigits("packetBits", bits >> 16, 4)
                .addUint("packets", count)
                .endObject();
        }
    }
    row.endArray();

    row.beginArray("samplers");
    for (const auto& [tex0, byTex1] : mSampler) {
        for (const auto& [tex1, byClamp] : byTex1) {
            for (const auto& [clamp, count] : byClamp) {
                row.beginObject()
                    .addUint("tfx", (tex0 >> 1) & 3)
                    .addUint("tcc", tex0 & 1)
                    .addUint("mag", tex1 & 1)
                    .addUint("min", (tex1 >> 1) & 7)
                    .addUint("wrap", clamp)
                    .addUint("meshes", count)
                    .endObject();
            }
        }
    }
    row.endArray();

    const auto histogram = [&row](const char* key, const auto& counts, const char* bucket) {
        row.beginArray(key);
        for (const auto& [value, count] : counts) {
            row.beginObject().addUint(bucket, value).addUint("meshes", count).endObject();
        }
        row.endArray();
    };
    histogram("orderingTables", mOt, "ot");
    histogram("drawnAttr", mAttr, "attrBits");
    histogram("bonePalettes", mBones, "bones");
    histogram("vertexInfluences", mInfluences, "influences");
    histogram("reflectiveSpecular", mSpecular, "color");

    row.beginArray("gsState");
    for (const auto& [gs, count] : mGs) {
        row.beginObject()
            .addHexDigits("alpha", u32(gs >> 32) & 0xFF, 2)
            .addHexDigits("test", u32(gs) & 0xFFFFFF, 6)
            .addUint("meshes", count)
            .endObject();
    }
    row.endArray();

    row.addUint("uprightNodes", mUprightNodes)
        .addUint("mirroredNodes", mMirroredNodes)
        .addUint("texturedMeshes", mTexturedMeshes)
        .addUint("missingTextureMeshes", mMissingTextureMeshes)
        .addUint("untexturedMeshes", mUntexturedMeshes)
        .addUint("texMatrixMeshes", mTexMatrix)
        .addUint("noTexMatrixMeshes", mNoTexMatrix)
        .addUint("multiTexturedMeshes", mMultiTextured)
        .addUint("singleTexturedMeshes", mSingleTextured)
        .addUint("tempPacketWalks", mTempPacketWalks)
        .addUint("shapedWalks", mShapedWalks)
        .addUint("staleMatPackets", mStaleMatPackets)
        .addUint("freshMatPackets", mFreshMatPackets)
        .addUint("setupRefs", mSetupRefs)
        .addUint("strayRefs", mStrayRefs)
        .addUint("settledMeshes", mSettledMeshes)
        .addUint("unsettledMeshes", mUnsettledMeshes);

    // Neither has another caller, so a difference here is our traversal reaching a different set
    // of packets, not a different way of drawing them.
    row.beginObject("traversal")
        .addUint("generatedWalked", mPackets[usize(Packet::Generated)])
        .addUint("generatedByEngine", mGuestGenerated)
        .addUint("shapedWalked", mPackets[usize(Packet::Shaped)])
        .addUint("shapedByEngine", mGuestShaped)
        .addBool("agrees", mPackets[usize(Packet::Generated)] == mGuestGenerated &&
                               mPackets[usize(Packet::Shaped)] == mGuestShaped)
        .endObject();
    row.endObject();
}

void ModelTrace::report() const {
    log.info("{} roots walked, {} of them with temp packets", mRoots, mTempPacketRoots);
    for (usize i = 0; i < usize(Node::Count); i++) {
        if (mNodes[i]) log.info("node {:<24} {}", nodeName(Node(i)), mNodes[i]);
    }
    for (usize i = 0; i < usize(Packet::Count); i++) {
        if (mPackets[i]) log.info("packet {:<22} {}", packetName(Packet(i)), mPackets[i]);
    }

    u64 seenElems = 0;
    u64 drawnElems = 0;
    for (usize i = 0; i < usize(Geom::Count); i++) {
        const Shape& entry = mGeom[i];
        seenElems += entry.seenElems;
        drawnElems += entry.drawnElems;
        if (!entry.seen) continue;
        log.info("geom {:<16} {} packets {} tris, drew {} packets {} tris", geomName(Geom(i)),
            entry.seen, entry.seenElems, entry.drawn, entry.drawnElems);
        for (usize r = 0; r < usize(Reject::Count); r++) {
            if (mRejects[i][r]) {
                log.info("     rejected {:<18} {}", rejectName(Reject(r)), mRejects[i][r]);
            }
        }
    }

    for (const auto& [type, byBits] : mTypes) {
        for (const auto& [bits, count] : byBits) {
            log.info("body type {} attr {:04X} packet {:04X}   {} packets", type, bits & 0xFFFF,
                bits >> 16, count);
        }
    }
    log.info("node transform winding: {} positive, {} mirrored", mUprightNodes, mMirroredNodes);

    for (const auto& [tex0, byTex1] : mSampler) {
        for (const auto& [tex1, byClamp] : byTex1) {
            for (const auto& [clamp, count] : byClamp) {
                log.info("sampler tfx {} tcc {}  mag {} min {}  wrap {}   {} meshes",
                    (tex0 >> 1) & 3, tex0 & 1, tex1 & 1, (tex1 >> 1) & 7, clamp, count);
            }
        }
    }
    for (const auto& [color, count] : mSpecular) {
        log.info("reflective mesh specular {:08X}   {} meshes", color, count);
    }
    log.info("branches never reached: {} temp packet walks, {} shaped packets", mTempPacketWalks,
             mShapedWalks);
    log.info("material packets: {} current, {} read before this frame's rebuild", mFreshMatPackets,
             mStaleMatPackets);
    log.info("mesh textures: {} found, {} missing, {} named none", mTexturedMeshes,
             mMissingTextureMeshes, mUntexturedMeshes);
    log.info("texture matrix: {} meshes with one, {} without", mTexMatrix, mNoTexMatrix);
    log.info("multi-texture pass: {} meshes on, {} off", mMultiTextured, mSingleTextured);
    for (const auto& [ot, count] : mOt) log.info("ordering table {}   {} meshes", ot, count);
    for (const auto& [gs, count] : mGs) {
        log.info("gs alpha {:02X} test {:06X}   {} meshes", u32(gs >> 32) & 0xFF,
            u32(gs) & 0xFFFFFF, count);
    }
    for (const auto& [attr, count] : mAttr) {
        log.info("drawn attr {:04X}   {} meshes", attr, count);
    }
    for (const auto& [bones, count] : mBones) {
        log.info("palette {:2} bones     {} meshes", bones, count);
    }
    for (const auto& [used, count] : mInfluences) {
        log.info("vertex  {:2} influences {} vertices{}", used, count,
            used > kMaxBoneInfluences ? "   <- OVER THE CAP" : "");
    }
    if (seenElems) {
        log.info("triangle coverage {} of {} ({:.1f}%)", drawnElems, seenElems,
            100.0 * double(drawnElems) / double(seenElems));
    }
    if (mSettledMeshes || mUnsettledMeshes) {
        log.info("mesh state: {} settled from the transmit chain, {} left on the table default",
            mSettledMeshes, mUnsettledMeshes);
    }
    if (mSetupRefs || mStrayRefs) {
        log.info("chain refs: {} setup, {} looked like a mesh and failed", mSetupRefs,
            mStrayRefs);
    }

    // Neither has another caller, so a difference here is our traversal reaching a different set
    // of packets, not a different way of drawing them.
    const u64 walkedGenerated = mPackets[usize(Packet::Generated)];
    const u64 walkedShaped = mPackets[usize(Packet::Shaped)];
    log.info("traversal: generated {} walked / {} by the engine{}", walkedGenerated,
        mGuestGenerated, walkedGenerated == mGuestGenerated ? "" : "   <- MISMATCH");
    if (mSplitMaterials) {
        log.warn("{} meshes shade differently from another mesh of the same material packet: a "
                 "field was skipped and kept its default", mSplitMaterials);
    }
    log.info("traversal: shaped    {} walked / {} by the engine{}", walkedShaped, mGuestShaped,
        walkedShaped == mGuestShaped ? "" : "   <- MISMATCH");
}

void ModelParser::attach(pcsx2hook_ctx* runtime, const Config& config, TextureCache* textures,
                         Trace* trace, const RenderState* state) {
    mState = state;
    mConfig = &config;
    mRuntime = runtime;
    mTextures = textures;
    mEvents = trace;
    mSdf3DScreenTm = {pcsx2hook_find_symbol(runtime, "sdf3DScreenTm")};
    mSdf3DLightDefault = {pcsx2hook_find_symbol(runtime, "sdf3DLightDefault")};
    mSdf3DFogDefault = {pcsx2hook_find_symbol(runtime, "sdf3DFogDefault")};
    mSdfFrameId = {pcsx2hook_find_symbol(runtime, "sdfFrameId")};
    mDds3BillboardTag = {pcsx2hook_find_symbol(runtime, "dds3BillboardTag")};
    mSdfFaceDivBias = {pcsx2hook_find_symbol(runtime, "sdfFaceDivBias")};
    mSdfFaceDivSoften = {pcsx2hook_find_symbol(runtime, "sdfFaceDivSoften")};
    mCorePktSoftenBias = {pcsx2hook_find_symbol(runtime, "corePktSoftenBias")};
    // sdf3DSpecularTex holds a texture handle where sdf3DRefMapParam holds a raw TEX0, and the
    // two are different passes.
    mSdf3DSpecularTex = {pcsx2hook_find_symbol(runtime, "sdf3DSpecularTex")};
    // Quadwords 3..6 are the projection, diag(1.9078, 2.5401, -1.0005). mScreenTm is projection
    // times view, so dividing its first two rows by them recovers the view rotation.
    mSdf3DScreenParam = {pcsx2hook_find_symbol(runtime, "sdf3DScreenParam")};
    mSdf3DRefMapParam = {pcsx2hook_find_symbol(runtime, "sdf3DRefMapParam")};
    if (const u64 tex0 = mSdf3DRefMapParam.valueOr()) {
        const sdfTex0_t t = sdfDecodeTex0(tex0);
        log.info("sdf3DRefMapParam tex0={:016X} tbp={} tbw={} psm={:#x} {}x{} tfx={} tcc={} cbp={}",
                 tex0, t.tbp, t.tbw, t.psm, t.width, t.height, t.tfx, t.tcc, t.cbp);
    }
}

// 0x2B9230/0x2B923C: sdfCorePacketGen keeps its two float arguments in $f20 and $f21 and hands
// them straight back, so entry is where they can be read.
void ModelParser::setFaceDivTerms(f32 soften, f32 bias) {
    if (!mFaceDivTermsSeen && mConfig) {
        log.info("face division terms from the guest: soften {} bias {}, fitted {} {}", soften, bias,
                 mConfig->faceDivSoften, mConfig->faceDivBias);
    }
    mFaceDivSoftenTerm = soften;
    mFaceDivBiasTerm = bias;
    mFaceDivTermsSeen = true;
}

// 0x2B6DF4 and 0x2B6E30 read the coordinate's scale from [0].w and its offset from [0].y, and
// 0x2B6E80 interpolates [16] to [16]+[32] across it.
void ModelParser::readFaceDivParm(sdfCorePacketCbPtr_t aCb, sdfVector4Ptr_t aParm) {
    const sdfVector4_t* parm = aParm.slice(3);
    if (!parm || !aCb) return;
    const u32 aMatPacket = aCb->matPacketPtr.addr;

    MeshRamp& ramp = mFaceDivRamps[aMatPacket];
    const f32 lo[3] = {parm[1].x, parm[1].y, parm[1].z};
    const f32 delta[3] = {parm[2].x, parm[2].y, parm[2].z};
    // 128 is one, as the GS modulate has it.
    for (int i = 0; i < 3; i++) {
        ramp.lo[i] = lo[i] / 128.0f;
        ramp.delta[i] = delta[i] / 128.0f;
    }
    ramp.known = true;

    if (!mFaceDivParmSeen) {
        mFaceDivParmSeen = true;
        log.info("face division ramp, material packet {:08X}: scale {} offset {}", aMatPacket,
                 parm[0].w, parm[0].y);
        log.info("  lo {} {} {}, delta {} {} {}", lo[0], lo[1], lo[2], delta[0], delta[1], delta[2]);
    }
}

void ModelParser::readLighting(sdf3DLightParallelPacketPtr_t aLight) {
    mLighting = {};
    const sdf3DLightParallelPacketPtr_t light = aLight ? aLight : mSdf3DLightDefault.valueOr();
    if (!light) return;

    for (u32 i = 0; i < kParallelLights; i++) {
        for (usize axis = 0; axis < 3; axis++) {
            // normal is a matrix whose columns are the light vectors, so a light reads down them.
            mLighting.direction[i][axis] = light->normal[axis][i];
            mLighting.color[i][axis] = light->color[i][axis];
        }
        mLighting.base[i] = light->colorBase[i];
        mLighting.ambientColor[i] = light->color[3][i];
        mLighting.fullColor[i] = light->color[4][i];
    }

    static std::set<u32> shown;
    if (mConfig->dumpLight && shown.insert(light.addr).second) {
        JsonObject row;
        row.addString("dump", "light").addHexDigits("at", light.addr, 8);
        row.addReals("base", {mLighting.base[0], mLighting.base[1], mLighting.base[2]}, 4);
        for (u32 c = 0; c < 5; c++) {
            const std::string name = std::format("color{}", c);
            row.addReals(name.c_str(),
                         {light->color[c][0], light->color[c][1], light->color[c][2]}, 4);
        }
        row.beginArray("lights");
        for (u32 i = 0; i < kParallelLights; i++) {
            row.beginObject();
            row.addReals("direction", {mLighting.direction[i][0], mLighting.direction[i][1],
                                       mLighting.direction[i][2]}, 4);
            row.addReals("color",
                         {mLighting.color[i][0], mLighting.color[i][1], mLighting.color[i][2]}, 4);
            row.endObject();
        }
        row.endArray();
        log.dump(row.take());
    }
}

// F is per vertex, 255 keeping the color and 0 replacing it with FOGCOL. VU1 builds it as
// `fogParm[2] + fogParm[3] / w` clamped to fogParm[0]..fogParm[1] (0x04DC, 0x04DF, 0x04E3).
void ModelParser::readFog(sdf3DFogPacketPtr_t aFog) {
    mFog = {};
    const sdf3DFogPacketPtr_t fog = aFog ? aFog : mSdf3DFogDefault.valueOr();
    if (!fog) return;

    const u32 fogcol = u32(fog->fogColor);
    mFog.on = true;
    mFog.minimum = fog->fogParm[0] / 255.0f;
    mFog.maximum = fog->fogParm[1] / 255.0f;
    mFog.offset = fog->fogParm[2] / 255.0f;
    mFog.scale = fog->fogParm[3] / 255.0f;
    mFog.color[0] = f32(fogcol & 0xFF) / 255.0f;
    mFog.color[1] = f32((fogcol >> 8) & 0xFF) / 255.0f;
    mFog.color[2] = f32((fogcol >> 16) & 0xFF) / 255.0f;
}

// The sampler state lives in the material's packet, which sdfMatUpdatePacketBasic fills from the
// texture handle rather than from the material.
void ModelParser::readMaterialPacket(const sdfMat_t& material, DrawMesh& mesh,
                                   Frame* frame) {
    mesh.diffuse = Rgba::fromPacked(material.diffuse);
    mesh.ambient = Rgba::fromPacked(material.ambient);

    // The finished buffer, not the one about to be rewritten: our walk runs at sdfModelLinkOt's
    // entry, so the index the frame id names is the one sdfMatListUpdatePacket is about to fill.
    const u32 frameId = mSdfFrameId ? (*mSdfFrameId & 1) ^ 1 : 0;
    const sdfMatPacketPtr_t packet{material.packet[frameId].addr};
    if (!packet) return;
    mesh.specular = Rgba::fromPacked(packet->specularColor);
    mTrace.countSpecular(mesh.attrBits, packet->specularColor);
    // 0x2B8D9C: one global texture for every reflection pass, so it is keyed by that address.
    if (mesh.attrBits & sdfCoreAttrRefMapPass) {
        mesh.refMap = mTextures->getHandleForTex0(TexHandle{mSdf3DRefMapParam.addr},
                                                  mSdf3DRefMapParam.valueOr());
    }
    // The specular pass names a handle, so it resolves like any other texture.
    if (mesh.attrBits & sdfCoreAttrSpecularPass) {
        if (const u32 handleAddr = mSdf3DSpecularTex.valueOr()) {
            const sdfTexHandlePtr_t handle{handleAddr};
            // The handle's own pixels, with its upload packet's TEX0 for TCC so the alpha
            // survives. The overlay's real texture is never uploaded. See TODO V7.
            mesh.specularTex = mTextures->getHandle(TexHandle{handleAddr}, frame);
            if (handle) {
                mesh.specularTex0 =
                    guest_ptr<u64>{handle->packet.addr + kTex0InPacket}.valueOr();
            }
        }
    }
    mesh.tex0 = packet->tex0Data;
    mesh.tex1 = packet->tex1Data;
    mesh.clamp = packet->clampData;
    // A material can name a TEX0 and carry no handle. The packet's address is stable, so the
    // recovered texture files under that.
    if (!mesh.texHandle.addr) {
        mesh.texHandle = mTextures->getHandleForTex0(TexHandle{packet.addr}, packet->tex0Data);
    }
    // Inline at texTm rather than behind it: that field's address is what corePktXfmLightClipUV
    // is handed.
    std::memcpy(mesh.texMatrix, packet->texTm, sizeof(mesh.texMatrix));
    mTrace.countTexMatrix(packet->texTm[0] != 0.0f || packet->texTm[3] != 0.0f);

    // 0x2B6A4C: read off the material *packet* at +44 and +40, plus the root's, soften clamped to
    // at least one. sdfMat_t carries the same two fields at 0x44 and 0x40 with different values.
    const f32 softenTerm = mFaceDivTermsSeen ? mFaceDivSoftenTerm : mConfig->faceDivSoften;
    const f32 biasTerm = mFaceDivTermsSeen ? mFaceDivBiasTerm : mConfig->faceDivBias;
    mesh.multi.soften = std::max(packet->fdSoften + softenTerm, 1.0f);
    mesh.multi.bias = packet->fdBias + biasTerm;
    mesh.faceDivSoften = mesh.multi.soften;
    mesh.faceDivBias = mesh.multi.bias;

    // Everything below belongs to the second pass. The ramp terms above do not, and leaving them
    // behind this guard put a seam along every mesh boundary.

    // lightSetup 0x00A3 files this at mem[910].w: it lerps the sum's base toward 1.0. It belongs
    // to every mesh, not only one with a second UV set, so it is read outside that guard.
    mesh.flatten = packet->constant;

    readMultiTexturePacket(material, packet, mesh, frame);
}

/// The second textured pass. Split from the material read so its early return cannot skip the
/// face-division terms, which belong to the first pass.
/// @fn corePktEmitMultiTexPass
/// @note SLUS-20911 0x002B8C30
void ModelParser::readMultiTexturePacket(const sdfMat_t& material, sdfMatPacketPtr_t packet,
                                         DrawMesh& mesh, Frame* frame) {
    if (!(mesh.attrBits & sdfCoreAttrMultiTexPass)) return;
    mesh.multi.texHandle = mTextures->getHandle(material.multiTexHandle, frame);
    // The second pass can name its TEX0 with no handle just as the first can. Keyed apart from the
    // first by an offset into the same packet, since one material carries both.
    if (!mesh.multi.texHandle.addr) {
        mesh.multi.texHandle =
            mTextures->getHandleForTex0(TexHandle{packet.addr + 8}, packet->mtexTex0Data);
    }
    mesh.multi.alpha = packet->mtexAlpha;
    mesh.multi.tex0 = packet->mtexTex0Data;
    mesh.multi.tex1 = packet->mtexTex1Data;
    mesh.multi.clamp = packet->mtexClampData;
    mesh.multi.diffuse = Rgba::fromPacked(packet->mtexDiffuse);
    mesh.multi.on = mesh.multi.texHandle.addr != 0;
    // 0x2B8C4C: the ramp runs only where the two diffuse colors differ, and only under algo 3.
    mesh.multi.algo = packet->mtexAlgo;
    // 0x2B8C90: the packet's own two colors, at +8 and +12. Taking the first from sdfMat_t is a
    // different value and claims a ramp on meshes the engine never ramps.
    mesh.multi.ramp = packet->mtexAlgo < 3 && packet->diffuse != packet->mtexDiffuse;
    mesh.multi.constant = packet->constant;
    // Generation runs after the walk, so these are the previous frame's endpoints. Every captured
    // one is algo 3 and every ramp mesh algo 1 or 2, so on the measured scenes this path is inert.
    if (const auto it = mFaceDivRamps.find(packet.addr); it != mFaceDivRamps.end()) {
        mesh.multi.rampColors = it->second;
    }
    if (packet->mtexAlgo > 3) reportUnsupported("corePktEmitMultiTexPass mtexAlgo", packet->mtexAlgo);

}

// cppcheck-suppress unusedFunction  // @todo TODO.md V19, kept for the next producer
bool ModelParser::isWorldSpace(const sdfVector3_t& point, f32 out[3]) const {
    f32 clip[4];
    transformPoint(mScreenTm, point.x, point.y, point.z, clip);
    const f32 w = clip[3] != 0.0f ? clip[3] : 1.0f;
    out[0] = clip[0] / w;
    out[1] = clip[1] / w;
    out[2] = w;
    // A point already in screen space projects far outside the clip box, so the test is the box
    // with a small margin, not a bound loose enough for anything to pass.
    constexpr f32 kMargin = 4.0f;
    return w > 0.0f && out[0] > -kMargin && out[0] < kMargin && out[1] > -kMargin &&
           out[1] < kMargin;
}

bool ModelParser::readScreenTm() {
    if (!mSdf3DScreenTm) return false;
    mScreenTm = *mSdf3DScreenTm;
    if (const sdfVector4_t* p = mSdf3DScreenParam.slice(7)) {
        mProjectionScale[0] = p[3].x;
        mProjectionScale[1] = p[4].y;
    }
    static bool shown = false;
    if (!shown && mConfig->dumpScreen) {
        shown = true;
        JsonObject row;
        row.addString("dump", "screenTm").beginArray("m");
        for (const auto& r : mScreenTm.m) {
            row.beginObject().addReals("row", {r[0], r[1], r[2], r[3]}, 4).endObject();
        }
        row.endArray();
        log.dump(row.take());
    }
    return true;
}

/// Culls a node by its bounds against the VU0 clip codes the engine tests.
/// @fn sdfModelCheckBounds
/// @note SLUS-20911 0x002B1838
bool ModelParser::sdfModelCheckBounds(sdfModelBoundsPtr_t aBounds, const Matrix4& clip) const {
    if (!aBounds) return false;
    const f32* box = &aBounds->min.x;

    u32 code = 0x3F;
    for (int corner = 0; corner < 8; corner++) {
        f32 out[4];
        transformPoint(clip, box[(corner & 1) ? 3 : 0], box[(corner & 2) ? 4 : 1],
                       box[(corner & 4) ? 5 : 2], out);
        code &= clipCode(out);
        if (!code) return false;
    }
    return code != 0;
}

void ModelParser::traceMeshes(const Frame& frame) {
    if (!mEvents) return;
    for (const DrawMesh& mesh : frame.meshes) mEvents->onMesh(mesh, mScreenTm, mDecodeFrame);
}

/// The engine's node list, turned into meshes instead of ordering-table entries.
/// @fn sdfModelLinkOt
/// @note SLUS-20911 0x002B1DB8
void ModelParser::sdfModelLinkOt(sdfModelRootNodePtr_t aRoot, Frame* frame) {
    if (!aRoot || !readScreenTm()) return;
    const guest_ref<sdfModelRootNode_t> root{aRoot};
    if (frame) {
        frame->screenTm = mScreenTm;
        frame->projectionScale[0] = mProjectionScale[0];
        frame->projectionScale[1] = mProjectionScale[1];
    }

    mTrace.countRoot((root->flags & sdfModelRootTempPackets) != 0);

    // 0x2B1E04: the temporary packets are linked first, over the same nodes.
    if (root->flags & sdfModelRootTempPackets) sdfModelTempPacketLinkOt(root, frame);

    // 0x2B1E58, 0x2B1E5C: the root's own light and fog, or the engine defaults when it has none.
    readLighting(root->light);
    readFog(root->fog);

    const u32 frameId = mSdfFrameId ? *mSdfFrameId : 0;

    // 0x2B1E6C: the root's own bias under flags & 2, else the global. It rides in the per-node
    // state packet, which nothing here models.
    reportFaceDiv("sdfModelLinkOt faceDivBias", (root->flags & sdfModelRootOwnFaceDivBias)
                                                    ? root->faceDivBias
                                                    : mSdfFaceDivBias.valueOr());
    // The terms reach sdfCorePacketGen in $f12 and $f13, so Config's values are only the first
    // frame's fallback.
    if (root->flags & sdfModelRootOwnFaceDivBias) {
        reportUnsupported("sdfModelLinkOt root faceDiv terms", root->flags);
    }
    if (const u8 soften = mCorePktSoftenBias.valueOr()) {
        reportUnsupported("corePktSoftenBias", soften);
    }
    // 0x2B1E94: flags & 8 suppresses the type 1 and 2 paths.
    const bool suppressLinked = (root->flags & sdfModelRootNoLinkedPackets) != 0;

    const sdfModelNodeListPtr_t list = root->nodeList;
    if (!list) return;
    mNodeListAddr = root->nodeList.addr;

    // A chain names a material by its sdfMatMakePacket address, so the lookup is built once.
    mMatByPacket.clear();
    if (const sdfMatListPtr_t matList = root->matList) {
        for (s16 m = 0; m < matList->count; m++) {
            const sdfMatPtr_t material{readListItem(root->matList.addr, u32(m))};
            if (!material) continue;
            for (const sdfVoidPtr_t& packet : material->packet) {
                if (packet.addr) mMatByPacket[packet.addr] = material.addr;
            }
        }
    }

    for (s16 i = 0; i < list->count; i++) {
        const u32 nodeAddr = readListItem(root->nodeList.addr, u32(i));
        const sdfModelNodePtr_t node{nodeAddr};
        if (!node) {
            mTrace.countNode(ModelTrace::Node::Unreadable);
            continue;
        }
        const u32 chainAddr = node->drawPacket[frameId & 1].addr;
        if (!chainAddr) {
            mTrace.countNode(ModelTrace::Node::NoChain);
            continue;
        }
        // 0x2B1EEC
        if (node->flags & sdfModelNodeHidden) {
            mTrace.countNode(ModelTrace::Node::Hidden);
            continue;
        }

        NodeDraw draw{};
        std::memcpy(&draw.model, &node->tm, sizeof(Matrix4));
        // 0x2B1F0C: MultMatrixRev(&sdf3DScreenTm) over the node's own tm, held in VU0.
        draw.clip = MultMatrixRev(draw.model, mScreenTm);
        // 0x2B1F20
        if (node->bounds && sdfModelCheckBounds(node->bounds, draw.clip)) {
            mTrace.countNode(ModelTrace::Node::BoundsCulled);
            continue;
        }

        draw.sortZ = draw.clip.m[3][3];
        reportRootDepthBias(root->zBias);
        // 0x2B1ADC: the node's own colour, or the root's.
        draw.color = (node->flags & sdfModelNodeOwnColor) ? node->color : root->color;  // 0x2B1F48
        // 0x2B1F6C: signed, so a drawAttr of 0xFFFF sign-extends and the node's high bits survive.
        // Truncating to 16 drops everything above them, the soften bias included.
        draw.attrMask = u32(s32(s16(root->drawAttr))) & node->drawAttr;
        // 0x2B1FC0: a packet's otIndex of zero means table 0 or 1 by the sign of the color.
        draw.otIndex = s32(draw.color) < 0 ? 0 : 1;
        // sdfCorePacketGen turns the same flag into corePktBuildFaceList's backfaceSign.
        draw.reverse = suppressLinked;

        mTrace.countNode(ModelTrace::Node::Drawn);
        walkChain(root, draw, chainAddr, frame);
    }
}

/// sdfModelLinkOt's inner loop over a node's packet chain; recheck the two together.
void ModelParser::walkChain(guest_ref<sdfModelRootNode_t> root, const NodeDraw& draw,
                          u32 chainAddr, Frame* frame) {
    const bool suppressLinked = (root->flags & sdfModelRootNoLinkedPackets) != 0;

    for (u32 addr = chainAddr; addr;) {
        const sdfModelPacketHeaderPtr_t header{addr};
        if (!header) {
            mTrace.countPacket(ModelTrace::Packet::Unknown);
            return;
        }
        NodeDraw packetDraw = draw;
        if (header->otIndex) packetDraw.otIndex = header->otIndex;

        // 0x2B2084 type 2, 0x2B2098 type 1, 0x2B20A8 type 3.
        switch (header->type) {
            case sdfPacketDmaCall:
                if (suppressLinked) {
                    mTrace.countPacket(ModelTrace::Packet::Suppressed);
                    break;
                }
                mTrace.countPacket(ModelTrace::Packet::DmaCall);
                dumpWords("call", addr + sizeof(sdfModelPacketHeader_t));
                sdfModelSetNormalDrawPacket(packetDraw, addr + u32(sizeof(sdfModelPacketHeader_t)), frame);
                break;
            case sdfPacketShaped:
                if (suppressLinked) {
                    mTrace.countPacket(ModelTrace::Packet::Suppressed);
                    break;
                }
                mTrace.countPacket(ModelTrace::Packet::Shaped);
                sdfModelLinkShapedDrawPacket(root, packetDraw, header->dataPtr.addr, frame);
                break;
            case sdfPacketGenerated:
                mTrace.countPacket(ModelTrace::Packet::Generated);
                mCurrentLink = mLinkIndex++;
                sdfCorePacketGen(root, packetDraw, sdfCorePacketPtr_t{header->dataPtr.addr}, frame);
                break;
            default:
                mTrace.countPacket(ModelTrace::Packet::Unknown);
                reportUnsupported("sdfModelLinkOt packet type", header->type);
                break;
        }
        addr = header->next.addr;
    }
}

void ModelParser::dumpWords(const char* what, u32 at) {
    static u32 left = mConfig->dumpCall;
    if (!left) return;
    left--;

    for (int tag = 0; tag < 8; tag++) {
        const u32* head = u32Ptr_t{at}.slice(2);
        if (!head) return;
        const u32 id = (head[0] >> 28) & 7;

        JsonObject row;
        row.addString("dump", what)
            .addHexDigits("tag", at, 8)
            .addUint("id", id)
            .addUint("qwc", head[0] & 0xFFFF)
            .addHexDigits("addr", head[1], 8);
        if (id == sdfDmaTagRef) {
            row.beginArray("words");
            for (u32 word = 0; word < mConfig->dumpCallWords; word++) {
                const u32Ptr_t at32{head[1] + 4 * word};
                if (!at32) break;
                const u32 raw = *at32;
                f32 value = 0.0f;
                std::memcpy(&value, &raw, sizeof(value));
                row.beginObject()
                    .addUint("at", 4 * word)
                    .addHexDigits("hex", raw, 8)
                    .addReal("f32", value, 5)
                    .endObject();
            }
            row.endArray();
        }
        log.dump(row.take());

        if (id != sdfDmaTagRef) return;
        at += 16;
    }
}

void ModelParser::dumpPayload(sdfCorePacketPtr_t aPacket) {
    static u32 left = mConfig->dumpMesh;
    if (!left || !aPacket) return;
    const sdfCorePacket_t& packet = *aPacket;
    if (mConfig->dumpMeshType >= 0 && packet.header.packetType != u8(mConfig->dumpMeshType)) return;
    left--;

    JsonObject row;
    row.addString("dump", "mesh")
        .addHexDigits("packet", aPacket.addr, 8)
        .addUint("packetType", packet.header.packetType)
        .addUint("mpgType", packet.header.mpgType)
        .addHexDigits("attrBits", packet.prim.attrBits, 4)
        .addHexDigits("packetBits", packet.prim.packetBits, 4)
        .addUint("faceCount", packet.prim.faceCount)
        .addUint("pointCount", packet.prim.pointCount)
        .addUint("matIndex", packet.header.matIndex);

    const u32 payload = aPacket.addr + sizeof(sdfCorePacket_t);
    row.beginArray("words");
    for (u32 word = 0; word < mConfig->dumpMeshWords; word++) {
        const u32Ptr_t at32{payload + 4 * word};
        if (!at32) break;
        const u32 raw = *at32;
        f32 value = 0.0f;
        std::memcpy(&value, &raw, sizeof(value));
        row.beginObject()
            .addUint("at", 4 * word)
            .addHexDigits("hex", raw, 8)
            .addReal("f32", value, 5)
            .endObject();
    }
    row.endArray();
    log.dump(row.take());
}

/// Reads the generated packet the engine would build, by packet type.
/// @fn sdfCorePacketGen
/// @note SLUS-20911 0x002B9178
void ModelParser::sdfCorePacketGen(guest_ref<sdfModelRootNode_t> root, const NodeDraw& draw,
                                  sdfCorePacketPtr_t aPacket, Frame* frame) {
    if (!aPacket) return;
    const sdfCorePacket_t& packet = *aPacket;

    const sdfCorePacketType_t type = sdfCorePacketType_t(packet.header.packetType);
    mTrace.countGeom(type, packet.prim.faceCount);
    mTrace.countPacketType(packet.header.packetType, packet.prim.attrBits, packet.prim.packetBits);
    dumpPayload(aPacket);

    switch (type) {
        case sdfCorePacketTypeRigid:
        case sdfCorePacketTypeWeighted:
        case sdfCorePacketTypeBlendShape: break;
        default:
            reportUnsupported("sdfCorePacketGen body type", packet.header.packetType);
            return;
    }
    if (!packet.prim.faceCount || !packet.prim.pointCount) {
        mTrace.countReject(type, ModelTrace::Reject::Empty);
        return;
    }

    const u32 nodesAddr = aPacket.addr + u32(sizeof(sdfCorePacket_t));
    u32 at = nodesAddr;
    if (type != sdfCorePacketTypeRigid) {
        const u16Ptr_t usedNodes{nodesAddr};
        if (!usedNodes) return;
        const u16 usedNodeCount = *usedNodes;
        at += 2 + 2u * usedNodeCount;
    }
    at = align16(at);

    DrawMesh mesh;
    mesh.sortZ = draw.sortZ;
    mesh.otIndex = draw.otIndex;
    mesh.color = Rgba::fromPacked(draw.color);
    mesh.attrBits = packet.prim.attrBits & draw.attrMask;
    mesh.reverseFacing = draw.reverse;

    if (!corePktBuildFaceList(at, packet, mesh)) {
        mTrace.countReject(type, ModelTrace::Reject::BadElems);
        return;
    }
    at = align16(at + 6u * packet.prim.faceCount);

    bool ok = false;
    switch (type) {
        case sdfCorePacketTypeRigid:
            mesh.bones.push_back(draw.model);
            ok = readRigidBatches(at, packet, mesh);
            break;
        case sdfCorePacketTypeWeighted:
            ok = readWeightedBatches(at, packet, nodesAddr, mesh);
            break;
        case sdfCorePacketTypeBlendShape:
            mesh.bones.push_back(draw.model);
            ok = readBlendShapes(at, packet, root, mesh);
            break;
    }
    if (!ok) {
        mTrace.countReject(type, ModelTrace::Reject::BadBatch);
        return;
    }

    mTrace.countSampler(mesh.tex0, mesh.tex1, mesh.clamp);
    mTrace.countDeterminant(mesh.bones[0]);
    mTrace.countAttr(mesh.attrBits);
    mTrace.countGs(mesh.gs);
    mTrace.countOt(mesh.otIndex);
    mTrace.countBones(u32(mesh.bones.size()));
    finishMesh(root, packet, type, mesh, frame);
}

// The chain alternates material packet and mesh, so a REF naming a known material sets it for
// the meshes after it.
void ModelParser::reportFaceDiv(const char* what, f32 value) {
    if (value == 0.0f) return;
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    reportUnsupported(what, bits);
}

/// The same node walk over the root's temporary packet list, run first under flags & 4.
/// @fn sdfModelTempPacketLinkOt
/// @note SLUS-20911 0x002B1950
void ModelParser::sdfModelTempPacketLinkOt(guest_ref<sdfModelRootNode_t> root, Frame* frame) {
    // @optout 0x2B1954 packetbuf: sdfFrameId picks which of the two per-frame packets to build
    // @optout 0x2B19A4 packetbuf: sdfMatListUpdatePacket rebuilds each material's DMA packet

    // 0x2B19FC..0x2B1A17: this walk takes faceDivSoften where sdfModelLinkOt takes faceDivBias,
    // and falls back to its own global.
    reportFaceDiv("sdfModelTempPacketLinkOt faceDivSoften",
                  (root->flags & sdfModelRootOwnFaceDivBias) ? root->faceDivSoften
                                                             : mSdfFaceDivSoften.valueOr());
    mTrace.countTempPacketWalk();

    const sdfModelNodeListPtr_t list = root->nodeList;
    if (!list) return;

    // 0x2B1A2C..0x2B1A7C, 0x2B1D60: this walk draws only nodes whose shape is a format 1
    // definition carrying a group array, and whose flags hide neither it nor its children.
    for (s16 i = 0; i < list->count; i++) {
        // @optout 0x2B1A40 elsewhere(readListItem): the list header's inline items
        const sdfModelNodePtr_t node{readListItem(root->nodeList.addr, u32(i))};
        if (!node || !node->shape || node->shape->format != 1) continue;
        const u32 groups = node->shape->shape[0].addr;
        if (!groups) continue;
        if (node->flags & sdfModelNodeHidden) continue;

        NodeDraw draw{};
        std::memcpy(&draw.model, &node->tm, sizeof(Matrix4));
        // @optout 0x2B1AA0 elsewhere(readScreenTm): sdf3DScreenTm is captured once a frame
        draw.clip = MultMatrixRev(draw.model, mScreenTm);
        // 0x2B1AA8..0x2B1AB8: a node without bounds is never culled.
        if (node->bounds && sdfModelCheckBounds(node->bounds, draw.clip)) continue;

        draw.sortZ = draw.clip.m[3][3];
        reportRootDepthBias(root->zBias);
        // 0x2B1ADC..0x2B1AEB: the node's own colour, or the root's.
        draw.color = (node->flags & sdfModelNodeOwnColor) ? node->color : root->color;
        // 0x2B1AEC..0x2B1B07: a negative colour, meaning an alpha over 0x80, sorts into table 0.
        draw.attrMask = u16(root->drawAttr) & node->drawAttr;
        draw.otIndex = s32(draw.color) < 0 ? 0 : 1;
        draw.reverse = (root->flags & sdfModelRootNoLinkedPackets) != 0;

        // @optout 0x2B1B08..0x2B1B23 packetbuf: clears the four per-ot draw tags
        // @optout 0x2B1B24 packetbuf: sdfCoreDrawSetZBias writes the bias into the packet

        // 0x2B1B30..0x2B1B5F, 0x2B1CF0: an otIndex word then a null-terminated array of temp
        // packet pointers. The guest folds the attr mask and colour sign into a qword; we carry
        // both on the draw.
        for (u32 g = 0; ; g++) {
            const u32 group = guest_ptr<u32>{groups + 4u * g}.valueOr();
            if (!group) break;

            NodeDraw packetDraw = draw;
            // 0x2B1B60: the group's own ot index, when it names one.
            if (const u16 otIndex = u16(guest_ptr<u32>{group}.valueOr() >> 16)) {
                packetDraw.otIndex = u8(otIndex);
            }

            // @optout 0x2B1B88..0x2B1C53 packetbuf: allocates this ot's draw tag, writes the
            // transform as a VIF packet and links it

            for (u32 e = 0; ; e++) {
                const sdfModelTempPacketPtr_t packet{guest_ptr<u32>{group + 4u + 4u * e}.valueOr()};
                // 0x2B1C58..0x2B1C70, 0x2B1CE4: the list ends at a null, and only type 1 draws.
                if (!packet) break;
                if (packet->type != 1) {
                    reportUnsupported("sdfModelTempPacketLinkOt packet type", packet->type);
                    continue;
                }
                // @optout 0x2B1C78..0x2B1CE3 dma: builds the material REF packet and links it
                const sdfMatPtr_t mat{readListItem(root->matList.addr, packet->matIndex)};
                if (!readVifStreamMesh(packet->dataPtr.addr,
                                       packet->dataPtr.addr + 16u * packet->qwc, packetDraw, mat,
                                       frame)) {
                    mTrace.countStrayRef();
                }
            }
        }
        // @optout 0x2B1CF8..0x2B1D3F packetbuf: files each allocated tag into its ordering table
        // 0x2B1D40..0x2B1D6B: every skip above lands on this same node-loop tail.
    }
    // @optout 0x2B1D6C..0x2B1D7F enginestate: the root's "drew" bit, which only the engine's own
    // linker reads back
    // @optout 0x2B1D80..0x2B1DB7 abi: epilogue
}

/// @fn sdfModelLinkShapedDrawPacket
/// @note SLUS-20911 0x002B0CB0
void ModelParser::sdfModelLinkShapedDrawPacket(guest_ref<sdfModelRootNode_t> root,
                                               const NodeDraw& draw, u32 dataAddr, Frame* frame) {
    mTrace.countShapedWalk();

    // 0x2B0CD0: the low half of word 1 indexes the morph list, the high half the material list.
    const u32 word1 = guest_ptr<u32>{dataAddr + 4}.valueOr();
    const u16 morphIndex = u16(word1 & 0xFFFF);
    const u16 matIndex = u16(word1 >> 16);
    const u16 shapeLists = u16(guest_ptr<u32>{dataAddr + 8}.valueOr() & 0xFFFF);

    const u32 morphItems = guest_ptr<u32>{root->morphList.addr + sdfListItems}.valueOr();
    const sdfModelMorphPtr_t morph{morphItems + u32(sizeof(sdfModelMorph_t)) * morphIndex};
    if (!morph) {
        reportUnsupported("sdfModelLinkShapedDrawPacket morph index", morphIndex);
        return;
    }

    // Only the targets the controller is actually blending towards reach the chain.
    u16 targets[kMorphTargets] = {};
    u32 used = 0;
    for (const sdfModelMorphParm_t& target : morph->target) {
        if (target.blend > sdfMorphEpsilon) targets[used++] = u16(target.targetId);
    }

    const sdfMatPtr_t mat{readListItem(root->matList.addr, matIndex)};
    for (u16 l = 0; l < shapeLists; l++) {
        sdfModelShapeListPtr_t list{guest_ptr<u32>{dataAddr + 12u + 4u * l}.valueOr()};
        if (!list) {
            reportUnsupported("sdfModelLinkShapedDrawPacket shape list", l);
            continue;
        }
        for (u32 t = 0; t < used; t++) {
            const sdfModelShapeEntry_t entry =
                guest_ptr<sdfModelShapeEntry_t>{list.addr + offsetof(sdfModelShapeList_t, entry) +
                                                u32(sizeof(sdfModelShapeEntry_t)) * targets[t]}
                    .valueOr();
            if (!entry.dataPtr) continue;
            if (!readVifStreamMesh(entry.dataPtr.addr, entry.dataPtr.addr + 16u * entry.qwc, draw,
                                   mat, frame)) {
                mTrace.countStrayRef();
            }
        }
        // The list's own stream closes the chain, which is the mesh the targets deform.
        if (!readVifStreamMesh(list->next.addr, list->next.addr + 16u * list->count, draw, mat,
                               frame)) {
            mTrace.countStrayRef();
        }
    }
}

/// One camera-facing quad, rotated in its own plane by the angle sdfBillBoardMake1 never sees.
/// @fn drawSprite3D
/// @note SLUS-20911 0x0012A2D0
void ModelParser::drawSprite3D(TexHandle aTexHandle, sdfSprite3DPtr_t aSprite, Frame* frame) {
    if (!frame || !aSprite) return;
    const sdfSprite3D_t& sprite = *aSprite;

    // A billboard faces the camera by construction, so the screen matrix's columns are exact.
    const auto axis = [](f32 x, f32 y, f32 z, f32 out[3]) {
        const f32 length = std::sqrt(x * x + y * y + z * z);
        const f32 scale = length > 1e-6f ? 1.0f / length : 0.0f;
        out[0] = x * scale;
        out[1] = y * scale;
        out[2] = z * scale;
    };
    f32 right[3];
    f32 up[3];
    axis(mScreenTm.m[0][0], mScreenTm.m[1][0], mScreenTm.m[2][0], right);
    axis(mScreenTm.m[0][1], mScreenTm.m[1][1], mScreenTm.m[2][1], up);

    DrawMesh mesh;
    Matrix4 identity{};
    for (int i = 0; i < 4; i++) identity.m[i][i] = 1.0f;
    mesh.bones.push_back(identity);
    mesh.color = Rgba::fromPacked(sprite.color);
    mesh.lighting = mLighting;
    mesh.fog = mFog;
    // Stamped alongside the state, or the row reports table 0 beside table 2's registers.
    mesh.otIndex = 2;
    mesh.gs = sdfGsStateForOt(mesh.otIndex);
    mesh.gs.frameMask = mState ? mState->frameMask : 0;
    mesh.attrBits = sdfCoreAttrTextured | sdfCoreAttrBlend | sdfPrimTriangle;
    mesh.lit = false;
    mesh.packetType = sdfPacketGenerated;
    mesh.texHandle = mTextures->getHandle(aTexHandle, frame);

    // 0x12A344: SetRotateMatrixZ turns the offsets in the quad's own plane before the camera
    // axes, which is why this call is the hook rather than sdfBillBoardMake1.
    const f32 turn = std::cos(sprite.rotation);
    const f32 lean = std::sin(sprite.rotation);
    const TexHandle bound{mesh.texHandle.addr};
    const f32 width = bound ? f32(bound->width) : 0.0f;
    const f32 height = bound ? f32(bound->height) : 0.0f;
    const f32 uScale = width > 0.0f ? 1.0f / (16.0f * width) : 0.0f;
    const f32 vScale = height > 0.0f ? 1.0f / (16.0f * height) : 0.0f;

    mesh.vertices.resize(4);
    for (u32 c = 0; c < 4; c++) {
        MeshVertex& out = mesh.vertices[c];
        const sdfVector2_t& at = sprite.offset.corner[c];
        const f32 x = at.x * turn - at.y * lean;
        const f32 y = at.x * lean + at.y * turn;
        setRigidInfluence(out,
                          sdfVector3_t{sprite.centre.x + right[0] * x + up[0] * y,
                                       sprite.centre.y + right[1] * x + up[1] * y,
                                       sprite.centre.z + right[2] * x + up[2] * y},
                          nullptr);
        out.u = f32(sprite.uv.corner[c].u) * uScale;
        out.v = f32(sprite.uv.corner[c].v) * vScale;
    }
    static constexpr u16 kQuad[6] = {0, 2, 1, 0, 3, 2};
    mesh.indices.assign(std::begin(kQuad), std::end(kQuad));
    mTrace.countBones(1);
    frame->addMesh(std::move(mesh));
}

/// An indexed triangle list in world space: the vertices are quadword-strided with only xyz used,
/// and +0x0C holds one byte index per lane, three to a word, with the word count at +0x00.
/// @fn sdf3DPrimMakePoly
/// @note SLUS-20911 0x002BAAB8
void ModelParser::sdf3DPrimMakePoly(sdf3DPolyPtr_t aPoly, Frame* frame) {
    if (!frame || !aPoly) return;
    const u32 vertexCount = u32(aPoly->vertexCount);
    const u32 triangles = u32(aPoly->extraQwords);
    if (!vertexCount || !triangles) return;

    const sdfVector4_t* point = sdfVector4Ptr_t{aPoly->position.addr}.slice(vertexCount);
    if (!point) point = sdfVector4Ptr_t{aPoly->unk10}.slice(vertexCount);
    const u32* index = u32Ptr_t{aPoly->unk0C}.slice(triangles);
    if (!point || !index) {
        reportUnsupported("sdf3DPrimMakePoly without geometry", aPoly.addr);
        return;
    }

    DrawMesh mesh;
    Matrix4 identity{};
    for (int axis = 0; axis < 4; axis++) identity.m[axis][axis] = 1.0f;
    mesh.bones.push_back(identity);
    mesh.lighting = mLighting;
    mesh.fog = mFog;
    mesh.gs = sdfGsStateForOt(0);
    // fldSdwDraw sets its own blend and test through rsetAlpha and rsetTest rather than taking an
    // ordering table's pair, so the state in force at the call is the one this draw runs under.
    if (mState) {
        mesh.gs.frameMask = mState->frameMask;
        mesh.gs.test = mState->test;
    }
    // 0x2BAAF4: the prim word is the descriptor's own or'd with 0x68, which is IIP, FGE and ABE.
    mesh.attrBits = u16(aPoly->prim | 0x68);
    const GsAlpha alpha = mState ? mState->alpha : GsAlpha::Blend;
    if (alpha == GsAlpha::Opaque) {
        mesh.attrBits = u16(mesh.attrBits & ~u16(sdfCoreAttrBlend));
    } else {
        mesh.gs.alpha = u64(alpha);
    }
    mesh.color = Rgba::fromPacked(aPoly->colorWord);
    mesh.lit = false;
    mesh.packetType = sdfPacketGenerated;

    mesh.vertices.resize(vertexCount);
    for (u32 v = 0; v < vertexCount; v++) {
        setRigidInfluence(mesh.vertices[v], sdfVector3_t{point[v].x, point[v].y, point[v].z},
                          nullptr);
        mesh.vertices[v].color = mesh.color;
    }
    for (u32 t = 0; t < triangles; t++) {
        for (int corner = 0; corner < 3; corner++) {
            const u32 at = (index[t] >> (8 * corner)) & 0xFF;
            if (at >= vertexCount) return;
            mesh.indices.push_back(u16(at));
        }
    }
    mTrace.countBones(1);
    mTrace.countStreamGeom(triangles);
    mTrace.countStreamMesh(triangles);
    frame->addMesh(std::move(mesh));
}

/// A flat triangle the field draws directly, with a colour per corner and no texture.
/// @fn fldDrawPoly3
/// @note SLUS-20911 0x0012A5E0
void ModelParser::fldDrawPoly3(const sdfVector3_t point[3], const u32 color[3], Frame* frame) {
    if (!frame) return;

    DrawMesh mesh;
    Matrix4 identity{};
    for (int axis = 0; axis < 4; axis++) identity.m[axis][axis] = 1.0f;
    mesh.bones.push_back(identity);
    mesh.lighting = mLighting;
    mesh.fog = mFog;
    mesh.gs = sdfGsStateForOt(0);
    // Gouraud and blended, with no texture: the call carries no UVs, so there is nothing to sample.
    mesh.attrBits = sdfCoreAttrGouraud | sdfCoreAttrBlend | sdfPrimTriangle;
    mesh.lit = false;
    mesh.packetType = sdfPacketGenerated;
    mesh.vertices.resize(3);
    for (u32 i = 0; i < 3; i++) {
        setRigidInfluence(mesh.vertices[i], point[i], nullptr);
        mesh.vertices[i].color = Rgba::fromPacked(color[i]);
    }
    static constexpr u16 kTriangle[3] = {0, 1, 2};
    mesh.indices.assign(std::begin(kTriangle), std::end(kTriangle));
    mTrace.countBones(1);
    frame->addMesh(std::move(mesh));
}

/// Self-describing, so it is read rather than rebuilt.
/// @fn fldSdwDraw
/// @note SLUS-20911 0x001193E8
bool ModelParser::fldSdwDraw(u32 aPacket, u32 aEnd, const sdfGsState_t& aState,
                             Frame* frame) {
    if (!frame || aEnd <= aPacket) return false;

    // The stream opens with a V4_8 color unpack and its settings, so the batch header is not at the
    // target the way a model batch's is. Walk the codes to it rather than assuming an offset.
    u32 at = aPacket;
    VifCode header;
    bool found = false;
    while (at + 8 <= aEnd) {
        VifCode code;
        if (!readVifCode(at, code)) return false;
        if (isUnpack(code) && code.count == 1 && unpackWidth(code) == 8) {
            header = code;
            found = true;
            break;
        }
        // Everything else is a one-word setting or a kick.
        at = isUnpack(code) ? code.next + unpackWidth(code) * code.count : code.next;
    }
    if (!found) return false;

    const u16* fields = u16Ptr_t{header.next}.slice(4);
    if (!fields) return false;
    const u32 faceCount = fields[0];
    const u32 pointCount = fields[1];
    const u16 attrBits = fields[2];
    if (!faceCount || !pointCount) return false;

    // Triangle indices follow as one V4_8 element per face, three bytes and a pad.
    VifCode indexCode;
    if (!readVifCode(header.next + 8, indexCode) || !isUnpack(indexCode) ||
        unpackWidth(indexCode) != 4 || indexCode.count != faceCount) {
        return false;
    }
    const u8* faces = u8Ptr_t{indexCode.next}.slice(faceCount * 4);
    if (!faces) return false;

    // The positions get their own unpack, V3_32, rather than trailing the indices: reading them at
    // the end of the index array takes the code word itself as the first x.
    VifCode pointCode;
    if (!readVifCode(indexCode.next + faceCount * 4, pointCode) || !isUnpack(pointCode) ||
        unpackWidth(pointCode) != 12 || pointCode.count != pointCount) {
        return false;
    }
    const sdfVector3_t* points = sdfVector3Ptr_t{pointCode.next}.slice(pointCount);
    if (!points) return false;

    DrawMesh mesh;
    Matrix4 identity{};
    for (int axis = 0; axis < 4; axis++) identity.m[axis][axis] = 1.0f;
    mesh.bones.push_back(identity);
    mesh.lighting = mLighting;
    mesh.fog = mFog;
    // The state the chain left in force at this packet, FBMSK included: the engine masks RGB for
    // part of this pass, and drawing that as visible color is what costs 24 points.
    mesh.gs = aState;
    mesh.attrBits = attrBits;
    mesh.lit = false;
    mesh.packetType = sdfPacketGenerated;

    mesh.vertices.resize(pointCount);
    for (u32 i = 0; i < pointCount; i++) setRigidInfluence(mesh.vertices[i], points[i], nullptr);

    mesh.indices.reserve(faceCount * 3);
    for (u32 f = 0; f < faceCount; f++) {
        for (u32 i = 0; i < 3; i++) {
            const u8 index = faces[f * 4 + i];
            if (index >= pointCount) return false;
            mesh.indices.push_back(index);
        }
    }

    mTrace.countBones(1);
    frame->addMesh(std::move(mesh));
    return true;
}

/// @fn dds3BillboardLinkOt
/// @note SLUS-20911 0x0022E2E8
constexpr u32 kMaxBillboardNodes = 4096;

void ModelParser::readBillboardTables() {
    mBillboardOt.clear();
    if (!mDds3BillboardTag) return;
    // The engine's own loop rewrites `next` as it consumes each node, so this runs at its entry.
    dds3BillboardNodePtr_t node{u32Ptr_t{mDds3BillboardTag.addr}.valueOr()};
    for (u32 steps = 0; node && steps < kMaxBillboardNodes; steps++) {
        mBillboardOt[node->batch.addr] = node->otIndex;
        node = node->next;
    }
}

/// Builds one batch of camera-facing quads from the centres, offsets and UVs
/// the engine is handed.
/// @fn sdfBillBoardMakeN
/// @note SLUS-20911 0x002BB0F0
void ModelParser::sdfBillBoardMakeN(sdfBillBoardBatchPtr_t aBatch, u32 aNum, Frame* frame) {
    if (!frame || !aBatch || !aNum || aNum > std::size(aBatch->centre)) return;
    const sdfBillBoardBatch_t& batch = *aBatch;

    // Offsets are view-space, so they need the camera's axes to land back in world.
    // A billboard faces the camera by construction, which is what makes taking them from the
    // matrix's columns exact rather than an approximation.
    const Matrix4& tm = mScreenTm;
    const auto axis = [](f32 x, f32 y, f32 z, f32 out[3]) {
        const f32 length = std::sqrt(x * x + y * y + z * z);
        const f32 scale = length > 1e-6f ? 1.0f / length : 0.0f;
        out[0] = x * scale;
        out[1] = y * scale;
        out[2] = z * scale;
    };
    f32 right[3];
    f32 up[3];
    axis(tm.m[0][0], tm.m[1][0], tm.m[2][0], right);
    axis(tm.m[0][1], tm.m[1][1], tm.m[2][1], up);

    // The node above the batch names its table; the GS trace puts the specters' additive batches at
    // ALPHA 0x48 with TEST 0x71801, which is what table 2 decodes to, so a miss says so rather than
    // silently picking one.
    const auto found = mBillboardOt.find(aBatch.addr);
    if (found == mBillboardOt.end()) reportUnsupported("billboard batch with no ordering table", 0);
    const u32 otIndex = found != mBillboardOt.end() ? found->second : 2;

    for (u32 i = 0; i < aNum; i++) {
        const sdfVector4_t& centre = batch.centre[i];
        const sdfBillBoardQuad_t& offset = batch.offset[i];
        const sdfBillBoardUv_t& uv = batch.uv[i];

        DrawMesh mesh;
        Matrix4 identity{};
        for (int axisIndex = 0; axisIndex < 4; axisIndex++) identity.m[axisIndex][axisIndex] = 1.0f;
        mesh.bones.push_back(identity);
        mesh.color = Rgba::fromPacked(batch.color[i]);
        mesh.lighting = mLighting;
        mesh.fog = mFog;
        mesh.otIndex = u8(otIndex);
        mesh.gs = sdfGsStateForOt(otIndex);
        mesh.gs.frameMask = mState ? mState->frameMask : 0;
        mesh.attrBits = sdfCoreAttrTextured | sdfCoreAttrBlend | sdfPrimTriangle;
        mesh.lit = false;
        mesh.billboard = true;
        mesh.packetType = sdfPacketGenerated;
        mesh.texHandle = mTextures->getHandle(mBillboardTexture, frame);
        // The batch stores texel coordinates in 12.4; the sampler wants them over the texture.
        const f32 width = mBillboardTexture ? f32(mBillboardTexture->width) : 0.0f;
        const f32 height = mBillboardTexture ? f32(mBillboardTexture->height) : 0.0f;
        const f32 uScale = width > 0.0f ? 1.0f / (16.0f * width) : 0.0f;
        const f32 vScale = height > 0.0f ? 1.0f / (16.0f * height) : 0.0f;
        mesh.vertices.resize(4);
        for (u32 c = 0; c < 4; c++) {
            MeshVertex& out = mesh.vertices[c];
            const sdfVector2_t& at = offset.corner[c];
            const sdfVector3_t world{centre.x + right[0] * at.x + up[0] * at.y,
                                     centre.y + right[1] * at.x + up[1] * at.y,
                                     centre.z + right[2] * at.x + up[2] * at.y};
            setRigidInfluence(out, world, nullptr);
            out.u = f32(uv.corner[c].u) * uScale;
            out.v = f32(uv.corner[c].v) * vScale;
        }
        // Wound the way the engine lists the corners, which is the reverse of a GL front face.
        static constexpr u16 kQuad[6] = {0, 2, 1, 0, 3, 2};
        mesh.indices.assign(std::begin(kQuad), std::end(kQuad));
        mTrace.countBones(1);
        frame->addMesh(std::move(mesh));
    }
}

/// Reads a type 1 DMA call chain, taking the material from the shape
/// record's matIndex and the stream from extData.
/// @fn sdfModelSetNormalDrawPacket
/// @note SLUS-20911 0x002B0AC0
void ModelParser::sdfModelSetNormalDrawPacket(const NodeDraw& draw, u32 chainAddr, Frame* frame) {
    mCurrentChain = chainAddr;
    sdfMatPtr_t mat;
    for (u32 at = chainAddr; ; at += 16) {
        const u32* tag = u32Ptr_t{at}.slice(2);
        if (!tag) return;

        const u32 id = (tag[0] >> 28) & 7;
        if (id != sdfDmaTagRef) return;

        const u32 target = tag[1];
        const auto found = mMatByPacket.find(target);
        if (found != mMatByPacket.end()) {
            mat = {found->second};
            continue;
        }

        // A mesh stream opens with the batch header unpack. Anything else in the chain is setup
        // the hardware consumes and we already have by other means, so it is counted, not read.
        VifCode code;
        if (!readVifCode(target, code) || !isUnpack(code) || code.count != 1 ||
            unpackWidth(code) != 8) {
            mTrace.countSetupRef();
            continue;
        }
        if (!readVifStreamMesh(target, target + 16u * (tag[0] & 0xFFFF), draw, mat, frame)) {
            mTrace.countStrayRef();
        }
    }
}

bool ModelParser::readVifStreamMesh(u32 at, u32 end, const NodeDraw& draw, sdfMatPtr_t aMat,
                                    Frame* frame) {
    DrawMesh mesh;
    mesh.linkAddr = mCurrentChain;
    mesh.sortZ = draw.sortZ;
    mesh.otIndex = draw.otIndex;
    mesh.color = Rgba::fromPacked(draw.color);
    mesh.bones.push_back(draw.model);
    mesh.lighting = mLighting;
    mesh.fog = mFog;
    mesh.gs = sdfGsStateForOt(mesh.otIndex);
    mesh.gs.frameMask = mState ? mState->frameMask : 0;
    mesh.reverseFacing = draw.reverse;
    mesh.worldGeometry = true;
    mesh.packetType = sdfPacketDmaCall;

    bool first = true;
    while (at + 16 <= end) {
        u32 headerData = 0;
        u32 next = 0;
        if (!readArray(at, 8, 1, headerData, next)) break;
        const sdfCoreBatchHeaderPtr_t header{headerData};
        if (!header) return false;

        const u16 faceCount = header->faceCount;
        const u16 pointCount = header->pointCount;
        const u16 attrBits = header->attrBits;
        const u16 packetBits = header->packetBits;
        const bool firstBatch = first;
        if (first) {
            mesh.attrBits = attrBits & draw.attrMask;
            mesh.packetBits = packetBits;
        }
        first = false;
        if (!faceCount || !pointCount) return false;

        u32 indices = 0;
        u32 positions = 0;
        if (!readArray(next, 4, faceCount, indices, next)) return false;
        if (!readArray(next, 12, pointCount, positions, next)) return false;

        // Ask the stream, not the flag: world geometry clears packetBits 0x80 and still carries
        // normals. Only positions and normals are 12 wide, and positions are already consumed.
        u32 normals = 0;
        const bool declared = (packetBits & sdfCorePacketNormals) != 0;
        if (!readArray(next, 12, pointCount, normals, next) && declared) return false;
        // Lit is what the stream turned out to carry, not what packetBits claimed.
        if (firstBatch) mesh.lit = normals != 0;

        u32 texcoords = 0;
        const bool bothUv = (attrBits & sdfCoreAttrMultiTexPass) != 0;
        if ((attrBits & sdfCoreAttrTextured) &&
            !readArray(next, bothUv ? 16 : 8, pointCount, texcoords, next)) {
            return false;
        }

        u32 colors = 0;
        if ((attrBits & sdfCoreAttrColor) &&
            !readArray(next, 4, pointCount, colors, next)) {
            return false;
        }

        const sdfVector3_t* position = sdfVector3Ptr_t{positions}.slice(pointCount);
        const sdfVector3_t* normal = sdfVector3Ptr_t{normals}.slice(pointCount);
        // One packet carries both UV sets when a second is asked for, so the stride doubles.
        const sdfVector2_t* uv =
            sdfVector2Ptr_t{texcoords}.slice(pointCount * (bothUv ? 2u : 1u));
        const u32* color = u32Ptr_t{colors}.slice(pointCount);
        const u32* triangles = u32Ptr_t{indices}.slice(faceCount);
        if (!position || !triangles) return false;

        const u32 base = u32(mesh.vertices.size());
        mesh.vertices.resize(base + pointCount);
        for (u16 v = 0; v < pointCount; v++) {
            MeshVertex& out = mesh.vertices[base + v];
            setRigidInfluence(out, position[v], normal ? &normal[v] : nullptr);
            if (uv) {
                const sdfVector2_t* pair = uv + (bothUv ? 2u : 1u) * v;
                out.u = pair[0].x;
                out.v = pair[0].y;
                if (bothUv) {
                    out.u2 = pair[1].x;
                    out.v2 = pair[1].y;
                }
            }
            if (color) {
                out.color = Rgba::fromPacked(color[v]);
                mesh.streamColors = true;
            }
            mTrace.countInfluences(1);
        }

        // Indices are a byte each and local to the batch, with a fourth byte the hardware ignores.
        for (u16 f = 0; f < faceCount; f++) {
            const u32 triangle = triangles[f];
            for (int corner = 0; corner < 3; corner++) {
                const u32 index = (triangle >> (8 * corner)) & 0xFF;
                if (index >= pointCount) return false;
                mesh.indices.push_back(u16(base + index));
            }
        }

        // The microprogram kick that closes a batch.
        VifCode kick;
        if (!readVifCode(next, kick)) return false;
        at = kick.next;
    }
    if (mesh.vertices.empty()) return false;

    if (aMat) {
        mesh.texHandle = mTextures->getHandle(aMat->texHandle, frame);
        mTrace.countTexture(aMat->texHandle.addr != 0, mesh.texHandle.addr != 0);
        readMaterialPacket(*aMat, mesh, frame);
    }

    mTrace.countOt(mesh.otIndex);
    mTrace.countSampler(mesh.tex0, mesh.tex1, mesh.clamp);
    mTrace.countDeterminant(mesh.bones[0]);
    mTrace.countStreamGeom(u32(mesh.indices.size() / 3));
    mTrace.countStreamMesh(u32(mesh.indices.size() / 3));
    mTrace.countBones(1);
    if (frame) frame->addMesh(std::move(mesh));
    return true;
}

/// Keeps a triangle when its signed screen area times the packet's backface
/// sign is non-negative.
/// @fn corePktBuildFaceList
/// @note SLUS-20911 0x002B7A80
bool ModelParser::corePktBuildFaceList(u32 at, const sdfCorePacket_t& packet,
                              DrawMesh& mesh) {
    const u32 bytes = 6u * packet.prim.faceCount;
    const u8* source = u8Ptr_t{at}.slice(bytes);
    if (!source) return false;

    mesh.indices.resize(3u * packet.prim.faceCount);
    std::memcpy(mesh.indices.data(), source, bytes);
    for (u16 index : mesh.indices) {
        if (index >= packet.prim.pointCount) return false;
    }
    return true;
}

// The second set trails every batch as a plain array, so it is read once the first is complete.
void ModelParser::readSecondUv(u32 at, const sdfCorePacket_t& packet,
                             DrawMesh& mesh) {
    if (!(packet.prim.attrBits & sdfCoreAttrMultiTexPass)) return;
    const sdfVector2_t* second = sdfVector2Ptr_t{at}.slice(packet.prim.pointCount);
    if (!second) return;
    for (u16 v = 0; v < packet.prim.pointCount; v++) {
        const sdfVector2_t& uv = second[v];
        mesh.vertices[v].u2 = uv.x;
        mesh.vertices[v].v2 = uv.y;
    }
}

bool ModelParser::readRigidBatches(u32 at, const sdfCorePacket_t& packet,
                                 DrawMesh& mesh) {
    while (mesh.vertices.size() < packet.prim.pointCount) {
        u16 count = 0;
        u16 unused = 0;
        u32 next = 0;
        if (!readBatchHeader(at, count, unused, next) || !count) return false;

        u32 positions = 0;
        u32 normals = 0;
        u32 texcoords = 0;
        if (!readArray(next, 12, count, positions, next)) return false;
        if (!readArray(next, 12, count, normals, next)) return false;
        if (!readArray(next, 8, count, texcoords, next)) return false;

        const sdfVector3_t* position = sdfVector3Ptr_t{positions}.slice(count);
        const sdfVector3_t* normal = sdfVector3Ptr_t{normals}.slice(count);
        const sdfVector2_t* uv = sdfVector2Ptr_t{texcoords}.slice(count);
        if (!position || !normal || !uv) return false;

        const usize base = mesh.vertices.size();
        mesh.vertices.resize(base + count);
        for (u16 v = 0; v < count; v++) {
            MeshVertex& out = mesh.vertices[base + v];
            setRigidInfluence(out, position[v], &normal[v]);
            out.u = uv[v].x;
            out.v = uv[v].y;
            mTrace.countInfluences(1);
        }

        at = skipBatchEnd(next);
        if (!at) return false;
    }
    if (mesh.vertices.size() != packet.prim.pointCount) return false;
    readSecondUv(at, packet, mesh);
    return true;
}

bool ModelParser::readWeightedBatches(u32 at, const sdfCorePacket_t& packet,
                                    u32 nodesAddr, DrawMesh& mesh) {
    const u16 usedNodeCount = u16Ptr_t{nodesAddr}.valueOr();
    if (!usedNodeCount || usedNodeCount > kMaxBones) return false;

    // Every batch blends the same node set, so the palette is built once from the mesh header.
    const u16* nodeIndices = u16Ptr_t{nodesAddr + 2}.slice(usedNodeCount);
    if (!nodeIndices) return false;
    mesh.bones.resize(usedNodeCount);
    for (u16 n = 0; n < usedNodeCount; n++) {
        const u32 nodeAddr = readListItem(mNodeListAddr, nodeIndices[n]);
        const Matrix4Ptr_t tm{nodeAddr + u32(offsetof(sdfModelNode_t, tm))};
        if (!tm) return false;
        mesh.bones[n] = *tm;
    }

    while (mesh.vertices.size() < packet.prim.pointCount) {
        u16 extraNodes = 0;
        u16 count = 0;
        u32 next = 0;
        if (!readBatchHeader(at, extraNodes, count, next) || !count) return false;
        if (usize(extraNodes) + 1 != usedNodeCount) return false;

        const usize base = mesh.vertices.size();
        mesh.vertices.resize(base + count);
        // A node's whole vertex array arrives before the next node's, so every vertex collects one
        // influence per pass rather than all of its own at once.
        for (u16 n = 0; n < usedNodeCount; n++) {
            u32 positions = 0;
            u32 normals = 0;
            if (!readArray(next, 16, count, positions, next)) return false;
            if (!readArray(next, 12, count, normals, next)) return false;

            // w is the node's weight for that vertex, which is why a blended position is a vec4.
            const sdfVector4_t* position = sdfVector4Ptr_t{positions}.slice(count);
            const sdfVector3_t* normal = sdfVector3Ptr_t{normals}.slice(count);
            if (!position || !normal) return false;

            for (u16 v = 0; v < count; v++) {
                const f32 weight = position[v].w;
                if (weight == 0.0f) continue;

                MeshVertex& out = mesh.vertices[base + v];
                usize slot = 0;
                while (slot < kMaxBoneInfluences && out.influence[slot].weight != 0.0f) slot++;
                if (slot == kMaxBoneInfluences) {
                    mTrace.countInfluences(kMaxBoneInfluences + 1);
                    continue;
                }
                MeshInfluence& influence = out.influence[slot];
                influence.x = position[v].x;
                influence.y = position[v].y;
                influence.z = position[v].z;
                influence.nx = normal[v].x;
                influence.ny = normal[v].y;
                influence.nz = normal[v].z;
                influence.weight = weight;
                influence.bone = f32(n);
            }
            VifCode kick;
            if (!readVifCode(next, kick)) return false;
            next = kick.next;
        }

        u32 texcoords = 0;
        if (!readArray(next, 8, count, texcoords, next)) return false;
        const sdfVector2_t* uv = sdfVector2Ptr_t{texcoords}.slice(count);
        if (!uv) return false;
        for (u16 v = 0; v < count; v++) {
            mesh.vertices[base + v].u = uv[v].x;
            mesh.vertices[base + v].v = uv[v].y;
        }

        at = skipBatchEnd(next);
        if (!at) return false;
    }
    if (mesh.vertices.size() != packet.prim.pointCount) return false;

    for (const MeshVertex& vertex : mesh.vertices) {
        u32 used = 0;
        for (const MeshInfluence& influence : vertex.influence) {
            used += influence.weight != 0.0f ? 1 : 0;
        }
        mTrace.countInfluences(used);
    }
    readSecondUv(at, packet, mesh);
    return true;
}

bool ModelParser::readBlendShapes(u32 at, const sdfCorePacket_t& packet,
                                guest_ref<sdfModelRootNode_t> root, DrawMesh& mesh) {
    const u16 shapeCount = sdfCoreShapeCount(packet);
    const u16 morphIndex = sdfCoreMorphIndex(packet);
    const u16 count = packet.prim.pointCount;
    if (!shapeCount) return false;

    // Shape zero is the mesh; the rest are deltas the morph controller mixes onto it.
    const u32 shapeStride = align16(12u * count) * 2;
    const sdfVector3_t* base = sdfVector3Ptr_t{at}.slice(count);
    const sdfVector3_t* baseNormal = sdfVector3Ptr_t{at + align16(12u * count)}.slice(count);
    if (!base || !baseNormal) return false;

    mesh.vertices.resize(count);
    for (u16 v = 0; v < count; v++) {
        MeshVertex& out = mesh.vertices[v];
        const sdfVector3_t& position = base[v];
        setRigidInfluence(out, position, &baseNormal[v]);
        mTrace.countInfluences(1);
    }

    const u32 morphItems = u32Ptr_t{root->morphList.addr + sdfListItems}.valueOr();
    const sdfModelMorphListPtr_t morphList = root->morphList;
    const sdfModelMorphPtr_t morph{morphItems + u32(sizeof(sdfModelMorph_t)) * morphIndex};
    if (!morphItems || !morphList || morphIndex >= morphList->count || !morph) {
        mTrace.countReject(sdfCorePacketTypeBlendShape, ModelTrace::Reject::NoMorph);
    } else {
        for (const sdfModelMorphParm_t& target : morph->target) {
            if (target.blend <= sdfMorphEpsilon || target.targetId >= shapeCount) continue;
            const u32 shape = at + shapeStride * (target.targetId + 1);
            const sdfVector3_t* delta = sdfVector3Ptr_t{shape}.slice(count);
            const sdfVector3_t* deltaNormal =
                sdfVector3Ptr_t{shape + align16(12u * count)}.slice(count);
            if (!delta || !deltaNormal) return false;
            for (u16 v = 0; v < count; v++) {
                const sdfVector3_t& position = delta[v];
                const sdfVector3_t& normal = deltaNormal[v];
                MeshInfluence& out = mesh.vertices[v].influence[0];
                out.x += target.blend * position.x;
                out.y += target.blend * position.y;
                out.z += target.blend * position.z;
                out.nx += target.blend * normal.x;
                out.ny += target.blend * normal.y;
                out.nz += target.blend * normal.z;
            }
        }
    }

    u32 next = at + shapeStride * shapeCount;
    if (packet.prim.attrBits & sdfCoreAttrTextured) {
        const sdfVector2_t* texcoords = sdfVector2Ptr_t{next}.slice(count);
        if (!texcoords) return false;
        for (u16 v = 0; v < count; v++) {
            const sdfVector2_t& uv = texcoords[v];
            mesh.vertices[v].u = uv.x;
            mesh.vertices[v].v = uv.y;
        }
        next += 8u * count;
    }
    readSecondUv(next, packet, mesh);
    return true;
}

bool ModelParser::finishMesh(guest_ref<sdfModelRootNode_t> root,
                           const sdfCorePacket_t& packet, sdfCorePacketType_t type, DrawMesh& mesh,
                           Frame* frame) {
    const sdfMatPtr_t material{readListItem(root->matList.addr, packet.header.matIndex)};
    if (!material) {
        mTrace.countReject(type, ModelTrace::Reject::NoMaterial);
        return false;
    }
    mesh.texHandle = mTextures->getHandle(material->texHandle, frame);
    mTrace.countTexture(material->texHandle.addr != 0, mesh.texHandle.addr != 0);
    readMaterialPacket(*material, mesh, frame);
    mesh.lighting = mLighting;
    mesh.fog = mFog;
    mesh.gs = sdfGsStateForOt(mesh.otIndex);
    mesh.gs.frameMask = mState ? mState->frameMask : 0;
    mesh.linkIndex = mCurrentLink;
    mesh.lit = (packet.prim.packetBits & sdfCorePacketNormals) != 0;
    mesh.matFlags = material->flags;
    mesh.matDuty = material->duty;
    mesh.packetType = packet.header.packetType;
    mesh.packetBits = packet.prim.packetBits;
    mTrace.countMatDuty(material->duty, mSdfFrameId ? *mSdfFrameId & 1 : 0);
    // Reported here rather than where the ramp is decided, because readMaterialPacket runs for
    // materials whose mesh is never drawn: on the drawn set every mesh is algo 0 or 3 and none
    // ramps, which is what the engine does (152 corePktSetConstColor, no corePktApplyFaceDivColor).
    if (mesh.multi.ramp && !mesh.multi.rampColors.known) {
        reportUnsupported("corePktMakeFaceDivParm endpoints for a drawn ramp mesh", mesh.multi.algo);
    }
    mesh.diffuse = Rgba::fromPacked(material->diffuse);
    mesh.ambient = Rgba::fromPacked(material->ambient);

    // Only the face-division terms are the material's alone: attrBits, lit and the second pass's
    // texture all vary per mesh by design. These two cannot, so a difference means one mesh missed
    // the assignment and kept the struct default.
    mTrace.countMaterialShading(material.addr,
                                (u64(std::bit_cast<u32>(mesh.faceDivSoften)) << 32) |
                                    u64(std::bit_cast<u32>(mesh.faceDivBias)));
    mTrace.countMultiTexture(mesh.multi.on);
    mTrace.countMesh(type, packet.prim.faceCount);
    if (frame) frame->addMesh(std::move(mesh));
    return true;
}

}  // namespace sdfhook
