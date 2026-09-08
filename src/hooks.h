#pragma once

#include <map>
#include <set>
#include <string>
#include <unordered_map>

#include "packet_type.h"
#include <vector>

#include "pcsx2hook/pcsx2hook.h"
#include "config.h"
#include "render_state.h"
#include "gif.h"
#include "hooks_impl_model.h"
#include "texture.h"
#include "render/frame.h"
#include "types.h"

namespace sdfhook {

class Trace;
class RenderThread;

// Read only once the frame stops growing: a packet is linked before its contents are written.

/// sdfModelLinkOt uses all three, so nothing else tells a node's transform packet from the
/// geometry beside it, and world geometry arrives only as a Call.
enum class LinkKind : u8 { Packet, Ref, Call };

struct LinkedPacket {
    u32 addr = 0;
    u32 caller = 0;
    LinkKind kind = LinkKind::Packet;
};

// o32: the first four arguments are in registers, the rest on the stack.
struct Context {
    struct Args {
        u32 a[8];
    };

    pcsx2hook_regs* regs;

    Args getArgs() const {
        Args out{};
        for (usize i = 0; i < 8; i++) out.a[i] = getArg(i);
        return out;
    }

    // The EE passes the first eight arguments in $a0-$a7 and the rest in 8-byte stack slots.
    u32 getArg(usize index) const { return regs->gpr[(4 + index) * 4]; }
    u32 getStackArgAddr(usize index) const { return getSp() + u32(index - 8) * 8; }
    f32 getFloatArg(usize index) const { return regs->fpr[12 + index]; }
    u32 getRa() const { return regs->gpr[31 * 4]; }
    u32 getSp() const { return regs->gpr[29 * 4]; }
    u32 getPc() const { return regs->pc; }
    u32 getResult() const { return regs->gpr[2 * 4]; }
};

using Handler = void (*)(Context&);

// Arguments live in registers only at entry, so a pre-handler records them for its post-handler.
// Keyed by pc and stacked, so recursion and reentrancy hold.
class CallStack {
public:
    void enter(const Context& c) {
        std::vector<Context::Args>& frames = mByFunction[c.getPc()];
        if (frames.size() < kMaxDepth) frames.push_back(c.getArgs());
    }

    Context::Args leave(const Context& c) {
        std::vector<Context::Args>& frames = mByFunction[c.getPc()];
        if (frames.empty()) return {};
        const Context::Args args = frames.back();
        frames.pop_back();
        return args;
    }

private:
    // A pre-handler that skips the body never reaches its post-handler, so the stack is bounded.
    static constexpr usize kMaxDepth = 16;
    std::unordered_map<u32, std::vector<Context::Args>> mByFunction;
};

// For functions hooked only to be watched: an edge from outside the module is an uncaptured path.
class CallGraph {
public:
    void record(const char* callee, const char* caller) {
        mEdges[std::string(callee) + " <- " + caller]++;
    }

    void recordReturn(const char* callee, u32 result) {
        Returns& seen = mReturns[callee];
        seen.count++;
        if (seen.distinct.size() < kMaxResults) seen.distinct.insert(result);
    }

    struct Returns {
        u64 count = 0;
        std::set<u32> distinct;
    };

    const std::map<std::string, u64>& getEdges() const { return mEdges; }
    const std::map<std::string, Returns>& getReturns() const { return mReturns; }

private:
    static constexpr usize kMaxResults = 256;
    std::map<std::string, u64> mEdges;
    std::map<std::string, Returns> mReturns;
};

// Owned by main.
/// Memoised: resolving per call costs more than the hook it serves.
class SymbolCache {
public:
    void attach(pcsx2hook_ctx* runtime) { mRuntime = runtime; }

    // A hooked address is always a function's entry, so its "+0x0" is noise.
    const char* getFunction(u32 address) { return lookup(address, true); }
    const char* getSite(u32 address) { return lookup(address, false); }

private:
    const char* lookup(u32 address, bool trimOffset);

