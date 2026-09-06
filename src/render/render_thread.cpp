#include "render_thread.h"

#include "../log.h"

#include <SDL3/SDL.h>

#include <cstdio>

#include "glad/gl.h"

namespace sdfhook {
namespace {

LogChannel log("render");

}  // namespace

bool RenderThread::open(const Config& config, int width, int height) {
    mConfig = &config;
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
    // Before SDL_Init, since the driver choice is read there. The window is for watching a run, not
    // driving one, so it must never take the keyboard away from the terminal that started it.
    //
    // X11 because a Wayland toplevel cannot refuse focus at all. A list, so a host without X11 still
    // comes up, and at DEFAULT priority so SDL_VIDEO_DRIVER in the environment still wins.
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "x11,wayland", SDL_HINT_DEFAULT);
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
    SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "0");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log.info("SDL_Init: {}", SDL_GetError());
        mOpenFailed.store(true, std::memory_order_release);
        return;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    // Created hidden and only shown on request: an automated run needs the GL context, not a
    // window, and mapping one takes the keyboard from the terminal that started it. UTILITY on top
    // of that, because under WSLg WM_HINTS input=False is set correctly and ignored.
    SDL_Window* window = SDL_CreateWindow(
        "sdfhook", width, height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_UTILITY | SDL_WINDOW_HIDDEN);
    if (!window) {
        log.info("SDL_CreateWindow: {}", SDL_GetError());
        mOpenFailed.store(true, std::memory_order_release);
        return;
    }
    SDL_SetWindowFocusable(window, false);
    if (mConfig->window) SDL_ShowWindow(window);
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context || !gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) {
        log.info("no GL context");
        mOpenFailed.store(true, std::memory_order_release);
        SDL_DestroyWindow(window);
        return;
    }
    log.info("{} | {} | sdl video {}", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
             reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
             SDL_GetCurrentVideoDriver());

    if (!mRenderer.init(*mConfig, width, height)) {
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
        const Frame* next = mFrames.takePending();
        if (next) {
            held = next;
            mDrawn.fetch_add(1, std::memory_order_relaxed);
            mSprites.fetch_add(next->sprites.size(), std::memory_order_relaxed);
        }
        // Only taken on an iteration that will actually draw, so the shot always holds a frame the
        // game produced and a diagnostic can describe that same frame.
        std::string shot;
        if (next) {
            const std::lock_guard<std::mutex> lock(mShotMutex);
            shot.swap(mShotPath);
        }
        // Only a frame the game actually produced is drawn. Redrawing the held one would run its
        // frame feedback again and compound the tap on a frame that never existed.
        if (next) {
            mRenderer.describeNextFrame(!shot.empty());
            mRenderer.draw(*held);
            mRenderer.present();
            mRenderer.describeNextFrame(false);
        } else if (held) {
            mRenderer.represent();
        }
        if (!shot.empty()) {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(window, &w, &h);
            writeShot(shot, w, h);
        }
        SDL_GL_SwapWindow(window);
    }

    log.info("{} frames taken, {} sprites", mDrawn.load(), mSprites.load());
    const Renderer::Bounds& bounds = mRenderer.getBounds();
    if (bounds.seen) {
        log.info("sprite bounds x {:.1f}..{:.1f} y {:.1f}..{:.1f} (display {}x{})",
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
        log.info("cannot write {}", path);
        return;
    }
    const std::string header = std::format("P6\n{} {}\n255\n", width, height);
    std::fwrite(header.data(), 1, header.size(), file);
    std::fwrite(px.data(), 1, px.size(), file);
    std::fclose(file);
    log.info("wrote {} ({}x{})", path, width, height);
}

}  // namespace sdfhook
