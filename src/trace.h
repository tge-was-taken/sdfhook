#pragma once

#include <cstdio>
#include <map>
#include <string>

#include "pcsx2hook/pcsx2hook.h"
#include "guest_ptr.h"
#include "hooks.h"
#include "render/frame.h"
#include "render/sprite.h"
#include "libsdf.h"
#include "types.h"

namespace sdfhook {

// One JSONL record per event, in the game's own order.
class Trace {
public:
    Trace(pcsx2hook_ctx* runtime, const std::string& path);
    ~Trace();

    bool isOpen() const { return mFile != nullptr; }
    u32 getFrame() const { return mFrame; }

    void onFrame(u32 frame);
    void onDds3DrawProcess(u32 arg);
    void onSdf3DSetCamera(u32 cameraAddr, u32 screenParamAddr);
    void onSdfModelLinkOt(u32 otAddr, u32 rootAddr);
    void onSdfCorePacketGen(u32 nodeAddr, u32 packetAddr, u32 attrMask, u32 color);
    void onSdfModelLinkShapedDrawPacket(u32 nodeAddr, u32 shapeAddr);
    void onSdfSortOtAvlRegist(u32 otAddr, u32 tagAddr, f32 z);
    void onSdfGrReqDraw(u32 tagAddr, u32 predrawAddr);
    void onSdfGrDrawFrame(u32 chainAddr);
    void onSdfMatSet(u32 matAddr);
    void onSdfTexSetEx(u32 texAddr);
    void onSdfGrSetClearPacket(u32 packetAddr);
    void onPrimEntry(const char* name, u32 arg);
    // Every packet the frame links, with the function that linked it and what its GIF tag draws.
    void onPacket(const LinkedPacket& packet, const char* by, const sdfDmaTag_t& tag, u64 verts);

    /// Carries the ranges that say whether the stream was read at the right stride. Written once
    /// the frame's chain exists, so it names the frame the mesh decoded in, not the current one.
    void onMesh(const DrawMesh& mesh, const Matrix4& screenTm, u32 frame);

    /// The same GS state fields a mesh carries, so 2D attributes against the GS trace too.
    void onSprite(const DrawSprite& sprite);

    /// The trace's last row, so two runs diff rather than being read.
    void onRunSummary(const std::string& row);

private:
    void write(const std::string& row);

    /// Written once and referenced by id, so a run carries the inputs beside the hardware's
    /// answer without a per-mesh copy.
    u32 litCase(const DrawMesh& mesh);

    std::map<std::string, u32> mLitCases;
    pcsx2hook_ctx* mRuntime;
    std::FILE* mFile = nullptr;
    u32 mFrame = 0;
    u64 mSeq = 0;
};

}  // namespace sdfhook
