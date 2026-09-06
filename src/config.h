#pragma once

#include <string>

#include "types.h"

namespace sdfhook {

/// Everything the environment can change about a run, read once at startup. A getenv buried in a
/// draw call is invisible, and its function-local static lets the first call decide for all later ones.
struct Config {
    /// Reads the whole environment once. The only place getenv is called.
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
    /// Show the window. The renderer runs either way: a hidden window still carries a GL context,
    /// and an automated run that maps one steals the keyboard from whatever started it.
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
    bool dumpRecover = false;
    bool describeFrame = false;
    bool debugDepth = false;

    /// Paints each mesh with its shaded normal, so a flat character can be told from a flat light.
    bool debugNormal = false;

    // Which parts of a frame to draw, for isolating one against the reference.
    usize meshFirst = 0;
    usize meshLast = ~usize(0);
    /// Highest ordering table to draw meshes from, for isolating one table's pass.
    u32 otMax = 3;
    bool noSprites = false;
    bool noFeedback = false;
    bool noFog = false;
    bool noSpecular = false;
    /// Draws aDrawBoxF_Direct's line pairs. Off because the GS rasterizes no line primitive in any
    /// sample scene, so whatever it files is never transmitted.
    bool drawDirectBox = false;

    /// Leaves every mesh on its ordering table's default GS state instead of the one its packet
    /// transmitted under.
    bool noTransmitState = false;

    /// The ordering table below which a 2D draw is behind the 3D. The engine files its 2D tables
    /// either side of the model ones and transmits them in index order.
    s32 spriteOtCut = 0;

    /// Captures a scene at this frame without a keypress, for scripting and for checking that the
    /// savestate a capture writes can actually be resumed.
    u32 captureFrame = 0;

    /// Where F9 writes a captured scene: a savestate beside the two shots it should reproduce.
    std::string captureDir = "captures";

    /// Draws the camera-facing quads dds3BillboardLinkOt batches, which is what the battle specters
    /// are.
    bool noBillboards = false;

    /// Draws fldDrawPoly3's gouraud triangles. Off: they decode and land on screen but do not
    /// rasterise, and an inert pass should not look enabled.
    bool poly = false;

    /// Draws meshes whose packet the engine generated and then linked nothing for. The hardware
    /// never drew them.
    bool drawUnlinkedMeshes = true;
    bool recoverTexturesFromGs = true;

    /// 'b' keeps back faces out, 'f' flips which side that is, 'n' draws both.
    char cull = 'b';

    /// 'f' draws nearest first, 'b' farthest first, 'n' leaves submission order alone.
    char meshOrder = 'b';
};

}  // namespace sdfhook
