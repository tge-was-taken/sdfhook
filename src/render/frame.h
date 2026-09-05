#pragma once

#include <mutex>
#include <vector>

#include "../guest_ptr.h"
#include "../libsdf.h"
#include "../types.h"

namespace sdfhook {

// A textured rectangle, as the engine's own draw call stated it: screen pixels, texels, one colour.
// Nothing here was recovered from a packet.
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

/// The frame already presented, which the engine samples to blur or fade over itself. It is the
/// renderer's own output rather than anything in guest memory, so it gets a reserved handle.
constexpr u32 kPreviousFrameHandle = 0xFFFFFFFF;

// The GS ALPHA register: (A - B) * C + D over the source, the destination and zero.
enum class GsAlpha : u32 {
    Blend = 0x44,            // (Cs - Cd) * As + Cd
    Additive = 0x48,         // Cs * As + Cd
    ReverseSubtract = 0x42,  // Cd - Cs * As
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
    TexHandle texHandle;
    GsAlpha alpha;

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
    Rgba background{0, 0, 0, 0};
    std::vector<DrawSprite> sprites;
    std::vector<TextureUpload> textureUploads;

    void reset(u64 next) {
        index = next;
        sprites.clear();
        textureUploads.clear();
    }
};

// The guest never waits: blocking the thread that dispatches guest calls would stall the emulator.
class FrameRing {
public:
    Frame* acquireBuild();
    void publish(Frame* frame);
    Frame* takePending();

private:
    static constexpr usize kSlots = 3;
    enum class Slot : u8 { Free, Building, Pending, Presenting };

    Frame mFrames[kSlots];
    Slot mState[kSlots] = {Slot::Free, Slot::Free, Slot::Free};
    std::mutex mMutex;
    u64 mNextIndex = 1;
};

}  // namespace sdfhook
