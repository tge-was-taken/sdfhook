#pragma once

#include <unordered_map>
#include <vector>

#include "../types.h"
#include "../config.h"
#include "frame.h"
#include "render_texture.h"
#include "sprite.h"

namespace sdfhook {

// Every call must be on the thread holding the context. Owns no window, thread or locking.
class Renderer {
public:
    // Vertices outside it mean the XYOFFSET the decode subtracts is wrong.
    struct Bounds {
        bool seen = false;
        f32 minX = 0, minY = 0, maxX = 0, maxY = 0;
        void add(const ScreenVertex& v);
    };

    /// Renders into its own target so a frame can sample the one before it.
    bool init(const Config& config, int width, int height);
    void shutdown();
    void draw(const Frame& frame);

    /// Picks a mesh by eye in a window run instead of by index out of a trace. -1 picks none.
    void setHighlight(int index) { mHighlight = index; }

    /// By painting every mesh with its index and reading the pixel back. -1 for none.
    int pickMesh(const Frame& frame, int x, int y);

    /// Kept as the one the next frame may sample.
    void present();

    /// Without redrawing: that would run the feedback tap again over a frame the game never
    /// produced.
    void represent();
    /// Top-down RGB, at the size rendered whatever the window shows.
    std::vector<u8> readPixels(int* width, int* height) const;

    const Bounds& getBounds() const { return mBounds; }

    /// Asks the next draw to describe what it holds, for the frame a shot will capture.
    void describeNextFrame(bool on) { mDescribe = on; }

private:
    // Uploaded the first frame that carries it.
    struct Texture {
        u32 name;
        f32 width;
        f32 height;
    };

    void uploadTextures(const Frame& frame);
    void drawMeshes(const Frame& frame);

    /// [fromOt, toOt). The 2D tables transmit in index order, so those filed before the model
    /// tables belong behind the meshes.
    void drawSprites(const Frame& frame, s32 fromOt, s32 toOt);

    /// Null when nothing was uploaded for the handle.
    const Texture* findTexture(TexHandle handle) const;

    Bounds mBounds;
    const Config* mConfig = nullptr;
    int mHighlight = -1;
    bool mPickPass = false;
    bool mDescribe = false;
    std::vector<ScreenVertex> mVerts;
    // Decided by the ordering tables, not submission.
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
