#include <map>
#include "renderer.h"

#include "../log.h"
#include "../json.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "shading_glsl.h"

#include "../unsupported.h"
#include "glad/gl.h"

namespace sdfhook {

namespace {

LogChannel log("render");

constexpr const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
layout(location = 3) in float aZ;
layout(location = 4) in float aFog;
out vec2 vUv;
out vec4 vColor;
out float vFog;
uniform vec2 uGuestSize;
uniform vec2 uPixelOffset;

// The GS treats 128 as fully opaque, not 255.
const float kPs2Alpha = 255.0 / 128.0;

void main() {
    vec2 ndc = vec2(aPos.x / uGuestSize.x * 2.0 - 1.0, 1.0 - aPos.y / uGuestSize.y * 2.0);
    gl_Position = vec4(ndc + uPixelOffset, aZ, 1.0);
    vUv = aUv;
    vFog = aFog;
    vColor = vec4(aColor.rgb, aColor.a * kPs2Alpha);
}
)";

constexpr const char* kFragmentShader = R"(#version 330 core
in vec2 vUv;
in vec4 vColor;
in float vFog;
out vec4 oColor;
uniform sampler2D uTexture;
uniform int uTextured;
uniform int uTexFunction;  // TEX0's TFX
uniform int uTexAlpha;     // TEX0's TCC
uniform float uTexelAlpha; // TEXA's TA0 for a 24-bit texture, negative for the texel's own
uniform int uFogOn;
uniform vec3 uFogColor;
uniform int uAtest;
uniform float uAtestRef;
uniform int uAtestInvert;  // AFAIL's half: the fragments that fail
uniform ivec2 uRegionClamp; // CLAMP's WMS and WMT are REGION_CLAMP
uniform vec4 uRegion;       // MINU, MAXU, MINV, MAXV in texels
uniform vec2 uTexSize;

// In the GS's integer units, as the mesh shader tests.
bool isAlphaTestFailing(float normalised) {
    float a = floor(min(normalised * 128.0, 255.0) + 0.5);
    return uAtest == 0
        || (uAtest == 2 && a >= uAtestRef)
        || (uAtest == 3 && a >  uAtestRef)
        || (uAtest == 4 && a != uAtestRef)
        || (uAtest == 5 && a <  uAtestRef)
        || (uAtest == 6 && a <= uAtestRef)
        || (uAtest == 7 && a == uAtestRef);
}

// Half a texel in from each bound keeps a bilinear tap inside the region, as the GS clamps each.
vec2 clampToRegion(vec2 uv) {
    vec2 texel = uv * uTexSize;
    if (uRegionClamp.x != 0) texel.x = clamp(texel.x, uRegion.x + 0.5, uRegion.y + 0.5);
    if (uRegionClamp.y != 0) texel.y = clamp(texel.y, uRegion.z + 0.5, uRegion.w + 0.5);
    return texel / uTexSize;
}

// The GS's texture functions, as PCSX2's tfx_fs.glsl has them. The vertex color is Cf / 255 with
// Af / 128, the texel Ct / 255 with At / 255, so modulating doubles.
void main() {
    oColor = vColor;
    if (uTextured != 0) {
        vec4 t = texture(uTexture, clampToRegion(vUv));
        if (uTexelAlpha >= 0.0) t.a = uTexelAlpha;
        vec3 modulated = t.rgb * vColor.rgb * 2.0;
        float vertexA = vColor.a * 128.0 / 255.0;
        float textureA = t.a * 2.0;
        oColor = uTexFunction == 0 ? vec4(modulated, t.a * vColor.a * 2.0)
               : uTexFunction == 1 ? vec4(t.rgb, textureA)
               : uTexFunction == 2 ? vec4(modulated + vertexA, textureA + vColor.a)
                                   : vec4(modulated + vertexA, textureA);
        if (uTexAlpha == 0) oColor.a = vColor.a;
    }
    if (uFogOn != 0) oColor.rgb = mix(uFogColor, oColor.rgb, vFog * 255.0 / 256.0);
    if (isAlphaTestFailing(oColor.a) != (uAtestInvert != 0)) discard;
}
)";

// A triangle over the whole target, marking in the stencil where alpha bit 7 is what DATM asks.
constexpr const char* kDateVertexShader = R"(#version 330 core
void main() {
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kDateFragmentShader = R"(#version 330 core
uniform sampler2D uColor;
uniform int uDatm;
out vec4 oColor;
void main() {
    // Alpha is stored as a / 128, so bit 7 set is the stored maximum.
    bool bitSet = texelFetch(uColor, ivec2(gl_FragCoord.xy), 0).a > 254.0 / 255.0;
    if (bitSet != (uDatm != 0)) discard;
    oColor = vec4(0.0);
}
)";

// No sample scene blends more than four, so this is margin.
constexpr int kMaxPaletteBones = 16;

constexpr const char* kMeshVertexShader = R"(
layout(location = 0) in vec4 aPos0;
layout(location = 1) in vec4 aNrm0;
layout(location = 2) in vec4 aPos1;
layout(location = 3) in vec4 aNrm1;
layout(location = 4) in vec4 aPos2;
layout(location = 5) in vec4 aNrm2;
layout(location = 6) in vec4 aPos3;
layout(location = 7) in vec4 aNrm3;
layout(location = 8) in vec4 aUv;
layout(location = 9) in vec4 aColor;

uniform mat4 uScreen;
uniform vec2 uDepthMap;  // screenScale.z and screenOffset.z; a zero scale leaves clip z as it is
uniform mat4 uBones[16];
uniform vec3 uLightDir[3];
uniform vec3 uLightColor[3];
uniform vec3 uLightBase;
uniform vec3 uLightAmbient;
uniform vec3 uLightFull;
uniform vec3 uSpecularAxis;
uniform vec3 uSpecular;
uniform vec3 uDrawColor;
uniform vec3 uAmbient;
uniform vec3 uDiffuse;
uniform int uLit;
uniform float uFlatten;
uniform int uFlipNormal;
uniform float uFaceDivSoften;
uniform float uFaceDivBias;
uniform int uDebugNormal;
uniform vec3 uSolid;      // a picked mesh draws in this, ignoring texture and lighting
uniform int uSolidOn;
uniform int uStreamColors;
uniform int uUnlitColors;
uniform int uRefMapPass;     // the reflection pass samples a ramp, so it needs its own UV
uniform int uOverlayPass;    // sdf3DSpecularTex: the texture alone, unlit
uniform vec2 uProjScale;     // the projection's x and y, so a normal can leave clip space
uniform float uRefMapRow;
uniform int uMtexPass;       // 0 first pass, 1 the ramp, 2 a constant color, 3 the first's colors
uniform vec3 uMtexDiffuse;
uniform float uMtexConstant;
uniform vec3 uMtexRampLo;     // the endpoints corePktMakeFaceDivParm wrote, when they were captured
uniform vec3 uMtexRampDelta;
uniform int uMtexRampKnown;
uniform float uMtexSoften;
uniform float uMtexBias;

uniform vec4 uFog;   // scale, minimum, maximum, offset
uniform int uFogOn;
uniform vec2 uPixelOffset;

