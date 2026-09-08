#include "packet_type.h"

#include <bit>
#include <cmath>
#include <cstring>

#include "guest_ptr.h"
#include "unsupported.h"

namespace sdfhook {

PacketBuilder packetBuilderFor(const char* symbol) {
    struct Entry {
        const char* symbol;
        PacketType type;
        u8 argument;
    };
    // The argument holding the packet, from each function's own declaration. A Set*RefPacket
    // writes a DMA REF tag into its buffer, so the buffer is the chain entry and the reference is
    // the data.
    static constexpr Entry kBuilders[] = {
        {"sdf3DPrimSetTmPacket", PacketType::Transform, 0},
        {"sdf3DMakeViewportPacket", PacketType::Viewport, 0},
        {"sdf3DMakeLightParallelPacket", PacketType::LightParallel, 0},
        {"sdf3DMakeFogPacket", PacketType::Fog, 0},
        {"sdf3DMakeDirectRefPacket", PacketType::DirectRef, 0},
        {"sdfGrSetClearPacket", PacketType::Clear, 0},
        {"sdfGrSetFramePacket", PacketType::FrameBuffer, 0},
        {"sdfGrSetDrawFramePacket", PacketType::DrawFrame, 0},
        {"sdfGrSetDrawDefParmPacket", PacketType::DrawDefParm, 0},
        {"sdfGrSetStdDrawEnv", PacketType::DrawEnv, 0},
        {"sdfModelSetTermPacket", PacketType::Terminator, 0},
        {"sdfMatMakePacket", PacketType::Material, 1},
        {"sdfModelSetTmRefPacket", PacketType::TransformRef, 1},
        {"sdfGrSetFrameMaskRGB", PacketType::FrameMask, 1},
        {"sdfGrSetFrameMaskReset", PacketType::FrameMask, 1},
        {"sdfModelSetMatRefPacket", PacketType::MaterialRef, 2},
        {"sdfModelSetNormalDrawPacket", PacketType::MeshNormal, 2},
        {"sdfModelSetOneSkinDrawPacket", PacketType::MeshSkinned, 2},
        {"sdfGrLinkDrawFrameTex", PacketType::FrameTex, 2},
    };
    for (const Entry& entry : kBuilders) {
        if (std::strcmp(entry.symbol, symbol) == 0) return {entry.type, entry.argument, true};
    }
    // Hooked but handed no packet: sdfMatUpdatePacket takes the material, MakeLightPacket a unit
    // handle, sdfTexMakeGIFPacket a texture handle, and sdf3DPrimLinkMat and sdf3DPrimLinkTm link
    // rather than build. sdfModelAllocTmPacket and sdfModelNewPacketHeader return theirs, which
    // needs a post-handler.
    return {};
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
