#include "model.h"

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

/// Fills the one influence a body without weights has, so every mesh reaches the shader the same
/// way whatever produced it.
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
/// "Rev" names: the argument multiplies from the right.
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

// The six bits VCLIP sets, in its own order. sdfModelCheckBounds ANDs them across the corners, so
// only agreement between bits matters, not which plane each one names.
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

// Both lists the walk reads keep a pointer to their item array at sdfListItems rather than the
// array itself, which is why reaching an item costs two loads.
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

/// One vertex array: the UNPACK that names it, checked against the width and count the caller
/// expected, so a misread offset is refused rather than decoded into plausible nonsense.
bool readArray(u32 at, u32 width, u32 count, u32& data, u32& next) {
    VifCode code;
    if (!readVifCode(at, code) || !isUnpack(code)) return false;
    if (unpackWidth(code) != width || code.count != count) return false;
    data = code.next;
    next = data + width * count;
    return true;
}

/// A batch's header names how many vertices it carries, and for a weighted mesh how many nodes
/// beyond the first blend into them.
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
    histogram("reflectiveSpecular", mSpecular, "colour");

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

    // sdfCorePacketGen and sdfModelLinkShapedDrawPacket have no other caller, so a difference here
    // is our traversal reaching a different set of packets, not a different way of drawing them.
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
    for (const auto& [colour, count] : mSpecular) {
        log.info("reflective mesh specular {:08X}   {} meshes", colour, count);
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

    // sdfCorePacketGen and sdfModelLinkShapedDrawPacket have no other caller, so a difference here
    // is our traversal reaching a different set of packets, not a different way of drawing them.
    const u64 walkedGenerated = mPackets[usize(Packet::Generated)];
    const u64 walkedShaped = mPackets[usize(Packet::Shaped)];
    log.info("traversal: generated {} walked / {} by the engine{}", walkedGenerated,
        mGuestGenerated, walkedGenerated == mGuestGenerated ? "" : "   <- MISMATCH");
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
}

// The directions are a matrix whose columns are the light vectors: VU1 multiplies the normal
// through it as rows and reads the three dot products out of the result.
void ModelParser::readLighting(sdf3DLightParallelPacketPtr_t aLight) {
    mLighting = {};
    const sdf3DLightParallelPacketPtr_t light = aLight ? aLight : mSdf3DLightDefault.valueOr();
    if (!light) return;

    for (u32 i = 0; i < kParallelLights; i++) {
        for (usize axis = 0; axis < 3; axis++) {
            // normal is a matrix whose columns are the light vectors, so a light reads down them.
            mLighting.direction[i][axis] = light->normal[axis][i];
            mLighting.colour[i][axis] = light->color[i][axis];
        }
        mLighting.base[i] = light->colorBase[i];
        mLighting.ambientColour[i] = light->color[3][i];
        mLighting.fullColour[i] = light->color[4][i];
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
            row.addReals("colour",
                         {mLighting.colour[i][0], mLighting.colour[i][1], mLighting.colour[i][2]}, 4);
            row.endObject();
        }
        row.endArray();
        log.dump(row.take());
    }
}

// GS fog is a per-vertex F where 255 keeps the colour and 0 replaces it with FOGCOL. VU1 builds F
// from 1/w, bounded by the first two parameters.
void ModelParser::readFog(sdf3DFogPacketPtr_t aFog) {
    mFog = {};
    const sdf3DFogPacketPtr_t fog = aFog ? aFog : mSdf3DFogDefault.valueOr();
    if (!fog) return;

    const u32 fogcol = u32(fog->fogColor);
    mFog.on = true;
    mFog.minimum = fog->fogParm[0] / 255.0f;
    mFog.maximum = fog->fogParm[1] / 255.0f;
    mFog.scale = fog->fogParm[3] / 255.0f;
    mFog.colour[0] = f32(fogcol & 0xFF) / 255.0f;
    mFog.colour[1] = f32((fogcol >> 8) & 0xFF) / 255.0f;
    mFog.colour[2] = f32((fogcol >> 16) & 0xFF) / 255.0f;
}

