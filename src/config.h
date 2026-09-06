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

    // Which parts of a frame to draw, for isolating one against the reference.
    usize meshFirst = 0;
    usize meshLast = ~usize(0);
    /// Highest ordering table to draw meshes from, for isolating one table's pass.
    u32 otMax = 3;
    bool noSprites = false;
    bool noFeedback = false;
    bool noFog = false;
    bool noSpecular = false;
    bool noDirectBox = false;
    bool recoverTexturesFromGs = true;

    /// 'b' keeps back faces out, 'f' flips which side that is, 'n' draws both.
    char cull = 'b';

    /// 'f' draws nearest first, 'b' farthest first, 'n' leaves submission order alone.
    char meshOrder = 'b';
};

}  // namespace sdfhook