out vec2 vUv;
out vec2 vUv2;
out vec3 vNormal;
out vec4 vColor;
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

    // emitTexTriangles 0x02BD puts screenOffset.z + screenScale.z * clip.z / clip.w on the GS,
    // and the depth buffer holds GS z as 1 - z / 2^24, which the decoded draws write outright.
    // Unclamped, so GL clips past the near plane as VU1 culls there.
    const float kDepthUnit = 2.0 / 16777216.0;
    float depthZ = uDepthMap.x != 0.0
        ? clip.w - kDepthUnit * (uDepthMap.y * clip.w + uDepthMap.x * clip.z)
        : -clip.z;
    gl_Position = vec4(clip.x + uPixelOffset.x * clip.w, -clip.y + uPixelOffset.y * clip.w,
                       depthZ, clip.w);

    // VU1's vertex-color variant at 0x0173 is not modelled. Dropping the light term outright
    // overshoots, so the flat term stands until that routine is transcribed.
    if (uFlipNormal != 0) facing = -facing;
    vLit = sdfMeshLit(vec3(0.0), false, uAmbient, uDiffuse, uDrawColor, uLightAmbient, uLightBase,
                      uLightFull, uLightDir[0], uLightDir[1], uLightDir[2], uLightColor[1],
                      uLightColor[2], uFlatten, uFaceDivSoften, uFaceDivBias,
                      uStreamColors != 0);

    // corePktApplyFaceDivColor gives the second pass its own vertex color, a half Lambert between
    // two endpoints; corePktSetConstColor is the flat case. Reusing the first pass's leaves it
    // smooth.
    if (uMtexPass == 1) {
        // The ramp is a step the engine subdivides faces to resolve, so the fragment stage owns it.
        vLit = vec3(0.0);
    } else if (uMtexPass == 2) {
        // corePktSetConstColor is the product of two colors over 128, not the material's alone.
        vLit = uMtexDiffuse * uDrawColor;
    }
    if (uUnlitColors != 0) vLit = vec3(1.0);
    if (uDebugNormal != 0) vLit = normalize(facing) * 0.5 + 0.5;
    if (uSolidOn != 0) vLit = uSolid;

    // transformVertices 0x04DC..0x04E3: offset + scale / w, clamped. The offset is fogParm[2],
    // not the minimum.
    vFog = uFogOn != 0 ? clamp(uFog.w + uFog.x / max(clip.w, 1e-3), uFog.y, uFog.z) : 1.0;

    vUv = aUv.xy;
    vUv2 = aUv.zw;
    // u is the dot the pass stores to the texcoord slot, clamped as CLAMP 5 asks; v picks the row.
    if (uRefMapPass != 0) {
        vUv = vec2(clamp(dot(normalize(facing), uSpecularAxis), 0.0, 1.0), uRefMapRow);
        vLit = uSpecular;
    }
    // The overlay's ST is normal-derived, the sphere map reflectPass builds at 0x05DF..0x05E6
    // as 0.5 + 0.5 * (M * normal).
    if (uOverlayPass != 0) {
        vec3 clipNormal = mat3(uScreen) * normalize(facing);
        vec3 viewNormal = normalize(vec3(clipNormal.x / uProjScale.x,
                                         clipNormal.y / uProjScale.y, clipNormal.z));
        vUv = viewNormal.xy * 0.5 + 0.5;
    }
    vNormal = uFlipNormal != 0 ? -facing : facing;
    vColor = aColor;
}
)";

constexpr const char* kMeshFragmentShader = R"(
in vec2 vUv;
in vec2 vUv2;
in vec3 vNormal;
in vec4 vColor;
in vec3 vLit;
in float vFog;
out vec4 oColor;
uniform sampler2D uTexture;
uniform int uTextured;
uniform int uDebugDepth;
uniform vec3 uFogColor;
uniform int uAtest;       // GS ATST
uniform float uAtestRef;  // AREF, in the GS's own 0..255 units
uniform int uAtestInvert;
uniform int uTexFunction; // GS TEX0 TFX
uniform int uTexAlpha;    // GS TEX0 TCC
uniform float uDiffuseAlpha;  // the material's own alpha, which VU1 writes as the vertex alpha
uniform int uBillboard;
uniform int uSecondUv;
uniform vec4 uTexMatrix;
uniform vec2 uTexOffset;
uniform vec3 uLightDir[3];
uniform vec3 uLightColor[3];
uniform vec3 uLightBase;
uniform vec3 uLightAmbient;
uniform vec3 uLightFull;
uniform vec3 uDrawColor;
uniform vec3 uAmbient;
uniform vec3 uDiffuse;
uniform int uLit;
uniform float uFlatten;
uniform int uFlipNormal;
uniform int uStreamColors;
uniform float uFaceDivSoften;
uniform float uFaceDivBias;
uniform vec3 uSolid;
uniform int uSolidOn;
uniform int uMtexPass;
uniform vec3 uMtexDiffuse;
uniform vec3 uMtexRampLo;
uniform vec3 uMtexRampDelta;
uniform int uMtexRampKnown;
uniform float uMtexConstant;
uniform float uMtexSoften;
uniform float uMtexBias;
uniform int uRefMapPass;
uniform int uOverlayPass;

const float kPs2Alpha = 255.0 / 128.0;


// Quantised as the GS does: AREF 128 against a normalised 1.0 is a knife-edge in float, and
// fragments either side of it write depth at random.
bool isAlphaTestFailing(float normalised) {
    float a = floor(min(normalised * 128.0, 255.0) + 0.5);
    return uAtest == 0                       // NEVER; 1 is ALWAYS and never fails
        || (uAtest == 2 && a >= uAtestRef)   // LESS
        || (uAtest == 3 && a >  uAtestRef)   // LEQUAL
        || (uAtest == 4 && a != uAtestRef)   // EQUAL
        || (uAtest == 5 && a <  uAtestRef)   // GEQUAL
        || (uAtest == 6 && a <= uAtestRef)   // GREATER
        || (uAtest == 7 && a == uAtestRef);  // NOTEQUAL
}

