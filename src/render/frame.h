#pragma once

#include <condition_variable>
#include <mutex>
#include <vector>

#include "../guest_ptr.h"
#include "../libsdf.h"
#include "../types.h"

namespace sdfhook {

// A packed GS colour. Alpha is in the PS2's own range, where 0x80 is opaque.
struct Rgba {
    u8 r, g, b, a;

    /// The game stores colours as 0xRRGGBBAA and byte-reverses them for the GS, in cmpExpandColor,
    /// frSwapColor and fclDrawSpriteCore alike.
    static Rgba fromColorWord(u32 value) {
        return Rgba{u8(value >> 24), u8(value >> 16), u8(value >> 8), u8(value)};
    }

    /// Already in GS order, red in the low byte.
    static Rgba fromPacked(u32 value) {
        return Rgba{u8(value & 0xFF), u8((value >> 8) & 0xFF), u8((value >> 16) & 0xFF),
                    u8((value >> 24) & 0xFF)};
    }
};

using TexHandle = guest_ptr<sdfTexHandle_t>;
using Matrix4Ptr_t = guest_ptr<struct Matrix4>;

/// The frame already presented, which the engine samples to blur or fade over itself. It is the
/// renderer's own output rather than anything in guest memory, so it gets a reserved handle.
constexpr u32 kPreviousFrameHandle = 0xFFFFFFFF;

// The GS ALPHA register: (A - B) * C + D over the source, the destination and zero.
enum class GsAlpha : u32 {
    Blend = 0x44,            // (Cs - Cd) * As + Cd
    Additive = 0x48,         // Cs * As + Cd
    ReverseSubtract = 0x42,  // Cd - Cs * As
    /// PRIM without ABE: the GS writes the source and reads nothing back.
    Opaque = 0xFFFFFFFF,
};

struct SpriteCorner {
    f32 x, y;
    f32 u, v;
    Rgba colour;
};

// Four corners in the order top left, top right, bottom right, bottom left. Sprites and boxes are
// axis aligned, but the engine also submits rotated ones, so the corners are carried outright.
struct DrawSprite {
    SpriteCorner corner[4];

    /// Where this draw sat in submission order. 2D and 3D share one ordering table, so a shadow
    /// quad submitted before a character has to be drawn before him rather than after every mesh.
    u32 sequence = 0;

    /// How the mesh is found in the frame's transmit chain, so the GS state it ran under can be
    /// settled. A world-geometry stream knows the chain address the engine links; a generated
    /// packet does not, since the engine links its own output, so it is found by position among the
    /// generated links instead. One link can produce several meshes.
    /// False when the engine generated this mesh's packet and linked nothing, so the hardware never
    /// drew it.
    bool linked = true;
    u32 linkAddr = 0;
    u32 linkIndex = 0xFFFFFFFF;
    TexHandle texHandle;
    GsAlpha alpha;

    /// The GS TEST in force. A draw whose alpha test can only fail and whose AFAIL writes depth
    /// alone is a Z barrier: it must not put a rectangle on the screen.
    u32 test = 0;

    /// SDF orders 2D by the ordering-table offset rsetOtOfs sets, not by a per-sprite depth.
    s32 otOffset = 0;

    /// Which engine call produced it, so a quad nobody expected names its own source.
    const char* producer = "?";

    static DrawSprite fromRect(f32 x, f32 y, f32 w, f32 h, f32 u0, f32 v0, f32 u1, f32 v1) {
        DrawSprite sprite{};
        sprite.corner[0] = {x, y, u0, v0, {}};
        sprite.corner[1] = {x + w, y, u1, v0, {}};
        sprite.corner[2] = {x + w, y + h, u1, v1, {}};
        sprite.corner[3] = {x, y + h, u0, v1, {}};
        return sprite;
    }

    void setColour(Rgba colour) {
        for (SpriteCorner& c : corner) c.colour = colour;
    }
};

/// Row-vector, the way the VU keeps one: a point is a row and multiplies from the left, so a
/// composition reads model then view.
struct Matrix4 {
    f32 m[4][4];
};

/// How many bones one vertex blends. The walk counts a vertex that would need more rather than
/// dropping the extra silently, so this is measured rather than assumed.
constexpr u32 kMaxBoneInfluences = 4;

/// One bone's contribution to a vertex. The body gives each bone its own copy of the position and
/// normal in that bone's space, so the shader sums transformed points and needs no bind pose.
struct MeshInfluence {
    f32 x, y, z, weight;
    f32 nx, ny, nz;

    /// A float so the whole influence is two vertex attributes rather than an integer one.
    f32 bone;
};

struct MeshVertex {
    MeshInfluence influence[kMaxBoneInfluences];
    f32 u, v;
    f32 u2, v2;
    Rgba colour{0x80, 0x80, 0x80, 0x80};
};

/// The parallel lights a mesh shades under, in the GS's units where 128 is 1.0. VU1 clamps the sum
/// to 255 before interpolation, so the clamp has to happen per vertex here too.
constexpr u32 kParallelLights = 3;

struct MeshLighting {
    f32 direction[kParallelLights][3];
    f32 colour[kParallelLights][3];
    f32 base[3];

    /// The light packet's colour 3, which VU1 pairs with the material's ambient to form the sum's
    /// base while colorBase is what light 0 reaches.
    f32 ambientColour[3];

    /// Its colour 4, which the packet stores as colorBase plus colour 3. A mesh with no normals
    /// takes this outright rather than running the sum.
    f32 fullColour[3];

