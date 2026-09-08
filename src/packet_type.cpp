#include "packet_type.h"

#include <cstring>

#include "unsupported.h"

namespace sdfhook {

PacketType packetTypeForBuilder(const char* symbol) {
    struct Entry {
        const char* symbol;
        PacketType type;
    };
    // Every builder the hook table installs. A symbol reaching here without an entry is a hook
    // added without deciding what it produces, which is why the default reports.
    static constexpr Entry kBuilders[] = {
        {"sdf3DPrimSetTmPacket", PacketType::Transform},
        {"sdfModelAllocTmPacket", PacketType::Transform},
        {"sdfModelSetTmRefPacket", PacketType::Transform},
        {"sdf3DMakeViewportPacket", PacketType::Viewport},
        {"sdf3DMakeLightParallelPacket", PacketType::LightParallel},
        {"MakeLightPacket", PacketType::LightParallel},
        {"sdf3DMakeFogPacket", PacketType::Fog},
        {"sdfMatMakePacket", PacketType::Material},
        {"sdfTexMakeGIFPacket", PacketType::TextureUpload},
        {"sdfGrSetClearPacket", PacketType::Clear},
        {"sdfGrSetFramePacket", PacketType::FrameBuffer},
        {"sdf3DMakeDirectRefPacket", PacketType::DirectRef},
    };
    for (const Entry& entry : kBuilders) {
        if (std::strcmp(entry.symbol, symbol) == 0) return entry.type;
    }
    reportUnsupported("packet builder without a type", 0);
    return PacketType::Unknown;
}

PacketType packetTypeForProducer(const char* symbol) {
    // Each links an 8-quadword block whose leading words match sdf3DPrimSetTmPacket's output
    // exactly, so what they inline is the transform packet.
    static constexpr const char* kInlineTransform[] = {"fldDrawPoly3", "fldSdwDraw",
                                                       "dds3TrackPolyLinkOt"};
    for (const char* name : kInlineTransform) {
        if (std::strcmp(name, symbol) == 0) return PacketType::Transform;
    }
    return PacketType::Unknown;
}

}  // namespace sdfhook
