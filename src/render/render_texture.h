#pragma once

#include "../types.h"

namespace sdfhook {

/// A colour target the renderer draws into and can then sample. The frame feedback needs one; so
/// will the movie frames and anything else the engine keeps in GS memory and reads back.
class RenderTexture {
public:
    bool create(int width, int height);
    void destroy();

    /// Makes this the target of subsequent drawing, with the viewport to match.
    void bind() const;
    /// Copies it to the default framebuffer.
    void blitToScreen() const;

    u32 getTexture() const { return mTexture; }
    int getWidth() const { return mWidth; }
    int getHeight() const { return mHeight; }

private:
    u32 mFramebuffer = 0;
    u32 mTexture = 0;
    int mWidth = 0;
    int mHeight = 0;
};

}  // namespace sdfhook
