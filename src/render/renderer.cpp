#include "renderer.h"

#include <algorithm>
#include <cstdio>

#include "../unsupported.h"
#include "glad/gl.h"

namespace sdfhook {
namespace {

constexpr const char* kVertexShader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColour;
out vec2 vUv;
out vec4 vColour;
uniform vec2 uGuestSize;

// The GS treats 128 as fully opaque, not 255.
const float kPs2Alpha = 255.0 / 128.0;

void main() {
    vec2 ndc = vec2(aPos.x / uGuestSize.x * 2.0 - 1.0, 1.0 - aPos.y / uGuestSize.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUv = aUv;
    vColour = vec4(aColour.rgb, aColour.a * kPs2Alpha);
}
)";

constexpr const char* kFragmentShader = R"(#version 330 core
in vec2 vUv;
in vec4 vColour;
out vec4 oColour;
uniform sampler2D uTexture;
uniform int uTextured;
void main() {
    oColour = uTextured != 0 ? texture(uTexture, vUv) * vColour * 2.0 : vColour;
}
)";

// The GS ALPHA register is (A - B) * C + D over Cs, Cd and zero, which only lines up with a fixed
// GL blend for the ordinary case; additive and subtract need their own.
void applyBlend(GsAlpha alpha) {
    switch (alpha) {
        case GsAlpha::Additive:
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            return;
        case GsAlpha::ReverseSubtract:
            glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            return;
        case GsAlpha::Blend:
            break;
        default:
            reportUnsupported("ALPHA", u32(alpha));
            break;
    }
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

u32 compile(GLenum type, const char* source) {
    const u32 shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    int ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[render] shader: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

void Renderer::Bounds::add(const ScreenVertex& v) {
    if (!seen) {
        seen = true;
        minX = maxX = v.x;
        minY = maxY = v.y;
        return;
    }
    minX = v.x < minX ? v.x : minX;
    minY = v.y < minY ? v.y : minY;
    maxX = v.x > maxX ? v.x : maxX;
    maxY = v.y > maxY ? v.y : maxY;
}

bool Renderer::init(int width, int height) {
    for (RenderTexture& target : mFrames) {
        if (!target.create(width, height)) return false;
    }
    const u32 vs = compile(GL_VERTEX_SHADER, kVertexShader);
    const u32 fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs) return false;
    mProgram = glCreateProgram();
    glAttachShader(mProgram, vs);
    glAttachShader(mProgram, fs);
    glLinkProgram(mProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);

    glGenVertexArrays(1, &mVao);
    glGenBuffers(1, &mVbo);
    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    constexpr int kStride = 8 * sizeof(f32);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, kStride, nullptr);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, kStride, (void*)(2 * sizeof(f32)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, kStride, (void*)(4 * sizeof(f32)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    return true;
}

void Renderer::shutdown() {
    for (RenderTexture& target : mFrames) target.destroy();
    glDeleteBuffers(1, &mVbo);
    glDeleteVertexArrays(1, &mVao);
    glDeleteProgram(mProgram);
}

void Renderer::present() {
    mFrames[mCurrent].blitToScreen();
    mCurrent ^= 1;
}

void Renderer::draw(const Frame& frame) {
    mFrames[mCurrent].bind();

    for (const TextureUpload& upload : frame.textureUploads) {
        if (mTextures.count(upload.handle)) continue;
        u32 name = 0;
        glGenTextures(1, &name);
        glBindTexture(GL_TEXTURE_2D, name);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLsizei(upload.width), GLsizei(upload.height), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, upload.rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        mTextures.emplace(upload.handle, Texture{name, f32(upload.width), f32(upload.height)});
    }

    const Rgba& back = frame.background;
    glClearColor(back.r / 255.0f, back.g / 255.0f, back.b / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (frame.sprites.empty()) return;

    glUseProgram(mProgram);
    glUniform2f(glGetUniformLocation(mProgram, "uGuestSize"), kGuestWidth, kGuestHeight);
    const int textured = glGetUniformLocation(mProgram, "uTextured");
    glBindVertexArray(mVao);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    glEnable(GL_BLEND);

    // One draw call per sprite for now: batching needs the texture and blend state to be equal, and
    // getting the state right comes before making it fast.
    for (const DrawSprite& sprite : frame.sprites) {
        // The previous frame is the renderer's own target, not an entry in the texture map, and
        // its UVs are in the guest's own draw space rather than texels.
        const bool isFeedback = sprite.texHandle.addr == kPreviousFrameHandle;
        const auto found = mTextures.find(sprite.texHandle);
        const bool hasTexture = isFeedback || found != mTextures.end();
        mVerts.clear();
        const bool hasEntry = found != mTextures.end();
        const f32 texWidth = isFeedback ? kGuestWidth : hasEntry ? found->second.width : 1.0f;
        const f32 texHeight = isFeedback ? kGuestHeight : hasEntry ? found->second.height : 1.0f;
        toTriangles(sprite, texWidth, texHeight, mVerts);
        // GL puts a render target's origin at the bottom left, the guest's is at the top.
        if (isFeedback) {
            for (ScreenVertex& vertex : mVerts) vertex.v = 1.0f - vertex.v;
        }

        std::vector<f32> attribs;
        attribs.reserve(mVerts.size() * 8);
        for (const ScreenVertex& v : mVerts) {
            mBounds.add(v);
            attribs.insert(attribs.end(), {v.x, v.y, v.u, v.v, v.colour.r / 255.0f,
                                           v.colour.g / 255.0f, v.colour.b / 255.0f,
                                           v.colour.a / 255.0f});
        }
        applyBlend(sprite.alpha);
        glUniform1i(textured, hasTexture ? 1 : 0);
        if (hasTexture) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D,
                          isFeedback ? mFrames[mCurrent ^ 1].getTexture() : found->second.name);
        }
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(attribs.size() * sizeof(f32)), attribs.data(),
                     GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(mVerts.size()));
    }
}

std::vector<u8> Renderer::readPixels(int width, int height) const {
    std::vector<u8> gl(usize(width) * usize(height) * 3);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, gl.data());
    std::vector<u8> out(gl.size());
    const usize stride = usize(width) * 3;
    for (int y = 0; y < height; y++) {
        std::copy_n(gl.begin() + GLsizeiptr(usize(height - 1 - y) * stride), stride,
                    out.begin() + GLsizeiptr(usize(y) * stride));
    }
    return out;
}

}  // namespace sdfhook
