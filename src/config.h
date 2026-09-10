#pragma once

#include <string>

#include "types.h"

namespace sdfhook {

/// Read once at startup: a getenv buried in a draw call is invisible, and its function-local
/// static lets the first call decide for every later one.
struct Config {
    /// The only place getenv is called.
    static Config fromEnvironment();

    // What the run is.
    std::string bios;
    std::string symbols;
    std::string logSpec;
    std::string tracePath;
    std::string shotPath;
    std::string nativeShotPath;
    std::string dumpTextures;
    std::string statePath;
    std::string frames;
    u32 shotFrame = 0;
    int scale = 2;
    /// Hidden still carries a GL context; mapping one steals the keyboard from the terminal.
    bool window = false;
    bool gsTrace = false;
    bool vuTrace = false;

    // Diagnostics, all off unless asked for.
    std::string dumpPackets;
    std::string skipProducer;
    u32 dumpCall = 0;
    u32 dumpCallWords = 32;
    u32 dumpMesh = 0;
    int dumpMeshType = -1;
    u32 dumpMeshWords = 40;
    bool dumpLight = false;
    bool dumpScreen = false;
    bool dumpDrawEnv = false;

    /// An inventory of the frame's DMA chain, one row per entry.
    bool dumpChainCensus = false;

    /// Build a second frame from the chain alone and report it beside the one the hooks built.
    bool chainFrame = false;
    bool dumpRecover = false;
    bool describeFrame = false;
    bool debugDepth = false;

    /// Tells a flat character from a flat light.
    bool debugNormal = false;

    /// How every sdf3DPrim producer reaches the screen.
    bool drawPrimPoly = true;

    /// Fitted, not read: $f16 into sdfCorePacketGen, never shown assigned in the decompilation.
    f32 faceDivSoften = 7.0f;

    /// Moves the terminator rather than sharpening it. $f17, also fitted.
    f32 faceDivBias = 0.0f;

    /// GL rasterises at pixel centres, the GS covers every pixel an edge reaches; and the target
    /// is twice the guest's height, so the two axes differ.
    f32 pixelOffsetX = 0.0f;
    f32 pixelOffsetY = 0.0f;

    // Which parts of a frame to draw, for isolating one against the reference.
    usize meshFirst = 0;
    usize meshLast = ~usize(0);
    u32 otMax = 3;
    bool noSprites = false;

    /// PCSX2HOOK_GS_ONLY's grammar, so a frame cuts to the same draw on both sides and diffs with
    /// no matching: "i=N", "i=A-B", "tbp=N". Empty draws everything.
    std::string drawOnly;
    bool noFeedback = false;
    bool noFog = false;
    bool noSpecular = false;

    /// Off: invisible in the reference, RGBA 0 against a blend resolving to the destination.
    /// A switch so that stays re-checkable.
    bool drawSpecularOverlay = false;

    /// aDrawBoxF_Direct's line pairs. Off: the GS rasterizes no line in any sample scene.
    bool drawDirectBox = false;

    /// Off: the geometry decodes but the color and blend are guessed, not read from the material
    /// packet, and cost 24 points as opaque grey.
    bool drawShadows = false;

    /// Meshes keep the ALPHA and TEST of their ordering table instead of the chain's.
    bool noTransmitState = false;

    /// Without a keypress, for scripting and for checking a capture resumes.
    u32 captureFrame = 0;

    /// A savestate beside the two shots it should reproduce.
    std::string captureDir = "captures";

    /// The camera-facing quads dds3BillboardLinkOt batches: the battle specters.
    bool noBillboards = false;

    /// fldDrawPoly3's gouraud triangles. Off: they land on screen but never rasterise.
    bool poly = false;

    bool recoverTexturesFromGs = true;

    /// 'b' keeps back faces out, 'f' flips which side that is, 'n' draws both.
    char cull = 'b';
};

}  // namespace sdfhook
