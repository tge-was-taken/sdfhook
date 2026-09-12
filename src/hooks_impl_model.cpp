#include <cstddef>
#include <cstring>
#include <vector>

#include "json.h"
#include "log.h"
#include "hooks_impl_model.h"
#include "model_internal.h"
#include "texture.h"
#include "trace.h"
#include "unsupported.h"

// The model hook implementations: what each hooked engine function does once hooks.cpp has read
// its arguments. model.cpp keeps the trace and the format decoders.
namespace sdfhook {

using namespace modelread;

namespace {

LogChannel log("model");

}  // namespace

// 0x2B9230/0x2B923C: sdfCorePacketGen keeps its two float arguments in $f20 and $f21 and hands
// them straight back, so entry is where they can be read.
void ModelHooks::setFaceDivTerms(f32 soften, f32 bias) {
    if (!mParser.mFaceDivTermsSeen && mParser.mConfig) {
        log.info("face division terms from the guest: soften {} bias {}, fitted {} {}", soften, bias,
                 mParser.mConfig->faceDivSoften, mParser.mConfig->faceDivBias);
    }
    mParser.mFaceDivSoftenTerm = soften;
    mParser.mFaceDivBiasTerm = bias;
    mParser.mFaceDivTermsSeen = true;
}
// 0x2B6DF4 and 0x2B6E30 read the coordinate's scale from [0].w and its offset from [0].y, and
// 0x2B6E80 interpolates [16] to [16]+[32] across it.
void ModelHooks::readFaceDivParm(corePktCbPtr_t aCb, sdfVector4Ptr_t aParm) {
    const sdfVector4_t* parm = aParm.slice(3);
    if (!parm || !aCb) return;
    const u32 aMatPacket = aCb->matPacketPtr.addr;

    MeshRamp& ramp = mParser.mFaceDivRamps[aMatPacket];
    const f32 lo[3] = {parm[1].x, parm[1].y, parm[1].z};
    const f32 delta[3] = {parm[2].x, parm[2].y, parm[2].z};
    // 128 is one, as the GS modulate has it.
    for (int i = 0; i < 3; i++) {
        ramp.lo[i] = lo[i] / 128.0f;
        ramp.delta[i] = delta[i] / 128.0f;
    }
    ramp.known = true;

    if (!mParser.mFaceDivParmSeen) {
        mParser.mFaceDivParmSeen = true;
        log.info("face division ramp, material packet {:08X}: scale {} offset {}", aMatPacket,
                 parm[0].w, parm[0].y);
        log.info("  lo {} {} {}, delta {} {} {}", lo[0], lo[1], lo[2], delta[0], delta[1], delta[2]);
    }
}
void ModelHooks::readLighting(sdf3DLightParallelPacketPtr_t aLight) {
    mParser.mLighting = {};
    const sdf3DLightParallelPacketPtr_t light = aLight ? aLight : mParser.mSdf3DLightDefault.readOr();
    if (!light) return;

    for (u32 i = 0; i < kParallelLights; i++) {
        for (usize axis = 0; axis < 3; axis++) {
            // normal is a matrix whose columns are the light vectors, so a light reads down them.
            mParser.mLighting.direction[i][axis] = light->normal[axis][i];
            mParser.mLighting.color[i][axis] = light->color[i][axis];
        }
        mParser.mLighting.base[i] = light->colorBase[i];
        mParser.mLighting.ambientColor[i] = light->color[3][i];
        mParser.mLighting.fullColor[i] = light->color[4][i];
    }

    static std::set<u32> shown;
    if (mParser.mConfig->dumpLight && shown.insert(light.addr).second) {
        JsonObject row;
        row.addString("dump", "light").addHexDigits("at", light.addr, 8);
        row.addReals("base", {mParser.mLighting.base[0], mParser.mLighting.base[1], mParser.mLighting.base[2]}, 4);
        for (u32 c = 0; c < 5; c++) {
            const std::string name = std::format("color{}", c);
            row.addReals(name.c_str(),
                         {light->color[c][0], light->color[c][1], light->color[c][2]}, 4);
        }
        row.beginArray("lights");
        for (u32 i = 0; i < kParallelLights; i++) {
            row.beginObject();
            row.addReals("direction", {mParser.mLighting.direction[i][0], mParser.mLighting.direction[i][1],
                                       mParser.mLighting.direction[i][2]}, 4);
            row.addReals("color",
                         {mParser.mLighting.color[i][0], mParser.mLighting.color[i][1], mParser.mLighting.color[i][2]}, 4);
            row.endObject();
        }
        row.endArray();
        log.dump(row.take());
    }
}
// F is per vertex, 255 keeping the color and 0 replacing it with FOGCOL. VU1 builds it as
// `fogParm[2] + fogParm[3] / w` clamped to fogParm[0]..fogParm[1] (0x04DC, 0x04DF, 0x04E3).
void ModelHooks::readFog(sdf3DFogPacketPtr_t aFog) {
    mParser.mFog = {};
    const sdf3DFogPacketPtr_t fog = aFog ? aFog : mParser.mSdf3DFogDefault.readOr();
    if (!fog) return;

    const u32 fogcol = u32(fog->fogColor);
    mParser.mFog.on = true;
    mParser.mFog.minimum = fog->fogParm[0] / 255.0f;
    mParser.mFog.maximum = fog->fogParm[1] / 255.0f;
    mParser.mFog.offset = fog->fogParm[2] / 255.0f;
    mParser.mFog.scale = fog->fogParm[3] / 255.0f;
    mParser.mFog.color[0] = f32(fogcol & 0xFF) / 255.0f;
    mParser.mFog.color[1] = f32((fogcol >> 8) & 0xFF) / 255.0f;
    mParser.mFog.color[2] = f32((fogcol >> 16) & 0xFF) / 255.0f;
}
// The sampler state lives in the material's packet, which sdfMatUpdatePacketBasic fills from the
// texture handle rather than from the material.
void ModelHooks::readMaterialPacket(const sdfMat_t& material, DrawMesh& mesh,
                                   Frame* frame) {
    mesh.diffuse = Rgba::unpack(material.diffuse);
    mesh.ambient = Rgba::unpack(material.ambient);

    // The finished buffer, not the one about to be rewritten: our walk runs at sdfModelLinkOt's
    // entry, so the index the frame id names is the one sdfMatListUpdatePacket is about to fill.
    const u32 frameId = mParser.mSdfFrameId ? (*mParser.mSdfFrameId & 1) ^ 1 : 0;
    const sdfMatPacketPtr_t packet = material.packet[frameId];
    if (!packet) return;
    mesh.specular = Rgba::unpack(packet->specularColor);
    mParser.mTrace.countSpecular(mesh.attrBits, packet->specularColor);
    // 0x2B8D9C: one global texture for every reflection pass, so it is keyed by that address.
    if (mesh.attrBits & sdfCoreAttrRefMapPass) {
        mesh.refMap = mParser.mTextures->getHandleForTex0(TexHandle{mParser.mSdf3DRefMapParam.addr},
                                                  mParser.mSdf3DRefMapParam.readOr());
    }
    // The specular pass names a handle, so it resolves like any other texture.
    if (mesh.attrBits & sdfCoreAttrSpecularPass) {
        if (const u32 handleAddr = mParser.mSdf3DSpecularTex.readOr()) {
            const sdfTexHandlePtr_t handle{handleAddr};
            // The handle's own pixels, with its upload packet's TEX0 for TCC so the alpha
            // survives. The overlay's real texture is never uploaded. See TODO V7.
            mesh.specularTex = mParser.mTextures->getHandle(TexHandle{handleAddr}, frame);
            if (handle) {
                mesh.specularTex0 = handle->packet.readOr().tex0.data;
            }
        }
    }
    mesh.tex0 = packet->tex0Data;
    mesh.tex1 = packet->tex1Data;
    mesh.clamp = packet->clampData;
    // A material can name a TEX0 and carry no handle. The packet's address is stable, so the
    // recovered texture files under that.
    if (!mesh.texHandle.addr) {
        mesh.texHandle = mParser.mTextures->getHandleForTex0(TexHandle{packet.addr}, packet->tex0Data);
    }
    // Inline at texTm rather than behind it: that field's address is what corePktXfmLightClipUV
    // is handed.
    std::memcpy(mesh.texMatrix, packet->texTm, sizeof(mesh.texMatrix));
    mParser.mTrace.countTexMatrix(packet->texTm[0] != 0.0f || packet->texTm[3] != 0.0f);

    // 0x2B6A4C: read off the material *packet* at +44 and +40, plus the root's, soften clamped to
    // at least one. sdfMat_t carries the same two fields at 0x44 and 0x40 with different values.
    const f32 softenTerm = mParser.mFaceDivTermsSeen ? mParser.mFaceDivSoftenTerm : mParser.mConfig->faceDivSoften;
    const f32 biasTerm = mParser.mFaceDivTermsSeen ? mParser.mFaceDivBiasTerm : mParser.mConfig->faceDivBias;
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
void ModelHooks::readMultiTexturePacket(const sdfMat_t& material, sdfMatPacketPtr_t packet,
                                         DrawMesh& mesh, Frame* frame) {
    if (!(mesh.attrBits & sdfCoreAttrMultiTexPass)) return;
    mesh.multi.texHandle = mParser.mTextures->getHandle(material.multiTexHandle, frame);
    // The second pass can name its TEX0 with no handle just as the first can. Keyed apart from the
    // first by an offset into the same packet, since one material carries both.
    if (!mesh.multi.texHandle.addr) {
        mesh.multi.texHandle =
            mParser.mTextures->getHandleForTex0(TexHandle{packet.addr + 8}, packet->mtexTex0Data);
    }
    mesh.multi.alpha = packet->mtexAlpha;
    mesh.multi.tex0 = packet->mtexTex0Data;
    mesh.multi.tex1 = packet->mtexTex1Data;
    mesh.multi.clamp = packet->mtexClampData;
    mesh.multi.diffuse = Rgba::unpack(packet->mtexDiffuse);
    mesh.multi.on = mesh.multi.texHandle.addr != 0;
    // 0x2B8C4C: the ramp runs only where the two diffuse colors differ, and only under algo 3.
    mesh.multi.algo = packet->mtexAlgo;
    // 0x2B8C90: the packet's own two colors, at +8 and +12. Taking the first from sdfMat_t is a
    // different value and claims a ramp on meshes the engine never ramps.
    mesh.multi.ramp = packet->mtexAlgo < 3 && packet->diffuse != packet->mtexDiffuse;
    mesh.multi.constant = packet->constant;
    // Generation runs after the walk, so these are the previous frame's endpoints. Every captured
    // one is algo 3 and every ramp mesh algo 1 or 2, so on the measured scenes this path is inert.
    if (const auto it = mParser.mFaceDivRamps.find(packet.addr); it != mParser.mFaceDivRamps.end()) {
        mesh.multi.rampColors = it->second;
    }
    if (packet->mtexAlgo > 3) reportUnsupported("corePktEmitMultiTexPass mtexAlgo", packet->mtexAlgo);

}
/// Culls a node by its bounds against the VU0 clip codes the engine tests.
/// @fn sdfModelCheckBounds
/// @note SLUS-20911 0x002B1838
bool ModelHooks::sdfModelCheckBounds(sdfModelBoundsPtr_t aBounds, const Matrix4& clip) const {
    if (!aBounds) return false;
    const sdfModelBounds_t& box = *aBounds;

    u32 code = 0x3F;
    for (int corner = 0; corner < 8; corner++) {
        f32 out[4];
        transformPoint(clip, (corner & 1) ? box.max.x : box.min.x, (corner & 2) ? box.max.y : box.min.y,
                       (corner & 4) ? box.max.z : box.min.z, out);
        code &= getClipCode(out);
        if (!code) return false;
    }
    return code != 0;
}
/// The engine's node list, turned into meshes instead of ordering-table entries.
/// @fn sdfModelLinkOt
/// @note SLUS-20911 0x002B1DB8
void ModelHooks::sdfModelLinkOt(sdfModelRootNodePtr_t aRoot, Frame* frame) {
    if (!aRoot || !mParser.readScreenTm()) return;
    const guest_ref<sdfModelRootNode_t> root{aRoot};
    if (frame) {
        frame->screenTm = mParser.mScreenTm;
        frame->projectionScale[0] = mParser.mProjectionScale[0];
        frame->projectionScale[1] = mParser.mProjectionScale[1];
        frame->depthScale = mParser.mDepthScale;
        frame->depthOffset = mParser.mDepthOffset;
    }

    mParser.mTrace.countRoot((root->flags & sdfModelRootTempPackets) != 0);

    // 0x2B1E04: the temporary packets are linked first, over the same nodes.
    if (root->flags & sdfModelRootTempPackets) sdfModelTempPacketLinkOt(root, frame);

    // 0x2B1E58, 0x2B1E5C: the root's own light and fog, or the engine defaults when it has none.
    readLighting(root->light);
    readFog(root->fog);

    const u32 frameId = mParser.mSdfFrameId ? *mParser.mSdfFrameId : 0;

    // 0x2B1E6C: the root's own bias under flags & 2, else the global. It rides in the per-node
    // state packet, which nothing here models.
    mParser.reportFaceDiv("sdfModelLinkOt faceDivBias", (root->flags & sdfModelRootOwnFaceDivBias)
                                                    ? root->faceDivBias
                                                    : mParser.mSdfFaceDivBias.readOr());
    // The terms reach sdfCorePacketGen in $f12 and $f13, so Config's values are only the first
    // frame's fallback.
    if (root->flags & sdfModelRootOwnFaceDivBias) {
        reportUnsupported("sdfModelLinkOt root faceDiv terms", root->flags);
    }
    if (const u8 soften = mParser.mCorePktSoftenBias.readOr()) {
        reportUnsupported("corePktSoftenBias", soften);
    }
    // 0x2B1E94: flags & 8 suppresses the type 1 and 2 paths.
    const bool suppressLinked = (root->flags & sdfModelRootNoLinkedPackets) != 0;

    const sdfModelNodeListPtr_t list = root->nodeList;
    if (!list) return;
    mParser.mNodeList = root->nodeList;

    // A chain names a material by its sdfMatMakePacket address, so the lookup is built once.
    mParser.mMatByPacket.clear();
    if (const sdfMatListPtr_t matList = root->matList) {
        for (s16 m = 0; m < matList->count; m++) {
            const sdfMatPtr_t material{readListItem(root->matList, u32(m))};
            if (!material) continue;
            for (const sdfMatPacketPtr_t& packet : material->packet) {
                if (packet.addr) mParser.mMatByPacket[packet.addr] = material.addr;
            }
        }
    }

    for (s16 i = 0; i < list->count; i++) {
        const sdfModelNodePtr_t node = readListItem(root->nodeList, u32(i));
        if (!node) {
            mParser.mTrace.countNode(ModelTrace::Node::Unreadable);
            continue;
        }
        const u32 chainAddr = node->drawPacket[frameId & 1].addr;
        if (!chainAddr) {
            mParser.mTrace.countNode(ModelTrace::Node::NoChain);
            continue;
        }
        // 0x2B1EEC
        if (node->flags & sdfModelNodeHidden) {
            mParser.mTrace.countNode(ModelTrace::Node::Hidden);
            continue;
        }

        NodeDraw draw{};
        std::memcpy(&draw.model, &node->tm, sizeof(Matrix4));
        // 0x2B1F0C: MultMatrixRev(&sdf3DScreenTm) over the node's own tm, held in VU0.
        draw.clip = MultMatrixRev(draw.model, mParser.mScreenTm);
        // 0x2B1F20
        if (node->bounds && sdfModelCheckBounds(node->bounds, draw.clip)) {
            mParser.mTrace.countNode(ModelTrace::Node::BoundsCulled);
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

        mParser.mTrace.countNode(ModelTrace::Node::Drawn);
        walkChain(root, draw, chainAddr, frame);
    }
}
/// sdfModelLinkOt's inner loop over a node's packet chain; recheck the two together.
void ModelHooks::walkChain(guest_ref<sdfModelRootNode_t> root, const NodeDraw& draw,
                          u32 chainAddr, Frame* frame) {
    const bool suppressLinked = (root->flags & sdfModelRootNoLinkedPackets) != 0;

    for (u32 addr = chainAddr; addr;) {
        const sdfModelPacketHeaderPtr_t header{addr};
        if (!header) {
            mParser.mTrace.countPacket(ModelTrace::Packet::Unknown);
            return;
        }
        NodeDraw packetDraw = draw;
        if (header->otIndex) packetDraw.otIndex = header->otIndex;

        // 0x2B2084 type 2, 0x2B2098 type 1, 0x2B20A8 type 3.
        switch (header->type) {
            case sdfPacketDmaCall:
                if (suppressLinked) {
                    mParser.mTrace.countPacket(ModelTrace::Packet::Suppressed);
                    break;
                }
                mParser.mTrace.countPacket(ModelTrace::Packet::DmaCall);
                mParser.dumpWords("call", addr + sizeof(sdfModelPacketHeader_t));
                sdfModelSetNormalDrawPacket(packetDraw, addr + u32(sizeof(sdfModelPacketHeader_t)), frame);
                break;
            case sdfPacketShaped:
                if (suppressLinked) {
                    mParser.mTrace.countPacket(ModelTrace::Packet::Suppressed);
                    break;
                }
                mParser.mTrace.countPacket(ModelTrace::Packet::Shaped);
                sdfModelLinkShapedDrawPacket(root, packetDraw, header->dataPtr.addr, frame);
                break;
            case sdfPacketGenerated:
                mParser.mTrace.countPacket(ModelTrace::Packet::Generated);
                mParser.mCurrentLink = mParser.mLinkIndex++;
                sdfCorePacketGen(root, packetDraw, sdfCorePacketPtr_t{header->dataPtr.addr}, frame);
                break;
            default:
                mParser.mTrace.countPacket(ModelTrace::Packet::Unknown);
                reportUnsupported("sdfModelLinkOt packet type", header->type);
                break;
        }
        addr = header->next.addr;
    }
}
/// Reads the generated packet the engine would build, by packet type.
/// @fn sdfCorePacketGen
/// @note SLUS-20911 0x002B9178
void ModelHooks::sdfCorePacketGen(guest_ref<sdfModelRootNode_t> root, const NodeDraw& draw,
                                  sdfCorePacketPtr_t aPacket, Frame* frame) {
    if (!aPacket) return;
    const sdfCorePacket_t& packet = *aPacket;

    const sdfCorePacketType_t type = sdfCorePacketType_t(packet.header.packetType);
    mParser.mTrace.countGeom(type, packet.prim.faceCount);
    mParser.mTrace.countPacketType(packet.header.packetType, packet.prim.attrBits, packet.prim.packetBits);
    mParser.dumpPayload(aPacket);

    switch (type) {
        case sdfCorePacketTypeRigid:
        case sdfCorePacketTypeWeighted:
        case sdfCorePacketTypeBlendShape: break;
        default:
            reportUnsupported("sdfCorePacketGen body type", packet.header.packetType);
            return;
    }
    if (!packet.prim.faceCount || !packet.prim.pointCount) {
        mParser.mTrace.countReject(type, ModelTrace::Reject::Empty);
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
    mesh.color = Rgba::unpack(draw.color);
    mesh.attrBits = packet.prim.attrBits & draw.attrMask;
    mesh.reverseFacing = draw.reverse;

    if (!corePktBuildFaceList(at, packet, mesh)) {
        mParser.mTrace.countReject(type, ModelTrace::Reject::BadElems);
        return;
    }
    at = align16(at + 6u * packet.prim.faceCount);

    bool ok = false;
    switch (type) {
        case sdfCorePacketTypeRigid:
            mesh.bones.push_back(draw.model);
            ok = mParser.readRigidBatches(at, packet, mesh);
            break;
        case sdfCorePacketTypeWeighted:
            ok = readWeightedBatches(at, packet, nodesAddr, mesh);
            break;
        case sdfCorePacketTypeBlendShape:
            mesh.bones.push_back(draw.model);
            ok = mParser.readBlendShapes(at, packet, root, mesh);
            break;
    }
    if (!ok) {
        mParser.mTrace.countReject(type, ModelTrace::Reject::BadBatch);
        return;
    }

    mParser.mTrace.countSampler(mesh.tex0, mesh.tex1, mesh.clamp);
    mParser.mTrace.countDeterminant(mesh.bones[0]);
    mParser.mTrace.countAttr(mesh.attrBits);
    mParser.mTrace.countGs(mesh.gs);
    mParser.mTrace.countOt(mesh.otIndex);
    mParser.mTrace.countBones(u32(mesh.bones.size()));
    mParser.finishMesh(root, packet, type, mesh, frame);
}
/// The same node walk over the root's temporary packet list, run first under flags & 4.
/// @fn sdfModelTempPacketLinkOt
/// @note SLUS-20911 0x002B1950
void ModelHooks::sdfModelTempPacketLinkOt(guest_ref<sdfModelRootNode_t> root, Frame* frame) {
    // @optout 0x2B1954 packetbuf: sdfFrameId picks which of the two per-frame packets to build
    // @optout 0x2B19A4 packetbuf: sdfMatListUpdatePacket rebuilds each material's DMA packet

    // 0x2B19FC..0x2B1A17: this walk takes faceDivSoften where sdfModelLinkOt takes faceDivBias,
    // and falls back to its own global.
    mParser.reportFaceDiv("sdfModelTempPacketLinkOt faceDivSoften",
                  (root->flags & sdfModelRootOwnFaceDivBias) ? root->faceDivSoften
                                                             : mParser.mSdfFaceDivSoften.readOr());
    mParser.mTrace.countTempPacketWalk();

    const sdfModelNodeListPtr_t list = root->nodeList;
    if (!list) return;

    // 0x2B1A2C..0x2B1A7C, 0x2B1D60: this walk draws only nodes whose shape is a format 1
    // definition carrying a group array, and whose flags hide neither it nor its children.
    for (s16 i = 0; i < list->count; i++) {
        // @optout 0x2B1A40 elsewhere(readListItem): the list's item array
        const sdfModelNodePtr_t node = readListItem(root->nodeList, u32(i));
        if (!node || !node->shape || node->shape->format != 1) continue;
        const u32 groups = node->shape->shape[0].addr;
        if (!groups) continue;
        if (node->flags & sdfModelNodeHidden) continue;

        NodeDraw draw{};
        std::memcpy(&draw.model, &node->tm, sizeof(Matrix4));
        // @optout 0x2B1AA0 elsewhere(readScreenTm): sdf3DScreenTm is captured once a frame
        draw.clip = MultMatrixRev(draw.model, mParser.mScreenTm);
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
            const u32 group = guest_ptr<u32>{groups + 4u * g}.readOr();
            if (!group) break;

            NodeDraw packetDraw = draw;
            // 0x2B1B60: the group's own ot index, when it names one.
            if (const u16 otIndex = u16(guest_ptr<u32>{group}.readOr() >> 16)) {
                packetDraw.otIndex = u8(otIndex);
            }

            // @optout 0x2B1B88..0x2B1C53 packetbuf: allocates this ot's draw tag, writes the
            // transform as a VIF packet and links it

            for (u32 e = 0; ; e++) {
                const sdfModelTempPacketPtr_t packet{guest_ptr<u32>{group + 4u + 4u * e}.readOr()};
                // 0x2B1C58..0x2B1C70, 0x2B1CE4: the list ends at a null, and only type 1 draws.
                if (!packet) break;
                if (packet->type != 1) {
                    reportUnsupported("sdfModelTempPacketLinkOt packet type", packet->type);
                    continue;
                }
                // @optout 0x2B1C78..0x2B1CE3 dma: builds the material REF packet and links it
                const sdfMatPtr_t mat{readListItem(root->matList, packet->matIndex)};
                if (!mParser.readVifStreamMesh(packet->dataPtr.addr,
                                       packet->dataPtr.addr + 16u * packet->qwc, packetDraw, mat,
                                       frame)) {
                    mParser.mTrace.countStrayRef();
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
void ModelHooks::sdfModelLinkShapedDrawPacket(guest_ref<sdfModelRootNode_t> root,
                                               const NodeDraw& draw, u32 dataAddr, Frame* frame) {
    mParser.mTrace.countShapedWalk();

    // 0x2B0CD0: the low half of word 1 indexes the morph list, the high half the material list.
    const u32 word1 = guest_ptr<u32>{dataAddr + 4}.readOr();
    const u16 morphIndex = u16(word1 & 0xFFFF);
    const u16 matIndex = u16(word1 >> 16);
    const u16 shapeLists = u16(guest_ptr<u32>{dataAddr + 8}.readOr() & 0xFFFF);

    const sdfModelMorphListPtr_t morphList = root->morphList;
    const u32 morphItems = morphList ? morphList->items.addr : 0;
    const sdfModelMorphPtr_t morph{morphItems + u32(sizeof(sdfModelMorph_t)) * morphIndex};
    if (!morphItems || !morph) {
        reportUnsupported("sdfModelLinkShapedDrawPacket morph index", morphIndex);
        return;
    }

    // Only the targets the controller is actually blending towards reach the chain.
    u16 targets[kMorphTargets] = {};
    u32 used = 0;
    for (const sdfModelMorphParm_t& target : morph->target) {
        if (target.blend > sdfMorphEpsilon) targets[used++] = u16(target.targetId);
    }

    const sdfMatPtr_t mat{readListItem(root->matList, matIndex)};
    for (u16 l = 0; l < shapeLists; l++) {
        sdfModelShapeListPtr_t list{guest_ptr<u32>{dataAddr + 12u + 4u * l}.readOr()};
        if (!list) {
            reportUnsupported("sdfModelLinkShapedDrawPacket shape list", l);
            continue;
        }
        for (u32 t = 0; t < used; t++) {
            const sdfModelShapeEntry_t entry =
                guest_ptr<sdfModelShapeEntry_t>{list.addr + offsetof(sdfModelShapeList_t, entry) +
                                                u32(sizeof(sdfModelShapeEntry_t)) * targets[t]}
                    .readOr();
            if (!entry.dataPtr) continue;
            if (!mParser.readVifStreamMesh(entry.dataPtr.addr, entry.dataPtr.addr + 16u * entry.qwc, draw,
                                   mat, frame)) {
                mParser.mTrace.countStrayRef();
            }
        }
        // The list's own stream closes the chain, which is the mesh the targets deform.
        if (!mParser.readVifStreamMesh(list->next.addr, list->next.addr + 16u * list->count, draw, mat,
                               frame)) {
            mParser.mTrace.countStrayRef();
        }
    }
}
/// One camera-facing quad, rotated in its own plane by the angle sdfBillBoardMake1 never sees.
/// @fn drawSprite3D
/// @note SLUS-20911 0x0012A2D0
void ModelHooks::drawSprite3D(TexHandle aTexHandle, Sprite3DPtr_t aSprite, Frame* frame) {
    if (!frame || !aSprite) return;
    const Sprite3D_t& sprite = *aSprite;

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
    axis(mParser.mScreenTm.m[0][0], mParser.mScreenTm.m[1][0], mParser.mScreenTm.m[2][0], right);
    axis(mParser.mScreenTm.m[0][1], mParser.mScreenTm.m[1][1], mParser.mScreenTm.m[2][1], up);

    DrawMesh mesh;
    mesh.producer = "drawSprite3D";
    Matrix4 identity{};
    for (int i = 0; i < 4; i++) identity.m[i][i] = 1.0f;
    mesh.bones.push_back(identity);
    mesh.color = Rgba::unpack(u32(sprite.col));
    mesh.lighting = mParser.mLighting;
    mesh.fog = mParser.mFog;
    // Stamped alongside the state, or the row reports table 0 beside table 2's registers.
    mesh.otIndex = 2;
    mesh.gs = sdfGetGsStateForOt(mesh.otIndex);
    mesh.attrBits = sdfCoreAttrTextured | sdfCoreAttrBlend | sdfPrimTriangle;
    mesh.lit = false;
    mesh.packetType = sdfPacketGenerated;
    mesh.texHandle = mParser.mTextures->getHandle(aTexHandle, frame);

    // 0x12A344: SetRotateMatrixZ turns the offsets in the quad's own plane before the camera
    // axes, which is why this call is the hook rather than sdfBillBoardMake1.
    const f32 turn = std::cos(sprite.rot);
    const f32 lean = std::sin(sprite.rot);
    const TexHandle bound{mesh.texHandle.addr};
    const f32 width = bound ? f32(bound->width) : 0.0f;
    const f32 height = bound ? f32(bound->height) : 0.0f;
    const f32 uScale = width > 0.0f ? 1.0f / (16.0f * width) : 0.0f;
    const f32 vScale = height > 0.0f ? 1.0f / (16.0f * height) : 0.0f;

    mesh.vertices.resize(4);
    for (u32 c = 0; c < 4; c++) {
        MeshVertex& out = mesh.vertices[c];
        const VTX_t& at = sprite.vt[c];
        const f32 x = at.x * turn - at.y * lean;
        const f32 y = at.x * lean + at.y * turn;
        setRigidInfluence(out,
                          sdfVector3_t{sprite.pos.x + right[0] * x + up[0] * y,
                                       sprite.pos.y + right[1] * x + up[1] * y,
                                       sprite.pos.z + right[2] * x + up[2] * y},
                          nullptr);
        out.u = f32(sprite.uv[c].u) * uScale;
        out.v = f32(sprite.uv[c].v) * vScale;
    }
    static constexpr u16 kQuad[6] = {0, 2, 1, 0, 3, 2};
    mesh.indices.assign(std::begin(kQuad), std::end(kQuad));
    mParser.mTrace.countBones(1);
    frame->addMesh(std::move(mesh));
}
/// An indexed triangle list in world space: the vertices are quadword-strided with only xyz used,
/// and each face is a word of three byte indices.
/// @fn sdf3DPrimMakePoly
/// @note SLUS-20911 0x002BAAB8
void ModelHooks::sdf3DPrimMakePoly(sdf3DPrimPolyParmPtr_t aPoly, Frame* frame) {
    if (!frame || !aPoly) return;
    const u32 vertexCount = u32(aPoly->pointCount);
    const u32 triangles = u32(aPoly->faceCount);
    if (!vertexCount || !triangles) return;

    const sdfVector4_t* point = aPoly->vertex.slice(vertexCount);
    const u32* index = aPoly->faceIndex.slice(triangles);
    if (!point) {
        reportUnsupported("sdf3DPrimMakePoly without vertices", aPoly.addr);
        return;
    }
    if (aPoly->faceIndex.addr && !index) {
        reportUnsupported("sdf3DPrimMakePoly face indices outside guest memory", aPoly->faceIndex.addr);
        return;
    }
    const u32 unread = (aPoly->normal.addr ? 1 : 0) | (aPoly->multiUv.addr ? 4 : 0);
    if (unread) reportUnsupported("sdf3DPrimMakePoly arrays (normal 1, multiUv 4)", unread);
    const sdfVector2_t* uv = aPoly->uv.addr ? aPoly->uv.slice(vertexCount) : nullptr;
    if (aPoly->uv.addr && !uv) reportUnsupported("sdf3DPrimMakePoly uvs outside guest memory", aPoly->uv.addr);
    // 0x2BAEA4: UNPACK V4-8, one RGBA word a vertex, red in the low byte.
    const u32* vertexColor = aPoly->vertexColor.addr ? aPoly->vertexColor.slice(vertexCount) : nullptr;
    if (aPoly->vertexColor.addr && !vertexColor) {
        reportUnsupported("sdf3DPrimMakePoly vertex colors outside guest memory", aPoly->vertexColor.addr);
    }

    DrawMesh mesh;
    mesh.producer = "sdf3DPrimMakePoly";
    mesh.chainTransform = true;
    Matrix4 identity{};
    for (int axis = 0; axis < 4; axis++) identity.m[axis][axis] = 1.0f;
    mesh.bones.push_back(identity);
    mesh.lighting = mParser.mLighting;
    mesh.fog = mParser.mFog;
    // Its callers set their own ALPHA and TEST through rsetAlpha and rsetTest rather than taking an
    // ordering table's pair, and the chain walk replaces this with what is in force at the packet.
    mesh.gs = sdfGetGsStateForOt(0);
    // 0x2BAAF4: the prim word is the descriptor's own or'd with 0x68, which is IIP, FGE and ABE;
    // 0x2BAB24 adds TME with UVs, 0x2BAB34 the second set, 0x2BAB4C the color flag.
    mesh.attrBits = u16(aPoly->attr | 0x68);
    if (aPoly->uv.addr) mesh.attrBits |= sdfCoreAttrTextured;
    if (aPoly->uv.addr && aPoly->multiUv.addr) mesh.attrBits |= sdfCoreAttrMultiTexPass;
    if (aPoly->vertexColor.addr) mesh.attrBits |= sdfCoreAttrColor;
    mesh.color = Rgba::unpack(aPoly->color);
    mesh.lit = false;
    // The GS trace shows each vertex at exactly its array's color.
    mesh.unlitColors = true;
    mesh.packetType = sdfPacketGenerated;

    mesh.vertices.resize(vertexCount);
    for (u32 v = 0; v < vertexCount; v++) {
        setRigidInfluence(mesh.vertices[v], sdfVector3_t{point[v].x, point[v].y, point[v].z},
                          nullptr);
        mesh.vertices[v].color = vertexColor ? Rgba::unpack(vertexColor[v]) : mesh.color;
        if (uv) {
            mesh.vertices[v].u = uv[v].x;
            mesh.vertices[v].v = uv[v].y;
        }
    }
    for (u32 t = 0; t < triangles; t++) {
        for (int corner = 0; corner < 3; corner++) {
            // 0x2BAD14: a null faceIndex takes sdf3DPrimDefaultFaceIndex, the list 3t, 3t+1, 3t+2.
            const u32 at = index ? (index[t] >> (8 * corner)) & 0xFF : 3 * t + u32(corner);
            if (at >= vertexCount) return;
            mesh.indices.push_back(u16(at));
        }
    }
    mParser.mTrace.countBones(1);
    mParser.mTrace.countStreamGeom(triangles);
    mParser.mTrace.countStreamMesh(triangles);
    frame->addMesh(std::move(mesh));
}
/// A flat triangle the field draws directly, with a colour per corner and no texture.
/// @fn fldDrawPoly3
/// @note SLUS-20911 0x0012A5E0
void ModelHooks::fldDrawPoly3(const sdfVector3_t point[3], const u32 color[3], Frame* frame) {
    if (!frame) return;

    DrawMesh mesh;
    mesh.producer = "fldDrawPoly3";
    Matrix4 identity{};
    for (int axis = 0; axis < 4; axis++) identity.m[axis][axis] = 1.0f;
    mesh.bones.push_back(identity);
    mesh.lighting = mParser.mLighting;
    mesh.fog = mParser.mFog;
    mesh.gs = sdfGetGsStateForOt(0);
    // Gouraud and blended, with no texture: the call carries no UVs, so there is nothing to sample.
    mesh.attrBits = sdfCoreAttrGouraud | sdfCoreAttrBlend | sdfPrimTriangle;
    mesh.lit = false;
    mesh.packetType = sdfPacketGenerated;
    mesh.vertices.resize(3);
    for (u32 i = 0; i < 3; i++) {
        setRigidInfluence(mesh.vertices[i], point[i], nullptr);
        mesh.vertices[i].color = Rgba::unpack(color[i]);
    }
    static constexpr u16 kTriangle[3] = {0, 1, 2};
    mesh.indices.assign(std::begin(kTriangle), std::end(kTriangle));
    mParser.mTrace.countBones(1);
    frame->addMesh(std::move(mesh));
}
/// Self-describing, so it is read rather than rebuilt.
/// @fn fldSdwDraw
/// @note SLUS-20911 0x001193E8
bool ModelHooks::fldSdwDraw(u32 aPacket, u32 aEnd, const sdfGsState_t& aState,
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
        if (isUnpack(code) && code.count == 1 && getUnpackWidth(code) == 8) {
            header = code;
            found = true;
            break;
        }
        // Everything else is a one-word setting or a kick.
        at = isUnpack(code) ? code.next + getUnpackWidth(code) * code.count : code.next;
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
        getUnpackWidth(indexCode) != 4 || indexCode.count != faceCount) {
        return false;
    }
    const u8* faces = u8Ptr_t{indexCode.next}.slice(faceCount * 4);
    if (!faces) return false;

    // The positions get their own unpack, V3_32, rather than trailing the indices: reading them at
    // the end of the index array takes the code word itself as the first x.
    VifCode pointCode;
    if (!readVifCode(indexCode.next + faceCount * 4, pointCode) || !isUnpack(pointCode) ||
        getUnpackWidth(pointCode) != 12 || pointCode.count != pointCount) {
        return false;
    }
    const sdfVector3_t* points = sdfVector3Ptr_t{pointCode.next}.slice(pointCount);
    if (!points) return false;

    DrawMesh mesh;
    mesh.producer = "fldSdwDraw";
    // The stream rides straight after the packet's own DMA tag, which is what the chain sends.
    mesh.linkAddr = aPacket - sdfQwordBytes;
    Matrix4 identity{};
    for (int axis = 0; axis < 4; axis++) identity.m[axis][axis] = 1.0f;
    mesh.bones.push_back(identity);
    mesh.lighting = mParser.mLighting;
    mesh.fog = mParser.mFog;
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

    mParser.mTrace.countBones(1);
    frame->addMesh(std::move(mesh));
    return true;
}
/// The cap dds3BillboardLinkOt walks to.
/// @fn dds3BillboardLinkOt
/// @note SLUS-20911 0x0015B7C0
constexpr u32 kMaxBillboardNodes = 4096;

void ModelHooks::readBillboardTables() {
    mParser.mBillboardOt.clear();
    if (!mParser.mDds3BillboardTag) return;
    // The engine's own loop rewrites `next` as it consumes each node, so this runs at its entry.
    dds3BillResourcePtr_t node = mParser.mDds3BillboardTag.readOr();
    for (u32 steps = 0; node && steps < kMaxBillboardNodes; steps++) {
        mParser.mBillboardOt[node->batch.addr] = node->current_mode;
        node = node->next;
    }
}
/// Builds one batch of camera-facing quads from the centres, offsets and UVs
/// the engine is handed.
/// @fn sdfBillBoardMakeN
/// @note SLUS-20911 0x002BB0F0
void ModelHooks::sdfBillBoardMakeN(sdfBillBoardBatchPtr_t aBatch, u32 aNum, Frame* frame) {
    if (!frame || !aBatch || !aNum || aNum > std::size(aBatch->centre)) return;
    const sdfBillBoardBatch_t& batch = *aBatch;

    // Offsets are view-space, so they need the camera's axes to land back in world.
    // A billboard faces the camera by construction, which is what makes taking them from the
    // matrix's columns exact rather than an approximation.
    const Matrix4& tm = mParser.mScreenTm;
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
    const auto found = mParser.mBillboardOt.find(aBatch.addr);
    if (found == mParser.mBillboardOt.end()) reportUnsupported("billboard batch with no ordering table", 0);
    const u32 otIndex = found != mParser.mBillboardOt.end() ? found->second : 2;

    for (u32 i = 0; i < aNum; i++) {
        const sdfVector4_t& centre = batch.centre[i];
        const VTX_t* offset = batch.offset[i];
        const UV_t* uv = batch.uv[i];

        DrawMesh mesh;
        mesh.producer = "sdfBillBoardMakeN";
        Matrix4 identity{};
        for (int axisIndex = 0; axisIndex < 4; axisIndex++) identity.m[axisIndex][axisIndex] = 1.0f;
        mesh.bones.push_back(identity);
        mesh.color = Rgba::unpack(batch.color[i]);
        mesh.lighting = mParser.mLighting;
        mesh.fog = mParser.mFog;
        mesh.otIndex = u8(otIndex);
        mesh.gs = sdfGetGsStateForOt(otIndex);
        mesh.attrBits = sdfCoreAttrTextured | sdfCoreAttrBlend | sdfPrimTriangle;
        mesh.lit = false;
        mesh.billboard = true;
        mesh.packetType = sdfPacketGenerated;
        // Texels from the batch's 12.4; the texture and its size are the chain's.
        constexpr f32 kTexel = 1.0f / 16.0f;
        mesh.vertices.resize(4);
        for (u32 c = 0; c < 4; c++) {
            MeshVertex& out = mesh.vertices[c];
            const VTX_t& at = offset[c];
            const sdfVector3_t world{centre.x + right[0] * at.x + up[0] * at.y,
                                     centre.y + right[1] * at.x + up[1] * at.y,
                                     centre.z + right[2] * at.x + up[2] * at.y};
            setRigidInfluence(out, world, nullptr);
            out.u = f32(uv[c].u) * kTexel;
            out.v = f32(uv[c].v) * kTexel;
        }
        // Wound the way the engine lists the corners, which is the reverse of a GL front face.
        static constexpr u16 kQuad[6] = {0, 2, 1, 0, 3, 2};
        mesh.indices.assign(std::begin(kQuad), std::end(kQuad));
        mParser.mTrace.countBones(1);
        frame->addMesh(std::move(mesh));
    }
}
/// Reads a type 1 DMA call chain, taking the material from the shape
/// record's matIndex and the stream from extData.
/// @fn sdfModelSetNormalDrawPacket
/// @note SLUS-20911 0x002B0AC0
void ModelHooks::sdfModelSetNormalDrawPacket(const NodeDraw& draw, u32 chainAddr, Frame* frame) {
    sdfMatPtr_t mat;
    std::vector<Matrix4> bones;
    for (u32 at = chainAddr; ; at += 16) {
        const u32* tag = u32Ptr_t{at}.slice(4);
        if (!tag) return;

        const u32 id = (tag[0] >> 28) & 7;
        const u32 qwc = tag[0] & 0xFFFF;
        if (id == sdfDmaTagRet) return;
        // Inline VU setup ahead of a weighted stream: the bone count and the kick that takes them.
        if (id == sdfDmaTagCnt) {
            mParser.mTrace.countSetupRef();
            at += 16 * qwc;
            continue;
        }
        if (id != sdfDmaTagRef) {
            reportUnsupported("type 1 chain tag", id);
            return;
        }

        const u32 target = tag[1];
        const auto found = mParser.mMatByPacket.find(target);
        if (found != mParser.mMatByPacket.end()) {
            mat = {found->second};
            continue;
        }

        if (isBoneRef(tag[3], qwc)) {
            const Matrix4Ptr_t tm{target};
            if (!tm) return;
            bones.push_back(*tm);
            continue;
        }

        // A mesh stream opens with the batch header unpack. Anything else in the chain is setup
        // the hardware consumes and we already have by other means, so it is counted, not read.
        VifCode code;
        if (!readVifCode(target, code) || !isUnpack(code) || code.count != 1 ||
            getUnpackWidth(code) != 8) {
            mParser.mTrace.countSetupRef();
            continue;
        }
        const u32 end = target + 16u * qwc;
        const sdfCoreBatchHeaderPtr_t header{code.next};
        const bool weighted = header && (header->packetBits & sdfCorePacketWeights);
        const bool read = weighted ? mParser.readWeightedVifStream(target, end, draw, mat, bones, frame)
                                   : mParser.readVifStreamMesh(target, end, draw, mat, frame);
        if (!read) mParser.mTrace.countStrayRef();
        bones.clear();
    }
}

/// A weighted stream's bone is the node's tm packet (sdfModelSetTmRefPacket), sent without its
/// trailing command quadword.
bool ModelHooks::isBoneRef(u32 vifCode, u32 qwc) {
    VifCode code;
    code.command = vifCode >> 24;
    code.count = (vifCode >> 16) & 0xFF;
    const u32 matrixQwords = u32(sizeof(sdf3DTmPacketData_t::tm) / sdfQwordBytes);
    return isUnpack(code) && getUnpackWidth(code) == 16 && code.count == matrixQwords &&
           qwc == matrixQwords;
}
/// Keeps a triangle when its signed screen area times the packet's backface
/// sign is non-negative.
/// @fn corePktBuildFaceList
/// @note SLUS-20911 0x002B7A80
bool ModelHooks::corePktBuildFaceList(u32 at, const sdfCorePacket_t& packet,
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
bool ModelHooks::readWeightedBatches(u32 at, const sdfCorePacket_t& packet,
                                    u32 nodesAddr, DrawMesh& mesh) {
    const u16 usedNodeCount = u16Ptr_t{nodesAddr}.readOr();
    if (!usedNodeCount || usedNodeCount > kMaxBones) return false;

    // Every batch blends the same node set, so the palette is built once from the mesh header.
    const u16* nodeIndices = u16Ptr_t{nodesAddr + 2}.slice(usedNodeCount);
    if (!nodeIndices) return false;
    mesh.bones.resize(usedNodeCount);
    for (u16 n = 0; n < usedNodeCount; n++) {
        const sdfModelNodePtr_t node = readListItem(mParser.mNodeList, nodeIndices[n]);
        const Matrix4Ptr_t tm{node.addr + u32(offsetof(sdfModelNode_t, tm))};
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
                    mParser.mTrace.countInfluences(kMaxBoneInfluences + 1);
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
        mParser.mTrace.countInfluences(used);
    }
    mParser.readSecondUv(at, packet, mesh);
    return true;
}

}  // namespace sdfhook
