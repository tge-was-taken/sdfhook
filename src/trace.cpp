#include "trace.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "json.h"
#include "render/shading_cpu.h"

namespace sdfhook {
namespace {

LogChannel log("trace");

/// The color the mesh shaders give one vertex, in the GS's 0..255 units, to diff against the
/// post-VU `rgbaq`. The sum is shading.inc, the same text the GPU runs, so it cannot drift.
void predictLit(const DrawMesh& mesh, const MeshVertex& vertex, f32 out[3]) {
    using shading::vec3;

    f32 n[3] = {};
    for (const MeshInfluence& influence : vertex.influence) {
        if (influence.weight == 0.0f) continue;
        const Matrix4& bone = mesh.bones[usize(influence.bone) % mesh.bones.size()];
        for (int axis = 0; axis < 3; axis++) {
            n[axis] += influence.weight * (influence.nx * bone.m[0][axis] +
                                           influence.ny * bone.m[1][axis] +
                                           influence.nz * bone.m[2][axis]);
        }
    }
    const f32 length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    const f32 scale = length > 1e-6f ? 1.0f / length : 0.0f;

    // gsRgb: the shader is handed colors over 128, and the vertex color over 255 like any
    // normalised attribute. Mixing the two is what a hand-written copy of this got wrong.
    const auto gsRgb = [](Rgba c) {
        return vec3{c.r / sdfGsUnit, c.g / sdfGsUnit, c.b / sdfGsUnit};
    };
    const vec3 lit = shading::sdfMeshLit(
        vec3{n[0] * scale, n[1] * scale, n[2] * scale}, mesh.lit, gsRgb(mesh.ambient),
        gsRgb(mesh.diffuse), gsRgb(mesh.color), vec3{mesh.lighting.ambientColor},
        vec3{mesh.lighting.base}, vec3{mesh.lighting.fullColor}, vec3{mesh.lighting.direction[0]},
        vec3{mesh.lighting.direction[1]}, vec3{mesh.lighting.direction[2]},
        vec3{mesh.lighting.color[1]}, vec3{mesh.lighting.color[2]}, mesh.flatten,
        mesh.faceDivSoften, mesh.faceDivBias, mesh.streamColors);

    const f32 vertexColor[3] = {f32(vertex.color.r) / 255.0f, f32(vertex.color.g) / 255.0f,
                                 f32(vertex.color.b) / 255.0f};
    for (int c = 0; c < 3; c++) {
        out[c] = std::min(vertexColor[c] * 2.0f * lit[c] * sdfGsUnit, 255.0f);
    }
}

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

/// The inputs sdfMeshLit is given, keyed by their bytes so meshes that shade alike share one row.
u32 Trace::litCase(const DrawMesh& mesh) {
    struct Key {
        f32 lighting[8][3];
        u8 rgb[4][3];
        f32 soften;
        f32 bias;
        u32 lit;
    } key{};
    for (int i = 0; i < 3; i++) {
        for (int c = 0; c < 3; c++) {
            key.lighting[i][c] = mesh.lighting.direction[i][c];
            key.lighting[3 + i][c] = mesh.lighting.color[i][c];
        }
        key.lighting[6][i] = mesh.lighting.base[i];
        key.lighting[7][i] = mesh.lighting.specularAxis[i];
    }
    const Rgba colors[4] = {mesh.ambient, mesh.diffuse, mesh.color, mesh.specular};
    for (int i = 0; i < 4; i++) {
        key.rgb[i][0] = colors[i].r;
        key.rgb[i][1] = colors[i].g;
        key.rgb[i][2] = colors[i].b;
    }
    key.soften = mesh.faceDivSoften;
    key.bias = mesh.faceDivBias;
    key.lit = mesh.lit ? 1u : 0u;

    const std::string bytes(reinterpret_cast<const char*>(&key), sizeof(key));
    const auto known = mLitCases.find(bytes);
    if (known != mLitCases.end()) return known->second;

    const u32 id = u32(mLitCases.size());
    mLitCases.emplace(bytes, id);

    JsonObject row;
    row.addString("ev", "litcase").addUint("id", id).addUint("lit", key.lit);
    const auto rgb = [&row](const char* name, Rgba c) {
        row.addReals(name, {c.r / sdfGsUnit, c.g / sdfGsUnit, c.b / sdfGsUnit}, 6);
    };
    rgb("ambient", mesh.ambient);
    rgb("diffuse", mesh.diffuse);
    rgb("draw", mesh.color);
    rgb("specular", mesh.specular);
    const auto vec = [&row](const char* name, const f32 v[3]) {
        row.addReals(name, {v[0], v[1], v[2]}, 6);
    };
    vec("lightAmbient", mesh.lighting.ambientColor);
    vec("lightBase", mesh.lighting.base);
    vec("lightFull", mesh.lighting.fullColor);
    vec("dir0", mesh.lighting.direction[0]);
    vec("dir1", mesh.lighting.direction[1]);
    vec("dir2", mesh.lighting.direction[2]);
    vec("color1", mesh.lighting.color[1]);
    vec("color2", mesh.lighting.color[2]);
    vec("specularAxis", mesh.lighting.specularAxis);
    row.addReal("soften", mesh.faceDivSoften).addReal("bias", mesh.faceDivBias);
    write(row.take());
    return id;
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
        // flags 0x08 suppresses the shaped and DMA paths, and a negative color flips the
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
void addGsState(JsonObject& row, u64 tex0, u64 tex1, u64 clamp, u64 alpha, u64 test, u16 attrBits,
                u64 frameMask = 0) {
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
        .addUint("afail", sdfTestAfail(test))
        .addHexDigits("fbmsk", frameMask, 8);
}

}  // namespace

void Trace::onMesh(const DrawMesh& mesh, const Matrix4& screenTm, u32 frame) {
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
    // The mean projected position, which pairs a batch to its own mesh where a bounding box cannot:
    // several meshes of one model overlap, and the box match then picks whichever comes first.
    double ndcSum[2] = {};
    u32 ndcCount = 0;
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
                ndcSum[i] += ndc[i];
            }
            ndcCount++;
            anyOnScreen = true;
        }
    }

    // The color range our shaders give this mesh, against which the GS trace's own post-VU rgbaq
    // per batch is the answer key.
    f32 litLo[3] = {};
    f32 litHi[3] = {};
    for (usize v = 0; v < mesh.vertices.size(); v++) {
        f32 lit[3];
        predictLit(mesh, mesh.vertices[v], lit);
        for (int c = 0; c < 3; c++) {
            litLo[c] = v == 0 || lit[c] < litLo[c] ? lit[c] : litLo[c];
            litHi[c] = v == 0 || lit[c] > litHi[c] ? lit[c] : litHi[c];
        }
    }

    JsonObject row;
    addGsState(row, mesh.tex0, mesh.tex1, mesh.clamp, mesh.gs.alpha, mesh.gs.test,
               u16((mesh.attrBits & ~7u) | sdfPrimTriangle), mesh.gs.frameMask);
    write(row
              .addString("ev", "mesh")
              .addUint("seq", mSeq++)
              .addUint("frame", u64(frame))
              .addUint("type", u64(mesh.packetType))
              .addHexDigits("attrBits", mesh.attrBits, 4)
              .addHexDigits("packetBits", mesh.packetBits, 4)
              .addUint("verts", u64(mesh.vertices.size()))
              .addUint("tris", u64(mesh.indices.size() / 3))
              .addUint("bones", u64(mesh.bones.size()))
              .addHexDigits("tex", mesh.texHandle.addr, 8)
              // The second pass samples its own texture, so a coverage check that reads only the
              // first reports it as never sampled.
              .addUint("mtex", mesh.multi.on ? 1u : 0u)
              .addUint("mtbp", u64(sdfDecodeTex0(mesh.multi.tex0).tbp))
              // The second pass carries its own blend, so a row with only the first pass's ALPHA
              // cannot say whether it matches the hardware.
              .addHexDigits("malpha", mesh.multi.alpha, 2)
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
              .addHexDigits("ambient",
                            u64(mesh.ambient.r) | (u64(mesh.ambient.g) << 8) |
                                (u64(mesh.ambient.b) << 16) | (u64(mesh.ambient.a) << 24),
                            8)
              // The draw color the node or root gave this mesh. Its alpha is half of what VU1
              // multiplies into the vertex alpha, so a row without it cannot be checked against
              // the GS's own per-batch alpha.
              .addHexDigits("drawColor",
                            u64(mesh.color.r) | (u64(mesh.color.g) << 8) |
                                (u64(mesh.color.b) << 16) | (u64(mesh.color.a) << 24),
                            8)
              // Which association gave this mesh its GS state: a chain address for world geometry,
              // else position among the generated links. A wrong table lands on one or the other.
              .addHexDigits("link", mesh.linkAddr, 8)
              .addUint("linkIndex", u64(mesh.linkIndex))
              .addBool("linked", mesh.linked)
              .addBool("streamColors", mesh.streamColors)
              .addUint("litcase", u64(litCase(mesh)))
              .addReals("litLo", {litLo[0], litLo[1], litLo[2]}, 4)
              .addReals("litHi", {litHi[0], litHi[1], litHi[2]}, 4)
              .addUint("ot", mesh.otIndex)
              .addReal("sortZ", mesh.sortZ)
              .addReal("w0", clipLo)
              .addReal("w1", clipHi)
              .addBool("onScreen", anyOnScreen)
              .addReals("ndc", {ndcLo[0], ndcLo[1], ndcHi[0], ndcHi[1]}, 4)
              .addReals("ndcMid", {ndcCount ? ndcSum[0] / ndcCount : 0.0,
                                   ndcCount ? ndcSum[1] / ndcCount : 0.0}, 4)
              .take());
}

void Trace::onSprite(const DrawSprite& sprite) {
    // A sprite names its texture by handle; the GS trace keys on TBP, so the register is read out
    // of the handle's own upload packet the way sdfTexGetTex0Reg does.
    u64 tex0 = 0;
    if (const sdfTexHandle_t* handle = sprite.texHandle.operator->()) {
        tex0 = handle->packet.valueOr().tex0.data;
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

void Trace::onPacket(const LinkedPacket& packet, const char* by, const sdfDmaTag_t& tag, u64 verts) {
    write(JsonObject()
              .addString("ev", "packet")
              .addUint("seq", mSeq++)
              .addUint("frame", u64(mFrame))
              .addHexDigits("packet", packet.addr, 8)
              .addString("by", by)
              .addUint("verts", verts)
              .addUint("qwc", u64(tag.qwc))
              .addUint("dmaId", u64(sdfDmaId(tag)))
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
