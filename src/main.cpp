#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "guest_ptr.h"
#include "hooks.h"
#include "log.h"
#include "pcsx2hook/pcsx2hook.h"
#include "render/render_thread.h"
#include "trace.h"
#include "json.h"
#include "unsupported.h"

namespace {


using sdfhook::JsonObject;

// A frame is submitted once per sdfGrReqDraw, which can be several emulated frames apart.
constexpr int kShotWaitMs = 5000;

sdfhook::LogChannel log("run");
sdfhook::LogChannel texLog("tex");
sdfhook::LogChannel todoLog("todo");
sdfhook::LogChannel gsLog("gs");


struct App {
    pcsx2hook_ctx* runtime = nullptr;
    sdfhook::Config config;
    std::unique_ptr<sdfhook::RenderThread> renderer;
    std::unique_ptr<sdfhook::Trace> trace;
    sdfhook::HookContext hooks;
};

App g_app;

// The state a run came from, kept beside its trace: a dump nobody can reproduce is worth little a
// week later, and the state is the only part that cannot be recovered from the rest.
void copyStateBesideTrace(const sdfhook::Config& config) {
    if (config.statePath.empty()) return;
    std::error_code ec;
    const std::filesystem::path beside =
        std::filesystem::path(config.tracePath).parent_path() / "state.p2s";
    std::filesystem::copy_file(config.statePath, beside,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) log.info("could not keep the state beside the trace: {}", ec.message());
}

// Hooked here because the symbol map is only readable once the VM knows its disc.
void onVmReady(void* user) {
    App& app = *static_cast<App*>(user);
    app.trace = std::make_unique<sdfhook::Trace>(app.runtime, app.config.tracePath);
    copyStateBesideTrace(app.config);
    // Hooks and this callback share the emulator's CPU thread, so one install covers both.
    uint32_t ramSize = 0;
    void* const ram = pcsx2hook_get_ram_base(app.runtime, &ramSize);
    sdfhook::installGuestRam(ram, ramSize);

    app.hooks.runtime = app.runtime;
    app.hooks.config = &app.config;
    if (!app.config.dumpTextures.empty()) app.hooks.textures.dumpTo(app.config.dumpTextures);
    sdfhook::installHooks(app.hooks, *app.trace, app.renderer.get());
    if (app.config.gsTrace) pcsx2hook_set_gs_recording(app.runtime, 1);
}

/// A scene as the sweep needs it: the state that reproduces it and the pair of shots to score. The
/// native shot is taken first and waited for, so both are of this frame rather than of two.
void captureScene(App& app, uint32_t frame) {
    std::error_code failed;
    std::filesystem::create_directories(app.config.captureDir, failed);

    static u32 index = 0;
    const std::string base = std::format("{}/capture_{:03}", app.config.captureDir, index++);
    app.renderer->requestShot(base + "-native.png");
    if (!app.renderer->waitForShot(kShotWaitMs)) {
        log.warn("no native shot within {}ms: the capture is of two frames", kShotWaitMs);
    }
    pcsx2hook_write_framebuffer(app.runtime, (base + "-emu.png").c_str());
    // The state is written once the VM reaches a safe point, which is after this frame rather than
    // during it, so the two shots land first and the file appears a moment later.
    pcsx2hook_save_state(app.runtime, (base + ".p2s").c_str());
    log.info("captured frame {}: {}-emu.png, {}-native.png, and {}.p2s to follow", frame, base,
             base, base);
}

void onFrame(uint32_t frame, void* user) {
    App& app = *static_cast<App*>(user);
    if (app.trace) app.trace->onFrame(frame);
    app.hooks.textures.recoverQueued();
    if (frame % 600 == 0) log.debug("frame {}", frame);

    const bool wanted = app.config.captureFrame && frame == app.config.captureFrame;
    if (app.renderer && (wanted || app.renderer->takeCaptureRequest())) captureScene(app, frame);

    if (app.config.shotFrame && frame == app.config.shotFrame) {
        // The native shot goes first and is waited for, so both are of the same emulated frame.
        // Taking them independently is what made a run's score move without the render changing.
        if (app.renderer) {
            app.renderer->requestShot(app.config.nativeShotPath);
            if (!app.renderer->waitForShot(kShotWaitMs)) {
                log.warn("no native shot within {}ms: the two are of different frames", kShotWaitMs);
            }
        }
        pcsx2hook_write_framebuffer(app.runtime, app.config.shotPath.c_str());
    }
}

/// The same totals as one JSON row at the end of the run's own trace. The text below is what a
/// person reads; this is what two runs are diffed on, which is otherwise a grep for "MISMATCH".
void writeRunSummary(const sdfhook::HookContext& hooks) {
    if (!hooks.trace) return;

    JsonObject row;
    row.addString("ev", "summary")
        .addUint("framesSubmitted", hooks.frames.getSubmittedFrames())
        .addUint("framesDiscarded", hooks.frames.getDiscardedFrames())
        .addUint("apiDraws", hooks.coverage.apiDraws)
        .addUint("unsupported", sdfhook::getUnsupportedCount());

    row.beginObject("gif");
    static const char* const kPrimNames[] = {"point",    "line",   "lineStrip", "tri",
                                             "triStrip", "triFan", "sprite",    "unknown"};
    for (int i = 0; i < 8; i++) {
        row.beginObject(kPrimNames[i])
            .addUint("batches", hooks.coverage.gif.prims[i])
            .addUint("verts", hooks.coverage.gif.verts[i])
            .endObject();
    }
    row.addUint("notGifLists", hooks.coverage.gif.unparsed)
        .addUint("untexturedDraws", hooks.coverage.gif.untexturedDraws);
    row.beginArray("texDraws");
    for (int i = 0; i < 64; i++) {
        if (!hooks.coverage.gif.texDraws[i]) continue;
        row.beginObject().addHexDigits("psm", u32(i), 2)
            .addUint("draws", hooks.coverage.gif.texDraws[i]).endObject();
    }
    row.endArray().endObject();

    row.beginObject("textures")
        .addUint("decoded", hooks.textures.getDecoded())
        .addUint("freed", hooks.textures.getFreed())
        .addUint("failed", hooks.textures.getFailed())
        .addUint("recoveredFromGs", hooks.textures.getRecovered())
        .addUint("recoveryAttempts", hooks.textures.getRecoveryAttempts())
        .addUint("stillQueued", hooks.textures.getQueued())
        .addUint("glyphUploads", hooks.textures.getGlyphUploads())
        .addBool("fontClut", hooks.textures.hasFontClut());
    row.beginArray("unresolvedByProducer");
    for (const auto& [producer, count] : hooks.coverage.unresolvedTextures) {
        row.beginObject().addString("producer", producer.c_str()).addUint("draws", count).endObject();
    }
    row.endArray().endObject();

    // Ranked, because the largest gap is the next producer worth hooking and intuition gets it wrong.
    std::vector<std::pair<std::string, uint64_t>> left(hooks.coverage.unhandledVerts.begin(),
                                                       hooks.coverage.unhandledVerts.end());
    std::sort(left.begin(), left.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    row.beginArray("todo");
    for (const auto& [producer, verts] : left) {
        row.beginObject().addString("producer", producer.c_str()).addUint("verts", verts).endObject();
    }
    row.endArray();

    // What reached the renderer: a draw is only drawn once the chain walk finds its packet.
    const auto byProducer = [&](const char* name, const std::map<std::string, u64>& counts) {
        row.beginArray(name);
        for (const auto& [producer, count] : counts) {
            row.beginObject().addString("producer", producer.c_str()).addUint("draws", count).endObject();
        }
        row.endArray();
    };
    byProducer("placedDraws", hooks.coverage.placedDraws);
    byProducer("unlinkedDraws", hooks.coverage.unlinkedDraws);
    byProducer("unreachedDraws", hooks.coverage.unreachedDraws);
    byProducer("decodedDraws", hooks.coverage.decodedDraws);
    byProducer("refusedDraws", hooks.coverage.refusedDraws);

    hooks.model.getTrace().addTo(row);
    hooks.trace->onRunSummary(row.take());
}

/// What the run covered, printed once at shutdown. Every count here is a claim the next run has to
/// keep matching, so they are reported together rather than scattered through the frame.
void reportRun(const sdfhook::HookContext& hooks) {
    log.info("{} frames submitted, {} discarded by dds3DrawFlag",
        hooks.frames.getSubmittedFrames(), hooks.frames.getDiscardedFrames());

    static const char* const kPrimNames[] = {"point",    "line",   "lineStrip", "tri",
                                             "triStrip", "triFan", "sprite",    "?"};
    std::string decoded = "decoded";
    for (int i = 0; i < 8; i++) {
        if (!hooks.coverage.gif.prims[i]) continue;
        decoded += std::format(" {} {} ({} verts)", hooks.coverage.gif.prims[i], kPrimNames[i],
                               hooks.coverage.gif.verts[i]);
    }
    if (hooks.coverage.gif.unparsed) {
        decoded += std::format(", {} packets not GIF lists", hooks.coverage.gif.unparsed);
    }
    log.info("{}", decoded);

    hooks.model.getTrace().report();

    texLog.info("{} recovered from GS memory, {} attempts, {} still queued",
                hooks.textures.getRecovered(), hooks.textures.getRecoveryAttempts(),
                hooks.textures.getQueued());
    for (const auto& [producer, count] : hooks.coverage.unresolvedTextures) {
        texLog.info("{:<24} named a texture we never decoded, {} times", producer, count);
    }

    // The texture module's internals should only ever be reached from inside it, or from the call
    // the hooks capture. Any other caller is a registration path these hooks never saw.
    static const char* const kTexInternals[] = {
        "sdfTexSetEx",    "sdfTexTrans", "sdfTexClutToGs",     "sdfTexAllocClut",
        "sdfTexCopyClut", "sdfTexDup",   "sdfTexSetClutColor", "sdfTexMakeGIFPacket"};

    std::vector<std::pair<std::string, uint64_t>> left(hooks.coverage.unhandledVerts.begin(),
                                                       hooks.coverage.unhandledVerts.end());
    std::sort(left.begin(), left.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    sdfhook::printUnsupported();
    for (const auto& [producer, verts] : left) {
        todoLog.info("{:<34} {} verts not drawn natively", producer, verts);
    }

    // A producer whose geometry VU1 builds carries no GIF vertices, so it never reaches the ranking
    // above however much it draws. Ranked apart rather than merged: these are packet counts.
    std::vector<std::pair<std::string, uint64_t>> silent(hooks.coverage.unhandledPackets.begin(),
                                                         hooks.coverage.unhandledPackets.end());
    std::sort(silent.begin(), silent.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (const auto& [producer, packets] : silent) {
        todoLog.info("{:<34} {} packets with no GIF geometry, not drawn natively", producer,
                     packets);
    }

    if (hooks.coverage.shadowMeshes || hooks.coverage.shadowRejects) {
        todoLog.info("fldSdwDraw shadow streams: {} read, {} refused", hooks.coverage.shadowMeshes,
                     hooks.coverage.shadowRejects);
    }

    // Only a draw the chain walk placed is rendered, so the other two are draws lost.
    for (const auto& [producer, placed] : hooks.coverage.placedDraws) {
        const auto count = [&](const std::map<std::string, u64>& counts) {
            const auto found = counts.find(producer);
            return found == counts.end() ? u64(0) : found->second;
        };
        log.info("{:<34} {} placed, {} unlinked, {} unreached", producer, placed,
                 count(hooks.coverage.unlinkedDraws), count(hooks.coverage.unreachedDraws));
    }
    for (const auto& [producer, lost] : hooks.coverage.unlinkedDraws) {
        if (!hooks.coverage.placedDraws.count(producer)) todoLog.info("{:<34} {} unlinked, none placed", producer, lost);
    }
    for (const auto& [producer, lost] : hooks.coverage.unreachedDraws) {
        if (!hooks.coverage.placedDraws.count(producer)) todoLog.info("{:<34} {} unreached, none placed", producer, lost);
    }

    for (const auto& [producer, tbps] : hooks.coverage.tbpByProducer) {
        std::string list;
        for (const auto& [tbp, count] : tbps) {
            list += std::format("{}x{} ", tbp, count);
        }
        texLog.info("{:<30} binds {}", producer, list);
    }

    for (const auto& [callee, seen] : hooks.coverage.graph.getReturns()) {
        texLog.info("{} returned {} times, {} distinct", callee, seen.count, seen.distinct.size());
    }
    for (const auto& [edge, count] : hooks.coverage.graph.getEdges()) {
        const std::string caller = edge.substr(edge.find("<- ") + 3);
        bool known = false;
        for (const char* prefix : kTexInternals) known |= caller.rfind(prefix, 0) == 0;
        texLog.info("{} x{}{}", edge, count, known ? "" : "   <- UNHOOKED PATH");
    }

    log.info("{} draws from the engine API, {} textures decoded, {} failed, {} freed",
        hooks.coverage.apiDraws, hooks.textures.getDecoded(), hooks.textures.getFailed(),
        hooks.textures.getFreed());
    log.info("{} glyph uploads, font clut {}", hooks.textures.getGlyphUploads(),
        hooks.textures.hasFontClut() ? "loaded" : "never seen");

    std::string gs = std::format("{} untextured draws", hooks.coverage.gif.untexturedDraws);
    for (int i = 0; i < 64; i++) {
        if (hooks.coverage.gif.texDraws[i]) {
            gs += std::format(", psm 0x{:02X} x{}", i, hooks.coverage.gif.texDraws[i]);
        }
    }
    gsLog.info("{}", gs);
}

}  // namespace

int main(int argc, char** argv) {
    g_app.config = sdfhook::Config::readEnvironment();
    const sdfhook::Config& cfg = g_app.config;
    sdfhook::configureLogging(cfg.logSpec);

    pcsx2hook_config config{};
    config.bios = cfg.bios.c_str();
    config.symbols = cfg.symbols.empty() ? nullptr : cfg.symbols.c_str();
    config.surfaceless = 1;
    config.software_gs = 1;
    config.on_vm_ready = &onVmReady;
    config.on_frame = &onFrame;
    config.user = &g_app;

    // Opened before the VM so the render thread owns its context from the start. Without a shot to
    // write or a window to show there is nothing to render for, and the hooks only trace.
    if (cfg.window || !cfg.nativeShotPath.empty()) {
        g_app.renderer = std::make_unique<sdfhook::RenderThread>();
        if (!g_app.renderer->open(cfg, 512 * cfg.scale, 448 * cfg.scale)) {
            log.warn("no window; tracing only");
            g_app.renderer.reset();
        }
    }

    g_app.runtime = pcsx2hook_create(&config);
    if (!g_app.runtime) {
        log.error("cannot create the runtime");
        return 1;
    }

    if (g_app.renderer) g_app.renderer->attachPad(g_app.runtime);

    // A window is there to be watched, so it runs at 1x. Unthrottled the guest outruns a
    // vsync-locked render thread by several frames each pass and spends the run inside the ring.
    pcsx2hook_set_speed_limit(g_app.runtime, cfg.window ? 1 : 0);

    // Every GS primitive batch the rasterizer resolves, which is what a draw has to be attributed
    // to. PCSX2HOOK_GS_TRACE names the file the runner writes them to.
    if (cfg.gsTrace) pcsx2hook_enable_trace(g_app.runtime, PCSX2HOOK_TRACE_GS);

    // The VU1 microprograms are ground truth for what the hardware does to a vertex: the lighting,
    // the color scale and the fog term are all in there rather than in any struct we can read.
    if (cfg.vuTrace) pcsx2hook_enable_trace(g_app.runtime, PCSX2HOOK_TRACE_VU);

    // The runner's own cap, so a capped run still reaches the writes it does at shutdown.
    if (!cfg.frames.empty()) {
        pcsx2hook_set_config(g_app.runtime, "PCSX2HOOK_FRAMES", cfg.frames.c_str());
    }

    pcsx2hook_early_init(g_app.runtime);
    if (!pcsx2hook_init(g_app.runtime, argc, argv)) {
        log.error("{}", pcsx2hook_get_last_error(g_app.runtime));
        pcsx2hook_destroy(g_app.runtime);
        return 1;
    }
    pcsx2hook_apply_settings(g_app.runtime);

    if (!pcsx2hook_start(g_app.runtime)) {
        log.error("{}", pcsx2hook_get_last_error(g_app.runtime));
        pcsx2hook_destroy(g_app.runtime);
        return 1;
    }
    pcsx2hook_pump_window(g_app.runtime, 1);

    const int code = pcsx2hook_join(g_app.runtime);
    reportRun(g_app.hooks);
    writeRunSummary(g_app.hooks);

    // Before the emulator tears down: the render thread holds frames the guest thread wrote, and
    // SDL's own signal handlers sit on top of the ones PCSX2's virtual memory needs.
    if (g_app.renderer) g_app.renderer->close();
    pcsx2hook_flush_traces(g_app.runtime);
    pcsx2hook_shutdown(g_app.runtime);
    g_app.trace.reset();
    pcsx2hook_destroy(g_app.runtime);
    return code;
}
