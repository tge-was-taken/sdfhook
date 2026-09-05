#pragma once

#include <unordered_map>
#include <vector>

#include "../types.h"
#include "frame.h"
#include "render_texture.h"
#include "sprite.h"

namespace sdfhook {

// Draws a frame with GL. Every call must be on the thread holding the context, which is
// RenderThread's; it owns no window, no thread and no synchronisation of its own.
class Renderer {
public:
    // Says whether the assumed display rect is the real one: vertices outside it mean the XYOFFSET
    // the decode subtracts is wrong.
    struct Bounds {
        bool seen = false;
        f32 minX = 0, minY = 0, maxX = 0, maxY = 0;
        void add(const ScreenVertex& v);
    };

    /// Renders into its own target so a frame can sample the one before it.
    bool init(int width, int height);
    void shutdown();
    void draw(const Frame& frame);
    /// Puts the finished frame on screen and keeps it as the one the next may sample.
    void present();
    // Top-down RGB, which is the order an image file wants and the reverse of GL's.
    std::vector<u8> readPixels(int width, int height) const;

    const Bounds& getBounds() const { return mBounds; }

private:
    Bounds mBounds;
    std::vector<ScreenVertex> mVerts;
    // One GL texture per engine texture id, uploaded the first frame that carries it.
    struct Texture {
        u32 name;
        f32 width;
        f32 height;
    };
    std::unordered_map<TexHandle, Texture> mTextures;
    // Two, so a frame can sample the one before it.
    RenderTexture mFrames[2];
    int mCurrent = 0;
    u32 mProgram = 0;
    u32 mVao = 0;
    u32 mVbo = 0;
};

}  // namespace sdfhook
