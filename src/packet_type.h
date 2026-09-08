#pragma once

#include "types.h"

namespace sdfhook {

/// What a packet in the frame's DMA chain is. A chain entry carries no type of its own, so this
/// comes either from the builder that made it or from a payload that describes itself.
enum class PacketType : u8 {
    Unknown,

    // Named by the builder that made them.
    Transform,
    Viewport,
    LightParallel,
    Fog,
    Material,
    TextureUpload,
    Clear,
    FrameBuffer,
    DirectRef,

    // Named by their own payload.
    GifPrimitive,   // a GIF list that draws
    GifRegisters,   // a GIF list of A+D register writes
    VifData,        // an UNPACK stream, VU1's input
    VifSetup,       // VIF state, microprogram uploads and calls
    Empty,          // a tag that carries no payload

    Count,
};

/// Stable names for reporting, indexed by PacketType.
constexpr const char* packetTypeName(PacketType type) {
    switch (type) {
        case PacketType::Unknown: return "unknown";
        case PacketType::Transform: return "transform";
        case PacketType::Viewport: return "viewport";
        case PacketType::LightParallel: return "light";
        case PacketType::Fog: return "fog";
        case PacketType::Material: return "material";
        case PacketType::TextureUpload: return "texUpload";
        case PacketType::Clear: return "clear";
        case PacketType::FrameBuffer: return "frameBuffer";
        case PacketType::DirectRef: return "directRef";
        case PacketType::GifPrimitive: return "gifPrim";
        case PacketType::GifRegisters: return "gifRegs";
        case PacketType::VifData: return "vifData";
        case PacketType::VifSetup: return "vifSetup";
        case PacketType::Empty: return "empty";
        case PacketType::Count: break;
    }
    return "invalid";
}

/// The builder a hook fired on, as the type it produces. Named for the engine symbol so the hook
/// table and this stay greppable together.
PacketType packetTypeForBuilder(const char* symbol);

/// Producers that build their packet inline, so no builder function exists to hook and the call
/// that linked it is what names it. Unknown for everything else.
PacketType packetTypeForProducer(const char* symbol);

}  // namespace sdfhook
