#pragma once

#include "../types.h"

namespace sdfhook {

/// Sampleable, for the frame feedback and anything else the engine reads back out of GS memory.
class RenderTexture {
public:
    bool create(int width, int height);
    void destroy();

    void bind() const;

    void blitToScreen() const;

    u32 getTexture() const { return mTexture; }

    /// For reading the render back at its own size. The window may be scaled by the display, so
    /// what it holds is not what the scoring compares.
    u32 getFramebuffer() const { return mFramebuffer; }
    int getWidth() const { return mWidth; }
    int getHeight() const { return mHeight; }

private:
    u32 mFramebuffer = 0;
    u32 mTexture = 0;
    u32 mDepth = 0;
    int mWidth = 0;
    int mHeight = 0;
};

}  // namespace sdfhook
