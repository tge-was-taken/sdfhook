#include "render_thread.h"

#include <SDL3/SDL.h>

#include <cstdio>

#include "glad/gl.h"

namespace sdfhook {

bool RenderThread::open(int width, int height) {
    mThread = std::thread(&RenderThread::run, this, width, height);
    while (!mReady.load(std::memory_order_acquire) && !mOpenFailed.load(std::memory_order_acquire)) {
        SDL_Delay(1);
    }
    return !mOpenFailed.load(std::memory_order_acquire);
}

void RenderThread::close() {
    mStop.store(true, std::memory_order_relaxed);
    if (mThread.joinable()) mThread.join();
}

void RenderThread::run(int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "[render] SDL_Init: %s\n", SDL_GetError());
        mOpenFailed.store(true, std::memory_order_release);
        return;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    SDL_Window* window = SDL_CreateWindow("sdfhook", width, height, SDL_WINDOW_OPENGL);
    if (!window) {
        std::fprintf(stderr, "[render] SDL_CreateWindow: %s\n", SDL_GetError());
        mOpenFailed.store(true, std::memory_order_release);
        return;
    }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context || !gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) {
        std::fprintf(stderr, "[render] no GL context\n");
        mOpenFailed.store(true, std::memory_order_release);
        SDL_DestroyWindow(window);
        return;
    }
    std::fprintf(stderr, "[render] %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    if (!mRenderer.init(width, height)) {
        mOpenFailed.store(true, std::memory_order_release);
        SDL_GL_DestroyContext(context);
        SDL_DestroyWindow(window);
        return;
    }
    mRunning.store(true, std::memory_order_release);
    mReady.store(true, std::memory_order_release);

    const Frame* held = nullptr;
    while (!mStop.load(std::memory_order_relaxed)) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) mStop.store(true, std::memory_order_relaxed);
        }
        if (const Frame* next = mFrames.takePending()) {
            held = next;
            mDrawn.fetch_add(1, std::memory_order_relaxed);
            mSprites.fetch_add(next->sprites.size(), std::memory_order_relaxed);
        }
        if (held) {
            mRenderer.draw(*held);
            mRenderer.present();
        }

        std::string shot;
        {
            const std::lock_guard<std::mutex> lock(mShotMutex);
            shot.swap(mShotPath);
        }
        if (!shot.empty()) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            writeShot(shot, w, h);
        }
        SDL_GL_SwapWindow(window);
    }

    std::fprintf(stderr, "[render] %llu frames taken, %llu sprites\n",
                 (unsigned long long)mDrawn.load(), (unsigned long long)mSprites.load());
    const Renderer::Bounds& bounds = mRenderer.getBounds();
    if (bounds.seen) {
        std::fprintf(stderr, "[render] sprite bounds x %.1f..%.1f y %.1f..%.1f (display %gx%g)\n",
                     bounds.minX, bounds.maxX, bounds.minY, bounds.maxY, kGuestWidth, kGuestHeight);
    }
    mRunning.store(false, std::memory_order_release);
    mRenderer.shutdown();
    SDL_GL_DestroyContext(context);
    SDL_DestroyWindow(window);
    // No SDL_Quit: it unloads the GL driver this thread still holds thread-local state in.
}

void RenderThread::requestShot(const std::string& path) {
    const std::lock_guard<std::mutex> lock(mShotMutex);
    mShotPath = path;
}

void RenderThread::writeShot(const std::string& path, int width, int height) {
    const std::vector<u8> px = mRenderer.readPixels(width, height);
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        std::fprintf(stderr, "[render] cannot write %s\n", path.c_str());
        return;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);
    std::fwrite(px.data(), 1, px.size(), file);
    std::fclose(file);
    std::fprintf(stderr, "[render] wrote %s (%dx%d)\n", path.c_str(), width, height);
}

}  // namespace sdfhook
