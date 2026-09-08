#pragma once

#include <vector>

#include "hooks.h"
#include "types.h"

namespace sdfhook {

/// An inventory of what the game feeds the GS, taken by walking the frame's DMA chain rather than
/// by hooking the functions that build it. One row per chain entry, so what a chain-walk renderer
/// would have to decode can be counted before one is written.
void censusChain(u32 aTag, const std::vector<LinkedPacket>& aLinked, SymbolCache& aSymbols,
                 u32 aFrame, const std::unordered_map<u32, const char*>& aTypes);

}  // namespace sdfhook
