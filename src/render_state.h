#pragma once

#include "render/frame.h"
#include "types.h"

namespace sdfhook {

/// The GS state the rset* calls put in force, which a draw is submitted under.
struct RenderState {
    GsAlpha alpha = GsAlpha::Blend;  // what rsetAlpha defaults to
    u32 test = 0;

    /// FRAME's write mask while the frame-mask pair is open: the silhouette pass draws the model
    /// again with RGB masked, to lay its shape into the alpha the bloom reads.
    u64 frameMask = 0;

    /// What rsetOtOfs last set: which ordering table a 2D draw goes into, and so its draw order.
    s32 otOffset = 0;

    /// Bound by sdf2DLinkTexPacket ahead of the draw that uses it, which is how a producer that
    /// takes no texture argument still names one.
    u32 texture = 0;

    /// What dds3InitDrawEnvSub puts in the standard draw env, so the frame clears to it.
    Rgba background{0, 0, 0, 0};

    /// Where the frame buffer and the light packet being built live, for the trace.
    u32 frameBuffer = 0;
    u32 currentLight = 0;
};

}  // namespace sdfhook
