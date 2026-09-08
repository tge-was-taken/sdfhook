#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "pcsx2hook/pcsx2hook.h"

#include "../types.h"
#include "frame.h"
#include "../config.h"
#include "renderer.h"

namespace sdfhook {

// Owns the window, the GL context and the loop that draws, on a thread of its own. The guest thread
// only ever touches the frame ring, so a hook never blocks on the GPU.
class RenderThread {
public:
    bool open(const Config& config, int width, int height);
    void close();
    /// Asks for the next frame the game produces to be written out. The emulator thread waits for
    /// it, or the two shots are of different frames and the score moves between identical runs.
    void requestShot(const std::string& path);

    /// Blocks until a requested shot has been written, or the deadline passes.
    bool waitForShot(int milliseconds);

    /// Whether the viewer has asked for a capture since this was last called, and clears it.
    bool takeCaptureRequest() { return mCaptureWanted.exchange(false, std::memory_order_acq_rel); }

    /// Lets the window drive the pad. Set after the runtime exists, which is later than open().
    void attachPad(pcsx2hook_ctx* runtime) { mPad.store(runtime, std::memory_order_release); }

    // Guest thread.
    Frame* beginFrame() { return mFrames.acquireBuild(); }
    void submit(Frame* frame) { mFrames.publish(frame); }
    void abandon(Frame* frame) { mFrames.abandon(frame); }

private:
    void run(int width, int height);
    void writeShot(const std::string& path);
    void onKey(u32 scancode, bool pressed);
    void describeMesh(const Frame& frame, int index);

    const Config* mConfig = nullptr;
    std::atomic<pcsx2hook_ctx*> mPad{nullptr};
    std::atomic<bool> mCaptureWanted{false};
    // Picking a mesh to isolate: the keys step the selection, a click asks for the one under the
    // cursor. Both are raised on the event thread and acted on where the frame is in hand.
    std::atomic<int> mHighlight{-1};
    std::atomic<int> mHighlightStep{0};
    std::atomic<bool> mPickWanted{false};
    std::atomic<int> mPickX{0};
    std::atomic<int> mPickY{0};
    Renderer mRenderer;
    FrameRing mFrames;
    std::thread mThread;
    std::atomic<bool> mRunning{false};
    std::atomic<bool> mStop{false};
    std::atomic<bool> mReady{false};
    std::atomic<bool> mOpenFailed{false};
    std::atomic<u64> mDrawn{0};
    std::atomic<u64> mSprites{0};
    std::mutex mShotMutex;
    std::atomic<bool> mShotTaken{false};
    std::string mShotPath;
};

}  // namespace sdfhook
