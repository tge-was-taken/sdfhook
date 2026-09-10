#pragma once

#include <memory>
#include <vector>

#include "guest_ptr.h"
#include "libsdf.h"
#include "types.h"

namespace sdfhook {

/// What a run's packets held, by GS primitive type: how much of a frame it accounts for, and
/// what is still going undrawn.
struct GifCounts {
    u64 verts[8] = {};
    u64 prims[8] = {};
    u64 texDraws[64] = {};  // textured draws by TEX0 pixel storage mode
    u64 untexturedDraws = 0;
    u64 unparsed = 0;  // packets whose quadwords are not a GIF list, mostly VU1 chains
};

/// Spans a whole frame rather than one packet, because GS state does.
class GifParser {
public:
    explicit GifParser(GifCounts* counts);
    ~GifParser();

    void parse(u32 packetAddr);

    /// TBP0 of every texture the last parse bound, so a draw that samples something we never do can
    /// be attributed to the producer that linked its packet.
    const std::vector<u32>& getParsedTbps() const;

private:
    class State;
    std::unique_ptr<State> mState;
};

/// ALPHA, TEST and FRAME as the GS keeps them, once per drawing context. PRIM's CTXT bit picks
/// which one a draw runs under.
struct GsContexts {
    sdfGsState_t context[2];
};

/// Applies the A+D register writes a packet opens with to the running GS state.
/// @return Whether they are the whole packet: pure register writes draw nothing.
bool applyGsRegisterWrites(u32 packetAddr, GsContexts& state);

}  // namespace sdfhook
