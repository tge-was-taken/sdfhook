#include "render_thread.h"

#include "../log.h"
#include "../image.h"

#include <SDL3/SDL.h>

#include <cstdio>

#include "glad/gl.h"

namespace sdfhook {
namespace {

LogChannel log("render");

// PCSX2's own default keyboard layout, so muscle memory carries over. The right stick is on the
// numpad because the letter keys are spoken for.
struct PadBinding {
    SDL_Scancode key;
    pcsx2hook_pad_button button;
};

constexpr PadBinding kPadBindings[] = {
    {SDL_SCANCODE_UP, PCSX2HOOK_PAD_UP},
    {SDL_SCANCODE_RIGHT, PCSX2HOOK_PAD_RIGHT},
    {SDL_SCANCODE_DOWN, PCSX2HOOK_PAD_DOWN},
    {SDL_SCANCODE_LEFT, PCSX2HOOK_PAD_LEFT},

    {SDL_SCANCODE_I, PCSX2HOOK_PAD_TRIANGLE},
    {SDL_SCANCODE_L, PCSX2HOOK_PAD_CIRCLE},
    {SDL_SCANCODE_K, PCSX2HOOK_PAD_CROSS},
    {SDL_SCANCODE_J, PCSX2HOOK_PAD_SQUARE},

    {SDL_SCANCODE_BACKSPACE, PCSX2HOOK_PAD_SELECT},
    {SDL_SCANCODE_RETURN, PCSX2HOOK_PAD_START},

    {SDL_SCANCODE_Q, PCSX2HOOK_PAD_L1},
    {SDL_SCANCODE_1, PCSX2HOOK_PAD_L2},
    {SDL_SCANCODE_E, PCSX2HOOK_PAD_R1},
    {SDL_SCANCODE_3, PCSX2HOOK_PAD_R2},
    {SDL_SCANCODE_Z, PCSX2HOOK_PAD_L3},
    {SDL_SCANCODE_C, PCSX2HOOK_PAD_R3},
    {SDL_SCANCODE_TAB, PCSX2HOOK_PAD_ANALOG},

    {SDL_SCANCODE_W, PCSX2HOOK_PAD_L_UP},
    {SDL_SCANCODE_D, PCSX2HOOK_PAD_L_RIGHT},
    {SDL_SCANCODE_S, PCSX2HOOK_PAD_L_DOWN},
    {SDL_SCANCODE_A, PCSX2HOOK_PAD_L_LEFT},

    {SDL_SCANCODE_KP_8, PCSX2HOOK_PAD_R_UP},
    {SDL_SCANCODE_KP_6, PCSX2HOOK_PAD_R_RIGHT},
    {SDL_SCANCODE_KP_2, PCSX2HOOK_PAD_R_DOWN},
    {SDL_SCANCODE_KP_4, PCSX2HOOK_PAD_R_LEFT},
};

}  // namespace

void RenderThread::onKey(u32 scancode, bool pressed) {
    // The capture runs on the CPU thread, where the savestate and the frame buffer are safe to
    // take, so the key only raises the request.
    if (pressed && SDL_Scancode(scancode) == SDL_SCANCODE_F9) {
        mCaptureWanted.store(true, std::memory_order_release);
        return;
    }

    pcsx2hook_ctx* runtime = mPad.load(std::memory_order_acquire);
    if (!runtime) return;
    for (const PadBinding& binding : kPadBindings) {
        if (binding.key != SDL_Scancode(scancode)) continue;
        pcsx2hook_set_pad_button(runtime, binding.button, pressed ? 1 : 0);
        return;
    }
}

bool RenderThread::open(const Config& config, int width, int height) {
    mConfig = &config;
    mFrames.setDropWhenBehind(config.window);
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
    // Shown means driven, so it takes focus and the keyboard like any other window. Hidden runs
    // keep the flags that stop it stealing focus from the terminal that started them.
    const SDL_WindowFlags interactive = mConfig->window
        ? SDL_WindowFlags(0)
        : SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_UTILITY;
    SDL_Window* window = SDL_CreateWindow(
        "sdfhook", width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | interactive);
    if (!window) {
        log.info("SDL_CreateWindow: {}", SDL_GetError());
        mOpenFailed.store(true, std::memory_order_release);
        return;
    }
    SDL_SetWindowFocusable(window, mConfig->window);
    if (mConfig->window) {
        SDL_ShowWindow(window);
        log.info("pad: arrows d-pad, WASD stick, IJKL triangle/square/cross/circle, "
                 "Q/E L1/R1, 1/3 L2/R2, Z/C L3/R3, backspace select, return start, tab analog");
        log.info("F9 captures the scene: a savestate and both shots, into {}",
                 mConfig->captureDir);
    }
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
            if (event.type == SDL_EVENT_QUIT) {
                log.info("window closed; the emulator keeps running headless");
                mStop.store(true, std::memory_order_relaxed);
            }
            // repeat is the autorepeat SDL synthesises; the pad only wants edges.
            if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
                !event.key.repeat) {
                onKey(event.key.scancode, event.type == SDL_EVENT_KEY_DOWN);
            }
        }
        const Frame* next = mFrames.takePending();
        if (next) {
            held = next;
            mDrawn.fetch_add(1, std::memory_order_relaxed);
            mSprites.fetch_add(next->sprites.size(), std::memory_order_relaxed);
        }
        // Taken whether or not a frame arrived this iteration: the emulator blocks on the request,
        // so nothing new can be submitted, and waiting for a frame that cannot come is what made a
        // shot land on a different frame than the emulated one between identical runs.
        std::string shot;
        {
            const std::lock_guard<std::mutex> lock(mShotMutex);
            if (!mShotPath.empty() && (next || held)) shot.swap(mShotPath);
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
            writeShot(shot);
            log.info("shot of frame {} ({} drawn, {} arrived this pass)", held ? held->index : 0,
                     mDrawn.load(), next ? 1 : 0);
            mShotTaken.store(true, std::memory_order_release);
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
    mShotTaken.store(false, std::memory_order_release);
}

bool RenderThread::waitForShot(int milliseconds) {
    for (int waited = 0; waited < milliseconds; waited++) {
        if (mShotTaken.load(std::memory_order_acquire)) return true;
        if (!mRunning.load(std::memory_order_acquire)) return false;
        SDL_Delay(1);
    }
    return false;
}

void RenderThread::writeShot(const std::string& path) {
    int width = 0;
    int height = 0;
    const std::vector<u8> px = mRenderer.readPixels(&width, &height);
    writePng(path.c_str(), u32(width), u32(height), px.data(), false);
    log.info("wrote {} ({}x{})", path, width, height);
}

}  // namespace sdfhook
