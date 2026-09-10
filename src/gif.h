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

/// The A+D register writes a chain carries, replayed transfer by transfer in transmit order. A
/// GIF tag's writes can arrive in a later transfer than the tag: sdfGrSetRefGifPacket sends the
/// tag in its own packet and REFs the writes.
class GsRegisterReplay {
public:
    /// @return Whether the transfer held A+D tags and writes and nothing else, none of them to a
    /// register that kicks a vertex: the fade quad sends its geometry as A+D.
    bool apply(u32 packetAddr);

    const sdfGsState_t& getContext(u32 context) const { return mState.context[context]; }

private:
    /// @return Whether a write kicked a vertex.
    bool write(u32 at, u32 count);

    GsContexts mState;

    /// Writes a tag announced that its own transfer did not hold. Only the next transfer carries
    /// them on.
    u32 mPending = 0;
};

/// Whether a packet is A+D state writes and nothing else, judged on its own.
bool isRegisterWrites(u32 packetAddr);

}  // namespace sdfhook
