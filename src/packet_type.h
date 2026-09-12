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
    MeshStream,
    MeshGenerated,
    DrawParams,
    PolyStream,
    VuMicrocode,
    Sprite,
    Glyph,
    Primitive2D,
    TestState,
    AlphaState,
    TexaState,
    TexBind,
    ZClear,
    Fade,
    BillboardBatch,
    BillboardTransform,
    Shadow,
    Packet2D,
    Primitive3D,
    BitBlt,
    DebugText,
    PacketHeader,
    Terminator,
    DrawEnv,
    FrameMask,
    FrameTex,
    DrawFrame,
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
constexpr const char* getPacketTypeName(PacketType type) {
    switch (type) {
        case PacketType::Unknown: return "unknown";
        case PacketType::Transform: return "transform";
        case PacketType::TransformRef: return "transformRef";
        case PacketType::MaterialRef: return "materialRef";
        case PacketType::MeshNormal: return "meshNormal";
        case PacketType::MeshSkinned: return "meshSkinned";
        case PacketType::MeshShaped: return "meshShaped";
        case PacketType::MeshStream: return "meshStream";
        case PacketType::MeshGenerated: return "meshGenerated";
        case PacketType::DrawParams: return "drawParams";
        case PacketType::PolyStream: return "polyStream";
        case PacketType::VuMicrocode: return "vuMicrocode";
        case PacketType::Sprite: return "sprite";
        case PacketType::Glyph: return "glyph";
        case PacketType::Primitive2D: return "primitive2D";
        case PacketType::TestState: return "testState";
        case PacketType::AlphaState: return "alphaState";
        case PacketType::TexaState: return "texaState";
        case PacketType::TexBind: return "texBind";
        case PacketType::ZClear: return "zClear";
        case PacketType::Fade: return "fade";
        case PacketType::BillboardBatch: return "billboardBatch";
        case PacketType::BillboardTransform: return "billboardTransform";
        case PacketType::Shadow: return "shadow";
        case PacketType::Packet2D: return "packet2D";
        case PacketType::Primitive3D: return "primitive3D";
        case PacketType::BitBlt: return "bitBlt";
        case PacketType::DebugText: return "debugText";
        case PacketType::PacketHeader: return "packetHeader";
        case PacketType::Terminator: return "terminator";
        case PacketType::DrawEnv: return "drawEnv";
        case PacketType::FrameMask: return "frameMask";
        case PacketType::FrameTex: return "frameTex";
        case PacketType::DrawFrame: return "drawFrame";
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
    bool returnsPacket = false;

    /// Bytes from the argument to the packet the chain sends, where the builder is handed a
    /// struct that holds it: an env whose predraw hook comes before its DMA tag.
    u32 offset = 0;

    /// What the caller writes straight after the packet, in the same allocation.
    PacketType trailing = PacketType::Unknown;
};

PacketBuilder getPacketBuilder(const char* symbol);


/// A packet named by its own contents, for the ones a savestate inherits from before the hooks
/// attached and whose builder call can never be seen. Inference, so it is the last resort and the
/// census counts it apart from what the engine told us.
PacketType getPacketTypeByShape(u32 aData, u32 aQwc);

/// The function a link hook's $ra names, where that function has exactly one link site and no
/// callee that tail-calls one, so everything linked from it is one kind of packet.
PacketType getPacketTypeByLinker(const char* symbol);

/// The engine globals a packet can reference outright, such as sdf3DPrimIdentityTm.
PacketType getPacketTypeByGlobal(const char* symbol);

/// Whether the GS or VU1 draws something from the packet, as against setting state for what
/// follows. An unidentified packet counts as a draw: it is where a producer's draw would be.
bool isDrawType(PacketType type);

/// How a packet's type was settled. Everything but Payload identifies a packet; Payload
/// merely describes its encoding, which says nothing about what the packet means, so census.py
/// counts it as unidentified.
enum class TypedBy : u8 { Nothing, Payload, Builder, Linker, Container, Shape };

constexpr const char* getTypedByName(TypedBy how) {
    switch (how) {
        case TypedBy::Nothing: return "-";
        case TypedBy::Payload: return "payload";
        case TypedBy::Builder: return "builder";
        case TypedBy::Linker: return "linker";
        case TypedBy::Container: return "container";
        case TypedBy::Shape: return "shape";
    }
    return "invalid";
}


}  // namespace sdfhook
