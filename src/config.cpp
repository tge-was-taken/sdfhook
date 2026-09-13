#include "config.h"

#include <cstdlib>

namespace sdfhook {
namespace {

const char* readText(const char* name, const char* fallback = "") {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

bool readFlag(const char* name) { return std::getenv(name) != nullptr; }

u32 readNumber(const char* name, u32 fallback) {
    const char* value = std::getenv(name);
    return value && *value ? u32(std::strtoul(value, nullptr, 10)) : fallback;
}

f32 readReal(const char* name, f32 fallback) {
    const char* value = std::getenv(name);
    return value && *value ? std::strtof(value, nullptr) : fallback;
}

}  // namespace

Config Config::readEnvironment() {
    Config config;
    config.bios = readText("SDF_BIOS", "assets/bios.bin");
    config.symbols = readText("SDF_SYMBOLS");
    config.logSpec = readText("SDF_LOG");
    config.window = readFlag("SDF_WINDOW");
    // A played run's trace costs frame rate and is never diffed.
    config.tracePath = readText("SDF_TRACE", config.window ? "" : "dumps/trace.jsonl");
    config.shotPath = readText("SDF_SHOT", "dumps/shot.png");
    config.nativeShotPath = readText("SDF_NATIVE_SHOT");
    config.dumpTextures = readText("SDF_DUMP_TEXTURES");
    config.statePath = readText("PCSX2HOOK_STATE");
    config.frames = readText("SDF_FRAMES");
    config.shotFrame = readNumber("SDF_SHOT_FRAME", 0);
    config.scale = int(readNumber("SDF_SCALE", 1));
    config.gsTrace = readFlag("PCSX2HOOK_GS_TRACE");
    config.vuTrace = readFlag("PCSX2HOOK_VU_TRACE");

    config.dumpPackets = readText("SDF_DUMP_PACKETS");
    config.dumpDecoded = readText("SDF_DUMP_DECODED");
    config.skipProducer = readText("SDF_SKIP_PRODUCER");
    config.dumpCall = readNumber("SDF_DUMP_CALL", 0);
    config.dumpCallWords = readNumber("SDF_DUMP_CALL_WORDS", 32);
    config.dumpMesh = readNumber("SDF_DUMP_MESH", 0);
    config.dumpMeshType = std::getenv("SDF_DUMP_MESH_TYPE") ? int(readNumber("SDF_DUMP_MESH_TYPE", 0)) : -1;
    config.dumpMeshWords = readNumber("SDF_DUMP_MESH_WORDS", 40);
    config.dumpLight = readFlag("SDF_DUMP_LIGHT");
    config.dumpScreen = readFlag("SDF_DUMP_SCREEN");
    config.dumpDrawEnv = readFlag("SDF_DUMP_DRAW_ENV");
    config.dumpChainCensus = readFlag("SDF_CHAIN_CENSUS");
    config.dumpRecover = readFlag("SDF_DUMP_RECOVER");
    config.describeFrame = readFlag("SDF_DESCRIBE");
    config.debugDepth = readFlag("SDF_DEBUG_DEPTH");
    config.debugNormal = readFlag("SDF_DEBUG_NORMAL");
    config.drawPrimPoly = !readFlag("SDF_NO_PRIM_POLY");
    config.pixelOffsetX = readReal("SDF_PIXEL_OFFSET_X", 0.5f);
    config.pixelOffsetY = readReal("SDF_PIXEL_OFFSET_Y", 0.5f);

    // SDF_MESH_ONLY names one mesh; SDF_MESH_UPTO names a prefix.
    if (const char* only = std::getenv("SDF_MESH_ONLY")) {
        config.meshFirst = usize(std::strtoul(only, nullptr, 10));
        config.meshLast = config.meshFirst;
    } else if (const char* upto = std::getenv("SDF_MESH_UPTO")) {
        config.meshLast = usize(std::strtoul(upto, nullptr, 10));
    }

    config.otMax = readNumber("SDF_OT_MAX", 3);
    config.noSprites = readFlag("SDF_NO_SPRITES");
    config.drawOnly = readText("SDF_DRAW_ONLY");
    config.noFeedback = readFlag("SDF_NO_FEEDBACK");
    config.noFog = readFlag("SDF_NO_FOG");
    config.noSpecular = readFlag("SDF_NO_SPECULAR");
    config.drawSpecularOverlay = readFlag("SDF_DRAW_SPECULAR_OVERLAY");
    config.drawDirectBox = readFlag("SDF_DRAW_DIRECT_BOX");
    config.drawShadows = readFlag("SDF_DRAW_SHADOWS");
    config.noBillboards = readFlag("SDF_NO_BILLBOARDS");
    config.poly = readFlag("SDF_POLY");
    config.noTransmitState = readFlag("SDF_NO_TRANSMIT_STATE");
    config.spriteHooks = readFlag("SDF_SPRITE_HOOKS");
    config.captureDir = readText("SDF_CAPTURE_DIR", "captures");
    config.captureFrame = u32(readNumber("SDF_CAPTURE_FRAME", 0));
    config.recoverTexturesFromGs = !readFlag("SDF_NO_GS_RECOVER");

    config.cull = readText("SDF_CULL", "b")[0];
    return config;
}

}  // namespace sdfhook