// The sampler state a draw runs under lives in the material's own packet, which
// sdfMatUpdatePacketBasic fills from the texture handle rather than from the material.
void ModelParser::readMaterialPacket(const sdfMat_t& material, DrawMesh& mesh,
                                   Frame* frame) {
    mesh.diffuse = Rgba::fromPacked(material.diffuse);
    mesh.ambient = Rgba::fromPacked(material.ambient);

    // The finished buffer, not the one about to be rewritten: our walk runs at sdfModelLinkOt's
    // entry and the engine's next call is sdfMatListUpdatePacket, so the index the frame id names is
    // the one it is about to fill. Reading the other one is what brings in the arena floor's blood
    // decals (tbp 12397), which the GS samples and we otherwise never did.
    const u32 frameId = mSdfFrameId ? (*mSdfFrameId & 1) ^ 1 : 0;
    const sdfMatPacketPtr_t packet{material.packet[frameId].addr};
    if (!packet) return;
    mesh.specular = Rgba::fromPacked(packet->specularColor);
    mTrace.countSpecular(mesh.attrBits, packet->specularColor);
    mesh.tex0 = packet->tex0Data;
    mesh.tex1 = packet->tex1Data;
    mesh.clamp = packet->clampData;
    // Inline at the packet's own texTm field rather than behind it: the field's address is what
    // corePktXfmLightClipUV is handed. Six floats, a 2x2 and an offset.
    std::memcpy(mesh.texMatrix, packet->texTm, sizeof(mesh.texMatrix));
    mTrace.countTexMatrix(packet->texTm[0] != 0.0f || packet->texTm[3] != 0.0f);

    if (!(mesh.attrBits & sdfCoreAttrMultiTexPass) || !material.multiTexHandle) return;
    mesh.multi.texHandle = mTextures->getHandle(material.multiTexHandle, frame);
    mesh.multi.alpha = packet->mtexAlpha;
    mesh.multi.tex0 = packet->mtexTex0Data;
    mesh.multi.tex1 = packet->mtexTex1Data;
    mesh.multi.clamp = packet->mtexClampData;
    mesh.multi.diffuse = Rgba::fromPacked(packet->mtexDiffuse);
    mesh.multi.on = mesh.multi.texHandle.addr != 0;
    // corePktEmitMultiTexPass recolours every vertex before the second pass draws: mtexAlgo < 3
    // runs the face-division ramp, 3 sets a constant colour. Neither is modelled, so it is reported.
    reportUnsupported("corePktEmitMultiTexPass mtexAlgo", packet->mtexAlgo);
}

bool ModelParser::readScreenTm() {
    if (!mSdf3DScreenTm) return false;
    mScreenTm = *mSdf3DScreenTm;
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

/// @fn sdfModelLinkOt
/// @note SLUS-20911 0x002B1DB8
void ModelParser::traceMeshes(const Frame& frame) {
    if (!mEvents) return;
    for (const DrawMesh& mesh : frame.meshes) mEvents->onMesh(mesh, mScreenTm, mDecodeFrame);
}

void ModelParser::sdfModelLinkOt(sdfModelRootNodePtr_t aRoot, Frame* frame) {
    if (!aRoot || !readScreenTm()) return;
    const guest_ref<sdfModelRootNode_t> root{aRoot};
    if (frame) frame->screenTm = mScreenTm;

    mTrace.countRoot((root->flags & sdfModelRootTempPackets) != 0);

    // 0x2B1E04: the temporary packets are linked first, over the same nodes.
    if (root->flags & sdfModelRootTempPackets) sdfModelTempPacketLinkOt(root, frame);

    // 0x2B1E58, 0x2B1E5C: the root's own light and fog, or the engine defaults when it has none.
    readLighting(root->light);
    readFog(root->fog);

    const u32 frameId = mSdfFrameId ? *mSdfFrameId : 0;

    // 0x2B1E6C: the root's own bias under flags & 2, else the global. It and corePktSoftenBias ride
    // in the per-node state packet and drive VU1's face division, which nothing here models. Both
    // are inert at zero, so a scene that sets either says so rather than quietly rendering coarse.
    const f32 faceDivBias = (root->flags & sdfModelRootOwnFaceDivBias)
                                ? root->faceDivBias
                                : mSdfFaceDivBias.valueOr();
    reportFaceDiv("sdfModelLinkOt faceDivBias", faceDivBias);
    if (const u8 soften = mCorePktSoftenBias.valueOr()) {
        reportUnsupported("corePktSoftenBias", soften);
    }
    // 0x2B1E94: flags & 8 suppresses the type 1 and 2 paths.
    const bool suppressLinked = (root->flags & sdfModelRootNoLinkedPackets) != 0;

    const sdfModelNodeListPtr_t list = root->nodeList;
    if (!list) return;
    mNodeListAddr = root->nodeList.addr;

    // A DMA call chain names a material by the address of the packet sdfMatMakePacket built for
    // it, so the lookup is built once here rather than searched per chain.
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
        draw.zBias = root->zBias;
        draw.colour = (node->flags & sdfModelNodeOwnColour) ? node->color : root->color;  // 0x2B1F48
        // 0x2B1F6C: the cast is signed, so a drawAttr of 0xFFFF sign-extends and the node's high
        // bits survive. Truncating to 16 dropped everything above them, the soften bias included.
        draw.attrMask = u32(s32(s16(root->drawAttr))) & node->drawAttr;
        // 0x2B1FC0: a packet's otIndex of zero means table 0 or 1 by the sign of the colour.
        draw.otIndex = s32(draw.colour) < 0 ? 0 : 1;
        // sdfCorePacketGen turns the same flag into corePktBuildFaceList's backfaceSign.
        draw.reverse = suppressLinked;

        mTrace.countNode(ModelTrace::Node::Drawn);
        walkChain(root, draw, chainAddr, frame);
    }
}

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

