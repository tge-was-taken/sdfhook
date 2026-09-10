#pragma once

#include "render/frame.h"
#include "types.h"

namespace sdfhook {

/// What a frame reads at call time that no packet carries. GS state and the bound texture are not
/// here: the chain walk replays them at each draw's packet.
struct RenderState {
    /// What dds3InitDrawEnvSub puts in the standard draw env, so the frame clears to it.
    Rgba background{0, 0, 0, 0};
};

}  // namespace sdfhook
