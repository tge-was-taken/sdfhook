#pragma once

#include <condition_variable>
#include <mutex>
#include <vector>

#include "../guest_ptr.h"
#include "../libsdf.h"
#include "../types.h"

namespace sdfhook {

// A packed GS color. Alpha is in the PS2's own range, where 0x80 is opaque.
struct Rgba {
    u8 r, g, b, a;

    /// 0xRRGGBBAA byte-reversed for the GS, as cmpExpandColor and frSwapColor do it.
    static Rgba unpackColorWord(u32 value) {
        return Rgba{u8(value >> 24), u8(value >> 16), u8(value >> 8), u8(value)};
    }

    /// Already in GS order, red in the low byte.
    static Rgba unpack(u32 value) {
        return Rgba{u8(value & 0xFF), u8((value >> 8) & 0xFF), u8((value >> 16) & 0xFF),
                    u8((value >> 24) & 0xFF)};
    }
};

using TexHandle = guest_ptr<sdfTexHandle_t>;
using Matrix4Ptr_t = guest_ptr<struct Matrix4>;

/// Sampled by the engine to blur over itself. Our output, not guest memory, so a reserved handle.
constexpr u32 kPreviousFrameHandle = 0xFFFFFFFF;

/// The frame GS memory held when the run started, which a blur samples before we have drawn one.
constexpr u32 kFrameSeedHandle = 0xFFFFFFFE;

/// ALPHA 0x44, (Cs - Cd) * As + Cd, which the engine sets for every sprite table.
constexpr u64 kGsAlphaBlend = 0x44;

struct SpriteCorner {
    f32 x, y;
    f32 u, v;
    Rgba color;

    /// GS z as the depth buffer holds it, 1 - z / 2^24, for a draw decoded from its packet.
    f32 z = 0.0f;

    /// The GS's F, 1.0 for none of FOGCOL.
    f32 fog = 1.0f;
};

/// A draw's index in the frame's generated links when its packet address is not known yet.
constexpr u32 kNoLinkIndex = 0xFFFFFFFF;

// Top left, top right, bottom right, bottom left. The engine submits rotated quads too, so the
// corners are carried outright rather than as a rectangle.
struct DrawSprite {
    SpriteCorner corner[4];

    /// The packet that carries the draw, which is how the chain walk finds it. Zero until the
    /// producer links one.
    u32 linkAddr = 0;
    TexHandle texHandle;

    /// The producer named no texture, so the sprite samples whatever the chain has bound at its
    /// packet.
    bool boundTexture = false;

    /// PRIM's ABE: without it the GS never reads ALPHA.
    bool blends = true;

    /// The ALPHA the chain has in force at the sprite's packet, FIX included.
    u64 alpha = kGsAlphaBlend;

    /// The renderer cannot draw it as the GS would, so it paints it magenta.
    bool unhandled = false;

    /// TEX0's TFX and TCC, and PRIM's FGE with FOGCOL, for a draw decoded from its packet. A hooked
    /// sprite modulates with the texture's alpha and has no fog.
    u32 textureFunction = sdfTexModulate;
    bool textureAlpha = true;

    /// A 24-bit texture has no alpha of its own: the GS reads TEXA's TA0 in its place, as a
    /// fraction of 255. Negative samples the texture's.
    f32 texelAlpha = -1.0f;
    bool fogs = false;
    Rgba fogColor{0, 0, 0, 0};

    /// Depth-tested against the meshes by TEST, and written unless ZBUF masks it. A hooked sprite
    /// never saw either.
    bool hasDepth = false;
    u64 zbuf = 0;

    /// TEX1 and CLAMP, for a draw decoded from its packet. A hooked sprite never saw them.
    bool hasSampler = false;
    u64 tex1 = 0;
    u64 clamp = 0;

    /// A draw whose alpha test can only fail and whose AFAIL writes depth alone is a Z barrier:
    /// it must not put a rectangle on the screen.
    u32 test = 0;

    /// FRAME's write mask in force at the sprite's packet.
    u64 frameMask = 0;

    /// So a quad nobody expected names its own source.
    const char* producer = "?";

    static DrawSprite makeRect(f32 x, f32 y, f32 w, f32 h, f32 u0, f32 v0, f32 u1, f32 v1) {
        DrawSprite sprite{};
        sprite.corner[0] = {x, y, u0, v0, {}};
        sprite.corner[1] = {x + w, y, u1, v0, {}};
        sprite.corner[2] = {x + w, y + h, u1, v1, {}};
        sprite.corner[3] = {x, y + h, u0, v1, {}};
        return sprite;
    }

    void setColor(Rgba color) {
        for (SpriteCorner& c : corner) c.color = color;
    }

