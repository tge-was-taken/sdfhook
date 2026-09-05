#include "frame.h"

namespace sdfhook {

Frame* FrameRing::acquireBuild() {
    const std::lock_guard<std::mutex> lock(mMutex);
    for (usize i = 0; i < kSlots; i++) {
        if (mState[i] != Slot::Free) continue;
        mState[i] = Slot::Building;
        mFrames[i].reset(mNextIndex++);
        return &mFrames[i];
    }
    // Every slot is spoken for, which means the render thread is behind. Reuse the oldest pending
    // one rather than stalling the guest.
    for (usize i = 0; i < kSlots; i++) {
        if (mState[i] != Slot::Pending) continue;
        mState[i] = Slot::Building;
        mFrames[i].reset(mNextIndex++);
        return &mFrames[i];
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

Frame* FrameRing::takePending() {
    const std::lock_guard<std::mutex> lock(mMutex);
    usize newest = kSlots;
    for (usize i = 0; i < kSlots; i++) {
        if (mState[i] == Slot::Pending &&
            (newest == kSlots || mFrames[i].index > mFrames[newest].index)) {
            newest = i;
        }
    }
    if (newest == kSlots) return nullptr;

    // Whatever was on screen is free now, and any older pending frame is skipped: the newest is the
    // only one worth drawing.
    for (usize i = 0; i < kSlots; i++) {
        if (i != newest && (mState[i] == Slot::Presenting || mState[i] == Slot::Pending)) {
            mState[i] = Slot::Free;
        }
    }
    mState[newest] = Slot::Presenting;
    return &mFrames[newest];
}

}  // namespace sdfhook
