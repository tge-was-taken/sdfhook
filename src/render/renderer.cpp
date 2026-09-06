#include "renderer.h"

#include "../log.h"
#include "../json.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "../unsupported.h"
#include "glad/gl.h"

namespace sdfhook {

namespace {

LogChannel log("render");

constexpr const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColour;
out vec2 vUv;
out vec4 vColour;
uniform vec2 uGuestSize;

// The GS treats 128 as fully opaque, not 255.
const float kPs2Alpha = 255.0 / 128.0;

void main() {
    vec2 ndc = vec2(aPos.x / uGuestSize.x * 2.0 - 1.0, 1.0 - aPos.y / uGuestSize.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUv = aUv;
    vColour = vec4(aColour.rgb, aColour.a * kPs2Alpha);
}
)";

constexpr const char* kFragmentShader = R"(#version 330 core
in vec2 vUv;
in vec4 vColour;
out vec4 oColour;
uniform sampler2D uTexture;
uniform int uTextured;
void main() {
    oColour = uTextured != 0 ? texture(uTexture, vUv) * vColour * 2.0 : vColour;
}
)";

// No sample scene blends more than four, so this is margin.
constexpr int kMaxPaletteBones = 16;

constexpr const char* kMeshVertexShader = R"(#version 330 core
layout(location = 0) in vec4 aPos0;
layout(location = 1) in vec4 aNrm0;
layout(location = 2) in vec4 aPos1;
layout(location = 3) in vec4 aNrm1;
layout(location = 4) in vec4 aPos2;
layout(location = 5) in vec4 aNrm2;
layout(location = 6) in vec4 aPos3;
layout(location = 7) in vec4 aNrm3;
layout(location = 8) in vec4 aUv;
layout(location = 9) in vec4 aColour;

uniform mat4 uScreen;
uniform mat4 uBones[16];
uniform vec3 uLightDir[3];
uniform vec3 uLightColour[3];
uniform vec3 uLightBase;
uniform vec3 uSpecularAxis;
uniform vec3 uSpecular;
uniform vec3 uDrawColour;
uniform vec3 uAmbient;
uniform vec3 uDiffuse;
uniform int uLit;
uniform vec4 uFog;   // scale, minimum, maximum, enabled
uniform float uZBias;

// Sample scenes span view distances of 20 to 3000.
const float kNearW = 1.0;
const float kFarW = 20000.0;

out vec2 vUv;
out vec2 vUv2;
out vec3 vNormal;
out vec4 vColour;
out vec3 vLit;
out float vFog;

// A vertex carries one position per bone, already in that bone's space.
void main() {
    vec4 position[4] = vec4[4](aPos0, aPos1, aPos2, aPos3);
    vec4 normal[4] = vec4[4](aNrm0, aNrm1, aNrm2, aNrm3);

    vec4 world = vec4(0.0);
    vec3 facing = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        float weight = position[i].w;
        if (weight == 0.0) continue;
        mat4 bone = uBones[int(normal[i].w)];
        world += weight * (bone * vec4(position[i].xyz, 1.0));
        facing += weight * (mat3(bone) * normal[i].xyz);
    }

    vec4 clip = uScreen * vec4(world.xyz, 1.0);

    // The engine's z is not GL clip space, so depth is rebuilt from w. Deliberately unclamped:
    // clamping pins near geometry to the front of the range instead of letting GL clip it.
    float viewDepth = (clip.w - uZBias - kNearW) / (kFarW - kNearW);
    // The guest's screen has +Y down.
    gl_Position = vec4(clip.x, -clip.y, (viewDepth * 2.0 - 1.0) * clip.w, clip.w);
    // VU1 clamps the lit colour per vertex, before interpolation. Its 255 is 2.0 here, with the
    // GS's 128-is-one modulate taken out.
    vec3 light = uLightBase * uAmbient;
    if (uLit != 0) {
        vec3 n = normalize(facing);
        vec3 diffuse = vec3(0.0);
        // The packet stores the direction light travels, not the surface-to-light vector.
        for (int i = 0; i < 3; i++) diffuse += max(dot(n, -uLightDir[i]), 0.0) * uLightColour[i];
        light += diffuse * uDiffuse;
        // corePktEmitRefMapPass is a highlight, not a lookup.
        light += max(dot(n, uSpecularAxis), 0.0) * uSpecular;
    } else {
        light = vec3(1.0);
    }
    vLit = min(light * uDrawColour, vec3(2.0));

