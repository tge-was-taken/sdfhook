#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>
#include <filesystem>
#include <cstdlib>
#include <memory>
#include <string>

#include "hooks.h"
#include "render/render_thread.h"
#include "pcsx2hook/pcsx2hook.h"
#include "trace.h"
#include "unsupported.h"

namespace {

struct App {
    pcsx2hook_ctx* runtime = nullptr;
    std::unique_ptr<sdfhook::RenderThread> renderer;
    std::unique_ptr<sdfhook::Trace> trace;
    sdfhook::HookContext hooks;
};

App g_app;

const char* env(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

// The state a run came from, kept beside its trace: a dump nobody can reproduce is worth little a
// week later, and the state is the only part that cannot be recovered from the rest.
void copyStateBesideTrace(const std::string& tracePath) {
    const char* state = std::getenv("PCSX2HOOK_STATE");
    if (!state) return;
    std::error_code ec;
    const std::filesystem::path beside =
        std::filesystem::path(tracePath).parent_path() / "state.p2s";
    std::filesystem::copy_file(state, beside, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) std::fprintf(stderr, "[sdf] could not keep the state beside the trace: %s\n",
                         ec.message().c_str());
}

// Hooked here because the symbol map is only readable once the VM knows its disc.
void onVmReady(void* user) {
    App& app = *static_cast<App*>(user);
    const std::string tracePath = env("SDF_TRACE", "dumps/trace.jsonl");
    app.trace = std::make_unique<sdfhook::Trace>(app.runtime, tracePath);
    copyStateBesideTrace(tracePath);
    app.hooks.runtime = app.runtime;
    app.hooks.dumpCaller = std::getenv("SDF_DUMP_PACKETS");
    if (const char* dir = std::getenv("SDF_DUMP_TEXTURES")) app.hooks.textures.dumpTo(dir);
    sdfhook::installHooks(app.hooks, *app.trace, app.renderer.get());
    if (std::getenv("PCSX2HOOK_GS_TRACE")) pcsx2hook_set_gs_recording(app.runtime, 1);
}

void onFrame(uint32_t frame, void* user) {
    App& app = *static_cast<App*>(user);
    if (app.trace) app.trace->onFrame(frame);
    if (const char* at = std::getenv("SDF_SHOT_FRAME");
        at && frame == std::strtoul(at, nullptr, 10)) {
        pcsx2hook_write_framebuffer(app.runtime, env("SDF_SHOT", "dumps/shot.ppm"));
        if (app.renderer) app.renderer->requestShot(env("SDF_NATIVE_SHOT", "dumps/native.ppm"));
    }
}

}  // namespace

int main(int argc, char** argv) {
    pcsx2hook_config config{};
    config.bios = env("SDF_BIOS", "assets/bios.bin");
    config.symbols = std::getenv("SDF_SYMBOLS");
    config.surfaceless = 1;
    config.software_gs = 1;
    config.on_vm_ready = &onVmReady;
    config.on_frame = &onFrame;
    config.user = &g_app;

    // Opened before the VM so the render thread owns its context from the start. Without SDF_WINDOW
    // the run stays headless and the hooks only trace.
    if (std::getenv("SDF_WINDOW")) {
        g_app.renderer = std::make_unique<sdfhook::RenderThread>();
        if (!g_app.renderer->open(1024, 896)) {
            std::fprintf(stderr, "sdfhook: no window; tracing only\n");
            g_app.renderer.reset();
        }
    }

    g_app.runtime = pcsx2hook_create(&config);
    if (!g_app.runtime) {
        std::fprintf(stderr, "sdfhook: cannot create the runtime\n");
        return 1;
    }

    // Every GS primitive batch the rasterizer resolves, which is what a draw has to be attributed
    // to. PCSX2HOOK_GS_TRACE names the file the runner writes them to.
    if (std::getenv("PCSX2HOOK_GS_TRACE")) {
        pcsx2hook_enable_trace(g_app.runtime, PCSX2HOOK_TRACE_GS);
    }

    // The runner's own cap, so a capped run still reaches the writes it does at shutdown.
    if (const char* frames = std::getenv("SDF_FRAMES")) {
        pcsx2hook_set_config(g_app.runtime, "PCSX2HOOK_FRAMES", frames);
    }

    pcsx2hook_early_init(g_app.runtime);
    if (!pcsx2hook_init(g_app.runtime, argc, argv)) {
        std::fprintf(stderr, "sdfhook: %s\n", pcsx2hook_get_last_error(g_app.runtime));
        pcsx2hook_destroy(g_app.runtime);
        return 1;
    }
    pcsx2hook_apply_settings(g_app.runtime);

    if (!pcsx2hook_start(g_app.runtime)) {
        std::fprintf(stderr, "sdfhook: %s\n", pcsx2hook_get_last_error(g_app.runtime));
        pcsx2hook_destroy(g_app.runtime);
        return 1;
    }
    pcsx2hook_pump_window(g_app.runtime, 1);

    const int code = pcsx2hook_join(g_app.runtime);
    const sdfhook::HookContext& hooks = g_app.hooks;
    std::fprintf(stderr, "[sdf] %llu frames submitted, %llu discarded by dds3DrawFlag\n",
                 (unsigned long long)hooks.frames.getSubmittedFrames(), (unsigned long long)hooks.frames.getDiscardedFrames());
    static const char* const kPrimNames[] = {"point",    "line",   "lineStrip", "tri",
                                             "triStrip", "triFan", "sprite",    "?"};
    std::fprintf(stderr, "[sdf] decoded");
    for (int i = 0; i < 8; i++) {
        if (!hooks.coverage.gif.prims[i]) continue;
        std::fprintf(stderr, " %llu %s (%llu verts)", (unsigned long long)hooks.coverage.gif.prims[i],
                     kPrimNames[i], (unsigned long long)hooks.coverage.gif.verts[i]);
    }
    if (hooks.coverage.gif.unparsed) {
        std::fprintf(stderr, ", %llu packets not GIF lists",
                     (unsigned long long)hooks.coverage.gif.unparsed);
    }
    std::fputc('\n', stderr);

    // The texture module's internals should only ever be reached from inside it, or from the call
    // the hooks capture. Any other caller is a registration path these hooks never saw.
    static const char* const kTexInternals[] = {
        "sdfTexSetEx",  "sdfTexTrans",        "sdfTexClutToGs",     "sdfTexAllocClut",
        "sdfTexCopyClut", "sdfTexDup",        "sdfTexSetClutColor", "sdfTexMakeGIFPacket"};
    std::vector<std::pair<std::string, uint64_t>> left(hooks.coverage.unhandledVerts.begin(),
                                                      hooks.coverage.unhandledVerts.end());
    std::sort(left.begin(), left.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    sdfhook::printUnsupported();
    for (const auto& [producer, verts] : left) {
        std::fprintf(stderr, "[todo] %-34s %llu verts not drawn natively\n", producer.c_str(),
                     (unsigned long long)verts);
    }
    for (const auto& [callee, seen] : hooks.coverage.graph.getReturns()) {
        std::fprintf(stderr, "[tex] %s returned %llu times, %zu distinct\n", callee.c_str(),
                     (unsigned long long)seen.count, seen.distinct.size());
    }
    for (const auto& [edge, count] : hooks.coverage.graph.getEdges()) {
        const std::string caller = edge.substr(edge.find("<- ") + 3);
        bool known = false;
        for (const char* prefix : kTexInternals) known |= caller.rfind(prefix, 0) == 0;
        std::fprintf(stderr, "[tex] %s x%llu%s\n", edge.c_str(), (unsigned long long)count,
                     known ? "" : "   <- UNHOOKED PATH");
    }
    std::fprintf(stderr, "[sdf] %llu draws from the engine API, %llu textures decoded, %llu failed\n",
                 (unsigned long long)hooks.coverage.apiDraws, (unsigned long long)hooks.textures.getDecoded(),
                 (unsigned long long)hooks.textures.getFailed());
    std::fprintf(stderr, "[sdf] %llu glyph uploads, font clut %s\n",
                 (unsigned long long)hooks.textures.getGlyphUploads(),
                 hooks.textures.hasFontClut() ? "loaded" : "never seen");
    std::fprintf(stderr, "[gs] %llu untextured draws", (unsigned long long)hooks.coverage.gif.untexturedDraws);
    for (int i = 0; i < 64; i++) {
        if (hooks.coverage.gif.texDraws[i]) {
            std::fprintf(stderr, ", psm 0x%02X x%llu", i,
                         (unsigned long long)hooks.coverage.gif.texDraws[i]);
        }
    }
    std::fputc('\n', stderr);
    // Before the emulator tears down: the render thread holds frames the guest thread wrote, and
    // SDL's own signal handlers sit on top of the ones PCSX2's virtual memory needs.
    if (g_app.renderer) g_app.renderer->close();
    pcsx2hook_flush_traces(g_app.runtime);
    pcsx2hook_shutdown(g_app.runtime);
    g_app.trace.reset();
    pcsx2hook_destroy(g_app.runtime);
    return code;
}