    void setDepth(f32 z) {
        for (SpriteCorner& c : corner) c.z = z;
    }
};

/// Row-vector, the way the VU keeps one: a point multiplies from the left, so a composition
/// reads model then view.
struct Matrix4 {
    f32 m[4][4];
};

/// The walk counts a vertex needing more rather than dropping the extra silently.
constexpr u32 kMaxBoneInfluences = 4;

/// The body gives each bone its own copy of the position and normal in that bone's space, so the
/// shader sums transformed points and needs no bind pose.
struct MeshInfluence {
    f32 x, y, z, weight;
    f32 nx, ny, nz;

    /// A float so the influence is two vertex attributes rather than an integer one.
    f32 bone;
};

struct MeshVertex {
    MeshInfluence influence[kMaxBoneInfluences];
    f32 u, v;
    f32 u2, v2;
    Rgba color{0x80, 0x80, 0x80, 0x80};
};

/// The parallel lights a mesh shades under, in the GS's units where 128 is 1.0. VU1 clamps the
/// sum to 255 before interpolation, so the clamp happens per vertex here too.
constexpr u32 kParallelLights = 3;

struct MeshLighting {
    f32 direction[kParallelLights][3];
    f32 color[kParallelLights][3];
    f32 base[3];

    /// Color 3, which VU1 pairs with the material's ambient as the sum's base, where colorBase is
    /// what light 0 reaches.
    f32 ambientColor[3];

    /// Color 4, stored as colorBase plus color 3. A mesh with no normals takes it outright.
    f32 fullColor[3];

    /// The axis corePktEmitRefMapPass dots the normal against.
    f32 specularAxis[3];
};

/// The GS fog term. VU1 computes `clamp(offset + scale / w, minimum, maximum)` per vertex
/// (transformVertices 0x04DC, 0x04DF, 0x04E3), so the offset is a field and both ends are real.
struct MeshFog {
    f32 color[3];
    f32 scale;
    f32 offset;
    f32 minimum;
    f32 maximum;
    bool on = false;
};

/// A ramp between two colors, held as its low endpoint and the difference the coordinate scales,
/// which is the form corePktMakeFaceDivParm leaves them in.
struct MeshRamp {
    f32 lo[3] = {};
    f32 delta[3] = {};
    bool known = false;
};

struct DrawMesh {
    /// Placed by the prim transform the chain has in force at its packet, as VU1 places a poly.
    bool chainTransform = false;

    std::vector<MeshVertex> vertices;
    std::vector<u16> indices;
    std::vector<Matrix4> bones;
    TexHandle texHandle;
    Rgba color{0x80, 0x80, 0x80, 0x80};
    f32 sortZ = 0.0f;

    /// The packet that carries the draw, or the stream a REF in the chain names. A generated
    /// packet's address is known only once it is linked, so it is found by linkIndex instead.
    u32 linkAddr = 0;
    u32 linkIndex = kNoLinkIndex;

    /// So a mesh the chain never reached names its source.
    const char* producer = "?";
    u16 attrBits = 0;
    u16 matFlags = 0;
    u8 matDuty = 0;
    u8 packetType = 0;
    u16 packetBits = 0;
    Rgba diffuse{0x80, 0x80, 0x80, 0x80};

    /// Drawn by VU1's spriteOne..spriteFour, which unpack no material.
    bool billboard = false;

    /// A billboard's centre, camera axes and corner offsets, so its corners can be rebuilt under
    /// the transform the chain sends its batch with.
    struct BillboardFrame {
        f32 centre[3];
        f32 right[3];
        f32 up[3];
        f32 offset[4][2];
    } billboardFrame{};
    Rgba ambient{0x80, 0x80, 0x80, 0x80};
    Rgba specular{0, 0, 0, 0};

    /// corePktEmitRefMapPass stores the dot to the texcoord slot, so it is a ramp lookup and not
    /// a scalar highlight: u from the clamped dot, v choosing a row by the material's alpha.
    TexHandle refMap;

    /// A real handle where refMap holds a raw TEX0, so it resolves through the ordinary cache.
    TexHandle specularTex;

    /// From the handle's own upload packet. The batch has TCC 1, so a zero TEX0 here would drop
    /// the alpha that makes the splotch sparse and paint it opaque.
    u64 specularTex0 = 0;

    /// TCC defaults set: every 3D batch in the trace has it, and an unread packet must not lose
    /// the texture's alpha.
    u64 tex0 = 1ull << 34;
    u64 tex1 = 0;
    u64 clamp = 0;