void main() {
    // Face division subdivides before applying the step, so the hardware samples it far more
    // densely than the original vertices; interpolating facets the mesh.
    if (uSolidOn != 0) {
        oColor = vec4(uSolid, 1.0);
        return;
    }
    vec3 lit = vLit;
    if (uMtexPass == 1) {
        lit = sdfMeshRamp(normalize(vNormal), uMtexRampLo, uMtexRampDelta, uMtexRampKnown != 0,
                          uMtexDiffuse, uLightBase, uAmbient, uLightAmbient, uDrawColor,
                          uLightDir[0], uMtexConstant, uMtexSoften, uMtexBias);
    } else if (uLit != 0 && (uMtexPass == 0 || uMtexPass == 3) && uRefMapPass == 0) {
        lit = sdfMeshLit(normalize(vNormal), true, uAmbient, uDiffuse, uDrawColor, uLightAmbient,
                         uLightBase, uLightFull, uLightDir[0], uLightDir[1], uLightDir[2],
                         uLightColor[1], uLightColor[2], uFlatten, uFaceDivSoften, uFaceDivBias,
                      uStreamColors != 0);
    }
    // 0x0174 and 0x019F scale by 1/128 against a colour ITOF0 leaves in 0..255, so the scale is
    // 255/128, not 2.
    const float kVertexColorScale = 255.0 / 128.0;
    vec4 color = uOverlayPass != 0 ? vec4(1.0, 1.0, 1.0, 1.0)
                                   : vec4(vColor.rgb * kVertexColorScale * lit, vColor.a);
    vec2 raw = uSecondUv != 0 ? vUv2 : vUv;
    // These passes carry a coordinate the vertex stage built, so the texture matrix must not
    // apply on top of it.
    vec2 uv = (uRefMapPass != 0 || uOverlayPass != 0)
                  ? vUv
                  : vec2(dot(raw, uTexMatrix.xy), dot(raw, uTexMatrix.zw)) + uTexOffset;
    vec4 texel = uTextured != 0 ? texture(uTexture, uv) : vec4(1.0);
    float texA = uTexAlpha != 0 ? min(texel.a * kPs2Alpha, 1.0) : 1.0;
    // The mesh path's kPs2Alpha looks like a double application, but removing it costs underground
    // 4.272 to 22.681, so something upstream is not in the units it appears to be. See TODO V8.
    // VU1's sprite path unpacks no material, so a billboard's alpha is the batch's own.
    float testA = uBillboard != 0 ? min(texA * color.a * kPs2Alpha, 1.0)
                                  : min(texA * color.a * kPs2Alpha * uDiffuseAlpha, 1.0);
    // AFAIL FB_ONLY is two GL draws; uAtestInvert selects the half.
    // HIGHLIGHT and HIGHLIGHT2 add the vertex alpha to the color; HIGHLIGHT adds it to the
    // texture's alpha too.
    float vertexA = color.a * kPs2Alpha;
    if (uTextured != 0 && uTexFunction >= 2) {
        testA = uTexAlpha == 0 ? min(vertexA, 1.0)
              : uTexFunction == 2 ? min(texA + vertexA, 1.0)
                                  : texA;
    }
    if (isAlphaTestFailing(testA) != (uAtestInvert != 0)) discard;

    vec3 rgb = uTexFunction == 0 ? texel.rgb * color.rgb             // MODULATE
             : uTexFunction == 1 ? texel.rgb                          // DECAL
                                 : texel.rgb * color.rgb + color.a;   // HIGHLIGHT, HIGHLIGHT2
    // Untextured, the GS writes the vertex color as it is; only MODULATE scales it by 1/128.
    if (uTextured == 0) rgb = color.rgb / kVertexColorScale;
    oColor = vec4(rgb, testA);
    oColor.rgb = mix(uFogColor, oColor.rgb, vFog);
    if (uDebugDepth != 0) oColor = vec4(vec3(gl_FragCoord.z), 1.0);
}
)";

/// A color in the GS's units, as a uniform's three floats.
struct GsRgb {
    f32 v[3];
};

GsRgb convertToGsUnits(Rgba c) { return {{c.r / sdfGsUnit, c.g / sdfGsUnit, c.b / sdfGsUnit}}; }

/// The GS ALPHA register, Cv = (A - B) * C + D, as a GL blend.
struct GsBlend {
    GLenum equation = GL_FUNC_ADD;
    GLenum src = GL_ONE;
    GLenum dst = GL_ZERO;

    /// FIX in GL's constant alpha, for a C that names it.
    f32 fix = 0.0f;
};

/// The GL depth comparison for a GS TEST register.
GLenum getDepthFunc(u64 test) {
    if (!sdfGetTestZte(test)) return GL_ALWAYS;
    switch (sdfGetTestZtst(test)) {
        case sdfZtstNever: return GL_NEVER;
        case sdfZtstAlways: return GL_ALWAYS;
        case sdfZtstGequal: return GL_LEQUAL;
        case sdfZtstGreater: return GL_LESS;
        default: return GL_LESS;
    }
}

/// One color's weight in the blend, k * C + m.
struct BlendWeight {
    int k = 0;
    int m = 0;
};

BlendWeight getBlendWeight(u32 a, u32 b, u32 d, u32 color) {
    return {int(a == color) - int(b == color), int(d == color)};
}

/// @return Whether GL has a factor for the weight; 1 + C it has not.
bool getBlendFactor(BlendWeight weight, GLenum coef, GLenum inverse, GLenum* factor,
                    bool* negative) {
    *negative = weight.k < 0 && weight.m == 0;
    if (weight.k == 0) *factor = weight.m ? GL_ONE : GL_ZERO;
    else if (weight.m == 0) *factor = coef;
    else if (weight.k < 0) *factor = inverse;
    else return false;
    return true;
}

GsBlend convertAlphaToBlend(u64 reg) {
    u32 a = sdfGetAlphaA(reg);
    u32 b = sdfGetAlphaB(reg);
    u32 c = sdfGetAlphaC(reg);
    u32 d = sdfGetAlphaD(reg);
    // Reserved encodings, which the GS manual leaves undefined.
    if (a == 3 || b == 3 || c == 3 || d == 3) {
        reportUnsupported("GS ALPHA reserved term", u32(reg & 0xFF));
        a = a == 3 ? sdfAlphaZero : a;
        b = b == 3 ? sdfAlphaZero : b;
        c = c == 3 ? sdfAlphaFixed : c;
        d = d == 3 ? sdfAlphaZero : d;
    }
    const GLenum coef = c == sdfAlphaAs   ? GL_SRC_ALPHA
                        : c == sdfAlphaAd ? GL_DST_ALPHA
                                          : GL_CONSTANT_ALPHA;
    const GLenum inverse = c == sdfAlphaAs   ? GL_ONE_MINUS_SRC_ALPHA
                           : c == sdfAlphaAd ? GL_ONE_MINUS_DST_ALPHA
                                             : GL_ONE_MINUS_CONSTANT_ALPHA;

    GsBlend blend;
    blend.fix = f32(sdfGetAlphaFix(reg)) / sdfGsUnit;
    if (c == sdfAlphaFixed && sdfGetAlphaFix(reg) > 0x80) {
        reportUnsupported("GS ALPHA FIX above 1.0", sdfGetAlphaFix(reg));
    }
    bool srcNegative = false;
    bool dstNegative = false;
    if (!getBlendFactor(getBlendWeight(a, b, d, sdfAlphaSource), coef, inverse, &blend.src,
                        &srcNegative) ||
        !getBlendFactor(getBlendWeight(a, b, d, sdfAlphaDest), coef, inverse, &blend.dst,
                        &dstNegative)) {
        reportUnsupported("GS ALPHA with a weight above 1", u32(reg & 0xFF));
        return GsBlend{};
    }
    if (srcNegative && dstNegative) {
        // Both terms subtract, so the sum clamps to zero.
        blend.src = GL_ZERO;
        blend.dst = GL_ZERO;
    } else if (srcNegative) {
        blend.equation = GL_FUNC_REVERSE_SUBTRACT;
    } else if (dstNegative) {
        blend.equation = GL_FUNC_SUBTRACT;
    }
    return blend;
}

/// The GS blends color only and writes the source alpha as it is.
void applyBlend(const GsBlend& blend) {
    glBlendEquationSeparate(blend.equation, GL_FUNC_ADD);
    glBlendFuncSeparate(blend.src, blend.dst, GL_ONE, GL_ZERO);
    glBlendColor(0.0f, 0.0f, 0.0f, blend.fix);
}

/// A decoded sprite samples as its TEX1 and CLAMP say; a hooked one as the texture was made. Set
/// every draw, since a mesh sampling the same texture leaves its own.
void applySpriteSampler(const DrawSprite& sprite, bool isFeedback) {
    const bool magLinear = !sprite.hasSampler || sdfGetTex1Mmag(sprite.tex1);
    const bool minLinear = !sprite.hasSampler || sdfGetTex1Mmin(sprite.tex1);
    const GLint edge = isFeedback ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    const auto getWrap = [&](u32 mode) {
        if (!sprite.hasSampler) return edge;
        if (mode == sdfWrapRegionRepeat) reportUnsupported("decoded sprite region repeat", mode);
        return mode == sdfWrapRepeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    };
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magLinear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minLinear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, getWrap(sdfGetClampWms(sprite.clamp)));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, getWrap(sdfGetClampWmt(sprite.clamp)));
}

