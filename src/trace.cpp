#include "trace.h"
#include "log.h"

#include <format>

#include "json.h"

namespace sdfhook {
namespace {

LogChannel log("trace");

}  // namespace

Trace::Trace(pcsx2hook_ctx* runtime, const std::string& path) : mRuntime(runtime) {
    mFile = std::fopen(path.c_str(), "w");
    if (!mFile) {
        log.info("cannot open {}", path);
    }
}

Trace::~Trace() {
    if (mFile) std::fclose(mFile);
}

void Trace::onRunSummary(const std::string& row) { write(row); }

void Trace::write(const std::string& row) {
    if (mFile) std::fwrite(row.data(), 1, row.size(), mFile);
}

void Trace::onFrame(u32 frame) {
    mFrame = frame;
    write(JsonObject().addString("ev", "frame").addUint("seq", mSeq++).addUint("frame", u64(frame)).take());
}

void Trace::onDds3DrawProcess(u32 arg) {
    write(JsonObject()
              .addString("ev", "dds3DrawProcess")
              .addUint("seq", mSeq++)
              .addUint("frame", u64(mFrame))
              .addHexDigits("arg", arg, 8)
              .take());
}

void Trace::onSdf3DSetCamera(u32 cameraAddr, u32 screenParamAddr) {
    write(JsonObject()
              .addString("ev", "sdf3DSetCamera")
              .addUint("seq", mSeq++)
              .addUint("frame", u64(mFrame))
              .addHexDigits("camera", cameraAddr, 8)
              .addHexDigits("screenParam", screenParamAddr, 8)
              .take());
}

void Trace::onSdfModelLinkOt(u32 otAddr, u32 rootAddr) {
    const sdfModelRootNodePtr_t rootPtr{rootAddr};
    const bool ok = bool(rootPtr);
    const sdfModelRootNode_t root = rootPtr.valueOr();
    const sdfModelNodeList_t list = ok ? root.nodeList.valueOr() : sdfModelNodeList_t{};
    const sdfModelOt_t ot = sdfModelOtPtr_t{otAddr}.valueOr();

    JsonObject row;
    row.addString("ev", "sdfModelLinkOt")
        .addUint("seq", mSeq++)
        .addUint("frame", u64(mFrame))
        .addHexDigits("ot", otAddr, 8)
        .addHexDigits("root", rootAddr, 8);
    if (ok) {
        // flags 0x08 suppresses the shaped and DMA paths, and a negative colour flips the
        // default ordering table.
        row.addUint("flags", u64(root.flags))
            .addUint("drawAttr", u64(root.drawAttr))
            .addHexDigits("color", root.color, 8)
            .addUint("nodes", u64(u16(list.count)))
            .addHexDigits("matList", root.matList.addr, 8)
            .addHexDigits("light", root.light.addr, 8)
            .addHexDigits("fog", root.fog.addr, 8)
            .addReal("zBias", root.zBias)
            .addReal("faceDivBias", root.faceDivBias);
    }
    for (int i = 0; i < 4; i++) {
        row.addHexDigits(std::format("ot{}", i).c_str(), ot.ot[i].addr, 8);
    }
    write(row.take());
}

void Trace::onSdfCorePacketGen(u32 nodeAddr, u32 packetAddr, u32 attrMask, u32 color) {
    JsonObject row;
    row.addString("ev", "sdfCorePacketGen")
        .addUint("seq", mSeq++)
        .addUint("frame", u64(mFrame))
        .addHexDigits("node", nodeAddr, 8)
        .addHexDigits("packet", packetAddr, 8)
        .addHexDigits("attrMask", attrMask, 4)
        .addHexDigits("color", color, 8);

    const sdfModelNodePtr_t node{nodeAddr};
    if (node) row.addHexDigits("nodeId", node->id, 8).addUint("nodeFlags", u64(node->flags));

    // node.shape is null at draw time: the geometry travels in the packet, and packetBits is what
    // decides which arrays the body carries.
    const sdfCorePacketPtr_t packet{packetAddr};
    if (packet) {
        row.addUint("type", u64(packet->header.packetType))
            .addUint("mpg", u64(packet->header.mpgType))
            .addUint("points", u64(packet->prim.pointCount))
            .addUint("faces", u64(packet->prim.faceCount))
            .addHexDigits("attrBits", packet->prim.attrBits, 4)
            .addHexDigits("packetBits", packet->prim.packetBits, 4)
            .addUint("mat", u64(packet->header.matIndex));
    }
    write(row.take());
}

void Trace::onSdfModelLinkShapedDrawPacket(u32 nodeAddr, u32 shapeAddr) {
    write(JsonObject()
              .addString("ev", "sdfModelLinkShapedDrawPacket")
              .addUint("seq", mSeq++)
              .addUint("frame", u64(mFrame))
              .addHexDigits("node", nodeAddr, 8)
              .addHexDigits("shape", shapeAddr, 8)
              .take());
}

namespace {

/// The GS state a draw should run under, under the same field names pcsx2-hook's GS trace uses, so
/// the two join and diff field by field rather than through a translation nobody maintains.
void addGsState(JsonObject& row, u64 tex0, u64 tex1, u64 clamp, u64 alpha, u64 test, u16 attrBits) {
    row.addUint("prim", attrBits & 7)
        .addUint("iip", (attrBits >> 3) & 1)
        .addUint("tme", sdfPrimTme(attrBits))
        .addUint("fge", (attrBits >> 5) & 1)
        .addUint("abe", sdfPrimAbe(attrBits));

    const GsTex0 t = GsTex0::fromReg(tex0);
    row.addHexDigits("tex0", tex0, 16)
        .addUint("tbp", t.tbp0)
        .addUint("tbw", t.tbw)
        .addUint("psm", t.psm)
        .addUint("tw", t.tw)
        .addUint("th", t.th)
        .addUint("tfx", sdfTex0Tfx(tex0))
        .addUint("tcc", sdfTex0Tcc(tex0));

    row.addHexDigits("tex1", tex1, 16)
        .addUint("mmag", sdfTex1Mmag(tex1))
        .addUint("mmin", sdfTex1Mmin(tex1));

    row.addHexDigits("clamp", clamp, 16)
        .addUint("wms", sdfClampWms(clamp))
        .addUint("wmt", sdfClampWmt(clamp));

    row.addHexDigits("alpha", alpha, 16)
        .addUint("a", sdfAlphaA(alpha))
        .addUint("b", sdfAlphaB(alpha))
        .addUint("c", sdfAlphaC(alpha))
        .addUint("d", sdfAlphaD(alpha))
        .addUint("fix", sdfAlphaFix(alpha));

    row.addHexDigits("test", test, 16)
        .addUint("ate", sdfTestAte(test))
        .addUint("atst", sdfTestAtst(test))
        .addUint("aref", sdfTestAref(test))
        .addUint("afail", sdfTestAfail(test));
}

}  // namespace

void Trace::onMesh(const DrawMesh& mesh, const Matrix4& screenTm) {
    const MeshInfluence& first = mesh.vertices[0].influence[0];
    f32 lo[3] = {first.x, first.y, first.z};
    f32 hi[3] = {lo[0], lo[1], lo[2]};
    f32 uvLo[2] = {mesh.vertices[0].u, mesh.vertices[0].v};
    f32 uvHi[2] = {uvLo[0], uvLo[1]};
    for (const MeshVertex& vertex : mesh.vertices) {
        const f32 p[3] = {vertex.influence[0].x, vertex.influence[0].y, vertex.influence[0].z};
        for (int i = 0; i < 3; i++) {
            lo[i] = p[i] < lo[i] ? p[i] : lo[i];
            hi[i] = p[i] > hi[i] ? p[i] : hi[i];
        }
        const f32 t[2] = {vertex.u, vertex.v};
        for (int i = 0; i < 2; i++) {
            uvLo[i] = t[i] < uvLo[i] ? t[i] : uvLo[i];
            uvHi[i] = t[i] > uvHi[i] ? t[i] : uvHi[i];
        }
    }
    // The clip w each vertex lands at, which is what decides whether it survives the near plane,
    // and the screen bounds it projects to, which is what joins a draw to a GS batch.
    f32 clipLo = 0.0f;
    f32 clipHi = 0.0f;
    f32 ndcLo[2] = {};
    f32 ndcHi[2] = {};
    bool anyWeight = false;
    bool anyOnScreen = false;
    for (const MeshVertex& vertex : mesh.vertices) {
        for (const MeshInfluence& influence : vertex.influence) {
            if (influence.weight == 0.0f) continue;
            const Matrix4& bone = mesh.bones[usize(influence.bone) % mesh.bones.size()];
            f32 world[3] = {};
            for (int axis = 0; axis < 3; axis++) {
                world[axis] = influence.x * bone.m[0][axis] + influence.y * bone.m[1][axis] +
                              influence.z * bone.m[2][axis] + bone.m[3][axis];
            }
            f32 clip[4] = {};
            for (int axis = 0; axis < 4; axis++) {
                clip[axis] = world[0] * screenTm.m[0][axis] + world[1] * screenTm.m[1][axis] +
                             world[2] * screenTm.m[2][axis] + screenTm.m[3][axis];
            }
            const f32 w = clip[3];
            clipLo = !anyWeight || w < clipLo ? w : clipLo;
            clipHi = !anyWeight || w > clipHi ? w : clipHi;
            anyWeight = true;

            // Behind the camera has no screen position; the renderer lets GL clip those.
            if (w <= 0.0f) continue;
            const f32 ndc[2] = {clip[0] / w, -clip[1] / w};
            for (int i = 0; i < 2; i++) {
                ndcLo[i] = !anyOnScreen || ndc[i] < ndcLo[i] ? ndc[i] : ndcLo[i];
                ndcHi[i] = !anyOnScreen || ndc[i] > ndcHi[i] ? ndc[i] : ndcHi[i];
            }
            anyOnScreen = true;
        }
    }

    JsonObject row;
    addGsState(row, mesh.tex0, mesh.tex1, mesh.clamp, mesh.gs.alpha, mesh.gs.test,
               u16((mesh.attrBits & ~7u) | sdfPrimTriangle));
    write(row
              .addString("ev", "mesh")
              .addUint("seq", mSeq++)
              .addUint("frame", u64(mFrame))
              .addUint("type", u64(mesh.packetType))
              .addHexDigits("attrBits", mesh.attrBits, 4)
              .addHexDigits("packetBits", mesh.packetBits, 4)
              .addUint("verts", u64(mesh.vertices.size()))
              .addUint("tris", u64(mesh.indices.size() / 3))
              .addUint("bones", u64(mesh.bones.size()))
              .addHexDigits("tex", mesh.texHandle.addr, 8)
              .addReal("x0", lo[0]).addReal("y0", lo[1]).addReal("z0", lo[2])
              .addReal("x1", hi[0]).addReal("y1", hi[1]).addReal("z1", hi[2])
              .addReal("u0", uvLo[0]).addReal("v0", uvLo[1])
              .addReal("u1", uvHi[0]).addReal("v1", uvHi[1])
              .addHexDigits("matFlags", mesh.matFlags, 4)
              .addUint("duty", u64(mesh.matDuty))
              .addHexDigits("diffuse",
                            u64(mesh.diffuse.r) | (u64(mesh.diffuse.g) << 8) |
                                (u64(mesh.diffuse.b) << 16) | (u64(mesh.diffuse.a) << 24),
                            8)
              .addReal("sortZ", mesh.sortZ)
              .addReal("w0", clipLo)
              .addReal("w1", clipHi)
              .addBool("onScreen", anyOnScreen)
              .addReals("ndc", {ndcLo[0], ndcLo[1], ndcHi[0], ndcHi[1]}, 4)
              .take());
}

void Trace::onSprite(const DrawSprite& sprite) {
    // A sprite names its texture by handle; the GS trace keys on TBP, so the register is read out
    // of the handle's own upload packet the way sdfTexGetTex0Reg does.
    u64 tex0 = 0;
    if (const sdfTexHandle_t* handle = sprite.texHandle.operator->()) {
        tex0 = guest_ptr<u64>{handle->packet.addr + kTex0InPacket}.valueOr();
    }
    const u16 attrBits = u16(sdfPrimSprite | (sprite.texHandle ? 0x10 : 0) |
                             (sprite.alpha != GsAlpha::Opaque ? 0x40 : 0));

    JsonObject row;
    addGsState(row, tex0, 0, 0, u64(sprite.alpha), sprite.test, attrBits);
    row.addString("ev", "sprite")
        .addUint("seq", mSeq++)
        .addUint("frame", u64(mFrame))
        .addString("producer", sprite.producer)
        .addHexDigits("texHandle", sprite.texHandle.addr, 8)
        .addUint("otOffset", u64(u32(sprite.otOffset)))
        .addUint("sequence", sprite.sequence);
    f32 ndcLo[2] = {};
    f32 ndcHi[2] = {};
    for (usize i = 0; i < std::size(sprite.corner); i++) {
        const f32 ndc[2] = {sprite.corner[i].x / kGuestWidth * 2.0f - 1.0f,
                            1.0f - sprite.corner[i].y / kGuestHeight * 2.0f};
        for (int a = 0; a < 2; a++) {
            ndcLo[a] = !i || ndc[a] < ndcLo[a] ? ndc[a] : ndcLo[a];
            ndcHi[a] = !i || ndc[a] > ndcHi[a] ? ndc[a] : ndcHi[a];
        }
    }
    row.addBool("onScreen", true)
        .addReals("ndc", {ndcLo[0], ndcLo[1], ndcHi[0], ndcHi[1]}, 4);
    row.beginArray("corners");
    for (const SpriteCorner& c : sprite.corner) {
        row.beginObject().addReals("xy", {c.x, c.y}, 1).addReals("uv", {c.u, c.v}, 1).endObject();
    }
    write(row.endArray().take());
}

void Trace::onSdfSortOtAvlRegist(u32 otAddr, u32 tagAddr, f32 z) {
    const sdfDrawTagPtr_t tagPtr{tagAddr};
    const bool ok = bool(tagPtr);
    const sdfDrawTag_t tag = tagPtr.valueOr();
    JsonObject row;
    row.addString("ev", "sdfSortOtAvlRegist")
        .addUint("seq", mSeq++)
        .addUint("frame", u64(mFrame))
        .addHexDigits("ot", otAddr, 8)
        .addHexDigits("tag", tagAddr, 8)
        .addReal("z", z);
    if (ok) {
        row.addReal("tagZ", tag.z)
            .addHexDigits("head", tag.head.addr, 8)
            .addHexDigits("tail", tag.tail.addr, 8)
            .addHexDigits("light", tag.lightPacket.addr, 8)
            .addHexDigits("fog", tag.fogPacket.addr, 8);
    }
    write(row.take());
}

void Trace::onSdfGrReqDraw(u32 tagAddr, u32 predrawAddr) {
    write(JsonObject()
              .addString("ev", "sdfGrReqDraw")
              .addUint("seq", mSeq++)
              .addUint("frame", u64(mFrame))
              .addHexDigits("tag", tagAddr, 8)
              .addHexDigits("predraw", predrawAddr, 8)
              .take());
}

void Trace::onSdfGrDrawFrame(u32 chainAddr) {
    write(JsonObject()
              .addString("ev", "sdfGrDrawFrame")
              .addUint("seq", mSeq++)
              .addUint("frame", u64(mFrame))
              .addHexDigits("chain", chainAddr, 8)
              .take());
}

void Trace::onSdfMatSet(u32 matAddr) {
    write(JsonObject()
              .addString("ev", "sdfMatSet")
              .addUint("seq", mSeq++)
              .addUint("frame", u64(mFrame))
              .addHexDigits("mat", matAddr, 8)
              .take());
}

void Trace::onSdfTexSetEx(u32 texAddr) {
    write(JsonObject()
              .addString("ev", "sdfTexSetEx")
              .addUint("seq", mSeq++)
              .addUint("frame", u64(mFrame))
              .addHexDigits("tex", texAddr, 8)
              .take());
}

void Trace::onPrimEntry(const char* name, u32 arg) {
    write(JsonObject()
              .addString("ev", name)
              .addUint("seq", mSeq++)
              .addUint("frame", u64(mFrame))
              .addHexDigits("arg", arg, 8)
              .take());
}

void Trace::onPacket(const LinkedPacket& packet, const char* by, const DmaTag& tag, u64 verts) {
    write(JsonObject()
              .addString("ev", "packet")
              .addUint("seq", mSeq++)
              .addUint("frame", u64(mFrame))
              .addHexDigits("packet", packet.addr, 8)
              .addString("by", by)
              .addUint("verts", verts)
              .addUint("qwc", u64(tag.qwc))
              .addUint("dmaId", u64(tag.id))
              .take());
}

void Trace::onSdfGrSetClearPacket(u32 packetAddr) {
    write(JsonObject()
              .addString("ev", "sdfGrSetClearPacket")
              .addUint("seq", mSeq++)
              .addUint("frame", u64(mFrame))
              .addHexDigits("packet", packetAddr, 8)
              .take());
}

}  // namespace sdfhook