    /// The UV transform corePktXfmLightClipUV applies, inline in the material packet: a 2x2 matrix
    /// then a 2-vector offset. Identity is (1, 0, 0, 1, 0, 0).
    f32 texMatrix[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    MeshLighting lighting{};
    MeshFog fog{};
    sdfGsState_t gs{};

    /// corePktEmitMultiTexPass's pass, through the second UV set and GS context 2.
    struct MultiTexture {
        TexHandle texHandle;
        u64 alpha = 0x44;
        u64 tex0 = 1ull << 34;
        u64 tex1 = 0;
        u64 clamp = 0;

        /// This pass's own UV transform, in the same form.
        f32 texMatrix[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        Rgba diffuse{0x80, 0x80, 0x80, 0x80};

        /// How corePktEmitMultiTexPass recolors first: under 3 the face-division ramp, 3 a
        /// constant color, above that the first pass's colors stand.
        u32 algo = 0;
        f32 constant = 0.0f;

        /// corePktXfmLightClip's coordinate is clamp(soften * (bias + dot), 0, 1), with soften at
        /// least 1, so it sharpens the terminator rather than softening it.
        f32 soften = 1.0f;
        f32 bias = 0.0f;
        bool ramp = false;
        bool on = false;

        /// The endpoints corePktMakeFaceDivParm wrote for this material, read from the guest rather
        /// than rebuilt from the light and material colors. Unset until a packet has been generated.
        MeshRamp rampColors{};
    } multi;

    /// mem[910].w, the lighting sum's flatten term.
    f32 flatten = 0.0f;

    f32 faceDivSoften = 1.0f;
    f32 faceDivBias = 0.0f;

    /// Without normals VU1 takes the unlit path, which is the draw color on its own.
    bool lit = false;

    /// attrBits & 0x800. The stream's color is the shading, so the light term must not multiply
    /// it a second time.
    bool streamColors = false;

    /// VU1 sends the vertex colors to the GS as they are, with no light term.
    bool unlitColors = false;

    /// A type 1 VIF stream, which is the level geometry and is built two-sided.
    bool worldGeometry = false;

    /// corePktBuildFaceList keeps a triangle whose screen-space signed area agrees with this.
    bool reverseFacing = false;
    u8 otIndex = 0;

    bool isTwoSided() const { return (attrBits & sdfCoreAttrTwoSided) != 0; }
};

// A decoded texture on its way to the render thread, sent once per texture rather than per frame.
struct TextureUpload {
    TexHandle handle;
    u32 width;
    u32 height;
    std::vector<u8> rgba;
};

/// One draw in the order the frame's DMA chain transmits it.
struct DrawRef {
    enum class Kind : u8 { Sprite, Mesh };
    Kind kind;
    u32 index;
};

// A frame outlives the hook that produced it without referring to guest memory.
struct Frame {
    u64 index = 0;
    Rgba background{0, 0, 0, 0};
    Matrix4 screenTm{};

    /// From sdf3DScreenParam quadwords 3 and 4. screenTm is projection times view, so dividing
    /// by these recovers the view rotation.
    f32 projectionScale[2] = {1.0f, 1.0f};

    /// VU1's map from clip z over w onto GS z, so a mesh's depth is the z the GS would test.
    f32 depthScale = 0.0f;
    f32 depthOffset = 0.0f;
    std::vector<DrawSprite> sprites;
    std::vector<DrawMesh> meshes;
    std::vector<TextureUpload> textureUploads;

    /// What the renderer draws, in chain order. A draw the chain never reached is not in it.
    std::vector<DrawRef> order;

    void addSprite(DrawSprite sprite) { sprites.push_back(std::move(sprite)); }
    void addMesh(DrawMesh mesh) { meshes.push_back(std::move(mesh)); }

    void reset(u64 next) {
        index = next;
        sprites.clear();
        meshes.clear();
        textureUploads.clear();
        order.clear();
    }
};

/// Every frame the game produces is drawn, in order: the feedback tap samples the frame before
/// it, so skipping to the newest makes one savestate score differently run to run.
class FrameRing {
public:
    /// Playing wants liveness and can afford a stale feedback tap; scoring wants every frame and
    /// can afford to stall.
    void setDropWhenBehind(bool drop) { mDropWhenBehind = drop; }

    /// Null when the ring is full and dropping is off: a frame not built, not a reason to retry.
    Frame* acquireBuild();
    void publish(Frame* frame);

    /// Returns a frame acquired and never published. Without it three discards exhaust the ring
    /// and the picture stops while the guest carries on.
    void abandon(Frame* frame);
    Frame* takePending();

private:
    static constexpr usize kSlots = 3;
    enum class Slot : u8 { Free, Building, Pending, Presenting };

    // A texture is uploaded once, so a frame not drawn hands its uploads to the next one.
    void carryUploads(Frame& frame);
    void takeCarriedUploads(Frame& frame);

    Frame mFrames[kSlots];
    Slot mState[kSlots] = {Slot::Free, Slot::Free, Slot::Free};
    std::mutex mMutex;
    std::condition_variable mFreed;
    bool mDropWhenBehind = false;
    u64 mNextIndex = 1;
    std::vector<TextureUpload> mCarried;
};

}  // namespace sdfhook