    pcsx2hook_ctx* mRuntime = nullptr;
    std::unordered_map<u32, std::string> mNames;
};

/// The frame under construction and how it reaches the renderer.
class FrameBuilder {
public:
    void attach(RenderThread* renderer, TextureCache* textures, Trace* trace) {
        mRenderer = renderer;
        mTextures = textures;
        mTrace = trace;
    }

    Frame* getFrame();

    void link(const LinkedPacket& packet) {
        // The engine links a generated packet straight after generating it, which is the only
        // thing tying our nth to its address: it builds its own output buffer.
        if (mGenerating && packet.kind == LinkKind::Packet) {
            mGeneratedLinks.back() = packet.addr;
            mGenerating = false;
        }
        mPending.push_back(packet);
    }

    /// Closes a generation that linked nothing, keeping the nth entry the nth generation.
    void beginGenerated() {
        mGeneratedLinks.push_back(0);
        mGenerating = true;
    }

    std::vector<LinkedPacket>& getPackets() { return mPending; }
    const std::vector<u32>& getGeneratedLinks() const { return mGeneratedLinks; }

    void begin() { mSubmitted = false; }
    void submit();
    void end();

    u64 getSubmittedFrames() const { return mSubmittedFrames; }
    u64 getDiscardedFrames() const { return mDiscardedFrames; }

private:
    bool mAcquireFailed = false;
    std::vector<u32> mGeneratedLinks;
    bool mGenerating = false;
    RenderThread* mRenderer = nullptr;
    TextureCache* mTextures = nullptr;
    Trace* mTrace = nullptr;
    Frame* mBuilding = nullptr;
    std::vector<LinkedPacket> mPending;
    bool mSubmitted = false;
    u64 mSubmittedFrames = 0;
    u64 mDiscardedFrames = 0;
};

/// What a run covered, for the summary and for the checks that read it.
struct Coverage {
    GifCounts gif;
    CallGraph graph;
    u64 apiDraws = 0;
    // By producer: vertices the GS draws that nothing here submits natively.
    std::map<std::string, u64> unhandledVerts;

    /// Packets from an unhandled producer that carry no GIF geometry, which is what a billboard or
    /// anything else VU1 builds looks like from here. Counting only vertices hides them completely:
    /// the specters' producer linked 998 packets a run and ranked nowhere.
    std::map<std::string, u64> unhandledPackets;

    /// Every texture a producer's packets bind, so a draw sampling something the renderer never
    /// does names the function responsible instead of leaving it to be guessed.
    std::map<std::string, std::map<u32, u64>> tbpByProducer;

    /// Producers that named a texture the cache had nothing for.
    std::map<std::string, u64> unresolvedTextures;

    /// fldSdwDraw's inline VIF streams, read and refused, so a shadow pass that decodes short says
    /// so rather than rendering fewer shadows in silence.
    u64 shadowMeshes = 0;
    u64 shadowRejects = 0;
};


// Owned by main.
struct HookContext {
    pcsx2hook_ctx* runtime = nullptr;
    const Config* config = nullptr;
    Trace* trace = nullptr;
    SymbolCache symbols;
    FrameBuilder frames;
    TextureCache textures;
    CallStack calls;
    ModelParser model;
    ModelHooks modelHooks{model};
    RenderState state;
    Coverage coverage;

    /// What each packet is, recorded where it is built. A packet whose type is unknown cannot be
    /// processed without running the hardware that consumes it, so this has to be exhaustive.
    std::unordered_map<u32, PacketType> packetTypes;

    /// The chain the last frame send carried, so the engine's second draw of it is not walked
    /// twice.
    u32 lastChainHead = 0;

    /// So a chain frame's sprite count is against the API draws of that frame, not the run.
    u64 lastApiDraws = 0;
};
// o32: the first four arguments are in registers, the rest on the stack.

/// Every handler observes and returns, so the game still renders itself.
void installHooks(HookContext& context, Trace& trace, RenderThread* render);

}  // namespace sdfhook
