#pragma once

#include "types.h"

namespace sdfhook {

/// What a packet in the frame's DMA chain is. A chain entry carries no type of its own, so this
/// comes either from the builder that made it or from a payload that describes itself.
enum class PacketType : u8 {
    Unknown,

    // Named by the builder that made them.
    Transform,
    TransformRef,
    Viewport,
    LightParallel,
    Fog,
    Material,
    MaterialRef,
    MeshNormal,
    MeshSkinned,
    MeshShaped,
    PacketHeader,
    Terminator,
    DrawEnv,
    FrameMask,
    FrameTex,
    DrawFrame,
    DrawDefParm,
    RefGif,
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
        case PacketType::TransformRef: return "transformRef";
        case PacketType::MaterialRef: return "materialRef";
        case PacketType::MeshNormal: return "meshNormal";
        case PacketType::MeshSkinned: return "meshSkinned";
        case PacketType::MeshShaped: return "meshShaped";
        case PacketType::PacketHeader: return "packetHeader";
        case PacketType::Terminator: return "terminator";
        case PacketType::DrawEnv: return "drawEnv";
        case PacketType::FrameMask: return "frameMask";
        case PacketType::FrameTex: return "frameTex";
        case PacketType::DrawFrame: return "drawFrame";
        case PacketType::DrawDefParm: return "drawDefParm";
        case PacketType::RefGif: return "refGif";
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

/// What a builder produces, and which of its arguments names the packet. The argument is the
/// engine's own, read off the declaration: the packet is argument 0 for some and argument 2 for
/// others, and a few take no packet at all.
struct PacketBuilder {
    PacketType type = PacketType::Unknown;
    u8 argument = 0;
    bool takesPacket = false;
};

PacketBuilder packetBuilderFor(const char* symbol);

/// Producers that build their packet inline, so no builder function exists to hook and the call
/// that linked it is what names it. Unknown for everything else.
PacketType packetTypeForProducer(const char* symbol);

/// A packet named by its own contents, for the ones a savestate inherits from before the hooks
/// attached and whose builder call can never be seen. Inference, so it is the last resort and the
/// census counts it apart from what the engine told us.
PacketType packetTypeForShape(u32 aData, u32 aQwc);

/// How a packet's type was settled. Only Builder, Producer and Shape identify a packet; Payload
/// merely describes its encoding, which says nothing about what the packet means, so census.py
/// counts it as unidentified.
enum class TypedBy : u8 { Nothing, Payload, Builder, Producer, Shape };

constexpr const char* typedByName(TypedBy how) {
    switch (how) {
        case TypedBy::Nothing: return "-";
        case TypedBy::Payload: return "payload";
        case TypedBy::Builder: return "builder";
        case TypedBy::Producer: return "producer";
        case TypedBy::Shape: return "shape";
    }
    return "invalid";
}


}  // namespace sdfhook