    // An offset plus a reciprocal, not a clamped reciprocal: only that fits the traced per-batch
    // fog values, which sit at 255 for all but the most distant geometry.
    vFog = uFog.w != 0.0 ? min(uFog.y + uFog.x / max(clip.w, 1e-3), uFog.z) : 1.0;

    vUv = aUv.xy;
    vUv2 = aUv.zw;
    vNormal = facing;
    vColour = aColour;
}
)";

constexpr const char* kMeshFragmentShader = R"(#version 330 core
in vec2 vUv;
in vec2 vUv2;
in vec3 vNormal;
in vec4 vColour;
in vec3 vLit;
in float vFog;
out vec4 oColour;
uniform sampler2D uTexture;
uniform int uTextured;
uniform int uDebugDepth;
uniform vec3 uFogColour;
uniform int uAtest;       // GS ATST
uniform float uAtestRef;  // AREF, where 128 is 1.0
uniform int uAtestInvert;
uniform int uTexFunction; // GS TEX0 TFX
uniform int uTexAlpha;    // GS TEX0 TCC
uniform int uSecondUv;
uniform vec4 uTexMatrix;
uniform vec2 uTexOffset;

// The GS treats 128 as opaque, not 255.
const float kPs2Alpha = 255.0 / 128.0;

// ALWAYS and EQUAL fall through.
bool atestFails(float a) {
    return uAtest == 0
        || (uAtest == 2 && a >= uAtestRef)
        || (uAtest == 3 && a >  uAtestRef)
        || (uAtest == 5 && a <  uAtestRef)
        || (uAtest == 6 && a <= uAtestRef)
        || (uAtest == 7 && a == uAtestRef);
}

void main() {
    // VU1 already scaled the vertex and material alpha, so kPs2Alpha applies to the texel only.
    vec4 colour = vec4(vColour.rgb * 2.0 * vLit, vColour.a);
    vec2 raw = uSecondUv != 0 ? vUv2 : vUv;
    vec2 uv = vec2(dot(raw, uTexMatrix.xy), dot(raw, uTexMatrix.zw)) + uTexOffset;
    vec4 texel = uTextured != 0 ? texture(uTexture, uv) : vec4(1.0);
    float texA = uTexAlpha != 0 ? min(texel.a * kPs2Alpha, 1.0) : 1.0;
    float testA = min(texA * colour.a * kPs2Alpha, 1.0);
    // AFAIL FB_ONLY is two GL draws, and this is the half uAtestInvert selects.
    if (atestFails(testA) != (uAtestInvert != 0)) discard;

    vec3 rgb = uTexFunction == 1 ? texel.rgb
             : uTexFunction == 0 ? texel.rgb * colour.rgb
                                 : texel.rgb * colour.rgb + colour.rgb;
    if (uTextured == 0) rgb = colour.rgb;
    oColour = vec4(rgb, testA);
    oColour.rgb = mix(uFogColour, oColour.rgb, vFog);
    if (uDebugDepth != 0) oColour = vec4(vec3(gl_FragCoord.z), 1.0);
}
)";

/// A colour in the GS's units, as a uniform's three floats.
struct GsRgb {
    f32 v[3];
};

GsRgb gsRgb(Rgba c) { return {{c.r / sdfGsUnit, c.g / sdfGsUnit, c.b / sdfGsUnit}}; }

/// The GS ALPHA register, Cv = (A - B) * C + D, as a GL blend. Follows p3fes SkyGL's
/// gsAlphaToBlend, minus the FIX cases no SDF draw uses.
struct GsBlend {
    GLenum equation = GL_FUNC_ADD;
    GLenum src = GL_SRC_ALPHA;
    GLenum dst = GL_ONE_MINUS_SRC_ALPHA;
};

