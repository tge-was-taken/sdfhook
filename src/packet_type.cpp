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
        {"sdfGrSetRefGifPacket", PacketType::RefGif, 0},
        {"sdfBillBoardMakeTMPacket", PacketType::BillboardTransform, 0},
        {"sdfBillBoardMake1", PacketType::BillboardBatch, 0},
        {"sdfBillBoardVcMake1", PacketType::BillboardBatch, 0},
        {"sdf2DGetPrimPtr", PacketType::Packet2D, 0},
        {"sdf2DMakePrimPacketHeader", PacketType::Packet2D, 0},
        {"sdf2DMakeStdPacket", PacketType::Packet2D, 0},
        {"effPkt2DGetPrimPtr", PacketType::Packet2D, 0},
        {"sdf2DMakeTexPacket", PacketType::TexBind, 0},
        {"sdfPrimSetOpaquePacket", PacketType::AlphaState, 0},
        {"sdfPrimSetBitBltPacket", PacketType::BitBlt, 0},
        {"sdf3DSetDefaultFog", PacketType::Fog, 0},
        {"frCreateDrawEnvPacket", PacketType::DrawEnv, 0},
        {"sdfSetStoreImage", PacketType::TextureUpload, 0},
        {"sdfSetLoadImage", PacketType::TextureUpload, 0},
    };
    for (const Entry& entry : kBuilders) {
        if (std::strcmp(entry.symbol, symbol) != 0) continue;
        PacketBuilder made{entry.type, entry.argument, true, false};
        // 0x2B2018: sdfModelLinkOt writes the draw's colour and face-division UNPACKs and its MSCAL
        // into the same 64-byte packet, straight after the ref.
        if (entry.type == PacketType::TransformRef) made.trailing = PacketType::DrawParams;
        return made;
    }
    // Builders that return the packet. The ones without an IDB type (dds3PrimLine3D, the pb*
    // functions) are here because the link sites link their return value.
    static constexpr Entry kReturners[] = {
        {"sdfModelAllocTmPacket", PacketType::Transform, 0},
        {"sdfModelNewPacketHeader", PacketType::PacketHeader, 0},
        {"sdf3DPrimMakePoly", PacketType::PolyStream, 0},
        {"effPkt3DPrimMakePoly", PacketType::PolyStream, 0},
        {"sdfBillBoardMakeN", PacketType::BillboardBatch, 0},
        {"sdfBillBoardVcMakeN", PacketType::BillboardBatch, 0},
        {"sdfMultiBillBoardMakeN", PacketType::BillboardBatch, 0},
        {"sdfMultiBillBoardVcMakeN", PacketType::BillboardBatch, 0},
        {"sdf2DAllocSprite", PacketType::Packet2D, 0},
        {"dds3PrimFrameRect", PacketType::Primitive2D, 0},
        {"dds3PrimLine", PacketType::Primitive2D, 0},
        {"effPkt2DAllocTriStrip", PacketType::Primitive2D, 0},
        {"effPkt2DAllocF4", PacketType::Primitive2D, 0},
        {"pbMakePrimPacket", PacketType::Primitive2D, 0},
        {"pbAllocPrimPacket", PacketType::Primitive2D, 0},
        {"pbMakeSpritePacket", PacketType::Sprite, 0},
        {"dds3PrimLine3D", PacketType::Primitive3D, 0},
        {"sdfDrawModelWithMatrix", PacketType::Primitive3D, 0},
        {"sdfDevVprintf", PacketType::DebugText, 0},
    };
    for (const Entry& entry : kReturners) {
        if (std::strcmp(entry.symbol, symbol) == 0) return {entry.type, 0, false, true};
    }
    // Handed no packet: sdfMatUpdatePacket takes the material, MakeLightPacket a unit handle,
    // sdfTexMakeGIFPacket a texture handle, sdf3DPrimLinkMat and sdf3DPrimLinkTm only link.
    return {};
}

PacketType packetTypeForLinker(const char* symbol) {
    struct Entry {
        const char* symbol;
        PacketType type;
    };
    // One sdfDrawTagLink* site each and no callee that tail-calls one, checked over the IDB. A
    // function that tail-calls its link (sdf3DPrimLinkMat, sdfGrLinkDrawFrameTex) never shows up
    // here, since $ra names its caller.
    static constexpr Entry kLinkers[] = {
        {"DrwSprite2D", PacketType::Sprite},
        {"DrwSprite2DF", PacketType::Sprite},
        {"fclDrawSpriteCore", PacketType::Sprite},
        {"nbPnlSprite", PacketType::Sprite},
        {"cmpDrawSprite", PacketType::Sprite},
        {"cmpDrawSprite2", PacketType::Sprite},
        {"frCreateDrawFontPacket_20550848", PacketType::Glyph},
        {"aDrawBoxG", PacketType::Primitive2D},
        {"aDrawBoxF", PacketType::Primitive2D},
        {"aDrawBoxF_Direct", PacketType::Primitive2D},
        {"cmpDrawBoxG", PacketType::Primitive2D},
        {"cmpDrawTriG", PacketType::Primitive2D},
        {"rsetTest", PacketType::TestState},
        {"nbPnl_rsetTest", PacketType::TestState},
        {"rsetAlpha", PacketType::AlphaState},
        {"nbPnl_rsetAlpha", PacketType::AlphaState},
        {"rsetTexa", PacketType::TexaState},
        {"sdf2DLinkTexPacket", PacketType::TexBind},
        {"sdfModelSetDrawEnv", PacketType::DrawEnv},
        {"dds3LinkZClearDrawTag", PacketType::ZClear},
        {"dds3DrawFade", PacketType::Fade},
        {"rsetFrameMaskRgbOn", PacketType::FrameMask},
        {"rsetFrameMaskRgbOff", PacketType::FrameMask},
        {"cmpDisableRGB", PacketType::FrameMask},
        {"sdfModelLinkShapedDrawPacket", PacketType::MeshShaped},
    };
    for (const Entry& entry : kLinkers) {
        if (std::strcmp(entry.symbol, symbol) == 0) return entry.type;
    }
    return PacketType::Unknown;
}

PacketType packetTypeForGlobal(const char* symbol) {
    // sdf3DPrimLinkResetTm references the identity matrix, sdf3DLinkInitPacket the VU1 microcode.
    if (std::strcmp(symbol, "sdf3DPrimIdentityTm") == 0) return PacketType::Transform;
    if (std::strcmp(symbol, "vu1MicroPacket") == 0) return PacketType::VuMicrocode;
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
