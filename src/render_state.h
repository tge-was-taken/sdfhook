#pragma once

#include "render/frame.h"
#include "types.h"

namespace sdfhook {

/// What producers read at call time that no packet of theirs carries. GS state is not here: the
/// chain walk replays it at each draw's packet.
struct RenderState {
    /// Bound by sdf2DLinkTexPacket ahead of the draw that uses it, which is how a producer that
    /// takes no texture argument still names one.
    u32 texture = 0;

    /// What dds3InitDrawEnvSub puts in the standard draw env, so the frame clears to it.
    Rgba background{0, 0, 0, 0};
};

}  // namespace sdfhook