    /// The axis corePktEmitRefMapPass dots the normal against, from the light packet's own matrix.
    f32 specularAxis[3];
};

/// The GS fog term: a fragment ends up mix(fogColour, colour, f) where f comes from the vertex's
/// own view depth through the engine's two parameters.
struct MeshFog {
    f32 colour[3];
    f32 scale;
    f32 minimum;
    f32 maximum;
    bool on = false;
};

/// One mesh body, decoded. Vertices are always bone-space and always skinned on the GPU, a rigid
/// body being the one-bone case, so the shader runs a single path.
struct DrawMesh {
    std::vector<MeshVertex> vertices;
    std::vector<u16> indices;
    std::vector<Matrix4> bones;
    TexHandle texHandle;
    Rgba colour{0x80, 0x80, 0x80, 0x80};
    f32 sortZ = 0.0f;

    /// The root's own depth bias, which sdfModelLinkOt sends to VU1 alongside the transform.
    f32 zBias = 0.0f;
    u32 sequence = 0;

    /// How the mesh is found in the frame's transmit chain, so the GS state it ran under can be
    /// settled. A world-geometry stream knows the chain address the engine links; a generated
    /// packet does not, since the engine links its own output, so it is found by position among the
    /// generated links instead. One link can produce several meshes.
    /// False when the engine generated this mesh's packet and linked nothing, so the hardware never
    /// drew it.
    bool linked = true;
    u32 linkAddr = 0;
    u32 linkIndex = 0xFFFFFFFF;
    u16 attrBits = 0;
    u16 matFlags = 0;
    u8 matDuty = 0;
    u8 packetType = 0;
    u16 packetBits = 0;
    Rgba diffuse{0x80, 0x80, 0x80, 0x80};
    Rgba ambient{0x80, 0x80, 0x80, 0x80};
    Rgba specular{0, 0, 0, 0};

    /// TEX0, TEX1 and CLAMP, copied out of the texture handle by the material packet. TCC defaults
    /// set: every 3D batch in the GS trace has it, and an unread packet must not lose texture alpha.
    u64 tex0 = 1ull << 34;
    u64 tex1 = 0;
    u64 clamp = 0;

    /// The UV transform corePktXfmLightClipUV applies, inline in the material packet: a 2x2 matrix
    /// then a 2-vector offset. Identity is (1, 0, 0, 1, 0, 0).
    f32 texMatrix[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    MeshLighting lighting{};
    MeshFog fog{};
    sdfGsState_t gs{};

    /// The second textured pass corePktEmitMultiTexPass runs, through the second UV set and GS
    /// context 2, with its own texture, colour and blend.
    struct MultiTexture {
        TexHandle texHandle;
        u64 alpha = 0x44;
        u64 tex0 = 1ull << 34;
        u64 tex1 = 0;
        u64 clamp = 0;

    /// The UV transform corePktXfmLightClipUV applies, inline in the material packet: a 2x2 matrix
    /// then a 2-vector offset. Identity is (1, 0, 0, 1, 0, 0).
    f32 texMatrix[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        Rgba diffuse{0x80, 0x80, 0x80, 0x80};
        bool on = false;
    } multi;

    /// Without normals VU1 takes the unlit path, which is the draw colour on its own.
    bool lit = false;

    /// A type 1 VIF stream, which is the level geometry and is built two-sided.
    bool worldGeometry = false;

    /// corePktBuildFaceList keeps a triangle when its screen-space signed area agrees with this,
    /// which sdfCorePacketGen sets from the draw's own reverse flag.
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

// What the guest thread builds and the render thread draws. A frame outlives the hook that produced
// it without referring to guest memory.
struct Frame {
    u64 index = 0;

    /// Handed out to each draw as it arrives, so the two lists can be merged back into one order.
    u32 nextSequence = 0;
    Rgba background{0, 0, 0, 0};
    Matrix4 screenTm{};
    std::vector<DrawSprite> sprites;
    std::vector<DrawMesh> meshes;
    std::vector<TextureUpload> textureUploads;


    /// Adding through these is what keeps the two lists mergeable: the sequence is never the
    /// caller's to remember.
    void addSprite(DrawSprite sprite) {
        sprite.sequence = nextSequence++;
        sprites.push_back(std::move(sprite));
    }

    void addMesh(DrawMesh mesh) {
        mesh.sequence = nextSequence++;
        meshes.push_back(std::move(mesh));
    }

    void reset(u64 next) {
        index = next;
        nextSequence = 0;
        sprites.clear();
        meshes.clear();
        textureUploads.clear();
    }
};

/// Every frame the game produces is drawn, in order. Skipping to the newest loses the frame
/// feedback tap, which samples the frame before it, so the same savestate scored differently
/// depending on how many frames the render thread happened to keep up with.
class FrameRing {
public:
    /// Whether a guest that has outrun the renderer drops its oldest frame or waits for it. Playing
    /// wants liveness and a dropped frame costs a slightly stale feedback tap; scoring wants every
    /// frame and can afford to stall, since nothing is watching.
    void setDropWhenBehind(bool drop) { mDropWhenBehind = drop; }

    /// Null when the ring is full and dropping is off, which the caller must treat as a frame it
    /// does not build rather than as a reason to ask again.
    Frame* acquireBuild();
    void publish(Frame* frame);

    /// Returns a frame that was acquired and never published. Without it a discarded frame keeps
    /// its slot for the life of the run, and three of them exhaust the ring: nothing can be built
    /// after that, so the picture stops while the guest carries on.
    void abandon(Frame* frame);
    Frame* takePending();

private:
    static constexpr usize kSlots = 3;
    enum class Slot : u8 { Free, Building, Pending, Presenting };

    Frame mFrames[kSlots];
    Slot mState[kSlots] = {Slot::Free, Slot::Free, Slot::Free};
    std::mutex mMutex;
    std::condition_variable mFreed;
    bool mDropWhenBehind = false;
    u64 mNextIndex = 1;
};

}  // namespace sdfhook
