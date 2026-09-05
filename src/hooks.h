#pragma once

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "pcsx2hook/pcsx2hook.h"
#include "gif.h"
#include "texture.h"
#include "render/frame.h"
#include "types.h"

namespace sdfhook {

class Trace;
class RenderThread;

// Read only once the frame stops growing: a packet is linked before its contents are written.
struct LinkedPacket {
    u32 addr;
    u32 caller;
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

// Arguments a post-handler needs are only in registers at entry, so a pre-handler records them and
// its post-handler takes them back. Keyed by pc and stacked, so recursion and reentrancy hold.
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

// Who called what, for functions hooked only to be watched. An edge from outside the module a
// function belongs to is a path the hooks do not capture.
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
/// Memoised, because resolving a symbol per call would cost more than the hook it serves.
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
    void attach(RenderThread* renderer) { mRenderer = renderer; }

    Frame* getFrame();
    void link(const LinkedPacket& packet) { mPending.push_back(packet); }
    std::vector<LinkedPacket>& getPackets() { return mPending; }

    void begin() { mSubmitted = false; }
    void submit();
    void end();

    u64 getSubmittedFrames() const { return mSubmittedFrames; }
    u64 getDiscardedFrames() const { return mDiscardedFrames; }

private:
    RenderThread* mRenderer = nullptr;
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
    // Vertices the GS will draw from a render function nothing here submits natively, by producer.
    std::map<std::string, u64> unhandledVerts;
};

/// The GS state the rset* calls put in force, which a draw is submitted under.
struct RenderState {
    GsAlpha alpha = GsAlpha::Blend;  // what rsetAlpha defaults to
    u32 test = 0;
    /// Bound by sdf2DLinkTexPacket ahead of the draw that uses it, which is how a producer that
    /// takes no texture argument still names one.
    u32 texture = 0;
    /// What dds3InitDrawEnvSub puts in the standard draw env, so the frame clears to it.
    Rgba background{0, 0, 0, 0};
};

// Owned by main.
struct HookContext {
    pcsx2hook_ctx* runtime = nullptr;
    Trace* trace = nullptr;
    SymbolCache symbols;
    FrameBuilder frames;
    TextureCache textures;
    CallStack calls;
    RenderState state;
    Coverage coverage;
    const char* dumpCaller = nullptr;
};
// o32: the first four arguments are in registers, the rest on the stack.

/// Every handler observes and returns, so the game still renders itself.
void installHooks(HookContext& context, Trace& trace, RenderThread* render);

}  // namespace sdfhook
