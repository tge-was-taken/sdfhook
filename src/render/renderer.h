#pragma once

#include <unordered_map>
#include <vector>

#include "../types.h"
#include "../config.h"
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
    bool init(const Config& config, int width, int height);
    void shutdown();
    void draw(const Frame& frame);

    /// Puts the finished frame on screen and keeps it as the one the next may sample.
    void present();

    /// Puts the last finished frame up again without redrawing it. Redrawing would run the frame
    /// feedback a second time over a frame the game never produced, compounding the tap.
    void represent();
    // Top-down RGB, which is the order an image file wants and the reverse of GL's.
    /// The frame just presented, at the size it was rendered, whatever the window is showing.
    std::vector<u8> readPixels(int* width, int* height) const;

    const Bounds& getBounds() const { return mBounds; }

    /// Asks the next draw to describe what it holds, for the frame a shot will capture.
    void describeNextFrame(bool on) { mDescribe = on; }

private:
    // One GL texture per engine texture id, uploaded the first frame that carries it.
    struct Texture {
        u32 name;
        f32 width;
        f32 height;
    };

    void uploadTextures(const Frame& frame);
    void drawMeshes(const Frame& frame);

    /// 2D and 3D share one ordering table, so sprites submitted before a mesh are drawn before it:
    /// a character's shadow quads go down first and he covers them.
    /// Sprites whose ordering table falls in [fromOt, toOt). The 2D tables transmit in index
    /// order, so the ones the engine files before the model tables belong behind the meshes.
    void drawSprites(const Frame& frame, s32 fromOt, s32 toOt);

    /// Null when nothing was uploaded for the handle.
    const Texture* findTexture(TexHandle handle) const;

    Bounds mBounds;
    const Config* mConfig = nullptr;
    bool mDescribe = false;
    std::vector<ScreenVertex> mVerts;
    // Draw order, which the ordering tables decide rather than submission.
    std::vector<usize> mOrder;
    std::vector<usize> mSpriteOrder;
    std::unordered_map<TexHandle, Texture> mTextures;
    // Two, so a frame can sample the one before it.
    RenderTexture mFrames[2];
    int mCurrent = 0;
    u32 mProgram = 0;
    u32 mVao = 0;
    u32 mVbo = 0;
    u32 mMeshProgram = 0;
    u32 mMeshVao = 0;
    u32 mMeshVbo = 0;
    u32 mMeshEbo = 0;
};

}  // namespace sdfhook