/// FRAME's write mask, with the alpha channel dropped as well when only RGB may be written.
void applyFrameMask(u64 mask, bool rgbOnly) {
    glColorMask((mask & 0x0000FF) != 0x0000FF, (mask & 0x00FF00) != 0x00FF00,
                (mask & 0xFF0000) != 0xFF0000, !rgbOnly && (mask & 0xFF000000u) != 0xFF000000u);
}

/// The passing half writes color and, unless ZBUF masks it, depth; the failing half whatever
/// AFAIL lets through. Back to back, so the blend order stays the engine's.
void drawSpriteHalves(const DrawSprite& sprite, bool alphaTests, int atestInvert, GLsizei count) {
    const bool depthWrites = sprite.hasDepth && !((sprite.zbuf >> 32) & 1);
    glUniform1i(atestInvert, 0);
    applyFrameMask(sprite.frameMask, false);
    glDepthMask(depthWrites ? GL_TRUE : GL_FALSE);
    glDrawArrays(GL_TRIANGLES, 0, count);
    if (!alphaTests) return;

    const u32 afail = sdfGetTestAfail(sprite.test);
    if (afail == sdfAfailKeep) return;
    glUniform1i(atestInvert, 1);
    if (afail == sdfAfailZbOnly) {
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    } else {
        applyFrameMask(sprite.frameMask, afail == sdfAfailRgbOnly);
    }
    glDepthMask(afail == sdfAfailZbOnly && depthWrites ? GL_TRUE : GL_FALSE);
    glDrawArrays(GL_TRIANGLES, 0, count);
}

/// PRIM without ABE: the GS writes the source and reads nothing back.
void applySpriteBlend(const DrawSprite& sprite) {
    applyBlend(sprite.blends ? convertAlphaToBlend(sprite.alpha) : GsBlend{});
}

/// PCSX2HOOK_GS_ONLY's own spec, so a frame cuts to the same draws on both sides.
bool isDrawWanted(const std::string& spec, usize index, u32 tbp) {
    if (spec.empty()) return true;
    usize at = 0;
    while (at < spec.size()) {
        const usize end = std::min(spec.find(',', at), spec.size());
        const std::string term = spec.substr(at, end - at);
        at = end + 1;
        if (term.rfind("tbp=", 0) == 0) {
            if (u32(std::strtoul(term.c_str() + 4, nullptr, 0)) == tbp) return true;
        } else if (term.rfind("i=", 0) == 0) {
            const char* p = term.c_str() + 2;
            char* stop = nullptr;
            const usize lo = usize(std::strtoul(p, &stop, 0));
            const usize hi = (stop && *stop == '-') ? usize(std::strtoul(stop + 1, nullptr, 0)) : lo;
            if (index >= lo && index <= hi) return true;
        }
    }
    return false;
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

/// Compiled with render/shading.inc above them, the same text shading_cpu.h compiles, so the GPU
/// and any CPU check run one implementation.
u32 compileMesh(GLenum type, const char* source) {
    const char* parts[3] = {"#version 330 core\n", kShadingSource, source};
    const u32 shader = glCreateShader(type);
    glShaderSource(shader, 3, parts, nullptr);
    glCompileShader(shader);
    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char info[512];
        glGetShaderInfoLog(shader, sizeof(info), nullptr, info);
        log.info("mesh shader: {}", info);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
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
    const u32 meshVs = compileMesh(GL_VERTEX_SHADER, kMeshVertexShader);
    const u32 meshFs = compileMesh(GL_FRAGMENT_SHADER, kMeshFragmentShader);
    if (!vs || !fs || !meshVs || !meshFs) return false;
    mProgram = link(vs, fs);
    const u32 dateVs = compile(GL_VERTEX_SHADER, kDateVertexShader);
    const u32 dateFs = compile(GL_FRAGMENT_SHADER, kDateFragmentShader);
    if (!dateVs || !dateFs) return false;
    mDateProgram = link(dateVs, dateFs);
    glGenVertexArrays(1, &mEmptyVao);
    mMeshProgram = link(meshVs, meshFs);

    glGenVertexArrays(1, &mVao);
    glGenBuffers(1, &mVbo);
    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    constexpr int kStride = 10 * sizeof(f32);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, nullptr);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride, (void*)(2 * sizeof(f32)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, kStride, (void*)(4 * sizeof(f32)));
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, kStride, (void*)(8 * sizeof(f32)));
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, kStride, (void*)(9 * sizeof(f32)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    glEnableVertexAttribArray(4);

    glGenVertexArrays(1, &mMeshVao);
    glGenBuffers(1, &mMeshVbo);
    glGenBuffers(1, &mMeshEbo);
    glBindVertexArray(mMeshVao);
    glBindBuffer(GL_ARRAY_BUFFER, mMeshVbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mMeshEbo);
    constexpr int kMeshStride = sizeof(MeshVertex);
    // Per influence: the position with its weight, then the normal with the palette index.
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
                          (void*)offsetof(MeshVertex, color));
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
    glDeleteProgram(mDateProgram);
    glDeleteVertexArrays(1, &mEmptyVao);
    glDeleteBuffers(1, &mMeshVbo);
    glDeleteBuffers(1, &mMeshEbo);
    glDeleteVertexArrays(1, &mMeshVao);
    glDeleteProgram(mMeshProgram);
}

const Renderer::Texture* Renderer::findTexture(TexHandle handle) const {
    const auto found = mTextures.find(handle);
    return found == mTextures.end() ? nullptr : &found->second;
}

void Renderer::getScreenRect(int* x, int* y, int* width, int* height) const {
    const RenderTexture& target = mFrames[mCurrent];
    const int screenWidth = mScreenWidth > 0 ? mScreenWidth : target.getWidth();
    const int screenHeight = mScreenHeight > 0 ? mScreenHeight : target.getHeight();
    const int scaled = std::min(screenWidth * target.getHeight(), screenHeight * target.getWidth());
    *width = scaled / target.getHeight();
    *height = scaled / target.getWidth();
    *x = (screenWidth - *width) / 2;
    *y = (screenHeight - *height) / 2;
}

void Renderer::markDestinationAlpha(u32 datm) {
    const RenderTexture& target = mFrames[mCurrent];
    const u32 color = target.copyColor();
    target.bind();
    glUseProgram(mDateProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, color);
    glUniform1i(glGetUniformLocation(mDateProgram, "uColor"), 0);
    glUniform1i(glGetUniformLocation(mDateProgram, "uDatm"), int(datm));
    glBindVertexArray(mEmptyVao);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glUseProgram(mProgram);
    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    glEnable(GL_BLEND);
}

void Renderer::fitTargetToScreen() {
    const int displayHeight = int(kGuestHeight) * 2;
    const int width = std::min(mScreenWidth, mScreenHeight * int(kGuestWidth) / displayHeight);
    const int height = width * displayHeight / int(kGuestWidth);
    // A minimised window reports next to nothing, and the picture should survive it.
    if (width < int(kGuestWidth) / 4 || height < displayHeight / 4) return;
    for (RenderTexture& target : mFrames) {
        if (!target.resize(width, height)) reportUnsupported("render target size", u32(width));
    }
}