GsBlend gsAlphaToBlend(u64 reg) {
    const u32 a = sdfAlphaA(reg);
    const u32 b = sdfAlphaB(reg);
    const u32 c = sdfAlphaC(reg);
    const u32 d = sdfAlphaD(reg);
    const GLenum coef = c == sdfAlphaAd ? GL_DST_ALPHA : GL_SRC_ALPHA;
    const GLenum inverse = c == sdfAlphaAd ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_SRC_ALPHA;

    GsBlend blend;
    if (a == sdfAlphaSource && b == sdfAlphaDest && d == sdfAlphaDest) {
        blend.src = coef;  // (Cs - Cd) * C + Cd
        blend.dst = inverse;
    } else if (a == sdfAlphaSource && b == sdfAlphaZero && d == sdfAlphaDest) {
        blend.src = coef;  // Cs * C + Cd, additive
        blend.dst = GL_ONE;
    } else if (a == sdfAlphaZero && b == sdfAlphaSource && d == sdfAlphaDest) {
        blend.equation = GL_FUNC_REVERSE_SUBTRACT;  // Cd - Cs * C
        blend.src = coef;
        blend.dst = GL_ONE;
    } else if (a == sdfAlphaSource && b == sdfAlphaZero && d == sdfAlphaZero) {
        blend.src = coef;  // Cs * C, nothing of the destination
        blend.dst = GL_ZERO;
    } else {
        reportUnsupported("GS ALPHA", u32(reg));
    }
    return blend;
}

void applyBlend(GsAlpha alpha) {
    switch (alpha) {
        case GsAlpha::Additive:
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            return;
        case GsAlpha::ReverseSubtract:
            glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            return;
        case GsAlpha::Opaque:
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_ONE, GL_ZERO);
            return;
        case GsAlpha::Blend:
            break;
        default:
            reportUnsupported("ALPHA", u32(alpha));
            break;
    }
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

u32 link(u32 vertex, u32 fragment) {
    const u32 program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    int ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char info[512];
        glGetProgramInfoLog(program, sizeof(info), nullptr, info);
        log.info("link: {}", info);
    }
    return program;
}

