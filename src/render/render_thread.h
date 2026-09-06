#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

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
    void requestShot(const std::string& path);

    // Guest thread.
    Frame* beginFrame() { return mFrames.acquireBuild(); }
    void submit(Frame* frame) { mFrames.publish(frame); }

private:
    void run(int width, int height);
    void writeShot(const std::string& path, int width, int height);

    const Config* mConfig = nullptr;
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
    std::string mShotPath;
};

}  // namespace sdfhook
