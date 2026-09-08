#include "packet_type.h"

#include <bit>
#include <cmath>
#include <cstring>

#include "guest_ptr.h"
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

/// A light packet by its own contents: three light directions, each a unit vector, in the rows at
/// +0x90, with the colours above them at +0x70 and +0x80.
/// @note Measured on smt3_field_unknown_realm against a packet sdf3DMakeLightParallelPacket built,
/// which carries the same layout at the same offsets.
PacketType packetTypeForShape(u32 aData, u32 aQwc) {
    constexpr u32 kLightQwords = 13;
    constexpr u32 kDirections = 0x90;
    if (aQwc != kLightQwords) return PacketType::Unknown;

    const u32* word = u32Ptr_t{aData + kDirections}.slice(12);
    if (!word) return PacketType::Unknown;
    for (u32 row = 0; row < 3; row++) {
        f32 sum = 0.0f;
        for (u32 axis = 0; axis < 3; axis++) {
            const f32 value = std::bit_cast<f32>(word[row * 4 + axis]);
            sum += value * value;
        }
        // A direction the engine normalised, against a tolerance loose enough for its rounding.
        if (std::fabs(sum - 1.0f) > 0.01f) return PacketType::Unknown;
    }
    return PacketType::LightParallel;
}

}  // namespace sdfhook
