#pragma once

#include "hooks.h"

namespace sdfhook {

/// Turns the frame's DMA chain into the Frame the renderer draws. Each packet is handled by the
/// type recorded for it when it was built or linked; a model node's packets are consumed as one
/// span. The draws come from what the producers recorded, in the order the chain transmits them.
void processCommandStream(HookContext& h, u32 aTag, Frame& frame);

/// sdfImageSetTrans sends the queued uploads ahead of the chain, so what was read from GS memory
/// there is stale.
void readImageQueue(HookContext& h);

}  // namespace sdfhook