void Renderer::mapToTarget(int x, int y, int* outX, int* outY) const {
    int boxX = 0, boxY = 0, boxWidth = 0, boxHeight = 0;
    getScreenRect(&boxX, &boxY, &boxWidth, &boxHeight);
    const RenderTexture& target = mFrames[mCurrent];
    // The blit flips vertically, so a window row counts from the bottom of the box.
    *outX = boxWidth ? (x - boxX) * target.getWidth() / boxWidth : x;
    *outY = boxHeight ? (y - boxY) * target.getHeight() / boxHeight : y;
}

void Renderer::present() {
    int x = 0, y = 0, width = 0, height = 0;
    getScreenRect(&x, &y, &width, &height);
    mFrames[mCurrent].blitToScreen(x, y, width, height);
    mCurrent ^= 1;
    mPresented++;
}

void Renderer::represent() {
    int x = 0, y = 0, width = 0, height = 0;
    getScreenRect(&x, &y, &width, &height);
    mFrames[mCurrent ^ 1].blitToScreen(x, y, width, height);
}

void Renderer::uploadTextures(const Frame& frame) {
    for (const TextureUpload& upload : frame.textureUploads) {
        // A reused guest address. The old texture goes only here, with the replacement in hand
        // and no draw in flight against it.
        const auto stale = mTextures.find(upload.handle);
        if (stale != mTextures.end()) {
            glDeleteTextures(1, &stale->second.name);
            mTextures.erase(stale);
        }
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

int Renderer::pickMesh(const Frame& frame, int x, int y) {
    RenderTexture& target = mFrames[mCurrent];
    target.bind();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    mRun.clear();
    for (const DrawRef ref : frame.order) {
        if (ref.kind == DrawRef::Kind::Mesh) mRun.push_back(ref.index);
    }
    mPickPass = true;
    drawMeshes(frame, mRun);
    mPickPass = false;

    const int w = int(target.getWidth());
    const int h = int(target.getHeight());
    if (x < 0 || y < 0 || x >= w || y >= h) return -1;
    // GL reads from the bottom left; the picker is given a top-left pixel.
    u8 rgba[4] = {};
    glReadPixels(x, h - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    const u32 id = u32(rgba[0]) | (u32(rgba[1]) << 8) | (u32(rgba[2]) << 16);
    return id ? int(id - 1) : -1;
}

void Renderer::draw(const Frame& frame) {
    mFrames[mCurrent].bind();
    uploadTextures(frame);

    const Rgba& back = frame.background;
    glClearColor(back.r / 255.0f, back.g / 255.0f, back.b / 255.0f, 1.0f);
    glClearDepth(1.0);
    glDepthFunc(GL_LESS);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    describeSprites(frame);

    // The chain's order is the GS's, so 2D and 3D interleave the way it has them.
    mRun.clear();
    for (usize i = 0; i < frame.order.size(); i++) {
        const DrawRef ref = frame.order[i];
        if (ref.kind == DrawRef::Kind::SaveDepth) {
            mFrames[mCurrent].saveDepth();
            continue;
        }
        if (ref.kind == DrawRef::Kind::RestoreDepth) {
            mFrames[mCurrent].restoreDepth();
            continue;
        }
        mRun.push_back(ref.index);
        if (i + 1 < frame.order.size() && frame.order[i + 1].kind == ref.kind) continue;
        if (ref.kind == DrawRef::Kind::Mesh) drawMeshes(frame, mRun);
        else drawSprites(frame, mRun);
        mRun.clear();
    }
}

void Renderer::drawMeshes(const Frame& frame, const std::vector<u32>& indices) {
    if (indices.empty()) return;

    // Bisecting a bad mesh costs an emulator run per step, so the range is a run-time choice.
    const usize firstMesh = mConfig->meshFirst;
    const usize lastMesh = std::min(mConfig->meshLast, frame.meshes.size());

    glUseProgram(mMeshProgram);
    glBindVertexArray(mMeshVao);
    glBindBuffer(GL_ARRAY_BUFFER, mMeshVbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mMeshEbo);
    glEnable(GL_DEPTH_TEST);
    // corePktBuildFaceList drops a triangle whose signed area disagrees with the draw's sign.
    if (mConfig->cull == 'n') glDisable(GL_CULL_FACE);
    else glEnable(GL_CULL_FACE);
    // Always the back: 'f' inverts the winding instead, since flipping both cancels out.
    glCullFace(GL_BACK);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Row-vector, which is what GL reads a row-major upload as untransposed, so uScreen * uBone *
    // point composes in the engine's order.
    glUniformMatrix4fv(glGetUniformLocation(mMeshProgram, "uScreen"), 1, GL_FALSE,
                       &frame.screenTm.m[0][0]);
    glUniform2f(glGetUniformLocation(mMeshProgram, "uDepthMap"), frame.depthScale, frame.depthOffset);
    const int bones = glGetUniformLocation(mMeshProgram, "uBones");
    const int textured = glGetUniformLocation(mMeshProgram, "uTextured");
    const int lightDir = glGetUniformLocation(mMeshProgram, "uLightDir");
    const int lightColor = glGetUniformLocation(mMeshProgram, "uLightColor");
    const int lightBase = glGetUniformLocation(mMeshProgram, "uLightBase");
    const int lightAmbient = glGetUniformLocation(mMeshProgram, "uLightAmbient");
    const int lightFull = glGetUniformLocation(mMeshProgram, "uLightFull");
    const int drawColor = glGetUniformLocation(mMeshProgram, "uDrawColor");
    const int lit = glGetUniformLocation(mMeshProgram, "uLit");
    const int flatten = glGetUniformLocation(mMeshProgram, "uFlatten");
    const int flipNormal = glGetUniformLocation(mMeshProgram, "uFlipNormal");
    const int streamColors = glGetUniformLocation(mMeshProgram, "uStreamColors");
    const int unlitColors = glGetUniformLocation(mMeshProgram, "uUnlitColors");
    const int diffuseAlpha = glGetUniformLocation(mMeshProgram, "uDiffuseAlpha");
    const int refMapPass = glGetUniformLocation(mMeshProgram, "uRefMapPass");
    const int overlayPass = glGetUniformLocation(mMeshProgram, "uOverlayPass");
    const int projScale = glGetUniformLocation(mMeshProgram, "uProjScale");
    const int refMapRow = glGetUniformLocation(mMeshProgram, "uRefMapRow");
    const int mtexPass = glGetUniformLocation(mMeshProgram, "uMtexPass");
    const int mtexDiffuse = glGetUniformLocation(mMeshProgram, "uMtexDiffuse");
    const int mtexConstant = glGetUniformLocation(mMeshProgram, "uMtexConstant");
    const int mtexRampLo = glGetUniformLocation(mMeshProgram, "uMtexRampLo");
    const int mtexRampDelta = glGetUniformLocation(mMeshProgram, "uMtexRampDelta");
    const int mtexRampKnown = glGetUniformLocation(mMeshProgram, "uMtexRampKnown");
    const int mtexSoften = glGetUniformLocation(mMeshProgram, "uMtexSoften");
    const int mtexBias = glGetUniformLocation(mMeshProgram, "uMtexBias");
    const int ambient = glGetUniformLocation(mMeshProgram, "uAmbient");
    const int diffuse = glGetUniformLocation(mMeshProgram, "uDiffuse");
    const int specular = glGetUniformLocation(mMeshProgram, "uSpecular");
    const int specularAxis = glGetUniformLocation(mMeshProgram, "uSpecularAxis");
    const int faceDivSoften = glGetUniformLocation(mMeshProgram, "uFaceDivSoften");
    const int faceDivBias = glGetUniformLocation(mMeshProgram, "uFaceDivBias");
    const int fog = glGetUniformLocation(mMeshProgram, "uFog");
    const int fogOn = glGetUniformLocation(mMeshProgram, "uFogOn");
    const int fogColor = glGetUniformLocation(mMeshProgram, "uFogColor");

    {
        // The shift in NDC, from an offset given in rendered pixels.
        const RenderTexture& target = mFrames[mCurrent];
        const f32 w = f32(target.getWidth() ? target.getWidth() : 1);
        const f32 h = f32(target.getHeight() ? target.getHeight() : 1);
        glUniform2f(glGetUniformLocation(mMeshProgram, "uPixelOffset"),
                    mConfig->pixelOffsetX * 2.0f / w, -mConfig->pixelOffsetY * 2.0f / h);
    }
    glUniform1i(glGetUniformLocation(mMeshProgram, "uDebugDepth"), mConfig->debugDepth);

    mMeshes.clear();
    for (const u32 index : indices) {
        if (index < firstMesh || index > lastMesh) continue;
        if (!isDrawWanted(mConfig->drawOnly, index, sdfDecodeTex0(frame.meshes[index].tex0).tbp)) {
            continue;
        }
        if (frame.meshes[index].otIndex <= mConfig->otMax) mMeshes.push_back(index);
    }

    const int billboard = glGetUniformLocation(mMeshProgram, "uBillboard");
    const int atest = glGetUniformLocation(mMeshProgram, "uAtest");
    const int atestRef = glGetUniformLocation(mMeshProgram, "uAtestRef");
    glUniform1i(glGetUniformLocation(mMeshProgram, "uDebugNormal"), mConfig->debugNormal ? 1 : 0);
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
        const u32 function = texture ? sdfGetTex0Tfx(tex0) : u32(sdfTexModulate);
        glUniform1i(functionLoc, int(function));
        glUniform1i(alphaLoc, texture && sdfGetTex0Tcc(tex0) ? 1 : 0);
        if (!texture) return;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture->name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        sdfGetTex1Mmag(tex1) ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        sdfGetTex1Mmin(tex1) ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                        sdfGetClampWms(clamp) == sdfWrapRepeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                        sdfGetClampWmt(clamp) == sdfWrapRepeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    };

    // The passing half writes everything, the failing half whatever AFAIL still lets through.
    // Back to back, so the blend order stays the engine's.
    const auto drawMesh = [&](const DrawMesh& mesh) {
        const GLsizei count = GLsizei(mesh.indices.size());
        glUniform1i(atestInvert, 0);
        glDepthMask(GL_TRUE);
        glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, nullptr);

        if (!sdfGetTestAte(mesh.gs.test)) return;
        const u32 afail = sdfGetTestAfail(mesh.gs.test);
        if (afail == sdfAfailKeep) return;

        // ZB_ONLY writes depth and no color, the reverse of the other two.
        const bool depth = afail == sdfAfailZbOnly;
        glDepthMask(depth ? GL_TRUE : GL_FALSE);
        if (depth) glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        if (afail == sdfAfailRgbOnly) glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);

        glUniform1i(atestInvert, 1);
        glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, nullptr);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    };

    const int solid = glGetUniformLocation(mMeshProgram, "uSolid");
    const int solidOn = glGetUniformLocation(mMeshProgram, "uSolidOn");

    for (const u32 index : mMeshes) {
        const DrawMesh& mesh = frame.meshes[index];
        // A pick pass paints each mesh with its index so the pixel under the cursor names it.
        if (mPickPass) {
            const u32 id = u32(index) + 1;
            glUniform3f(solid, f32(id & 0xFF) / 255.0f, f32((id >> 8) & 0xFF) / 255.0f,
                        f32((id >> 16) & 0xFF) / 255.0f);
            glUniform1i(solidOn, 1);
        } else {
            glUniform1i(solidOn, int(index) == mHighlight ? 1 : 0);
            glUniform3f(solid, 1.0f, 0.0f, 1.0f);
        }
        // Every texture a frame names is uploaded before it is drawn, so one missing never will be.
        if (!mPickPass && mesh.texHandle.addr && (mesh.attrBits & sdfCoreAttrTextured) &&
            !findTexture(mesh.texHandle)) {
            reportUnsupported("mesh texture never uploaded", mesh.texHandle.addr);
            glUniform1i(solidOn, 1);
        }
        if (mesh.bones.size() > kMaxPaletteBones) {
            reportUnsupported("bones in one mesh", u32(mesh.bones.size()));
            continue;
        }
        glUniformMatrix4fv(bones, GLsizei(mesh.bones.size()), GL_FALSE, &mesh.bones[0].m[0][0]);
        glUniform3fv(lightDir, 3, &mesh.lighting.direction[0][0]);
        glUniform3fv(lightColor, 3, &mesh.lighting.color[0][0]);
        glUniform3fv(lightBase, 1, mesh.lighting.base);
        glUniform3fv(lightAmbient, 1, mesh.lighting.ambientColor);
        glUniform3fv(lightFull, 1, mesh.lighting.fullColor);
        glUniform1i(lit, mesh.lit ? 1 : 0);
        glUniform1f(flatten, mesh.flatten);
        glUniform1i(flipNormal, 0);
        glUniform1i(streamColors, mesh.streamColors ? 1 : 0);
        glUniform1i(unlitColors, mesh.unlitColors ? 1 : 0);
        glUniform1f(faceDivSoften, mesh.faceDivSoften);
        glUniform1f(faceDivBias, mesh.faceDivBias);
        glUniform1i(mtexPass, 0);
        glUniform1i(refMapPass, 0);
        glUniform1i(overlayPass, 0);
        glUniform3fv(drawColor, 1, convertToGsUnits(mesh.color).v);
        glUniform3fv(ambient, 1, convertToGsUnits(mesh.ambient).v);
        // VU1 writes the material's alpha as the vertex alpha, which the GS trace confirms.
        // Dropping it blends every additive mesh at full strength.
        glUniform1f(diffuseAlpha, f32(mesh.diffuse.a) / sdfGsUnit);
        glUniform1i(billboard, mesh.billboard ? 1 : 0);
        glUniform3fv(diffuse, 1, convertToGsUnits(mesh.diffuse).v);

        // Only a draw that asks for the reflection pass gets the highlight.
        const bool reflective =
            !mConfig->noSpecular && (mesh.attrBits & sdfCoreAttrRefMapPass) != 0;
        const GsRgb spec = reflective ? convertToGsUnits(mesh.specular) : GsRgb{};
        glUniform3fv(specular, 1, spec.v);
        glUniform3fv(specularAxis, 1, mesh.lighting.specularAxis);

        const bool fogged =
            !mConfig->noFog && mesh.fog.on && (mesh.attrBits & sdfCoreAttrFog) != 0;
        glUniform4f(fog, mesh.fog.scale, mesh.fog.minimum, mesh.fog.maximum, mesh.fog.offset);
        glUniform1i(fogOn, fogged ? 1 : 0);
        glUniform3fv(fogColor, 1, mesh.fog.color);

        // The hardware runs every pass twice, once over the back faces with normals flipped.
        if (mConfig->cull != 'n') {
            if (mesh.isTwoSided()) {
                // drawBatch 0x01D0 negates every normal and 0x01D2 lights again, so the back
                // faces get their own shading rather than the front pass's.
                glEnable(GL_CULL_FACE);
                const bool flip = mConfig->cull == 'f';
                glFrontFace(mesh.reverseFacing != flip ? GL_CW : GL_CCW);
            } else {
                glEnable(GL_CULL_FACE);
                const bool flip = mConfig->cull == 'f';
                glFrontFace(mesh.reverseFacing != flip ? GL_CW : GL_CCW);
            }
        }
        // The GS's z counts the other way, so its GEQUAL is our LEQUAL. Forcing LESS drops a
        // decal sitting exactly on the surface it decorates.
        glDepthFunc(getDepthFunc(mesh.gs.test));
        if (sdfGetTestDate(mesh.gs.test)) reportUnsupported("mesh with a destination alpha test", 0);
        // FRAME's write mask. The game re-draws a character with RGB masked to build the alpha
        // its bloom reads.
        const u64 mask = mesh.gs.frameMask;
        glColorMask((mask & 0x0000FF) != 0x0000FF, (mask & 0x00FF00) != 0x00FF00,
                    (mask & 0xFF0000) != 0xFF0000, (mask & 0xFF000000u) != 0xFF000000u);
        applyBlend(convertAlphaToBlend(mesh.gs.alpha));
        glUniform1i(atest, sdfGetTestAte(mesh.gs.test) ? int(sdfGetTestAtst(mesh.gs.test)) : 1);
        glUniform1f(atestRef, f32(sdfGetTestAref(mesh.gs.test)));

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
        if (mesh.isTwoSided()) {
            glCullFace(GL_FRONT);
            glUniform1i(flipNormal, 1);
            drawMesh(mesh);
            glUniform1i(flipNormal, 0);
            glCullFace(GL_BACK);
        }

        // corePktEmitMultiTexPass runs it again through the second UV set, in GS context 2.
        if (mesh.multi.on) {
            applyBlend(convertAlphaToBlend(mesh.multi.alpha));
            glUniform1i(mtexPass, mesh.multi.ramp ? 1 : (mesh.multi.algo == 3 ? 2 : 3));
            glUniform3fv(mtexDiffuse, 1, convertToGsUnits(mesh.multi.diffuse).v);
            // The pass's vertex alpha is its own material's, which the RGB above drops.
            glUniform1f(diffuseAlpha, f32(mesh.multi.diffuse.a) / sdfGsUnit);
            glUniform1f(mtexConstant, mesh.multi.constant);
            glUniform3fv(mtexRampLo, 1, mesh.multi.rampColors.lo);
            glUniform3fv(mtexRampDelta, 1, mesh.multi.rampColors.delta);
            glUniform1i(mtexRampKnown, mesh.multi.rampColors.known ? 1 : 0);
            glUniform1f(mtexSoften, mesh.multi.soften);
            glUniform1f(mtexBias, mesh.multi.bias);
            bindTexture(mesh.multi.texHandle, mesh.multi.tex0, mesh.multi.tex1, mesh.multi.clamp,
                        textured, texFunction, texAlpha);
            glUniform1i(secondUv, 1);
            drawMesh(mesh);
            glUniform1f(diffuseAlpha, f32(mesh.diffuse.a) / sdfGsUnit);
        }

        // corePktEmitRefMapPass, additive with ALPHA 72. The per-vertex loop writes u as the
        // clamped dot and v picks a row, so it is a ramp rather than a highlight.
        if (reflective && mesh.refMap.addr) {
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
            glUniform1i(mtexPass, 0);
            glUniform1i(refMapPass, 1);
            glUniform1f(refMapRow, f32(mesh.specular.a) / sdfGsUnit);
            bindTexture(mesh.refMap, 0, 0, 5, textured, texFunction, texAlpha);
            drawMesh(mesh);
            glUniform1i(refMapPass, 0);
        }

        // The sdf3DSpecularTex overlay, attrBits 0x0002. The GS issues 45 of these a frame and
        // they paint nothing: every vertex carries RGBA 0 and ALPHA 0x44 has C=0, so the blend
        // resolves to Cd. Drawing it opaque scores better only by compensating error. See TODO V7.
        if (mConfig->drawSpecularOverlay && (mesh.attrBits & sdfCoreAttrSpecularPass) &&
            mesh.specularTex.addr) {
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
            glDepthMask(GL_FALSE);
            glUniform1i(mtexPass, 0);
            glUniform1i(refMapPass, 0);
            glUniform4f(fog, mesh.fog.scale, mesh.fog.minimum, mesh.fog.maximum, mesh.fog.offset);
            glUniform1i(fogOn, 0);
            glUniform1i(overlayPass, 1);
            glUniform2f(projScale, frame.projectionScale[0], frame.projectionScale[1]);
            bindTexture(mesh.specularTex, mesh.specularTex0, 0, 0, textured, texFunction,
                        texAlpha);
            drawMesh(mesh);
            glUniform1i(overlayPass, 0);
            glUniform1i(fogOn, 1);
            glDepthMask(GL_TRUE);
        }
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    // The 2D pass submits quads in the engine's own winding, so culling must not outlive the
    // mesh pass or half of every sprite goes.
    glDisable(GL_CULL_FACE);
}

