#include "model.h"

#include "libsdf_impl.h"

#include <bit>
#include <cmath>

#include <cstddef>
#include <cstring>
#include <vector>

#include "log.h"
#include "texture.h"
#include "trace.h"
#include "json.h"
#include "libsdf_impl.h"
#include "sink.h"
#include "unsupported.h"

namespace sdfhook {


namespace {

LogChannel log("model");

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

void ModelParser::beginFrame(u32 frame) {
    gModel.linkIndex = 0;
    mDecodeFrame = frame;
}

void ModelParser::attach(pcsx2hook_ctx* runtime, const Config& config, TextureCache* textures,
                         Trace* trace) {
    gConvert = this;
    mConfig = &config;
    mRuntime = runtime;
    mTextures = textures;
    mEvents = trace;
    gModel.sdf3DScreenTm = {pcsx2hook_find_symbol(runtime, "sdf3DScreenTm")};
    gModel.sdf3DLightDefault = {pcsx2hook_find_symbol(runtime, "sdf3DLightDefault")};
    gModel.sdf3DFogDefault = {pcsx2hook_find_symbol(runtime, "sdf3DFogDefault")};
    gModel.sdfFrameId = {pcsx2hook_find_symbol(runtime, "sdfFrameId")};
    gModel.dds3FrameID = {pcsx2hook_find_symbol(runtime, "dds3FrameID")};
    gModel.sdfModelShapeWeightEpsilon =
        {pcsx2hook_find_symbol(runtime, "sdfModelShapeWeightEpsilon")};
    mDds3BillboardTag = {pcsx2hook_find_symbol(runtime, "dds3BillboardTag")};
    gModel.sdfFaceDivBias = {pcsx2hook_find_symbol(runtime, "sdfFaceDivBias")};
    gModel.sdfFaceDivSoften = {pcsx2hook_find_symbol(runtime, "sdfFaceDivSoften")};
    gModel.corePktSoftenBias = {pcsx2hook_find_symbol(runtime, "corePktSoftenBias")};
    // sdf3DSpecularTex holds a texture handle where sdf3DRefMapParam holds a raw TEX0, and the
    // two are different passes.
    gModel.sdf3DSpecularTex = {pcsx2hook_find_symbol(runtime, "sdf3DSpecularTex")};
    // Quadwords 3..6 are the projection, diag(1.9078, 2.5401, -1.0005). gModel.screenTm is projection
    // times view, so dividing its first two rows by them recovers the view rotation.
    gModel.sdf3DScreenParam = {pcsx2hook_find_symbol(runtime, "sdf3DScreenParam")};
    gModel.sdf3DRefMapParam = {pcsx2hook_find_symbol(runtime, "sdf3DRefMapParam")};
    if (const u64 tex0 = gModel.sdf3DRefMapParam.valueOr()) {
        const sdfTex0_t t = sdfDecodeTex0(tex0);
        log.info("sdf3DRefMapParam tex0={:016X} tbp={} tbw={} psm={:#x} {}x{} tfx={} tcc={} cbp={}",
                 tex0, t.tbp, t.tbw, t.psm, t.width, t.height, t.tfx, t.tcc, t.cbp);
    }
}

// cppcheck-suppress unusedFunction  // @todo TODO.md V19, kept for the next producer
bool ModelParser::isWorldSpace(const sdfVector3_t& point, f32 out[3]) const {
    f32 clip[4];
    transformPoint(gModel.screenTm, point.x, point.y, point.z, clip);
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

void ModelParser::traceMeshes(const Frame& frame) {
    if (!mEvents) return;
    for (const DrawMesh& mesh : frame.meshes) mEvents->onMesh(mesh, gModel.screenTm, mDecodeFrame);
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

// The chain alternates material packet and mesh, so a REF naming a known material sets it for
// the meshes after it.
void ModelParser::reportFaceDiv(const char* what, f32 value) {
    if (value == 0.0f) return;
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    reportUnsupported(what, bits);
}

/// @fn dds3BillboardLinkOt
/// @note SLUS-20911 0x0022E2E8
constexpr u32 kMaxBillboardNodes = 4096;

void ModelParser::readBillboardTables() {
    gModel.billboardOt.clear();
    if (!mDds3BillboardTag) return;
    // The engine's own loop rewrites `next` as it consumes each node, so this runs at its entry.
    dds3BillboardNodePtr_t node{u32Ptr_t{mDds3BillboardTag.addr}.valueOr()};
    for (u32 steps = 0; node && steps < kMaxBillboardNodes; steps++) {
        gModel.billboardOt[node->batch.addr] = node->otIndex;
        node = node->next;
    }
}

bool ModelParser::readVifStreamMesh(u32 at, u32 end, const NodeDraw& draw, sdfMatPtr_t aMat,
                                    Frame* frame) {
    DrawMesh mesh;
    mesh.linkAddr = gModel.currentChain;
    mesh.sortZ = draw.sortZ;
    mesh.otLocal = u8(draw.otIndex & 3);
    mesh.otIndex = u8(gModel.otTable[mesh.otLocal]);
    mesh.color = Rgba::fromPacked(draw.color);
    mesh.bones.push_back(draw.model);
    mesh.lighting = gModel.lighting;
    mesh.fog = gModel.fog;
    mesh.gs = sdfGsStateForOt(mesh.otLocal);
    mesh.gs.frameMask = gState.frameMask;
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
        xsdfMatMakePacket(*aMat, mesh, frame);
    }

    mTrace.countOt(mesh.otIndex);
    mTrace.countSampler(mesh.tex0, mesh.tex1, mesh.clamp);
    mTrace.countDeterminant(mesh.bones[0]);
    mTrace.countStreamGeom(u32(mesh.indices.size() / 3));
    mTrace.countStreamMesh(u32(mesh.indices.size() / 3));
    mTrace.countBones(1);
    if (frame) DrawSink::addMeshTo(frame, std::move(mesh));
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
        const f32 epsilon = gModel.sdfModelShapeWeightEpsilon
                                ? *gModel.sdfModelShapeWeightEpsilon
                                : 0.000001f;
        for (const sdfModelMorphParm_t& target : morph->target) {
            if (target.blend <= epsilon || target.targetId >= shapeCount) continue;
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
    xsdfMatMakePacket(*material, mesh, frame);
    mesh.lighting = gModel.lighting;
    mesh.fog = gModel.fog;
    mesh.gs = sdfGsStateForOt(mesh.otLocal);
    mesh.gs.frameMask = gState.frameMask;
    mesh.linkIndex = gModel.currentLink;
    mesh.lit = (packet.prim.packetBits & sdfCorePacketNormals) != 0;
    mesh.matFlags = material->flags;
    mesh.matDuty = material->duty;
    mesh.packetType = packet.header.packetType;
    mesh.packetBits = packet.prim.packetBits;
    mTrace.countMatDuty(material->duty, gModel.sdfFrameId ? *gModel.sdfFrameId & 1 : 0);
    // Reported here rather than where the ramp is decided, because xsdfMatMakePacket runs for
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
    if (frame) DrawSink::addMeshTo(frame, std::move(mesh));
    return true;
}

}  // namespace sdfhook
