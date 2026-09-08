#include "config.h"

#include <cstdlib>

namespace sdfhook {
namespace {

const char* text(const char* name, const char* fallback = "") {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

bool flag(const char* name) { return std::getenv(name) != nullptr; }

u32 number(const char* name, u32 fallback) {
    const char* value = std::getenv(name);
    return value && *value ? u32(std::strtoul(value, nullptr, 10)) : fallback;
}

f32 real(const char* name, f32 fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::strtof(value, nullptr) : fallback;
}

}  // namespace

Config Config::fromEnvironment() {
    Config config;
    config.bios = text("SDF_BIOS", "assets/bios.bin");
    config.symbols = text("SDF_SYMBOLS");
    config.logSpec = text("SDF_LOG");
    config.tracePath = text("SDF_TRACE", "dumps/trace.jsonl");
    config.shotPath = text("SDF_SHOT", "dumps/shot.png");
    config.nativeShotPath = text("SDF_NATIVE_SHOT");
    config.dumpTextures = text("SDF_DUMP_TEXTURES");
    config.statePath = text("PCSX2HOOK_STATE");
    config.frames = text("SDF_FRAMES");
    config.shotFrame = number("SDF_SHOT_FRAME", 0);
    config.scale = int(number("SDF_SCALE", 1));
    config.window = flag("SDF_WINDOW");
    config.gsTrace = flag("PCSX2HOOK_GS_TRACE");
    config.vuTrace = flag("PCSX2HOOK_VU_TRACE");

    config.dumpPackets = text("SDF_DUMP_PACKETS");
    config.skipProducer = text("SDF_SKIP_PRODUCER");
    config.dumpCall = number("SDF_DUMP_CALL", 0);
    config.dumpCallWords = number("SDF_DUMP_CALL_WORDS", 32);
    config.dumpMesh = number("SDF_DUMP_MESH", 0);
    config.dumpMeshType = std::getenv("SDF_DUMP_MESH_TYPE") ? int(number("SDF_DUMP_MESH_TYPE", 0)) : -1;
    config.dumpMeshWords = number("SDF_DUMP_MESH_WORDS", 40);
    config.dumpLight = flag("SDF_DUMP_LIGHT");
    config.dumpScreen = flag("SDF_DUMP_SCREEN");
    config.dumpDrawEnv = flag("SDF_DUMP_DRAW_ENV");
    config.dumpRecover = flag("SDF_DUMP_RECOVER");
    config.describeFrame = flag("SDF_DESCRIBE");
    config.debugDepth = flag("SDF_DEBUG_DEPTH");
    config.debugNormal = flag("SDF_DEBUG_NORMAL");
    config.drawPrimPoly = !flag("SDF_NO_PRIM_POLY");
    config.faceDivSoften = real("SDF_FACEDIV_SOFTEN", 7.0f);
    config.faceDivBias = real("SDF_FACEDIV_BIAS", 0.0f);
    config.pixelOffsetX = real("SDF_PIXEL_OFFSET_X", 0.5f);
    config.pixelOffsetY = real("SDF_PIXEL_OFFSET_Y", 0.5f);

    // SDF_MESH_ONLY names one mesh; SDF_MESH_UPTO names a prefix.
    if (const char* only = std::getenv("SDF_MESH_ONLY")) {
        config.meshFirst = usize(std::strtoul(only, nullptr, 10));
        config.meshLast = config.meshFirst;
    } else if (const char* upto = std::getenv("SDF_MESH_UPTO")) {
        config.meshLast = usize(std::strtoul(upto, nullptr, 10));
    }

    config.otMax = number("SDF_OT_MAX", 3);
    config.noSprites = flag("SDF_NO_SPRITES");
    config.drawOnly = text("SDF_DRAW_ONLY");
    config.noFeedback = flag("SDF_NO_FEEDBACK");
    config.noFog = flag("SDF_NO_FOG");
    config.noSpecular = flag("SDF_NO_SPECULAR");
    config.drawSpecularOverlay = flag("SDF_DRAW_SPECULAR_OVERLAY");
    config.drawDirectBox = flag("SDF_DRAW_DIRECT_BOX");
    config.drawShadows = flag("SDF_DRAW_SHADOWS");
    config.noBillboards = flag("SDF_NO_BILLBOARDS");
    config.poly = flag("SDF_POLY");
    config.noTransmitState = flag("SDF_NO_TRANSMIT_STATE");
    config.drawUnlinkedMeshes = !flag("SDF_NO_UNLINKED_MESHES");
    config.captureDir = text("SDF_CAPTURE_DIR", "captures");
    config.captureFrame = u32(number("SDF_CAPTURE_FRAME", 0));
    config.spriteOtCut = s32(number("SDF_SPRITE_OT_CUT", 0));
    config.recoverTexturesFromGs = !flag("SDF_NO_GS_RECOVER");

    config.cull = text("SDF_CULL", "b")[0];
    config.meshOrder = text("SDF_ORDER", "q")[0];
    return config;
}

}  // namespace sdfhook
