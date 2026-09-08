#pragma once

#include "hooks.h"
#include "render/frame.h"
#include "types.h"

namespace sdfhook {

/// What one chain walk produced, so it can be compared against the frame the API hooks built
/// before either is trusted to draw.
struct ChainFrameCounts {
    u32 entries = 0;
    u32 sprites = 0;
    u32 registerWrites = 0;
    u32 opaque = 0;
    u32 verticesDropped = 0;

    /// The bounds the walk produced, so a gross coordinate error shows without a render.
    f32 minX = 0.0f, maxX = 0.0f, minY = 0.0f, maxY = 0.0f;
};

/// Builds a frame from the DMA chain alone, dispatching each packet on its type. The prototype of
/// a renderer driven by what the game submits rather than by the calls that submit it.
ChainFrameCounts buildFrameFromChain(u32 aTagHead, HookContext& h, Frame* frame);

}  // namespace sdfhook
