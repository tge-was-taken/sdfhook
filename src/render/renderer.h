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

    /// The window's drawable size in pixels. The picture is letterboxed into it.
    void setScreenSize(int width, int height) { mScreenWidth = width; mScreenHeight = height; }

    /// Maps a window pixel to a render-target pixel, undoing the letterbox.
    void mapToTarget(int x, int y, int* outX, int* outY) const;

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

    /// One run of `frame.order`, which holds draws of one kind back to back. Each kind has its own
    /// pass, so the order interleaves them run by run.
    void drawMeshes(const Frame& frame, const std::vector<u32>& indices);
    void drawSprites(const Frame& frame, const std::vector<u32>& indices);
    void describeSprites(const Frame& frame);

    /// Null when nothing was uploaded for the handle.
    const Texture* findTexture(TexHandle handle) const;

    Bounds mBounds;
    const Config* mConfig = nullptr;
    int mHighlight = -1;
    bool mPickPass = false;
    bool mDescribe = false;
    std::vector<ScreenVertex> mVerts;
    std::vector<u32> mRun;
    std::vector<u32> mMeshes;
    std::unordered_map<TexHandle, Texture> mTextures;
    // Two, so a frame can sample the one before it.
    RenderTexture mFrames[2];
    int mScreenWidth = 0;
    int mScreenHeight = 0;

    /// Aspect-preserving destination rectangle for the blit, in window pixels.
    void screenRect(int* x, int* y, int* width, int* height) const;
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
