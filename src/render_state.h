#pragma once

#include "render/frame.h"
#include "types.h"

namespace sdfhook {

/// The GS state the rset* calls put in force, which a draw is submitted under.
struct RenderState {
    GsAlpha alpha = GsAlpha::Blend;  // what rsetAlpha defaults to
    u32 test = 0;

    /// What rsetOtOfs last set: which ordering table a 2D draw goes into, and so its draw order.
    s32 otOffset = 0;

    /// Bound by sdf2DLinkTexPacket ahead of the draw that uses it, which is how a producer that
    /// takes no texture argument still names one.
    u32 texture = 0;

    /// What dds3InitDrawEnvSub puts in the standard draw env, so the frame clears to it.
    Rgba background{0, 0, 0, 0};
};

}  // namespace sdfhook
