#include "render_texture.h"
#include "../log.h"

#include <cstdio>

#include "glad/gl.h"

namespace sdfhook {
namespace {

LogChannel log("render");

}  // namespace

bool RenderTexture::create(int width, int height) {
    mWidth = width;
    mHeight = height;

    glGenTextures(1, &mTexture);
    glBindTexture(GL_TEXTURE_2D, mTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &mDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, mDepth);
    // Stencil for the GS's destination alpha test.
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, width, height);

    glGenTextures(1, &mSnapshot);
    glBindTexture(GL_TEXTURE_2D, mSnapshot);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenFramebuffers(1, &mFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mTexture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, mDepth);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    glGenRenderbuffers(1, &mSavedDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, mSavedDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, width, height);
    glGenFramebuffers(1, &mSavedFramebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, mSavedFramebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                              mSavedDepth);
    if (status == GL_FRAMEBUFFER_COMPLETE) status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        log.error("{}x{} target incomplete: 0x{:X}", width, height, status);
        destroy();
        return false;
    }
    return true;
}

void RenderTexture::destroy() {
    glDeleteFramebuffers(1, &mFramebuffer);
    glDeleteRenderbuffers(1, &mDepth);
    glDeleteTextures(1, &mTexture);
    glDeleteTextures(1, &mSnapshot);
    glDeleteFramebuffers(1, &mSavedFramebuffer);
    glDeleteRenderbuffers(1, &mSavedDepth);
    mSavedFramebuffer = 0;
    mSavedDepth = 0;
    mFramebuffer = 0;
    mDepth = 0;
    mTexture = 0;
    mSnapshot = 0;
}

bool RenderTexture::resize(int width, int height) {
    if (width == mWidth && height == mHeight) return true;
    RenderTexture next;
    if (!next.create(width, height)) return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, mFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, next.mFramebuffer);
    glBlitFramebuffer(0, 0, mWidth, mHeight, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    destroy();
    *this = next;
    return true;
}

u32 RenderTexture::copyColor() const {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, mFramebuffer);
    glBindTexture(GL_TEXTURE_2D, mSnapshot);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, mWidth, mHeight);
    return mSnapshot;
}

namespace {

// A blit is scissored, and the sprites leave the scissor on.
void blitDepth(u32 from, u32 to, int width, int height) {
    const GLboolean scissored = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, from);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    if (scissored) glEnable(GL_SCISSOR_TEST);
}

}  // namespace

void RenderTexture::saveDepth() const {
    blitDepth(mFramebuffer, mSavedFramebuffer, mWidth, mHeight);
    glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffer);
}

void RenderTexture::restoreDepth() const {
    blitDepth(mSavedFramebuffer, mFramebuffer, mWidth, mHeight);
    glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffer);
}

void RenderTexture::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, mFramebuffer);
    glViewport(0, 0, mWidth, mHeight);
}

void RenderTexture::blitToScreen(int x, int y, int width, int height) const {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, mFramebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    // The letterbox is not overwritten by the blit, so it is painted before it.
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, mWidth, mHeight, x, y, x + width, y + height, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

}  // namespace sdfhook
