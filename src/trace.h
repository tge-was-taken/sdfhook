#pragma once

#include <cstdio>
#include <string>

#include "guest.h"
#include "hooks.h"
#include "render/sprite.h"
#include "libsdf.h"
#include "types.h"

namespace sdfhook {

// One JSONL record per event, in the order the game produced them.
class Trace {
public:
    Trace(pcsx2hook_ctx* runtime, const std::string& path);
    ~Trace();

    bool isOpen() const { return mFile != nullptr; }

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
    void onPacket(const LinkedPacket& packet, const char* by, const DmaTag& tag, u64 verts);

private:
    void write(const std::string& row);

    pcsx2hook_ctx* mRuntime;
    Guest mGuest;
    std::FILE* mFile = nullptr;
    u32 mFrame = 0;
    u64 mSeq = 0;
};

}  // namespace sdfhook
