#pragma once

#include "../types.h"

namespace sdfhook {

/// A colour target the renderer draws into and can then sample, with the depth the 3D pass needs.
/// The frame feedback needs one, as will anything else the engine keeps in GS memory and reads back.
class RenderTexture {
public:
    bool create(int width, int height);
    void destroy();

    /// Makes this the target of subsequent drawing, with the viewport to match.
    void bind() const;

    /// Copies it to the default framebuffer.
    void blitToScreen() const;

    u32 getTexture() const { return mTexture; }

private:
    u32 mFramebuffer = 0;
    u32 mTexture = 0;
    u32 mDepth = 0;
    int mWidth = 0;
    int mHeight = 0;
};

}  // namespace sdfhook