u32 compile(GLenum type, const char* source) {
    const u32 shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char info[512];
        glGetShaderInfoLog(shader, sizeof(info), nullptr, info);
        log.info("shader: {}", info);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

void Renderer::Bounds::add(const ScreenVertex& v) {
    if (!seen) {
        seen = true;
        minX = maxX = v.x;
        minY = maxY = v.y;
        return;
    }
    minX = v.x < minX ? v.x : minX;
    minY = v.y < minY ? v.y : minY;
    maxX = v.x > maxX ? v.x : maxX;
    maxY = v.y > maxY ? v.y : maxY;
}

bool Renderer::init(const Config& config, int width, int height) {
    mConfig = &config;
    for (RenderTexture& target : mFrames) {
        if (!target.create(width, height)) return false;
    }
    const u32 vs = compile(GL_VERTEX_SHADER, kVertexShader);
    const u32 fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    const u32 meshVs = compile(GL_VERTEX_SHADER, kMeshVertexShader);
    const u32 meshFs = compile(GL_FRAGMENT_SHADER, kMeshFragmentShader);
    if (!vs || !fs || !meshVs || !meshFs) return false;
    mProgram = link(vs, fs);
    mMeshProgram = link(meshVs, meshFs);

    glGenVertexArrays(1, &mVao);
    glGenBuffers(1, &mVbo);
    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    constexpr int kStride = 8 * sizeof(f32);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, nullptr);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride, (void*)(2 * sizeof(f32)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, kStride, (void*)(4 * sizeof(f32)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    glGenVertexArrays(1, &mMeshVao);
    glGenBuffers(1, &mMeshVbo);
    glGenBuffers(1, &mMeshEbo);
    glBindVertexArray(mMeshVao);
    glBindBuffer(GL_ARRAY_BUFFER, mMeshVbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mMeshEbo);
    constexpr int kMeshStride = sizeof(MeshVertex);
    // Two attributes per influence: the position with its weight, then the normal with the palette
    // index that transforms both.
    for (int slot = 0; slot < int(kMaxBoneInfluences); slot++) {
        const usize at = offsetof(MeshVertex, influence) + usize(slot) * sizeof(MeshInfluence);
        glVertexAttribPointer(2 * slot, 4, GL_FLOAT, GL_FALSE, kMeshStride, (void*)at);
        glVertexAttribPointer(2 * slot + 1, 4, GL_FLOAT, GL_FALSE, kMeshStride,
                              (void*)(at + 4 * sizeof(f32)));
        glEnableVertexAttribArray(2 * slot);
        glEnableVertexAttribArray(2 * slot + 1);
    }
    glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE, kMeshStride, (void*)offsetof(MeshVertex, u));
    glVertexAttribPointer(9, 4, GL_UNSIGNED_BYTE, GL_TRUE, kMeshStride,
                          (void*)offsetof(MeshVertex, colour));
    glEnableVertexAttribArray(8);
    glEnableVertexAttribArray(9);
    glBindVertexArray(0);
    return true;
}

void Renderer::shutdown() {
    for (RenderTexture& target : mFrames) target.destroy();
    glDeleteBuffers(1, &mVbo);
    glDeleteVertexArrays(1, &mVao);
    glDeleteProgram(mProgram);
    glDeleteBuffers(1, &mMeshVbo);
    glDeleteBuffers(1, &mMeshEbo);
    glDeleteVertexArrays(1, &mMeshVao);
    glDeleteProgram(mMeshProgram);
}

const Renderer::Texture* Renderer::findTexture(TexHandle handle) const {
    const auto found = mTextures.find(handle);
    return found == mTextures.end() ? nullptr : &found->second;
}

void Renderer::present() {
    mFrames[mCurrent].blitToScreen();
    mCurrent ^= 1;
}

void Renderer::represent() { mFrames[mCurrent ^ 1].blitToScreen(); }

void Renderer::uploadTextures(const Frame& frame) {
    for (const TextureUpload& upload : frame.textureUploads) {
        if (mTextures.count(upload.handle)) continue;
        u32 name = 0;
        glGenTextures(1, &name);
        glBindTexture(GL_TEXTURE_2D, name);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLsizei(upload.width), GLsizei(upload.height), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, upload.rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        mTextures.emplace(upload.handle, Texture{name, f32(upload.width), f32(upload.height)});
    }
}

void Renderer::draw(const Frame& frame) {
    mFrames[mCurrent].bind();
    uploadTextures(frame);

    const Rgba& back = frame.background;
    glClearColor(back.r / 255.0f, back.g / 255.0f, back.b / 255.0f, 1.0f);
    glClearDepth(1.0);
    glDepthFunc(GL_LESS);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    drawMeshes(frame);
    // 2D and 3D share ordering table 0, which sorts by z, so sprites belong among the meshes.
    // Ours carry no z yet, and submission order alone costs 25 points.
    drawSprites(frame, 0, 0xFFFFFFFFu);
}

void Renderer::drawMeshes(const Frame& frame) {
    if (frame.meshes.empty()) return;

    // Bisecting a bad mesh costs an emulator run per step, so the range is a run-time choice.
    const usize firstMesh = mConfig->meshFirst;
    const usize lastMesh = std::min(mConfig->meshLast, frame.meshes.size());

    glUseProgram(mMeshProgram);
    glBindVertexArray(mMeshVao);
    glBindBuffer(GL_ARRAY_BUFFER, mMeshVbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mMeshEbo);
    glEnable(GL_DEPTH_TEST);
    // corePktBuildFaceList drops a triangle whose screen-space signed area disagrees with the
    // draw's own sign, which is what GL_CULL_FACE does.
    if (mConfig->cull == 'n') glDisable(GL_CULL_FACE);
    else glEnable(GL_CULL_FACE);
    glCullFace(mConfig->cull == 'f' ? GL_FRONT : GL_BACK);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // The matrices are row-vector, which is what GL reads a row-major upload as when it is not
    // asked to transpose, so uScreen * uBone * point composes in the engine's own order.
    glUniformMatrix4fv(glGetUniformLocation(mMeshProgram, "uScreen"), 1, GL_FALSE,
                       &frame.screenTm.m[0][0]);
    const int bones = glGetUniformLocation(mMeshProgram, "uBones");
    const int textured = glGetUniformLocation(mMeshProgram, "uTextured");
    const int lightDir = glGetUniformLocation(mMeshProgram, "uLightDir");
    const int lightColour = glGetUniformLocation(mMeshProgram, "uLightColour");
    const int lightBase = glGetUniformLocation(mMeshProgram, "uLightBase");
    const int drawColour = glGetUniformLocation(mMeshProgram, "uDrawColour");
    const int lit = glGetUniformLocation(mMeshProgram, "uLit");
    const int ambient = glGetUniformLocation(mMeshProgram, "uAmbient");
    const int diffuse = glGetUniformLocation(mMeshProgram, "uDiffuse");
    const int specular = glGetUniformLocation(mMeshProgram, "uSpecular");
    const int specularAxis = glGetUniformLocation(mMeshProgram, "uSpecularAxis");
    const int fog = glGetUniformLocation(mMeshProgram, "uFog");
    const int fogColour = glGetUniformLocation(mMeshProgram, "uFogColour");
    const int zBias = glGetUniformLocation(mMeshProgram, "uZBias");
    glUniform1i(glGetUniformLocation(mMeshProgram, "uDebugDepth"), mConfig->debugDepth);

    // The ordering tables sort by the tag's z before transmitting, so submission order is not draw
    // order. Stable, so meshes at equal depth keep the order they were built in.
    mOrder.clear();
    mOrder.reserve(frame.meshes.size());
    for (usize index = firstMesh; index < frame.meshes.size() && index <= lastMesh; index++) {
        if (frame.meshes[index].otIndex <= mConfig->otMax) mOrder.push_back(index);
    }
    if (mConfig->meshOrder != 'n') {
        const bool nearFirst = mConfig->meshOrder == 'f';
        std::stable_sort(mOrder.begin(), mOrder.end(), [&](usize a, usize b) {
            const DrawMesh& x = frame.meshes[a];
            const DrawMesh& y = frame.meshes[b];
            if (x.otIndex != y.otIndex) return x.otIndex < y.otIndex;
            return nearFirst ? x.sortZ < y.sortZ : x.sortZ > y.sortZ;
        });
    }

    const int atest = glGetUniformLocation(mMeshProgram, "uAtest");
    const int atestRef = glGetUniformLocation(mMeshProgram, "uAtestRef");
    const int atestInvert = glGetUniformLocation(mMeshProgram, "uAtestInvert");
    const int texFunction = glGetUniformLocation(mMeshProgram, "uTexFunction");
    const int texAlpha = glGetUniformLocation(mMeshProgram, "uTexAlpha");
    const int secondUv = glGetUniformLocation(mMeshProgram, "uSecondUv");
    const int texMatrix = glGetUniformLocation(mMeshProgram, "uTexMatrix");
    const int texOffset = glGetUniformLocation(mMeshProgram, "uTexOffset");

    // TEX1 and CLAMP belong to the draw rather than to the texture object, so they are set here.
    const auto bindTexture = [&](TexHandle handle, u64 tex0, u64 tex1, u64 clamp, int texturedLoc,
                                 int functionLoc, int alphaLoc) {
        const Texture* texture = findTexture(handle);
        glUniform1i(texturedLoc, texture ? 1 : 0);
        glUniform1i(functionLoc, texture ? int(sdfTex0Tfx(tex0)) : int(sdfTexModulate));
        glUniform1i(alphaLoc, texture && sdfTex0Tcc(tex0) ? 1 : 0);
        if (!texture) return;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture->name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        sdfTex1Mmag(tex1) ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        sdfTex1Mmin(tex1) ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        sdfClampWms(clamp) == sdfWrapRepeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        sdfClampWmt(clamp) == sdfWrapRepeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    };

    // The passing half of the alpha test writes depth; the failing half, which AFAIL still lets
    // write colour, does not. Back to back so the blend order stays the engine's own.
    const auto drawMesh = [&](const DrawMesh& mesh) {
        glUniform1i(atestInvert, 0);
        glDepthMask(GL_TRUE);
        glDrawElements(GL_TRIANGLES, GLsizei(mesh.indices.size()), GL_UNSIGNED_SHORT, nullptr);
        if (sdfTestAte(mesh.gs.test) && sdfTestAfail(mesh.gs.test) != sdfAfailKeep) {
            glUniform1i(atestInvert, 1);
            glDepthMask(GL_FALSE);
            glDrawElements(GL_TRIANGLES, GLsizei(mesh.indices.size()), GL_UNSIGNED_SHORT, nullptr);
        }
    };

    for (usize index : mOrder) {
        const DrawMesh& mesh = frame.meshes[index];
        if (mesh.bones.size() > kMaxPaletteBones) {
            reportUnsupported("bones in one mesh", u32(mesh.bones.size()));
            continue;
        }
        glUniformMatrix4fv(bones, GLsizei(mesh.bones.size()), GL_FALSE, &mesh.bones[0].m[0][0]);
        glUniform3fv(lightDir, 3, &mesh.lighting.direction[0][0]);
        glUniform3fv(lightColour, 3, &mesh.lighting.colour[0][0]);
        glUniform3fv(lightBase, 1, mesh.lighting.base);
        glUniform1i(lit, mesh.lit ? 1 : 0);
        glUniform3fv(drawColour, 1, gsRgb(mesh.colour).v);
        glUniform3fv(ambient, 1, gsRgb(mesh.ambient).v);
        glUniform3fv(diffuse, 1, gsRgb(mesh.diffuse).v);

        // Only a draw that asks for the reflection pass gets the highlight.
        const bool reflective =
            !mConfig->noSpecular && (mesh.attrBits & sdfCoreAttrRefMapPass) != 0;
        const GsRgb spec = reflective ? gsRgb(mesh.specular) : GsRgb{};
        glUniform3fv(specular, 1, spec.v);
        glUniform3fv(specularAxis, 1, mesh.lighting.specularAxis);

        const bool fogged =
            !mConfig->noFog && mesh.fog.on && (mesh.attrBits & sdfCoreAttrFog) != 0;
        glUniform4f(fog, mesh.fog.scale, mesh.fog.minimum, mesh.fog.maximum, fogged ? 1.0f : 0.0f);
        glUniform3fv(fogColour, 1, mesh.fog.colour);
        glUniform1f(zBias, mesh.zBias);

        // A two-sided draw runs every pass twice on the hardware, once over the back faces with
        // their normals flipped, so nothing is culled for it here.
        if (mConfig->cull != 'n') {
            if (mesh.isTwoSided()) {
                glDisable(GL_CULL_FACE);
            } else {
                glEnable(GL_CULL_FACE);
                const bool flip = mConfig->cull == 'f';
                glFrontFace(mesh.reverseFacing != flip ? GL_CW : GL_CCW);
            }
        }
        const GsBlend blend = gsAlphaToBlend(mesh.gs.alpha);
        glBlendEquation(blend.equation);
        glBlendFunc(blend.src, blend.dst);
        glUniform1i(atest, sdfTestAte(mesh.gs.test) ? int(sdfTestAtst(mesh.gs.test)) : 1);
        glUniform1f(atestRef, f32(sdfTestAref(mesh.gs.test)) / sdfGsUnit);

        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(mesh.vertices.size() * sizeof(MeshVertex)),
                     mesh.vertices.data(), GL_STREAM_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(mesh.indices.size() * sizeof(u16)),
                     mesh.indices.data(), GL_STREAM_DRAW);

        bindTexture(mesh.texHandle, mesh.tex0, mesh.tex1, mesh.clamp, textured, texFunction,
                    texAlpha);
        glUniform4fv(texMatrix, 1, mesh.texMatrix);
        glUniform2fv(texOffset, 1, mesh.texMatrix + 4);
        glUniform1i(secondUv, 0);
        drawMesh(mesh);

        // corePktEmitMultiTexPass runs the whole thing again through the second UV set, its own
        // texture and its own blend, which is GS context 2 on the hardware.
        if (mesh.multi.on) {
            const GsBlend second = gsAlphaToBlend(mesh.multi.alpha);
            glBlendEquation(second.equation);
            glBlendFunc(second.src, second.dst);
            glUniform3fv(drawColour, 1, gsRgb(mesh.multi.diffuse).v);
            bindTexture(mesh.multi.texHandle, mesh.multi.tex0, mesh.multi.tex1, mesh.multi.clamp,
                        textured, texFunction, texAlpha);
            glUniform1i(secondUv, 1);
            drawMesh(mesh);
        }
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    // The 2D pass submits quads in whatever winding the engine built them, so culling must not
    // outlive the mesh pass. Leaving it on quietly deleted half of every sprite.
    glDisable(GL_CULL_FACE);
}

void Renderer::drawSprites(const Frame& frame, u32 fromSequence, u32 toSequence) {
    const bool skip = mConfig->noSprites;
    if (skip || frame.sprites.empty()) return;

    glUseProgram(mProgram);
    glUniform2f(glGetUniformLocation(mProgram, "uGuestSize"), kGuestWidth, kGuestHeight);
    const int textured = glGetUniformLocation(mProgram, "uTextured");
    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    glEnable(GL_BLEND);

    // One draw call per sprite: batching needs equal texture and blend state, and getting the state
    // right comes first.
    if (mDescribe && mConfig->describeFrame) {
        log.dump(JsonObject()
                     .addString("dump", "frame")
                     .addUint("sprites", frame.sprites.size())
                     .addUint("meshes", frame.meshes.size())
                     .take());
        for (const DrawSprite& sprite : frame.sprites) {
            f32 lo[2] = {sprite.corner[0].x, sprite.corner[0].y};
            f32 hi[2] = {lo[0], lo[1]};
            for (const SpriteCorner& c : sprite.corner) {
                lo[0] = std::min(lo[0], c.x);
                lo[1] = std::min(lo[1], c.y);
                hi[0] = std::max(hi[0], c.x);
                hi[1] = std::max(hi[1], c.y);
            }
            JsonObject row;
            row.addString("dump", "sprite")
                .addString("producer", sprite.producer)
                .addReals("min", {lo[0], lo[1]}, 1)
                .addReals("max", {hi[0], hi[1]}, 1)
                .addHexDigits("texHandle", sprite.texHandle.addr, 8)
                .addHexDigits("test", sprite.test, 6)
                .addUint("alpha", u32(sprite.alpha))
                .beginArray("corners");
            for (const SpriteCorner& c : sprite.corner) {
                row.pushHexDigits(u32(c.colour.r) << 24 | u32(c.colour.g) << 16 |
                                      u32(c.colour.b) << 8 | c.colour.a,
                                  8);
            }
            log.dump(row.endArray().take());
        }
    }

    // Isolating a producer that is over- or under-contributing, without a run per guess.
    const char* const skipProducer =
        mConfig->skipProducer.empty() ? nullptr : mConfig->skipProducer.c_str();

    for (const DrawSprite& sprite : frame.sprites) {
        if (sprite.sequence < fromSequence || sprite.sequence >= toSequence) continue;
        if (skipProducer && std::strstr(sprite.producer, skipProducer)) continue;
        // ATST NEVER with AFAIL ZB_ONLY writes no colour: it lays a depth barrier, and drawing it
        // paints a rectangle the hardware never shows.
        if (sdfTestAte(sprite.test) && sdfTestAtst(sprite.test) == 0 &&
            sdfTestAfail(sprite.test) == sdfAfailZbOnly) {
            continue;
        }
        // The previous frame is the renderer's own target, not an entry in the texture map, and
        // its UVs are in the guest's own draw space rather than texels.
        const bool isFeedback = sprite.texHandle.addr == kPreviousFrameHandle;
        const bool noFeedback = mConfig->noFeedback;
        if (isFeedback && noFeedback) continue;
        const Texture* entry = findTexture(sprite.texHandle);
        const bool hasTexture = isFeedback || entry != nullptr;
        mVerts.clear();
        const f32 texWidth = isFeedback ? kGuestWidth : entry ? entry->width : 1.0f;
        const f32 texHeight = isFeedback ? kGuestHeight : entry ? entry->height : 1.0f;
        toTriangles(sprite, texWidth, texHeight, mVerts);
        // GL puts a render target's origin at the bottom left, the guest's is at the top.
        if (isFeedback) {
            for (ScreenVertex& vertex : mVerts) vertex.v = 1.0f - vertex.v;
        }

        std::vector<f32> attribs;
        attribs.reserve(mVerts.size() * 8);
        for (const ScreenVertex& v : mVerts) {
            mBounds.add(v);
            attribs.insert(attribs.end(), {v.x, v.y, v.u, v.v, v.colour.r / 255.0f,
                                           v.colour.g / 255.0f, v.colour.b / 255.0f,
                                           v.colour.a / 255.0f});
        }
        applyBlend(sprite.alpha);
        glUniform1i(textured, hasTexture ? 1 : 0);
        if (hasTexture) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D,
                          isFeedback ? mFrames[mCurrent ^ 1].getTexture() : entry->name);
        }
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(attribs.size() * sizeof(f32)), attribs.data(),
                     GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(mVerts.size()));
    }
}

std::vector<u8> Renderer::readPixels(int width, int height) const {
    std::vector<u8> gl(usize(width) * usize(height) * 3);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, gl.data());
    std::vector<u8> out(gl.size());
    const usize stride = usize(width) * 3;
    for (int y = 0; y < height; y++) {
        std::copy_n(gl.begin() + GLsizeiptr(usize(height - 1 - y) * stride), stride,
                    out.begin() + GLsizeiptr(usize(y) * stride));
    }
    return out;
}

}  // namespace sdfhook
