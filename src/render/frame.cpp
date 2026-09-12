#include "frame.h"

#include <chrono>

namespace sdfhook {

Frame* FrameRing::acquireBuild() {
    std::unique_lock<std::mutex> lock(mMutex);
    for (usize attempt = 0; attempt < 2; attempt++) {
        for (usize i = 0; i < kSlots; i++) {
            if (mState[i] != Slot::Free) continue;
            mState[i] = Slot::Building;
            mFrames[i].reset(mNextIndex++);
            takeCarriedUploads(mFrames[i]);
            return &mFrames[i];
        }
        if (mDropWhenBehind) {
            for (usize i = 0; i < kSlots; i++) {
                if (mState[i] != Slot::Pending) continue;
                mState[i] = Slot::Building;
                carryUploads(mFrames[i]);
                mFrames[i].reset(mNextIndex++);
                takeCarriedUploads(mFrames[i]);
                return &mFrames[i];
            }
            return nullptr;
        }
        // A render thread that has stopped must not hang the emulator, so the wait is bounded and
        // the second pass gives up rather than waiting again.
        if (!attempt) mFreed.wait_for(lock, std::chrono::seconds(2));
    }
    return nullptr;
}

void FrameRing::publish(Frame* frame) {
    const std::lock_guard<std::mutex> lock(mMutex);
    for (usize i = 0; i < kSlots; i++) {
        if (&mFrames[i] != frame) continue;
        mState[i] = Slot::Pending;
        return;
    }
}

void FrameRing::abandon(Frame* frame) {
    const std::lock_guard<std::mutex> lock(mMutex);
    for (usize i = 0; i < kSlots; i++) {
        if (&mFrames[i] != frame || mState[i] != Slot::Building) continue;
        carryUploads(mFrames[i]);
        mState[i] = Slot::Free;
        mFreed.notify_one();
        return;
    }
}

void FrameRing::carryUploads(Frame& frame) {
    for (TextureUpload& upload : frame.textureUploads) mCarried.push_back(std::move(upload));
    frame.textureUploads.clear();
}

void FrameRing::takeCarriedUploads(Frame& frame) {
    frame.textureUploads = std::move(mCarried);
    mCarried.clear();
}

Frame* FrameRing::takePending() {
    const std::lock_guard<std::mutex> lock(mMutex);
    usize oldest = kSlots;
    for (usize i = 0; i < kSlots; i++) {
        if (mState[i] == Slot::Pending &&
            (oldest == kSlots || mFrames[i].index < mFrames[oldest].index)) {
            oldest = i;
        }
    }
    if (oldest == kSlots) return nullptr;

    for (usize i = 0; i < kSlots; i++) {
        if (i != oldest && mState[i] == Slot::Presenting) mState[i] = Slot::Free;
    }
    mState[oldest] = Slot::Presenting;
    mFreed.notify_one();
    return &mFrames[oldest];
}

}  // namespace sdfhook