// Both print words and floats side by side, which is what settles a stride without guessing.
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

    // A body that names the nodes it blends lists them before its triangles.
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
    mesh.zBias = draw.zBias;
    mesh.otIndex = draw.otIndex;
    mesh.colour = Rgba::fromPacked(draw.colour);
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

// The chain alternates the material's own packet with the mesh that draws under it, so a REF that
// names a known material sets the material for the meshes after it.
/// @fn sdfModelTempPacketLinkOt
/// @note SLUS-20911 0x002B1950
void ModelParser::reportFaceDiv(const char* what, f32 value) {
    if (value == 0.0f) return;
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    reportUnsupported(what, bits);
}

void ModelParser::sdfModelTempPacketLinkOt(guest_ref<sdfModelRootNode_t> root, Frame* frame) {
    // 0x2B198C: this walk takes faceDivSoften where sdfModelLinkOt takes faceDivBias, and falls
    // back to its own global. Neither is modelled, so both are reported where they are read.
    reportFaceDiv("sdfModelTempPacketLinkOt faceDivSoften",
                  (root->flags & sdfModelRootOwnFaceDivBias) ? root->faceDivSoften
                                                             : mSdfFaceDivSoften.valueOr());
    mTrace.countTempPacketWalk();

    const sdfModelNodeListPtr_t list = root->nodeList;
    if (!list) return;

    for (s16 i = 0; i < list->count; i++) {
        const sdfModelNodePtr_t node{readListItem(root->nodeList.addr, u32(i))};
        // 0x2B19E4: this walk draws only nodes whose shape is a format 1 definition.
        if (!node || !node->shape || node->shape->format != 1) continue;
        if (node->flags & sdfModelNodeHidden) continue;

        NodeDraw draw{};
        std::memcpy(&draw.model, &node->tm, sizeof(Matrix4));
        draw.clip = MultMatrixRev(draw.model, mScreenTm);
        if (node->bounds && sdfModelCheckBounds(node->bounds, draw.clip)) continue;

        draw.sortZ = draw.clip.m[3][3];
        draw.zBias = root->zBias;
        draw.colour = (node->flags & sdfModelNodeOwnColour) ? node->color : root->color;
        draw.attrMask = u16(root->drawAttr) & node->drawAttr;
        draw.otIndex = s32(draw.colour) < 0 ? 0 : 1;
        draw.reverse = (root->flags & sdfModelRootNoLinkedPackets) != 0;

        // shape[0] names a null-terminated array of groups; each group is an otIndex word followed
        // by a null-terminated array of temp packet pointers.
        const u32 groups = guest_ptr<u32>{node->shape.addr}.valueOr();
        for (u32 g = 0; ; g++) {
            const u32 group = guest_ptr<u32>{groups + 4u * g}.valueOr();
            if (!group) break;

            NodeDraw packetDraw = draw;
            if (const u16 otIndex = u16(guest_ptr<u32>{group}.valueOr() >> 16)) {
                packetDraw.otIndex = u8(otIndex);
            }
            for (u32 e = 0; ; e++) {
                const sdfModelTempPacketPtr_t packet{guest_ptr<u32>{group + 4u + 4u * e}.valueOr()};
                if (!packet) break;
                if (packet->type != 1) {
                    reportUnsupported("sdfModelTempPacketLinkOt packet type", packet->type);
                    continue;
                }
                const sdfMatPtr_t mat{readListItem(root->matList.addr, packet->matIndex)};
                if (!readVifStreamMesh(packet->dataPtr.addr,
                                       packet->dataPtr.addr + 16u * packet->qwc, packetDraw, mat,
                                       frame)) {
                    mTrace.countStrayRef();
                }
            }
        }
    }
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

void ModelParser::fldDrawPoly3(const sdfVector3_t point[3], const u32 colour[3], Frame* frame) {
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
        mesh.vertices[i].colour = Rgba::fromPacked(colour[i]);
    }
    static constexpr u16 kTriangle[3] = {0, 1, 2};
    mesh.indices.assign(std::begin(kTriangle), std::end(kTriangle));
    mTrace.countBones(1);
    frame->addMesh(std::move(mesh));
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

void ModelParser::sdfBillBoardMakeN(sdfBillBoardBatchPtr_t aBatch, u32 aNum, Frame* frame) {
    if (!frame || !aBatch || !aNum || aNum > std::size(aBatch->centre)) return;
    const sdfBillBoardBatch_t& batch = *aBatch;

    // The camera's own axes, so a quad's offsets land back in the world the screen matrix expects.
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
        mesh.colour = Rgba::fromPacked(batch.colour[i]);
        mesh.lighting = mLighting;
        mesh.fog = mFog;
        mesh.gs = sdfGsStateForOt(otIndex);
        mesh.gs.frameMask = mState ? mState->frameMask : 0;
        mesh.attrBits = sdfCoreAttrTextured | sdfCoreAttrBlend | sdfPrimTriangle;
        mesh.lit = false;
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
    mesh.zBias = draw.zBias;
    mesh.otIndex = draw.otIndex;
    mesh.colour = Rgba::fromPacked(draw.colour);
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
        if (first) {
            mesh.attrBits = attrBits & draw.attrMask;
            mesh.lit = (packetBits & sdfCorePacketNormals) != 0;
        }
        first = false;
        if (!faceCount || !pointCount) return false;

        u32 indices = 0;
        u32 positions = 0;
        if (!readArray(next, 4, faceCount, indices, next)) return false;
        if (!readArray(next, 12, pointCount, positions, next)) return false;

        u32 normals = 0;
        if ((packetBits & sdfCorePacketNormals) &&
            !readArray(next, 12, pointCount, normals, next)) {
            return false;
        }

        // One packet carries both sets when a second is asked for, four floats wide.
        u32 texcoords = 0;
        const bool bothUv = (attrBits & sdfCoreAttrMultiTexPass) != 0;
        if ((attrBits & sdfCoreAttrTextured) &&
            !readArray(next, bothUv ? 16 : 8, pointCount, texcoords, next)) {
            return false;
        }

        u32 colours = 0;
        if ((attrBits & sdfCoreAttrColour) &&
            !readArray(next, 4, pointCount, colours, next)) {
            return false;
        }

        const sdfVector3_t* position = sdfVector3Ptr_t{positions}.slice(pointCount);
        const sdfVector3_t* normal = sdfVector3Ptr_t{normals}.slice(pointCount);
        // One packet carries both UV sets when a second is asked for, so the stride doubles.
        const sdfVector2_t* uv =
            sdfVector2Ptr_t{texcoords}.slice(pointCount * (bothUv ? 2u : 1u));
        const u32* colour = u32Ptr_t{colours}.slice(pointCount);
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
            if (colour) out.colour = Rgba::fromPacked(colour[v]);
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
    mesh.diffuse = Rgba::fromPacked(material->diffuse);
    mesh.ambient = Rgba::fromPacked(material->ambient);

    mTrace.countMultiTexture(mesh.multi.on);
    mTrace.countMesh(type, packet.prim.faceCount);
    if (frame) frame->addMesh(std::move(mesh));
    return true;
}

}  // namespace sdfhook