void Renderer::describeSprites(const Frame& frame) {
    if (!mDescribe || !mConfig->describeFrame) return;
    log.dump(JsonObject()
                 .addString("dump", "frame")
                 .addUint("sprites", frame.sprites.size())
                 .addUint("meshes", frame.meshes.size())
                 .addUint("placed", frame.order.size())
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
            row.pushHexDigits(u32(c.color.r) << 24 | u32(c.color.g) << 16 | u32(c.color.b) << 8 |
                                  c.color.a,
                              8);
        }
        log.dump(row.endArray().take());
    }
}

void Renderer::drawSprites(const Frame& frame, const std::vector<u32>& indices) {
    if (mConfig->noSprites || indices.empty()) return;

    glUseProgram(mProgram);
    glUniform2f(glGetUniformLocation(mProgram, "uGuestSize"), kGuestWidth, kGuestHeight);
    {
        const RenderTexture& target = mFrames[mCurrent];
        const f32 w = f32(target.getWidth() ? target.getWidth() : 1);
        const f32 h = f32(target.getHeight() ? target.getHeight() : 1);
        glUniform2f(glGetUniformLocation(mProgram, "uPixelOffset"),
                    mConfig->pixelOffsetX * 2.0f / w, -mConfig->pixelOffsetY * 2.0f / h);
    }
    const int textured = glGetUniformLocation(mProgram, "uTextured");
    const int atest = glGetUniformLocation(mProgram, "uAtest");
    const int atestRef = glGetUniformLocation(mProgram, "uAtestRef");
    const int atestInvert = glGetUniformLocation(mProgram, "uAtestInvert");
    const int texFunction = glGetUniformLocation(mProgram, "uTexFunction");
    const int texAlpha = glGetUniformLocation(mProgram, "uTexAlpha");
    const int texelAlpha = glGetUniformLocation(mProgram, "uTexelAlpha");
    const int regionClamp = glGetUniformLocation(mProgram, "uRegionClamp");
    const int region = glGetUniformLocation(mProgram, "uRegion");
    const int texSize = glGetUniformLocation(mProgram, "uTexSize");
    const int fogOn = glGetUniformLocation(mProgram, "uFogOn");
    const int fogColor = glGetUniformLocation(mProgram, "uFogColor");
    glBindVertexArray(mVao);
    // A decoded draw's z can sit past the near plane, as the HUD's and the shadows' largest z do;
    // the GS clamps rather than clips it.
    glEnable(GL_DEPTH_CLAMP);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    glEnable(GL_BLEND);

    // One draw call per sprite: batching needs equal state, and the state comes first. The skip
    // isolates a producer that is over- or under-contributing without a run per guess.
    const char* const skipProducer =
        mConfig->skipProducer.empty() ? nullptr : mConfig->skipProducer.c_str();

    // The stencil marks what a destination alpha test passes, from the alpha as the last run of
    // tested draws found it; any other draw may have changed the alpha since.
    int markedDatm = -1;
    for (const u32 index : indices) {
        const DrawSprite& sprite = frame.sprites[index];
        if (skipProducer && std::strstr(sprite.producer, skipProducer)) continue;
        const bool destinationTest = sdfGetTestDate(sprite.test) && !sprite.unhandled;
        if (destinationTest && markedDatm != int(sdfGetTestDatm(sprite.test))) {
            markedDatm = int(sdfGetTestDatm(sprite.test));
            markDestinationAlpha(u32(markedDatm));
        } else if (!destinationTest) {
            markedDatm = -1;
        }
        // ATST NEVER with AFAIL ZB_ONLY writes no color: a hooked sprite has no depth to lay, so
        // drawing it would only paint a rectangle the hardware never shows.
        if (!sprite.hasDepth && sdfGetTestAte(sprite.test) && sdfGetTestAtst(sprite.test) == 0 &&
            sdfGetTestAfail(sprite.test) == sdfAfailZbOnly) {
            continue;
        }
        // The previous frame is the renderer's own target, not an entry in the texture map, and
        // its UVs are in the guest's own draw space rather than texels.
        const bool isFeedback = sprite.texHandle.addr == kPreviousFrameHandle;
        const bool noFeedback = mConfig->noFeedback;
        if (isFeedback && noFeedback) continue;
        const Texture* seed =
            isFeedback && !mPresented ? findTexture(TexHandle{kFrameSeedHandle}) : nullptr;
        if (isFeedback && !mPresented && !seed) {
            reportUnsupported("frame feedback with no previous frame", 0);
        }
        const bool samplesTarget = isFeedback && !seed;
        const Texture* entry = seed ? seed : findTexture(sprite.texHandle);
        const bool hasTexture = samplesTarget || entry != nullptr;
        const bool neverUploaded = sprite.texHandle.addr && !hasTexture;
        if (neverUploaded) reportUnsupported("sprite texture never uploaded", sprite.texHandle.addr);
        const bool unhandled = sprite.unhandled || neverUploaded;
        mVerts.clear();
        const f32 texWidth = samplesTarget ? kGuestWidth : entry ? entry->width : 1.0f;
        const f32 texHeight = samplesTarget ? kGuestHeight : entry ? entry->height : 1.0f;
        triangulate(sprite, texWidth, texHeight, mVerts);
        // GL puts a render target's origin at the bottom left, the guest's is at the top.
        if (samplesTarget) {
            for (ScreenVertex& vertex : mVerts) vertex.v = 1.0f - vertex.v;
        }

        std::vector<f32> attribs;
        attribs.reserve(mVerts.size() * 8);
        for (ScreenVertex& v : mVerts) {
            // Magenta, as the mesh shader paints an unmodelled state.
            if (unhandled) v.color = Rgba{0xFF, 0x00, 0xFF, 0x80};
            mBounds.add(v);
            attribs.insert(attribs.end(), {v.x, v.y, v.u, v.v, v.color.r / 255.0f,
                                           v.color.g / 255.0f, v.color.b / 255.0f,
                                           v.color.a / 255.0f, v.z, v.fog});
        }
        applySpriteBlend(sprite);
        if (unhandled) applyBlend(GsBlend{});
        glUniform1i(textured, hasTexture && !unhandled ? 1 : 0);
        glUniform1i(texFunction, int(sprite.textureFunction));
        glUniform1i(texAlpha, sprite.textureAlpha ? 1 : 0);
        glUniform1f(texelAlpha, sprite.texelAlpha);
        const bool regions = sprite.hasSampler && !samplesTarget;
        glUniform2i(regionClamp,
                    regions && sdfGetClampWms(sprite.clamp) == sdfWrapRegionClamp ? 1 : 0,
                    regions && sdfGetClampWmt(sprite.clamp) == sdfWrapRegionClamp ? 1 : 0);
        glUniform4f(region, f32(sdfGetClampMinu(sprite.clamp)), f32(sdfGetClampMaxu(sprite.clamp)),
                    f32(sdfGetClampMinv(sprite.clamp)), f32(sdfGetClampMaxv(sprite.clamp)));
        glUniform2f(texSize, texWidth, texHeight);
        glUniform1i(fogOn, sprite.fogs && !unhandled ? 1 : 0);
        glUniform3f(fogColor, sprite.fogColor.r / 255.0f, sprite.fogColor.g / 255.0f,
                    sprite.fogColor.b / 255.0f);
        const bool alphaTests = sdfGetTestAte(sprite.test) && !unhandled;
        glUniform1i(atest, alphaTests ? int(sdfGetTestAtst(sprite.test)) : int(sdfAtstAlways));
        glUniform1f(atestRef, f32(sdfGetTestAref(sprite.test)));
        if (sprite.hasDepth) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(getDepthFunc(sprite.test));
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        if (destinationTest) {
            // A fragment that leaves bit 7 other than DATM fails the next one over it, as the GS
            // tests each against the alpha the one before wrote.
            const bool writesAlpha = (sprite.frameMask & 0xFF000000u) != 0xFF000000u;
            const bool bitSet = sprite.corner[2].color.a >= 0x80;
            const bool clears = writesAlpha && bitSet != (sdfGetTestDatm(sprite.test) != 0);
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(GL_EQUAL, 1, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, clears ? GL_ZERO : GL_KEEP);
        } else {
            glDisable(GL_STENCIL_TEST);
        }
        if (hasTexture) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D,
                          samplesTarget ? mFrames[mCurrent ^ 1].getTexture() : entry->name);
            applySpriteSampler(sprite, isFeedback);
        }
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(attribs.size() * sizeof(f32)), attribs.data(),
                     GL_STREAM_DRAW);
        drawSpriteHalves(sprite, alphaTests, atestInvert, GLsizei(mVerts.size()));
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_CLAMP);
    glDisable(GL_STENCIL_TEST);
}

std::vector<u8> Renderer::readPixels(int* width, int* height) const {
    // The target that was just presented, not the window: a display that scales the window would
    // otherwise decide the shot's size and the reference could not be compared against it.
    const RenderTexture& target = mFrames[mCurrent ^ 1];
    *width = target.getWidth();
    *height = target.getHeight();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target.getFramebuffer());

    std::vector<u8> gl(usize(*width) * usize(*height) * 3);
    // Rows of RGB are not a multiple of 4 bytes at every width, and GL pads to 4 by default.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, *width, *height, GL_RGB, GL_UNSIGNED_BYTE, gl.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    const int width_ = *width;
    const int height_ = *height;
    std::vector<u8> out(gl.size());
    const usize stride = usize(width_) * 3;
    for (int y = 0; y < height_; y++) {
        std::copy_n(gl.begin() + GLsizeiptr(usize(height_ - 1 - y) * stride), stride,
                    out.begin() + GLsizeiptr(usize(y) * stride));
    }
    return out;
}

}  // namespace sdfhook
