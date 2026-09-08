#include "libsdf_impl.h"

#include <cstddef>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <set>
#include <unordered_set>
#include <memory>
#include <vector>

#include "render/frame.h"
#include "render/sprite.h"
#include "sink.h"
#include "unsupported.h"
#include "log.h"
#include "json.h"
#include "texture.h"

// Our shadow of the engine: every function here is SDF's, reimplemented, and carries the `x`
// prefix so the guest symbol and ours never read as the same thing.
namespace sdfhook {

/// The 2D packet a producer fills: our decoded sprite where the guest keeps quadwords.
struct xsdf2DPrim_t {
    DrawSprite sprite;

    /// The GIF element the prim setters fill, in the guest's own slots. The sprite is derived from
    /// it, so the values a draw carries are the values the guest wrote.
    sdfPrimFt4Elem_t ft4{};
    sdfPrimG4Elem_t g4{};
};

struct xsdfPacket_t {
    xsdf2DPrim_t prim;
};

/// rsetOtOfs writes this; every 2D producer indexes xsdfGrOtTable with it.
u32 xrsetFrameMaskOtOffset = 0;

// The fade's own globals, which dds3DrawFade both writes and reads.
u8 xdds3FadeState = 0;
u8 xdds3FadeColorB = 0;
u8 xdds3FadeColorG = 0;
u32 xfadergba = 0;
u8 xdds3FadePacket[352] = {};

/// The glyph slots frTranslateUserData fills, which the font packet indexes.
u32 xfrFontEntryTable[1] = {};

dds3DrawEnv_t xdds3DrawEnv[2] = {};

namespace {

/// The guest bump-allocates out of a per-frame arena and recycles it; ours recycles the same way,
/// so a packet pointer is valid only until the frame closes.
std::vector<std::unique_ptr<xsdfPacket_t>> gPacketPool;

}  // namespace

/// @fn sdfGrAllocPacketBuff
/// @note SLUS-20911 0x002ACB50
xsdfPacket_t* xsdfGrAllocPacketBuff(u32) {
    // 0x2ACB50..0x2ACB6F
    // @optout 0x2ACB50 alloc: sdfGrPacketBuffPtr is the guest's bump pointer into the frame's
    // packet buffer, where ours owns each packet
    gPacketPool.push_back(std::make_unique<xsdfPacket_t>());
    return gPacketPool.back().get();
}

/// @fn sdf2DGetPrimPacketSize
/// @note SLUS-20911 0x002B9D68
u32 xsdf2DGetPrimPacketSize(u32 aCount) {
    // 0x2B9D68..0x2B9D77: quadwords for the guest, sprites for us, since ours is not a packet.
    return aCount;
}

/// @fn sdf2DMakePrimPacketHeader
/// @note SLUS-20911 0x002B9D80
// cppcheck-suppress unusedFunction  // @todo TODO.md A1: called once the producers
// are rewritten onto the shadow pipeline; xsdfPrimLinkG4 is the pattern
void xsdf2DMakePrimPacketHeader(xsdfPacket_t*, u32) {
    // 0x2B9D80..0x2B9DCB
    // @optout 0x2B9DAC packetbuf: the packet header's quadword count
    // @optout 0x2B9DB4 packetbuf: the GIF tag's register descriptor
    // @optout 0x2B9DB8 packetbuf: and its PRIM word, all of which our sprite carries as fields
}

/// @fn sdf2DGetPrimPtr
/// @note SLUS-20911 0x002B9D78
xsdf2DPrim_t* xsdf2DGetPrimPtr(xsdfPacket_t* aPacket) {
    // 0x2B9D78..0x2B9D7F
    return aPacket ? &aPacket->prim : nullptr;
}

/// @fn sdf2DAllocSprite
/// @note SLUS-20911 0x002B9DD0
// cppcheck-suppress unusedFunction  // @todo TODO.md A1: called once the producers
// are rewritten onto the shadow pipeline; xsdfPrimLinkG4 is the pattern
xsdfPacket_t* xsdf2DAllocSprite() {
    // 0x2B9DD0..0x2B9E2F
    xsdf2DGetPrimPacketSize(1);                       // 0x2B9DE8
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(1);  // 0x2B9DF0
    xsdf2DMakePrimPacketHeader(packet, 0);            // 0x2B9E10
    return packet;
}

/// @fn sdfPrimSetFt4Elem
/// @note SLUS-20911 0x002AF5C0
void xsdfPrimSetFt4Elem(xsdf2DPrim_t* aPrim, int aCorner, f32 aX, f32 aY, f32 aU, f32 aV,
                        Rgba aColor) {
    // 0x2AF5C0..0x2AF717: the element's slots hold what a corner is, so ours are filled first and
    // the sprite corner is read back out of them.
    if (!aPrim) return;
    sdfPrimFt4Elem_t& elem = aPrim->ft4;

    // 0x2AF690, 0x2AF694, 0x2AF69C: the tag, PRIM word and colour the four corners share.
    elem.giftag = 0;
    elem.prim = gState.test;
    elem.rgbaq =
        u64(aColor.r) | (u64(aColor.g) << 8) | (u64(aColor.b) << 16) | (u64(aColor.a) << 24);

    // 12.4, the way the GS takes a coordinate.
    const u64 uv = (u64(u32(aU * 16.0f)) & 0xFFFF) | (u64(u32(aV * 16.0f) & 0xFFFF) << 32);
    const u64 xy = (u64(u32(aX * 16.0f)) & 0xFFFF) | (u64(u32(aY * 16.0f) & 0xFFFF) << 32);
    // 0x2AF6EC, 0x2AF6F4, 0x2AF6F8, 0x2AF6FC: one uv slot a corner.
    // 0x2AF700, 0x2AF704, 0x2AF708, 0x2AF70C: and one xy slot beside it.
    switch (aCorner) {
        case 0: elem.uv0 = uv; elem.xy0 = xy; break;
        case 1: elem.uv1 = uv; elem.xy1 = xy; break;
        case 2: elem.uv2 = uv; elem.xy2 = xy; break;
        default: elem.uv3 = uv; elem.xy3 = xy; break;
    }
    aPrim->sprite.corner[aCorner] = SpriteCorner{aX, aY, aU, aV, aColor};
}

/// @fn sdfPrimSetG4Elem
/// @note SLUS-20911 0x002AF0F8
void xsdfPrimSetG4Elem(xsdf2DPrim_t* aPrim, int aCorner, f32 aX, f32 aY, Rgba aColor) {
    // 0x2AF0F8..0x2AF20B: the untextured element, a colour and a position a corner.
    if (!aPrim) return;
    sdfPrimG4Elem_t& elem = aPrim->g4;

    // 0x2AF1DC, 0x2AF208: the tag the four corners share and the trailing nop.
    elem.prim = gState.test;
    elem.nop = 0;

    const u64 rgbaq =
        u64(aColor.r) | (u64(aColor.g) << 8) | (u64(aColor.b) << 16) | (u64(aColor.a) << 24);
    const u64 xy = (u64(u32(aX * 16.0f)) & 0xFFFF) | (u64(u32(aY * 16.0f) & 0xFFFF) << 32);
    // 0x2AF1E4, 0x2AF1EC, 0x2AF1F4, 0x2AF1FC: one colour slot a corner.
    // 0x2AF1E8, 0x2AF1F0, 0x2AF1F8, 0x2AF200: and one position slot beside it.
    switch (aCorner) {
        case 0: elem.rgbaq0 = rgbaq; elem.xy0 = xy; break;
        case 1: elem.rgbaq1 = rgbaq; elem.xy1 = xy; break;
        case 2: elem.rgbaq2 = rgbaq; elem.xy2 = xy; break;
        default: elem.rgbaq3 = rgbaq; elem.xy3 = xy; break;
    }
    xsdfPrimSetFt4Elem(aPrim, aCorner, aX, aY, 0.0f, 0.0f, aColor);
}

/// The handle carries its own TEX1, so the filter is already in what the draw binds.
/// @fn sdfTexSetFilter
/// @note SLUS-20911 0x002AB0F8
// cppcheck-suppress unusedFunction  // @todo TODO.md A1: called once the producers
// are rewritten onto the shadow pipeline; xsdfPrimLinkG4 is the pattern
void xsdfTexSetFilter(xsdf2DPrim_t*, u32) {
    // 0x2AB0F8..0x2AB11F
    // @optout 0x2AB0F8 gif: the filter is a bit in the handle's own TEX1, which the sprite takes
    // from the GS state instead
}

/// @fn sdf2DLinkTexPacket
/// @note SLUS-20911 0x002B9CC8
// cppcheck-suppress unusedFunction  // @todo TODO.md A1: called once the producers
// are rewritten onto the shadow pipeline; xsdfPrimLinkG4 is the pattern
void xsdf2DLinkTexPacket(xsdfDrawTag_t* aTag, u32 aTexHandle) {
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(xsdf2DGetTexPacketSize());  // 0x2B9CE8, 0x2B9CF0
    xsdf2DMakeTexPacket(packet, aTexHandle);                                 // 0x2B9D00
    xsdfDrawTagLink(aTag, packet);                                           // 0x2B9D10
    aTag->texHandle = aTexHandle;
}

/// Marks the tag as carrying something, which is what sdfGrOtNormalReg tests at +0x04.
/// @fn sdfDrawTagLink
/// @note SLUS-20911 0x002ACBB8
// cppcheck-suppress unusedFunction  // @todo TODO.md A1: called once the producers
// are rewritten onto the shadow pipeline; xsdfPrimLinkG4 is the pattern
void xsdfDrawTagLink(xsdfDrawTag_t* aTag, xsdfPacket_t* aPacket) {
    // 0x2ACBB8..0x2ACBC3, 0x2ACBC4..0x2ACBCB, 0x2ACBD0..0x2ACBE3, 0x2ACBE4..0x2ACBEB
    // @optout 0x2ACBB8 packetbuf: tag->tail is where the guest appends the next packet in the
    // chain, and ours carries one packet a tag
    // @optout 0x2ACBD8 packetbuf: the packet's DMA tag word, which ours does not carry
    // @optout 0x2ACBE0 packetbuf: and its length
    if (!aPacket) return;
    aTag->head = true;
    aTag->packet = aPacket;
}

/// @fn fclDrawPanelSprite2
/// @note SLUS-20911 0x00239DF8
void xfclDrawPanelSprite2(xsdf2DPrim_t* aPrim, f32 aX, f32 aY, f32 aW, f32 aH, f32 aU0, f32 aV0,
                          f32 aU1, f32 aV1) {
    // 0x239DF8..0x239E13
    if (aPrim) aPrim->sprite = DrawSprite::fromRect(aX, aY, aW, aH, aU0, aV0, aU1, aV1);
}

/// The draw tag a producer links its packet into.
/// @fn sdfPrimAllocDrawTag
/// @note SLUS-20911 0x002AE090
xsdfDrawTag_t* xsdfPrimAllocDrawTag() {
    // 0x2AE090..0x2AE0A7, 0x2AE0A8..0x2AE0AF, 0x2AE0B0..0x2AE0D7
    // @optout 0x2AE0A8 alloc: the guest calls its allocator through the sdfGrAllocPacketBuff
    // pointer, where ours calls xsdfAllocDrawTag directly
    // @optout 0x2AE0B0 alloc: that indirect call
    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);
    return tag;
}

/// Binds the previously displayed frame as a texture. Ours is a reserved handle, so the env's
/// packets are read for the tag rather than written.
/// @fn sdfGrLinkDispFrameTex
/// @note SLUS-20911 0x002AD890
void xsdfGrLinkDispFrameTex(xsdfDrawTag_t* aTag, sdfGrStdDrawEnv_t* aEnv) {
    // 0x2AD890..0x2AD8EB
    // @optout 0x2AD89C gs: the env's clear packet names the GS buffer we hold as a handle
    if (!aTag || !aEnv) return;
    aTag->texPacket[0] = aEnv->header.addr;
}

/// @fn dds3GetFrameID
/// @note SLUS-20911 0x00100418
// cppcheck-suppress unusedFunction  // @todo TODO.md A1: called once the producers
// are rewritten onto the shadow pipeline; xsdfPrimLinkG4 is the pattern
u32 xdds3GetFrameID() {
    // 0x100418..0x10041F: which of the two per-frame buffers is live. Returning a constant would
    // pin every dds3DrawEnv and material packet to buffer zero.
    return gModel.dds3FrameID ? *gModel.dds3FrameID : 0;
}

/// @fn dds3FontReqAllocDrawTag
/// @note SLUS-20911 0x00117B08
// cppcheck-suppress unusedFunction  // @todo TODO.md A1: called once the producers
// are rewritten onto the shadow pipeline; xsdfPrimLinkG4 is the pattern
xsdfDrawTag_t* xdds3FontReqAllocDrawTag() {
    // 0x117B08..0x117B3B
    xsdfGrAllocPacketBuff(1);  // 0x117B14
    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);
    return tag;
}

// cppcheck-suppress unusedFunction  // @todo TODO.md A1: called once the producers
// are rewritten onto the shadow pipeline; xsdfPrimLinkG4 is the pattern
void xsdfPacketPoolReset() {
    gPacketPool.clear();
}

xsdfGrOt_t xsdfGrOtTable[kXsdfOrderingTables];

namespace {

u32 gSequence = 0;

/// The guest's tags live in its packet arena and are recycled per frame; so are ours.
std::vector<std::unique_ptr<xsdfDrawTag_t>> gTagPool;

}  // namespace

/// @fn frSwapColor
/// @note SLUS-20911 0x001C6338
u32 xfrSwapColor(u32 aColor) {  // 0x1C6338..0x1C635F
    return ((aColor >> 24) & 0xFF) | ((aColor >> 8) & 0xFF00) | ((aColor & 0xFF00) << 8) |
           (aColor << 24);
}

/// @fn cmpExpandColor
/// @note SLUS-20911 0x002399A0
void xcmpExpandColor(u32 (&aOut)[4], u32 aColor) {
    aOut[0] = (aColor >> 24) & 0xFF;  // 0x2399A8
    aOut[1] = (aColor >> 16) & 0xFF;  // 0x2399A0
    aOut[2] = (aColor >> 8) & 0xFF;   // 0x2399BC
    aOut[3] = aColor & 0xFF;          // 0x2399A4
}


/// @fn sdfDrawTagInit
/// @note SLUS-20911 0x002ACB90
// cppcheck-suppress unusedFunction  // @todo TODO.md A1 stage 2 wires this
void xsdfDrawTagInit(xsdfDrawTag_t* aTag) {
    *aTag = xsdfDrawTag_t{};
    aTag->texPacket[1] = 0;                                 // 0x2ACB94
    std::memcpy(&aTag->z, &kXsdfNoSortZ, sizeof(aTag->z));  // 0x2ACB98
    aTag->next = nullptr;                                   // 0x2ACB9C
    aTag->head = false;                                     // 0x2ACBA0
    aTag->tail = nullptr;                                   // 0x2ACBA4
    aTag->lightPacket = nullptr;                            // 0x2ACBA8
    aTag->fogPacket = nullptr;                              // 0x2ACBAC
    aTag->texPacket[0] = 0;                                 // 0x2ACBB0
}

/// Splices the light and fog packets where two tags disagree.
/// @fn sdfGrJoinDrawTag
/// @note SLUS-20911 0x002ACEE8
void xsdfGrJoinDrawTag(xsdfDrawTag_t* aFore, xsdfDrawTag_t* aAft) {
    // 0x2ACEE8..0x2ACF0F, 0x2ACF10..0x2ACF17, 0x2ACF18..0x2ACF23, 0x2ACF24..0x2ACF33,
    // 0x2ACF34..0x2ACF3B, 0x2ACF3C..0x2ACF4B, 0x2ACF4C..0x2ACF77
    // @optout 0x2ACF18 packetbuf: sdfGrAddExPacket splices the packet that carries the change
    // @optout 0x2ACF0C packetbuf: the tail it splices onto
    // @optout 0x2ACF50 packetbuf: that packet's DMA tag word
    // @optout 0x2ACF5C packetbuf: the head it links from
    // @optout 0x2ACF6C packetbuf: and its length
    if (!aFore || !aAft) return;
    if (aFore->lightPacket != aAft->lightPacket) aAft->lightSeam = true;  // 0x2ACF00
    if (aFore->fogPacket != aAft->fogPacket) aAft->fogSeam = true;        // 0x2ACF24
}

/// FIFO append. An empty tag registers nothing, which is what keeps a producer that built no
/// geometry out of the table.
/// @fn sdfGrOtNormalReg
/// @note SLUS-20911 0x002ACD40
void xsdfGrOtNormalReg(xsdfGrOt_t* aOt, xsdfDrawTag_t* aTag) {
    if (!aTag->head) return;  // 0x2ACD54, 0x2ACD58
    if (!aOt->tagTail) {      // 0x2ACD60
        aOt->tagHead = aTag;  // 0x2ACD70
    } else {
        xsdfGrJoinDrawTag(aOt->tagTail, aTag);  // 0x2ACD78
        aOt->tagTail->next = aTag;              // 0x2ACD7C
    }
    aOt->tagTail = aTag;  // 0x2ACD80, 0x2ACD84
}

/// @todo TODO.md A1 stage 3: the AVL walk is descending z with LIFO ties, and the 0xFFFF sentinel
/// goes to a separate list emitted first in reverse. Registers FIFO until that is modelled.
/// @fn sdfGrSortOtReg
/// @note SLUS-20911 0x002C7F88
// cppcheck-suppress unusedFunction  // @todo TODO.md A1 stage 2 wires this
void xsdfGrSortOtReg(xsdfGrOt_t* aOt, xsdfDrawTag_t* aTag) {
    if (!aTag->head) return;  // 0x2C7F94
    u32 z;
    std::memcpy(&z, &aTag->z, sizeof(z));  // 0x2C7FA0
    if (z == kXsdfNoSortZ) {
        aTag->next = aOt->noSortTag;  // 0x2C7FAC, prepended, so it emits reversed
        aOt->noSortTag = aTag;
        return;
    }
    aOt->data = aTag;  // 0x2C7FC0, the AVL root sdfSortOtAvlRegist walks
    xsdfGrOtNormalReg(aOt, aTag);  // 0x2C7FD0, 0x2C7FD4
}

/// @fn sdfGrOtInit
/// @note SLUS-20911 0x002ACDC0
// cppcheck-suppress unusedFunction  // @todo TODO.md A1 stage 2 wires this
xsdfDrawTag_t* xsdfAllocDrawTag() {
    gTagPool.push_back(std::make_unique<xsdfDrawTag_t>());
    return gTagPool.back().get();
}

u32 xsdfGrOtIndexOf(u32 aGuestAddr) {
    static constexpr u32 kBase = 0x2F5F18;  // sdfGrOtInit(0x2F5F18, 0x55)
    if (aGuestAddr < kBase) return ~0u;
    const u32 index = (aGuestAddr - kBase) / 32;
    return index < kXsdfOrderingTables ? index : ~0u;
}

/// @fn sdfGrOtClose
/// @note SLUS-20911 0x002AD010
void xsdfGrOtClose(std::vector<xsdfDrawTag_t*>& aOut) {
    aOut.clear();
    for (xsdfGrOt_t* ot = &xsdfGrOtTable[0]; ot; ot = ot->next) {  // 0x2AD010..0x2AD0BB
        if (ot->callbackFunc) {
            ot->callbackFunc(ot);      // 0x2AD034, 0x2AD040
            xsdfGrInitExPacket(ot);    // 0x2AD070
        }
        // The sentinel list is built by prepending, so it comes out in reverse registration order.
        for (xsdfDrawTag_t* tag = ot->noSortTag; tag; tag = tag->next) {
            if (tag->hasDraw) aOut.push_back(tag);
        }
        xsdfDrawTag_t* prev = nullptr;
        for (xsdfDrawTag_t* tag = ot->tagHead; tag; tag = tag->next) {
            xsdfGrJoinDrawTag(prev, tag);  // 0x2AD05C
            if (tag->hasDraw) aOut.push_back(tag);
            prev = tag;
        }
    }
}

/// Prepares the packet a table's callback fills. Ours carries the decoded draw on the tag, so
/// there is no packet to prepare.
/// @fn sdfGrInitExPacket
/// @note SLUS-20911 0x002ACF78
void xsdfGrInitExPacket(xsdfGrOt_t*) {
    // 0x2ACF78..0x2ACF93, 0x2ACF94..0x2ACFC3, 0x2ACFC4..0x2ACFCF, 0x2ACFD0..0x2AD003,
    // 0x2AD004..0x2AD00F
    // @optout 0x2ACF88 packetbuf: the table's own light and fog packets, which the seam flags on
    // each tag replace
    // @optout 0x2ACF94 packetbuf: sdfGrNewExPacket carves one
    // @optout 0x2ACF9C packetbuf: the head it is linked before
    // @optout 0x2ACFC0 packetbuf: its quadword count
    // @optout 0x2ACFC4 packetbuf: and the light packet it carries
}

/// The default per-table callback, which builds that layer's packets. Ours builds none.
/// @fn sdfGrOtNormalCallback
/// @note SLUS-20911 0x002ACD98
void xsdfGrOtNormalCallback(xsdfGrOt_t*) {
    // 0x2ACD98..0x2ACDA7, 0x2ACDA8..0x2ACDAF, 0x2ACDB0..0x2ACDBB
    // @optout 0x2ACDA8 packetbuf: sdfGrOtFuncLinkToHead links the layer's own packet, and ours
    // builds none
}

/// Installs the registrar on all 85. Runs once at boot, as the guest's does.
/// @fn sdfGrOtInit
/// @note SLUS-20911 0x002ACDC0
void xsdfGrOtInit() {
    for (u32 i = 0; i < kXsdfOrderingTables; i++) {  // 0x2ACDF0, 0x2ACDF8, 0x2ACE2C
        xsdfGrOt_t* ot = &xsdfGrOtTable[i];
        ot->regFunc = &xsdfGrOtNormalReg;        // 0x2ACDC8, 0x2ACDD4
        ot->callbackFunc = &xsdfGrOtNormalCallback;  // 0x2ACDCC, 0x2ACDD8
        ot->tagHead = nullptr;                   // 0x2ACDDC
        ot->tagTail = nullptr;                   // 0x2ACDE0
        ot->noSortTag = nullptr;                 // 0x2ACDE4
        ot->data = nullptr;                      // 0x2ACDEC
        // Next-linked in address order, which is the order sdfGrOtClose walks.
        ot->next = i + 1 < kXsdfOrderingTables ? &xsdfGrOtTable[i + 1] : nullptr;
    }
}

/// The lists and the arena the guest recycles per frame. The registrars survive, as they do in
/// the guest: sdfGrOtInit runs once at boot.
void xsdfGrOtReset() {
    gSequence = 0;
    gTagPool.clear();
    gPacketPool.clear();
    for (xsdfGrOt_t& ot : xsdfGrOtTable) {
        ot.tagHead = nullptr;
        ot.tagTail = nullptr;
        ot.noSortTag = nullptr;
        ot.data = nullptr;
    }
}

xsdfGrOt_t* xsdfGrOtGet(u32 aIndex) {
    if (aIndex >= kXsdfOrderingTables) {
        reportUnsupported("ordering table index", aIndex);
        return nullptr;
    }
    return &xsdfGrOtTable[aIndex];
}

// cppcheck-suppress unusedFunction  // @todo TODO.md A1 stage 2 wires this
void xsdfGrOtRegister(u32 aIndex, xsdfDrawTag_t* aTag) {
    xsdfGrOt_t* ot = xsdfGrOtGet(aIndex);
    if (!ot || !ot->regFunc) return;
    aTag->sequence = gSequence++;
    ot->regFunc(ot, aTag);
}

namespace {

/// 12.4 screen coordinate to the guest's draw space, XYOFFSET removed.
f32 fromFixed(s32 aValue, f32 aOffset) {
    return f32(aValue) / 16.0f - aOffset;
}

}  // namespace

/// The GS state packet a rset* call builds. Ours carries the resolved register instead.
/// @fn sdf2DMakeStdPacket
/// @note SLUS-20911 0x002B9D38
void xsdf2DMakeStdPacket(xsdfPacket_t*, u32, u64) {}

/// @fn sdf2DGetTexPacketSize
/// @note SLUS-20911 0x002B9C18
u32 xsdf2DGetTexPacketSize() {
    return 1;
}

/// @fn sdf2DMakeTexPacket
/// @note SLUS-20911 0x002B9C20
void xsdf2DMakeTexPacket(xsdfPacket_t* aPacket, u32 aTexHandle) {
    if (aPacket) aPacket->prim.sprite.texHandle = TexHandle{aTexHandle};
}


/// Sets FBMSK so draws after it write no color, which is how the silhouette pass lays its shape
/// into the alpha the bloom reads.
/// @fn sdfGrSetFrameMaskRGB
/// @note SLUS-20911 0x002AD800
void xsdfGrSetFrameMaskRGB(sdfGrStdDrawEnv_t* aEnv, xsdfPacket_t*) {
    // 0x2AD800..0x2AD81B, 0x2AD81C..0x2AD82B, 0x2AD830..0x2AD847
    if (!aEnv) return;
    // @optout 0x2AD818 gif: stdDrawEnv's A+D slots are the packet the mask is written into
    // @optout 0x2AD838 gif: and its header
    gState.frameMask = sdfFrameMaskRgb;
}

/// @fn sdfGrSetFrameMaskReset
/// @note SLUS-20911 0x002AD848
void xsdfGrSetFrameMaskReset(sdfGrStdDrawEnv_t* aEnv, xsdfPacket_t*) {
    if (!aEnv) return;
    gState.frameMask = 0;
}

/// The REF-GIF link: the packet is data the game already holds, so ours carries the register the
/// mask pair set rather than the quadwords.
/// @fn sdfDrawTagLinkRefGif
/// @note SLUS-20911 0x002ACC68
void xsdfDrawTagLinkRefGif(xsdfDrawTag_t* aTag, xsdfPacket_t* aPacket) {
    xsdfDrawTagLink(aTag, aPacket);
}

/// The frame-mask pair, which bracket the silhouette pass.
/// @fn rsetFrameMaskRgbOn
/// @note SLUS-20911 0x0012A3F0
void xrsetFrameMaskRgbOn() {
    xsdfPacket_t* tagBuf = xsdfGrAllocPacketBuff(0x20);  // 0x12A400
    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);                                // 0x12A40C
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(0x40);
    const u32 frame = xdds3GetFrameID();                 // 0x12A41C
    dds3DrawEnv_t* drawEnv = &xdds3DrawEnv[frame & 1];
    xsdfGrSetFrameMaskRGB(&drawEnv->stdDrawEnv, packet);  // 0x12A444, 0x12A448
    xsdfDrawTagLinkRefGif(tag, tagBuf);                  // 0x12A454
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];  // 0x12A45C, 0x12A464
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);  // 0x12A470, 0x12A474
    }
}

/// @fn rsetFrameMaskRgbOff
/// @note SLUS-20911 0x0012A490
void xrsetFrameMaskRgbOff() {
    xsdfPacket_t* tagBuf = xsdfGrAllocPacketBuff(0x20);  // 0x12A4A0
    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);                                // 0x12A4AC
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(0x40);
    const u32 frame = xdds3GetFrameID();                 // 0x12A4BC
    dds3DrawEnv_t* drawEnv = &xdds3DrawEnv[frame & 1];
    xsdfGrSetFrameMaskReset(&drawEnv->stdDrawEnv, packet);  // 0x12A4E4, 0x12A4E8
    xsdfDrawTagLinkRefGif(tag, tagBuf);                  // 0x12A4F4
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];  // 0x12A4FC, 0x12A504
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);  // 0x12A510, 0x12A514
    }
}

/// @fn rsetOtOfs
/// @note SLUS-20911 0x00128FD0
void xrsetOtOfs(u32 aOtOfs) {
    xrsetFrameMaskOtOffset = aOtOfs;  // 0x128FD4
}

/// @fn rsetAlpha
/// @note SLUS-20911 0x001291B0
void xrsetAlpha(u32 aMode) {
    // 0x1291B0..0x129207: the mode, then one block a case resolving it to an ALPHA register.
    GsAlpha alpha = GsAlpha::Blend;
    switch (aMode) {
        case 1: alpha = GsAlpha::Additive; break;               // 0x129208
        case 2:                                                 // 0x129220
        case 6: alpha = GsAlpha::ReverseSubtract; break;        // 0x129228
        case 3: alpha = GsAlpha(0x84); break;                   // 0x129230
        case 4: alpha = GsAlpha(0x06); break;                   // 0x129238
        case 5: alpha = GsAlpha(0x89); break;                   // 0x129240
        case 10: alpha = GsAlpha(0x2A); break;                  // 0x129248
        case 20: alpha = GsAlpha(0x4A); break;                  // 0x129250
        case 0:                                                 // 0x129258
        case 7:                                                 // 0x129260
        case 8: alpha = GsAlpha::Blend; break;                  // 0x129264
        default: reportUnsupported("rsetAlpha mode", aMode); break;
    }
    gState.alpha = alpha;
    const u32 aAlpha = u32(alpha);
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(1);  // 0x1291CC
    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);                             // 0x1291D8
    xsdf2DMakeStdPacket(packet, 0, aAlpha);      // 0x1291F0
    xsdfDrawTagLink(tag, packet);                     // 0x129274
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];  // 0x12927C, 0x129284
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);  // 0x129290, 0x129294
    }
}

/// @fn rsetTest
/// @note SLUS-20911 0x00129098
void xrsetTest(u32 aAte, u32 aAtst, u32 aAref, u32 aAfail, u32 aDate, u32 aDatm, u32 aZtst) {
    // 0x129098..0x1291AB: the TEST register as the guest packs it, ZTE always set.
    const u32 aTest = aAte | (aAtst << 1) | (aAref << 4) | (aAfail << 12) | (aDate << 14)
                      | (aDatm << 15) | 0x10000u | (aZtst << 17);
    gState.test = aTest;
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(1);  // 0x1290E4
    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);                             // 0x1290F8
    xsdf2DMakeStdPacket(packet, 0, aTest);            // 0x129118
    xsdfDrawTagLink(tag, packet);                     // 0x129154
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];  // 0x12915C, 0x129164
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);  // 0x129170, 0x129174
    }
}

/// @fn DrwSprite2D
/// @note SLUS-20911 0x001288B8
void xDrwSprite2D(f32 aX, f32 aY, f32 aW, f32 aH, f32 aU, f32 aV, f32 aUw, f32 aVh, Rgba aRgba,
                  u32 aTexHandle) {
    xsdfPacket_t* packet = xsdf2DAllocSprite();          // 0x12890C
    xsdf2DPrim_t* prim = xsdf2DGetPrimPtr(packet);       // 0x12891C
    prim->sprite = DrawSprite::fromRect(aX, aY * 0.5f, aW, aH * 0.5f, aU, aV, aU + aUw, aV + aVh);
    prim->sprite.setColor(aRgba);
    prim->sprite.test = gState.test;
    prim->sprite.otOffset = s32(xrsetFrameMaskOtOffset);
    prim->sprite.alpha = gState.alpha;
    prim->sprite.texHandle = gSink.resolveTexture(aTexHandle);

    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfGrAllocPacketBuff(1);           // 0x1289B0
    xsdfDrawTagInit(tag);               // 0x1289BC
    xsdf2DLinkTexPacket(tag, aTexHandle);  // 0x1289CC
    xsdfDrawTagLink(tag, packet);      // 0x1289D8
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];  // 0x1289E0
    gSink.addSprite(tag, prim->sprite);
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);  // 0x1289F4, 0x1289F8
    }
}

/// @fn aDrawBoxG
/// @note SLUS-20911 0x00129C08
void xaDrawBoxG(s32 aX, s32 aY, s32 aW, s32 aH, const u32 (&aRgba)[16]) {
    // One corner a block, four words each.
    Rgba aColor[4];
    for (int i = 0; i < 4; i++) {
        aColor[i] = Rgba{u8(aRgba[i * 4]), u8(aRgba[i * 4 + 1]), u8(aRgba[i * 4 + 2]),
                         u8(aRgba[i * 4 + 3])};
    }
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(1);  // 0x129CC0
    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);                            // 0x129CCC
    xsdf2DGetPrimPacketSize(4);                       // 0x129CDC
    xsdf2DMakePrimPacketHeader(packet, 0);            // 0x129D0C
    xsdf2DPrim_t* prim = xsdf2DGetPrimPtr(packet);    // 0x129D14
    prim->sprite = DrawSprite::fromRect(f32(aX), f32(aY), f32(aW), f32(aH), 0, 0, 0, 0);
    // 0x129DA0..0x129E2B: one corner a block
    for (int i = 0; i < 4; i++) prim->sprite.corner[i].color = aColor[i];
    prim->sprite.alpha = gState.alpha;
    prim->sprite.test = gState.test;
    prim->sprite.otOffset = s32(xrsetFrameMaskOtOffset);
    xsdfDrawTagLink(tag, packet);  // 0x129E30
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];  // 0x129E38
    gSink.addSprite(tag, prim->sprite);
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);  // 0x129E4C, 0x129E50
    }
}

/// @fn aDrawBoxF
/// @note SLUS-20911 0x00129558
void xaDrawBoxF(s32 aX, s32 aY, s32 aW, s32 aH, u32 aR, u32 aG, u32 aB, u32 aA) {
    const Rgba aColor{u8(aR), u8(aG), u8(aB), u8(aA)};
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(1);  // 0x1295E4
    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);                            // 0x1295F0
    xsdf2DGetPrimPacketSize(1);                       // 0x1295FC
    xsdf2DMakePrimPacketHeader(packet, 0);            // 0x129620
    xsdf2DPrim_t* prim = xsdf2DGetPrimPtr(packet);    // 0x129628
    prim->sprite = DrawSprite::fromRect(f32(aX), f32(aY), f32(aW), f32(aH), 0, 0, 0, 0);
    prim->sprite.setColor(aColor);  // 0x129648..0x12968B
    prim->sprite.alpha = gState.alpha;
    xsdfDrawTagLink(tag, packet);  // 0x129690
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];  // 0x129698
    gSink.addSprite(tag, prim->sprite);
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);  // 0x1296AC, 0x1296B0
    }
}

/// PRIM 0x41 is a LINE, so only the top and bottom edges draw.
/// @fn aDrawBoxF_Direct
/// @note SLUS-20911 0x00129870
void xaDrawBoxF_Direct(s32 aX0, s32 aY0, s32 aX1, s32 aY1, u32 aR, u32 aG, u32 aB, u32 aA) {
    const Rgba aColor{u8(aR), u8(aG), u8(aB), u8(aA)};
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(1);  // 0x1298E4
    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);                            // 0x1298F0
    xsdf2DGetPrimPacketSize(2);                       // 0x1298FC
    xsdf2DMakePrimPacketHeader(packet, 0x41);         // 0x129920
    xsdf2DPrim_t* prim = xsdf2DGetPrimPtr(packet);    // 0x129928
    const f32 left = fromFixed(aX0, kOffsetX);
    const f32 right = fromFixed(aX1, kOffsetX);
    // 0x129948..0x12997B: the two edges
    for (s32 y : {aY0, aY1}) {
        prim->sprite =
            DrawSprite::fromRect(left, fromFixed(y, kOffsetY), right - left, 1.0f, 0, 0, 0, 0);
        prim->sprite.setColor(aColor);
        prim->sprite.alpha = gState.alpha;
        prim->sprite.test = gState.test;
        prim->sprite.otOffset = s32(xrsetFrameMaskOtOffset);
        gSink.addSprite(tag, prim->sprite);
    }
    xsdfDrawTagLink(tag, packet);  // 0x129980
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];  // 0x129988
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);  // 0x12999C, 0x1299A0
    }
}

/// @fn frCreateDrawFontPacket_20550848
/// @note SLUS-20911 0x001C6438
void xfrCreateDrawFontPacket(s32 aX, s32 aY, s32 aWidth, s32 aHeight, u32 aCol, const f32 (&aUv)[4],
                             frGsTex_t* aGsTex) {
    const Rgba color = Rgba::fromPacked(xfrSwapColor(aCol));  // 0x1C6490
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(1);          // 0x1C649C
    xsdf2DPrim_t* prim = xsdf2DGetPrimPtr(packet);
    xfrFontEntryTable[0] = 0;  // 0x1C653C
    // 0x1C6534, 0x1C654C: TEX0 takes the log2 pair, we recover from the pixel pair, and a sheet
    // whose two disagree would be recovered at the wrong size.
    if ((1u << aGsTex->WidthLog2) != aGsTex->Width
        || (1u << aGsTex->HeightLog2) != aGsTex->Height) {
        reportUnsupported("font sheet log2 size disagrees with its pixels", aGsTex->WidthLog2);
    }
    // The palettes are static data, so they read whether or not the glyph uploads ran in the
    // capture window.
    gSink.textures->loadFontClut(gModel.frFontClutTable);
    gSink.textures->wantFontSheet(aGsTex->Addr, aGsTex->BufWidth, aGsTex->Width,
                                  aGsTex->Height);  // 0x1C6554, 0x1C655C
    prim->sprite = DrawSprite::fromRect(f32(aX) / 16.0f, f32(aY) / 16.0f, f32(aWidth),
                                        f32(aHeight), aUv[0], aUv[1], aUv[2], aUv[3]);
    prim->sprite.setColor(color);
    prim->sprite.texHandle = gSink.textures->getFontSheet(aGsTex->Width, gSink.frame);
    prim->sprite.alpha = gState.alpha;

    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);          // 0x1C6630
    xsdfDrawTagLink(tag, packet);  // 0x1C663C
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];  // 0x1C6644
    gSink.addSprite(tag, prim->sprite);
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);  // 0x1C6658, 0x1C665C
    }
}

/// A strip (TL, TR, BL, BR) into a cycle, so the third and fourth corners swap.
/// @fn sdfPrimLinkFt4
/// @note SLUS-20911 0x002AF718
void xsdfPrimLinkFt4(u32 aColor, u32 aPrim, const PrimVertex (&aVertex)[4],
                     xsdfAllocFn aAllocator) {
    // 0x2AF718..0x2AF7CB: the caller's allocator, or the engine's own when it passed none.
    xsdfAllocFn alloc = aAllocator ? aAllocator : &xsdfGrAllocPacketBuff;  // 0x2AF7CC
    xsdfPacket_t* packet = alloc(xsdf2DGetPrimPacketSize(4));               // 0x2AF7D4
    xsdf2DPrim_t* prim = xsdf2DGetPrimPtr(packet);
    static constexpr int kCycle[4] = {0, 1, 3, 2};
    const Rgba color = Rgba::fromPacked(aColor);
    for (int i = 0; i < 4; i++) {  // 0x2AF87C
        const PrimVertex& v = aVertex[i];
        xsdfPrimSetFt4Elem(prim, kCycle[i], fromFixed(v.x, kOffsetX), fromFixed(v.y, kOffsetY),
                           f32(v.u) / 16.0f, f32(v.v) / 16.0f, color);
    }
    prim->sprite.texHandle = gSink.resolveTexture(0);
    prim->sprite.alpha = sdfPrimAbe(aPrim) ? gState.alpha : GsAlpha::Opaque;
    // The caller owns the tag the guest links into and registers it itself; we have no handle on
    // it, so the draw is ordered by the table the 2D state names.
    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);
    xsdfDrawTagLink(tag, packet);
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];
    gSink.addSprite(tag, prim->sprite);
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);
    }
}

/// @fn sdfPrimLinkG4
/// @note SLUS-20911 0x002AF210
void xsdfPrimLinkG4(u32 aPrim, const PrimVertex (&aVertex)[4], xsdfAllocFn aAllocator) {
    // 0x2AF210..0x2AF29B: the caller's allocator, or the engine's own when it passed none.
    xsdfAllocFn alloc = aAllocator ? aAllocator : &xsdfGrAllocPacketBuff;  // 0x2AF29C
    xsdfPacket_t* packet = alloc(xsdf2DGetPrimPacketSize(4));               // 0x2AF2A4
    xsdf2DPrim_t* prim = xsdf2DGetPrimPtr(packet);
    static constexpr int kCycle[4] = {0, 1, 3, 2};
    for (int i = 0; i < 4; i++) {  // 0x2AF324
        const PrimVertex& v = aVertex[i];
        xsdfPrimSetG4Elem(prim, kCycle[i], fromFixed(v.x, kOffsetX), fromFixed(v.y, kOffsetY),
                          Rgba::fromPacked(v.color));
    }
    prim->sprite.alpha = sdfPrimAbe(aPrim) ? gState.alpha : GsAlpha::Opaque;
    // The caller owns the tag the guest links into and registers it itself; we have no handle on
    // it, so the draw is ordered by the table the 2D state names.
    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);
    xsdfDrawTagLink(tag, packet);
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];
    gSink.addSprite(tag, prim->sprite);
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);
    }
}

/// @fn fclDrawSpriteCore
/// @note SLUS-20911 0x00214730
void xfclDrawSpriteCore(s32 aX, s32 aY, s32 aW, s32 aH, u32 aCol, fclSprite_PTR_t aSprite,
                        u32 aFlipFlag, u32 aAlphaFlag) {
    if (!aSprite) return;  // 0x214730..0x21478B
    xsdfTexSetFilter(nullptr, 0);  // 0x21478C, 0x214794, 0x2147A8
    xsdf2DGetPrimPacketSize(1);                       // 0x2147BC
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(1);  // 0x2147C4
    xsdf2DMakePrimPacketHeader(packet, 0);            // 0x2147E4
    xsdf2DPrim_t* prim = xsdf2DGetPrimPtr(packet);    // 0x2147EC

    // Half a texel in, on every edge, the way the engine offsets them.
    const f32 u0 = f32(u16(aSprite->X) + 8) / 16.0f;             // 0x214800
    const f32 u1 = f32(u16(u32(aSprite->X) >> 16) + 8) / 16.0f;
    const f32 v0 = f32(u16(aSprite->Y) + 8) / 16.0f;              // 0x2147FC
    const f32 v1 = f32(u16(u32(aSprite->Y) >> 16) + 8) / 16.0f;
    // 0x21484C, 0x214858, 0x214864, 0x21486C, 0x214874, 0x2148A0: the flip flag's four branches
    // 0x214940, 0x21494C, 0x214958, 0x214960: and the alpha flag's
    xfclDrawPanelSprite2(prim, f32(aX) / 16.0f - 0.5f, f32(aY) / 16.0f - 0.25f, f32(aW),
                         f32(aH) * 0.5f, (aFlipFlag & 1) ? u1 : u0, (aFlipFlag & 2) ? v1 : v0,
                         (aFlipFlag & 1) ? u0 : u1, (aFlipFlag & 2) ? v0 : v1);
    // 0x214964, 0x214978, 0x214980
    // 0x2148F4 puts Flag in the packet's Q slot, measured 0 and 0x10 over a menu, so it is the
    // flag the database types and not a second color. The color is Col.
    if (aSprite->Flag & ~0x10u) reportUnsupported("fclSprite Flag", aSprite->Flag);
    const u32 colorWord =
        aCol ? u32Ptr_t{aCol}.valueOr()
             : u32Ptr_t{aSprite.addr + u32(offsetof(fclSprite_t, Col))}.valueOr();
    prim->sprite.setColor(Rgba::fromColorWord(colorWord));
    prim->sprite.texHandle = gSink.textures->getHandle(aSprite->TexHandle, gSink.frame);
    prim->sprite.alpha = aAlphaFlag == 1   ? GsAlpha::Additive
                         : aAlphaFlag == 2 ? GsAlpha::ReverseSubtract
                                           : GsAlpha::Blend;

    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);                            // 0x21498C
    xsdf2DLinkTexPacket(tag, aSprite->TexHandle.addr);  // 0x21499C
    xsdfDrawTagLink(tag, packet);                    // 0x2149A8
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];  // 0x2149B4
    gSink.addSprite(tag, prim->sprite);
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);  // 0x2149C0, 0x2149C4
    }
}

/// Alpha counts down from 128, and nothing draws once the fade is clear.
/// The frame already presented, put back over the screen. Both entry points differ only in the
/// alpha, so they share a body the way the guest's two copies do.
/// @fn fldDrawFrameSprite
/// @note SLUS-20911 0x0012AC08
/// @fn fldDrawFrameSpriteAlpha
/// @note SLUS-20911 0x0012AFD0
void xfldDrawFrameSprite(u8 aAlpha) {
    xsdfPrimAllocDrawTag();                           // 0x12AC38
    xsdfPacket_t* env = xsdfGrAllocPacketBuff(1);     // 0x12AC44
    const u32 frame = xdds3GetFrameID();              // 0x12AC4C

    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);
    dds3DrawEnv_t* drawEnv = &xdds3DrawEnv[frame & 1];
    xsdfGrLinkDispFrameTex(tag, &drawEnv->stdDrawEnv);  // 0x12AC88, 0x12AC8C, 0x12B058, 0x12B05C
    // 0x12AFD0..0x12B1BB: the alpha entry runs the same body with the caller's alpha
    xsdfDrawTagLink(tag, env);                       // 0x12ACD4
    xsdfPacket_t* packet = xsdf2DAllocSprite();       // 0x12AD24
    xsdf2DPrim_t* prim = xsdf2DGetPrimPtr(packet);    // 0x12AD30
    prim->sprite = DrawSprite::fromRect(-0.5f, -0.3125f, 512.0f, 224.0f, 0.0f, 0.0f,
                                        kGuestWidth, kGuestHeight);
    prim->sprite.setColor(Rgba{0x80, 0x80, 0x80, aAlpha});
    prim->sprite.texHandle = TexHandle{kPreviousFrameHandle};
    prim->sprite.alpha = GsAlpha::Blend;
    xsdfDrawTagLink(tag, packet);                    // 0x12ADA0

    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];  // 0x12ADA8, 0x12ADB0
    gSink.addSprite(tag, prim->sprite);
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);  // 0x12ADBC, 0x12ADC0
    }
}

/// @fn dds3DrawFade
/// @note SLUS-20911 0x001052D8
void xdds3DrawFade(u8 aR, u8 aG, u8 aB, u8 aState) {
    xdds3GetFrameID();  // 0x105304
    xdds3FadeState = aState;  // 0x10530C
    if (xdds3FadeState == 0) return;
    xdds3FadeColorB = aB;                                    // 0x105330
    xfadergba = u32(aR) | (u32(aG) << 8) | (u32(aB) << 16);  // 0x105338
    xdds3FadeColorG = aG;                                    // 0x105348
    // @optout 0x105370 packetbuf: dds3FadePacket is the quadword pair the fade is written
    // into, and we build a sprite instead
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(1);
    xsdf2DPrim_t* prim = xsdf2DGetPrimPtr(packet);
    prim->sprite = DrawSprite::fromRect(0, 0, kGuestWidth, kGuestHeight, 0, 0, 0, 0);
    prim->sprite.setColor(Rgba{aR, aG, aB, u8(128 - aState)});
    prim->sprite.alpha = GsAlpha::Blend;

    xsdfDrawTag_t* tag = xdds3FontReqAllocDrawTag();
    tag->next = nullptr;  // 0x105408
    tag->z = 0.0f;        // 0x10547C
    tag->head = true;     // 0x10551C
    tag->tail = packet;   // 0x105524, 0x105528
    xsdfDrawTagLink(tag, packet);  // 0x105534
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);  // 0x105548
    }
    // 0x105554..0x10557F
    gSink.addSprite(tag, prim->sprite);
}

/// @fn cmpDrawBoxG
/// @note SLUS-20911 0x0023A1A8
void xcmpDrawBoxG(s32 aX, s32 aY, s32 aW, s32 aH, u32 aPCol) {
    u32 aCol[4];
    for (int c = 0; c < 4; c++) aCol[c] = u32Ptr_t{aPCol + u32(c) * 4}.valueOr();
    xsdf2DGetPrimPacketSize(4);                       // 0x23A210
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(1);  // 0x23A21C
    xsdf2DMakePrimPacketHeader(packet, 0);            // 0x23A240
    xsdf2DPrim_t* prim = xsdf2DGetPrimPtr(packet);    // 0x23A248
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];  // 0x23A25C
    prim->sprite = DrawSprite::fromRect(f32(aX) / 16.0f, f32(aY) / 16.0f, f32(aW),
                                       f32(aH) * 0.5f, 0, 0, 0, 0);
    // Top left, top right, bottom left, bottom right.
    static constexpr int kCorner[4] = {0, 1, 3, 2};
    for (int i = 0; i < 4; i++) {
        u32 lane[4];
        xcmpExpandColor(lane, aCol[i]);  // 0x23A268
        prim->sprite.corner[kCorner[i]].color =
            Rgba{u8(lane[0]), u8(lane[1]), u8(lane[2]), u8(lane[3])};
    }
    prim->sprite.alpha = gState.alpha;

    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);          // 0x23A2EC
    xsdfDrawTagLink(tag, packet);  // 0x23A2F8
    gSink.addSprite(tag, prim->sprite);
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);  // 0x23A304, 0x23A308
    }
}

/// @fn cmpDrawTriG
/// @note SLUS-20911 0x00239FD0
void xcmpDrawTriG(u32 aPX, u32 aPY, u32 aPCol) {
    s32 aX[3];
    s32 aY[3];
    u32 aCol[3];
    for (int c = 0; c < 3; c++) {
        aX[c] = s32(u32Ptr_t{aPX + u32(c) * 4}.valueOr());
        aY[c] = s32(u32Ptr_t{aPY + u32(c) * 4}.valueOr());
        aCol[c] = u32Ptr_t{aPCol + u32(c) * 4}.valueOr();
    }
    xsdf2DGetPrimPacketSize(3);                       // 0x23A010
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(1);  // 0x23A01C
    xsdf2DMakePrimPacketHeader(packet, 0);            // 0x23A040
    xsdf2DPrim_t* prim = xsdf2DGetPrimPtr(packet);    // 0x23A048
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];  // 0x23A058
    for (int i = 0; i < 3; i++) {
        u32 lane[4];
        xcmpExpandColor(lane, aCol[i]);  // 0x23A064
        prim->sprite.corner[i] =
            SpriteCorner{f32(aX[i]) / 16.0f, f32(aY[i]) / 16.0f, 0.0f, 0.0f,
                         Rgba{u8(lane[0]), u8(lane[1]), u8(lane[2]), u8(lane[3])}};
    }
    // A sprite carries four corners, so the last repeats and its triangle collapses.
    prim->sprite.corner[3] = prim->sprite.corner[2];
    prim->sprite.alpha = gState.alpha;

    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);          // 0x23A108
    xsdfDrawTagLink(tag, packet);  // 0x23A114
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);  // 0x23A120, 0x23A124
    }
    gSink.addSprite(tag, prim->sprite);
}

// The palette is indexed by a byte in the vertex, and no measured mesh comes near this.

// unpackTexture scales screenScale.z by this at 0x00CC; the shader has no such term and every
// savestate measures it zero.
void reportRootDepthBias(f32 bias) {
    if (bias != 0.0f) reportUnsupported("root depth bias", std::bit_cast<u32>(bias));
}

/// The one influence a body without weights has, so every mesh reaches the shader alike.
void setRigidInfluence(MeshVertex& vertex, const sdfVector3_t& position,
                       const sdfVector3_t* normal) {
    static constexpr sdfVector3_t kNoNormal{};
    const sdfVector3_t& n = normal ? *normal : kNoNormal;
    MeshInfluence& only = vertex.influence[0];
    only.x = position.x;
    only.y = position.y;
    only.z = position.z;
    only.nx = n.x;
    only.ny = n.y;
    only.nz = n.z;
    only.weight = 1.0f;
    only.bone = 0.0f;
}


// A packet's payload advances quadword by quadword, whatever a stream's own element size is.
u32 align16(u32 address) { return (address + 15) & ~15u; }

/// The engine keeps the left operand in VU0's VF28-31 and passes only the right, which is what
/// "Rev" names.
/// @fn MultMatrixRev
/// @note SLUS-20911 0x002B66B8
Matrix4 xMultMatrixRev(const Matrix4& aLeft, const Matrix4& aMatrix) {
    // 0x2B66B8..0x2B66DB
    Matrix4 out{};
    for (int i = 0; i < 4; i++) {
        for (int c = 0; c < 4; c++) {
            out.m[i][c] = aLeft.m[i][0] * aMatrix.m[0][c] + aLeft.m[i][1] * aMatrix.m[1][c] +
                          aLeft.m[i][2] * aMatrix.m[2][c] + aLeft.m[i][3] * aMatrix.m[3][c];
        }
    }
    return out;
}

// The six bits VCLIP sets. sdfModelCheckBounds ANDs them across the corners, so only agreement
// matters, not which plane each names.
u32 clipCode(const f32 v[4]) {
    const f32 w = v[3];
    u32 code = 0;
    for (int axis = 0; axis < 3; axis++) {
        if (v[axis] > w) code |= 1u << (2 * axis);
        if (v[axis] < -w) code |= 1u << (2 * axis + 1);
    }
    return code;
}

void transformPoint(const Matrix4& m, f32 x, f32 y, f32 z, f32 out[4]) {
    for (int c = 0; c < 4; c++) {
        out[c] = x * m.m[0][c] + y * m.m[1][c] + z * m.m[2][c] + m.m[3][c];
    }
}

// Both lists keep a pointer to their item array at sdfListItems, so an item costs two loads.
u32 readListItem(u32 listAddr, u32 index) {
    const u32 items = u32Ptr_t{listAddr + sdfListItems}.valueOr();
    return items ? u32Ptr_t{items + 4 * index}.valueOr() : 0;
}

/// A VIF code, as the mesh body carries it: an immediate, a count, then the command.

bool readVifCode(u32 at, VifCode& out) {
    const u32Ptr_t at32{at};
    if (!at32) return false;
    const u32 word = *at32;
    out.immediate = word & 0xFFFF;
    out.count = (word >> 16) & 0xFF;
    out.command = (word >> 24) & 0xFF;
    out.next = at + 4;
    return true;
}

bool isUnpack(const VifCode& code) { return (code.command & sdfVifUnpack) == sdfVifUnpack; }

/// Bytes one unpacked element takes, from the command's own component count and element size.
u32 unpackWidth(const VifCode& code) {
    static const u32 kElementBytes[4] = {4, 2, 1, 0};
    return kElementBytes[code.command & 3] * (((code.command >> 2) & 3) + 1);
}

/// The UNPACK naming one vertex array, checked against the caller's width and count so a misread
/// offset is refused rather than decoded into plausible nonsense.
bool readArray(u32 at, u32 width, u32 count, u32& data, u32& next) {
    VifCode code;
    if (!readVifCode(at, code) || !isUnpack(code)) return false;
    if (unpackWidth(code) != width || code.count != count) return false;
    data = code.next;
    next = data + width * count;
    return true;
}

/// Vertices in the batch, and for a weighted mesh the nodes beyond the first blending into them.
bool readBatchHeader(u32 at, u16& first, u16& second, u32& next) {
    VifCode code;
    if (!readVifCode(at, code) || !isUnpack(code) || code.count != 1) return false;
    const u16* pair = u16Ptr_t{code.next}.slice(2);
    if (!pair) return false;
    first = pair[0];
    second = pair[1];
    next = code.next + 4;
    return true;
}

/// Past the microprogram kick and the flush that close a batch, to the next quadword.
u32 skipBatchEnd(u32 at) {
    for (int code = 0; code < 4; code++) {
        VifCode tag;
        if (!readVifCode(at, tag)) return 0;
        at = tag.next;
        if (tag.command == sdfVifFlushEnd) return align16(at);
    }
    return 0;
}

LogChannel log("model");

XsdfModel gModel;
ModelParser* gConvert = nullptr;

// 0x2B9230/0x2B923C: sdfCorePacketGen keeps its two float arguments in $f20 and $f21 and hands
// them straight back, so entry is where they can be read.
/// @fn corePktSetFaceDivParm
/// @note SLUS-20911 0x002B6A30
void xcorePktSetFaceDivParm(f32 soften, f32 bias) {
    // 0x2B6A30..0x2B6A6B
    // @optout 0x2B6A3C enginestate: corePktCurrLight is the packet the terms are stamped into,
    // and we carry them on the draw instead
    if (!gModel.faceDivTermsSeen && gConvert->getConfig()) {
        log.info("face division terms from the guest: soften {} bias {}, fitted {} {}", soften, bias,
                 gConvert->getConfig()->faceDivSoften, gConvert->getConfig()->faceDivBias);
    }
    gModel.faceDivSoftenTerm = soften;
    gModel.faceDivBiasTerm = bias;
    gModel.faceDivTermsSeen = true;
}

// 0x2B6DF4 and 0x2B6E30 read the coordinate's scale from [0].w and its offset from [0].y, and
// 0x2B6E80 interpolates [16] to [16]+[32] across it.
/// @fn corePktApplyFaceDivColor
/// @note SLUS-20911 0x002B72D8
void xcorePktApplyFaceDivColor(corePktCbPtr_t aCb, sdfVector4Ptr_t aParm) {
    // 0x2B72D8..0x2B730B, 0x2B730C..0x2B7337, 0x2B7338..0x2B7343, 0x2B7344..0x2B7347,
    // 0x2B7348..0x2B7367, 0x2B7368..0x2B736F
    const sdfVector4_t* parm = aParm.slice(3);
    if (!parm || !aCb) return;
    const u32 aMatPacket = aCb->matPacketPtr.addr;

    // @optout 0x2B72F8 packetbuf: cb->pointBuf0 is the vertex buffer the ramp is written into
    // @optout 0x2B72FC packetbuf: cb->pointCount bounds that write
    // @optout 0x2B7338 packetbuf: cb->unk120 is its second run, and the shader interpolates the
    // endpoints instead of any of it
    MeshRamp& ramp = gModel.faceDivRamps[aMatPacket];
    const f32 lo[3] = {parm[1].x, parm[1].y, parm[1].z};
    const f32 delta[3] = {parm[2].x, parm[2].y, parm[2].z};
    // 128 is one, as the GS modulate has it.
    for (int i = 0; i < 3; i++) {
        ramp.lo[i] = lo[i] / 128.0f;
        ramp.delta[i] = delta[i] / 128.0f;
    }
    ramp.known = true;

    if (!gModel.faceDivParmSeen) {
        gModel.faceDivParmSeen = true;
        log.info("face division ramp, material packet {:08X}: scale {} offset {}", aMatPacket,
                 parm[0].w, parm[0].y);
        log.info("  lo {} {} {}, delta {} {} {}", lo[0], lo[1], lo[2], delta[0], delta[1], delta[2]);
    }
}

void readLighting(sdf3DLightParallelPacketPtr_t aLight) {
    gModel.lighting = {};
    const sdf3DLightParallelPacketPtr_t light = aLight ? aLight : gModel.sdf3DLightDefault.valueOr();
    if (!light) return;

    for (u32 i = 0; i < kParallelLights; i++) {
        for (usize axis = 0; axis < 3; axis++) {
            // normal is a matrix whose columns are the light vectors, so a light reads down them.
            gModel.lighting.direction[i][axis] = light->normal[axis][i];
            gModel.lighting.color[i][axis] = light->color[i][axis];
        }
        gModel.lighting.base[i] = light->colorBase[i];
        gModel.lighting.ambientColor[i] = light->color[3][i];
        gModel.lighting.fullColor[i] = light->color[4][i];
    }

    static std::set<u32> shown;
    if (gConvert->getConfig()->dumpLight && shown.insert(light.addr).second) {
        JsonObject row;
        row.addString("dump", "light").addHexDigits("at", light.addr, 8);
        row.addReals("base", {gModel.lighting.base[0], gModel.lighting.base[1], gModel.lighting.base[2]}, 4);
        for (u32 c = 0; c < 5; c++) {
            const std::string name = std::format("color{}", c);
            row.addReals(name.c_str(),
                         {light->color[c][0], light->color[c][1], light->color[c][2]}, 4);
        }
        row.beginArray("lights");
        for (u32 i = 0; i < kParallelLights; i++) {
            row.beginObject();
            row.addReals("direction", {gModel.lighting.direction[i][0], gModel.lighting.direction[i][1],
                                       gModel.lighting.direction[i][2]}, 4);
            row.addReals("color",
                         {gModel.lighting.color[i][0], gModel.lighting.color[i][1], gModel.lighting.color[i][2]}, 4);
            row.endObject();
        }
        row.endArray();
        log.dump(row.take());
    }
}

// F is per vertex, 255 keeping the color and 0 replacing it with FOGCOL. VU1 builds it as
// `fogParm[2] + fogParm[3] / w` clamped to fogParm[0]..fogParm[1] (0x04DC, 0x04DF, 0x04E3).
void readFog(sdf3DFogPacketPtr_t aFog) {
    gModel.fog = {};
    const sdf3DFogPacketPtr_t fog = aFog ? aFog : gModel.sdf3DFogDefault.valueOr();
    if (!fog) return;

    const u32 fogcol = u32(fog->fogColor);
    gModel.fog.on = true;
    gModel.fog.minimum = fog->fogParm[0] / 255.0f;
    gModel.fog.maximum = fog->fogParm[1] / 255.0f;
    gModel.fog.offset = fog->fogParm[2] / 255.0f;
    gModel.fog.scale = fog->fogParm[3] / 255.0f;
    gModel.fog.color[0] = f32(fogcol & 0xFF) / 255.0f;
    gModel.fog.color[1] = f32((fogcol >> 8) & 0xFF) / 255.0f;
    gModel.fog.color[2] = f32((fogcol >> 16) & 0xFF) / 255.0f;
}

// The sampler state lives in the material's packet, which sdfMatUpdatePacketBasic fills from the
// texture handle rather than from the material.
/// @fn sdfMatMakePacket
/// @note SLUS-20911 0x002B3558
void xsdfMatMakePacket(const sdfMat_t& material, DrawMesh& mesh, Frame* frame) {
    // 0x2B3558..0x2B3593
    mesh.diffuse = Rgba::fromPacked(material.diffuse);
    mesh.ambient = Rgba::fromPacked(material.ambient);

    // The finished buffer, not the one about to be rewritten: our walk runs at sdfModelLinkOt's
    // entry, so the index the frame id names is the one sdfMatListUpdatePacket is about to fill.
    const u32 frameId = gModel.sdfFrameId ? (*gModel.sdfFrameId & 1) ^ 1 : 0;
    const sdfMatPacketPtr_t packet{material.packet[frameId].addr};
    if (!packet) return;
    mesh.specular = Rgba::fromPacked(packet->specularColor);
    gConvert->getTrace().countSpecular(mesh.attrBits, packet->specularColor);
    // 0x2B8D9C: one global texture for every reflection pass, so it is keyed by that address.
    if (mesh.attrBits & sdfCoreAttrRefMapPass) {
        mesh.refMap = gConvert->getTextures()->getHandleForTex0(TexHandle{gModel.sdf3DRefMapParam.addr},
                                                  gModel.sdf3DRefMapParam.valueOr());
    }
    // The specular pass names a handle, so it resolves like any other texture.
    if (mesh.attrBits & sdfCoreAttrSpecularPass) {
        if (const u32 handleAddr = gModel.sdf3DSpecularTex.valueOr()) {
            const sdfTexHandlePtr_t handle{handleAddr};
            // The handle's own pixels, with its upload packet's TEX0 for TCC so the alpha
            // survives. The overlay's real texture is never uploaded. See TODO V7.
            mesh.specularTex = gConvert->getTextures()->getHandle(TexHandle{handleAddr}, frame);
            if (handle) {
                mesh.specularTex0 =
                    guest_ptr<u64>{handle->packet.addr + kTex0InPacket}.valueOr();
            }
        }
    }
    mesh.tex0 = packet->tex0Data;
    mesh.tex1 = packet->tex1Data;
    mesh.clamp = packet->clampData;
    // A material can name a TEX0 and carry no handle. The packet's address is stable, so the
    // recovered texture files under that.
    if (!mesh.texHandle.addr) {
        mesh.texHandle = gConvert->getTextures()->getHandleForTex0(TexHandle{packet.addr}, packet->tex0Data);
    }
    // Inline at texTm rather than behind it: that field's address is what corePktXfmLightClipUV
    // is handed.
    std::memcpy(mesh.texMatrix, packet->texTm, sizeof(mesh.texMatrix));
    gConvert->getTrace().countTexMatrix(packet->texTm[0] != 0.0f || packet->texTm[3] != 0.0f);

    // 0x2B6A4C: read off the material *packet* at +44 and +40, plus the root's, soften clamped to
    // at least one. sdfMat_t carries the same two fields at 0x44 and 0x40 with different values.
    const f32 softenTerm = gModel.faceDivTermsSeen ? gModel.faceDivSoftenTerm : gConvert->getConfig()->faceDivSoften;
    const f32 biasTerm = gModel.faceDivTermsSeen ? gModel.faceDivBiasTerm : gConvert->getConfig()->faceDivBias;
    mesh.multi.soften = std::max(packet->fdSoften + softenTerm, 1.0f);
    mesh.multi.bias = packet->fdBias + biasTerm;
    mesh.faceDivSoften = mesh.multi.soften;
    mesh.faceDivBias = mesh.multi.bias;

    // Everything below belongs to the second pass. The ramp terms above do not, and leaving them
    // behind this guard put a seam along every mesh boundary.

    // lightSetup 0x00A3 files this at mem[910].w: it lerps the sum's base toward 1.0. It belongs
    // to every mesh, not only one with a second UV set, so it is read outside that guard.
    mesh.flatten = packet->constant;

    xcorePktEmitMultiTexPass(material, packet, mesh, frame);
}

/// The second textured pass. Split from the material read so its early return cannot skip the
/// face-division terms, which belong to the first pass.
/// Algo 3's flat second colour, in place of the two-endpoint ramp.
/// @fn corePktSetConstColor
/// @note SLUS-20911 0x002B7370
void xcorePktSetConstColor(DrawMesh& mesh, u32 aColor) {
    mesh.multi.diffuse = Rgba::fromPacked(aColor);
}

/// @fn corePktEmitMultiTexPass
/// @note SLUS-20911 0x002B8C30
void xcorePktEmitMultiTexPass(const sdfMat_t& material, sdfMatPacketPtr_t packet,
                                         DrawMesh& mesh, Frame* frame) {
    // 0x2B8C30..0x2B8C7F, 0x2B8C80..0x2B8C87, 0x2B8C88..0x2B8C8F, 0x2B8C98..0x2B8CDB,
    // 0x2B8CE0..0x2B8CE7, 0x2B8CE8..0x2B8CEB, 0x2B8CEC..0x2B8D17
    // @optout 0x2B8C44 elsewhere(sdfCorePacketGen): the cb's material packet, our argument
    // @optout 0x2B8C48 elsewhere(sdfCorePacketGen): its vertex count, which the mesh carries
    // @optout 0x2B8C4C vu0: the second UV buffer corePktXfmUV writes
    // @optout 0x2B8C54 vu0: corePktXfmUV transforms them, where our shader applies texTm
    // @optout 0x2B8C58 vu0: the vertex buffer it reads
    // @optout 0x2B8C5C vu0: corePktLerpUVChain walks the chained sets
    // @optout 0x2B8C60 vu0: and the chain it walks
    // @optout 0x2B8C98 elsewhere(corePktApplyFaceDivColor): fdSoften, one of the ramp's terms
    // @optout 0x2B8C9C elsewhere(sdfCorePacketGen): the draw colour the ramp is built against
    // @optout 0x2B8CA4 elsewhere(corePktApplyFaceDivColor): fdBias, the other
    // @optout 0x2B8CB0 elsewhere(corePktApplyFaceDivColor): corePktCurrLight, its third input
    // @optout 0x2B8CBC elsewhere(corePktApplyFaceDivColor): and the ambient it starts from
    // @optout 0x2B8CC0 elsewhere(corePktApplyFaceDivColor): corePktMakeFaceDivParm builds the
    // endpoints on the stack, which is the only place they exist
    // @optout 0x2B8CCC hooked: corePktApplyFaceDivColor is where we take them
    // @optout 0x2B8CFC gif: corePktEmitTexTrisCtx2 writes the pass, which the mesh carries
    if (!(mesh.attrBits & sdfCoreAttrMultiTexPass)) return;
    mesh.multi.texHandle = gConvert->getTextures()->getHandle(material.multiTexHandle, frame);
    // The second pass can name its TEX0 with no handle just as the first can. Keyed apart from the
    // first by an offset into the same packet, since one material carries both.
    if (!mesh.multi.texHandle.addr) {
        mesh.multi.texHandle =
            gConvert->getTextures()->getHandleForTex0(TexHandle{packet.addr + 8}, packet->mtexTex0Data);
    }
    mesh.multi.alpha = packet->mtexAlpha;
    mesh.multi.tex0 = packet->mtexTex0Data;
    mesh.multi.tex1 = packet->mtexTex1Data;
    mesh.multi.clamp = packet->mtexClampData;
    mesh.multi.diffuse = Rgba::fromPacked(packet->mtexDiffuse);
    mesh.multi.on = mesh.multi.texHandle.addr != 0;
    // The ramp runs only where the two diffuse colors differ, and only under algo 3.
    mesh.multi.algo = packet->mtexAlgo;
    // 0x2B8C90: the packet's own two colors, at +8 and +12. Taking the first from sdfMat_t is a
    // different value and claims a ramp on meshes the engine never ramps.
    mesh.multi.ramp = packet->mtexAlgo < 3 && packet->diffuse != packet->mtexDiffuse;
    // 0x2B8CE0: algo 3 takes the second diffuse as a flat colour instead of a ramp.
    if (packet->mtexAlgo == 3) xcorePktSetConstColor(mesh, packet->mtexDiffuse);
    mesh.multi.constant = packet->constant;
    // Generation runs after the walk, so these are the previous frame's endpoints. Every captured
    // one is algo 3 and every ramp mesh algo 1 or 2, so on the measured scenes this path is inert.
    if (const auto it = gModel.faceDivRamps.find(packet.addr); it != gModel.faceDivRamps.end()) {
        mesh.multi.rampColors = it->second;
    }
    if (packet->mtexAlgo > 3) reportUnsupported("corePktEmitMultiTexPass mtexAlgo", packet->mtexAlgo);

}

bool readScreenTm() {
    if (!gModel.sdf3DScreenTm) return false;
    gModel.screenTm = *gModel.sdf3DScreenTm;
    if (const sdfVector4_t* p = gModel.sdf3DScreenParam.slice(7)) {
        gModel.projectionScale[0] = p[3].x;
        gModel.projectionScale[1] = p[4].y;
    }
    static bool shown = false;
    if (!shown && gConvert->getConfig()->dumpScreen) {
        shown = true;
        JsonObject row;
        row.addString("dump", "screenTm").beginArray("m");
        for (const auto& r : gModel.screenTm.m) {
            row.beginObject().addReals("row", {r[0], r[1], r[2], r[3]}, 4).endObject();
        }
        row.endArray();
        log.dump(row.take());
    }
    return true;
}

/// Culls a node by its bounds against the VU0 clip codes the engine tests.
/// @fn sdfModelCheckBounds
/// @note SLUS-20911 0x002B1838
bool xsdfModelCheckBounds(sdfModelBoundsPtr_t aBounds, const Matrix4& clip) {
    // 0x2B1838..0x2B194B: eight corners through the clip matrix, VCLIP's six bits ANDed.
    if (!aBounds) return false;
    const f32* box = &aBounds->min.x;

    u32 code = 0x3F;
    for (int corner = 0; corner < 8; corner++) {
        f32 out[4];
        transformPoint(clip, box[(corner & 1) ? 3 : 0], box[(corner & 2) ? 4 : 1],
                       box[(corner & 4) ? 5 : 2], out);
        code &= clipCode(out);
        if (!code) return false;
    }
    return code != 0;
}

/// The engine's node list, turned into meshes instead of ordering-table entries.
/// @fn sdfModelLinkOt
/// @note SLUS-20911 0x002B1DB8
void xsdfModelLinkOt(sdfModelOtPtr_t aOt, sdfModelRootNodePtr_t aRoot, Frame* frame) {
    // The group names four of the 85 tables, and a packet's otIndex picks one. The guest reads it
    // per packet at registration; we resolve it once, since the node loop below needs the mapping.
    for (int i = 0; i < 4; i++) {
        const u32 table = aOt ? xsdfGrOtIndexOf(aOt->ot[i].addr) : ~0u;
        if (table == ~0u) {
            reportUnsupported("model ordering table outside sdfGrOtTable", i);
            gModel.otTable[i] = 0;
        } else {
            gModel.otTable[i] = table;
        }
    }
    if (!aRoot || !readScreenTm()) return;
    const guest_ref<sdfModelRootNode_t> root{aRoot};
    if (frame) {
        frame->screenTm = gModel.screenTm;
        frame->projectionScale[0] = gModel.projectionScale[0];
        frame->projectionScale[1] = gModel.projectionScale[1];
    }

    // 0x2B1DB8..0x2B1E0B
    gConvert->getTrace().countRoot((root->flags & sdfModelRootTempPackets) != 0);

    // 0x2B1E0C..0x2B1E13: the temporary packets are linked first, over the same nodes.
    if (root->flags & sdfModelRootTempPackets) xsdfModelTempPacketLinkOt(root, frame);

    // 0x2B1E14..0x2B1E73: the root's own light and fog, or the engine defaults when it has none.
    // @optout 0x2B1E24 packetbuf: sdfMatListUpdatePacket rebuilds each material's DMA packet
    readLighting(root->light);
    readFog(root->fog);

    const u32 frameId = gModel.sdfFrameId ? *gModel.sdfFrameId : 0;

    // 0x2B1E6C: the root's own bias under flags & 2, else the global. It rides in the per-node
    // state packet, which nothing here models.
    gConvert->reportFaceDiv("sdfModelLinkOt faceDivBias", (root->flags & sdfModelRootOwnFaceDivBias)
                                                    ? root->faceDivBias
                                                    : gModel.sdfFaceDivBias.valueOr());
    // The terms reach sdfCorePacketGen in $f12 and $f13, so Config's values are only the first
    // frame's fallback.
    if (root->flags & sdfModelRootOwnFaceDivBias) {
        reportUnsupported("sdfModelLinkOt root faceDiv terms", root->flags);
    }
    if (const u8 soften = gModel.corePktSoftenBias.valueOr()) {
        reportUnsupported("corePktSoftenBias", soften);
    }
    // 0x2B1E74..0x2B1E7F
    // @optout 0x2B1E80 elsewhere(sdfModelTempPacketLinkOt): sdfFaceDivSoften, which that walk takes
    // 0x2B1E80..0x2B1E87, 0x2B1E88..0x2B1EAB
    // 0x2B1E94: flags & 8 suppresses the type 1 and 2 paths.
    const bool suppressLinked = (root->flags & sdfModelRootNoLinkedPackets) != 0;

    const sdfModelNodeListPtr_t list = root->nodeList;
    if (!list) return;
    gModel.nodeListAddr = root->nodeList.addr;

    // 0x2B1EAC..0x2B1EBF
    // A chain names a material by its sdfMatMakePacket address, so the lookup is built once.
    gModel.matByPacket.clear();
    if (const sdfMatListPtr_t matList = root->matList) {
        for (s16 m = 0; m < matList->count; m++) {
            const sdfMatPtr_t material{readListItem(root->matList.addr, u32(m))};
            if (!material) continue;
            for (const sdfVoidPtr_t& packet : material->packet) {
                if (packet.addr) gModel.matByPacket[packet.addr] = material.addr;
            }
        }
    }

    // 0x2B1EC0: the node pointers follow the list header.
    // @optout 0x2B1ECC enginestate: node->prev is the list's own link, where we index the array
    for (s16 i = 0; i < list->count; i++) {
        const sdfModelNodePtr_t node{u32Ptr_t{list->items.addr + 4u * u32(i)}.valueOr()};
        if (!node) {
            gConvert->getTrace().countNode(ModelTrace::Node::Unreadable);
            continue;
        }
        const u32 chainAddr = node->drawPacket[frameId & 1].addr;
        if (!chainAddr) {
            gConvert->getTrace().countNode(ModelTrace::Node::NoChain);
            continue;
        }
        // 0x2B1EEC
        if (node->flags & sdfModelNodeHidden) {
            gConvert->getTrace().countNode(ModelTrace::Node::Hidden);
            continue;
        }

        NodeDraw draw{};
        std::memcpy(&draw.model, &node->tm, sizeof(Matrix4));
        // 0x2B1EF4..0x2B1F1F: xMultMatrixRev over the node's own tm and the screen matrix, which
        // 0x2B1F10 loads from sdf3DScreenTm and readScreenTm captured once this frame.
        // readScreenTm captured and conditioned this once for the frame; the raw guest matrix is
        // not the same thing.
        draw.clip = xMultMatrixRev(draw.model, gModel.screenTm);
        // 0x2B1F20
        if (node->bounds && xsdfModelCheckBounds(node->bounds, draw.clip)) {
            gConvert->getTrace().countNode(ModelTrace::Node::BoundsCulled);
            continue;
        }

        // 0x2B1F20..0x2B1F2F, 0x2B1F30..0x2B1F4F, 0x2B1F50..0x2B1F53
        draw.sortZ = draw.clip.m[3][3];
        // 0x2B1F48: the node's own colour, or the root's.
        draw.color = (node->flags & sdfModelNodeOwnColor) ? node->color : root->color;
        // 0x2B1F54..0x2B1F6F, and 0x2B1F6C is signed, so a drawAttr of 0xFFFF sign-extends and the
        // node's high bits survive. Truncating to 16 drops the soften bias with them.
        draw.attrMask = u32(s32(s16(root->drawAttr))) & node->drawAttr;
        // 0x2B1F70..0x2B1F8B
        // @optout 0x2B1F8C..0x2B1FBF packetbuf: the z bias goes into the state packet
        // @optout 0x2B1F94 packetbuf: sdfCoreDrawSetZBias writes it
        reportRootDepthBias(root->zBias);
        // 0x2B1FC0: a packet's otIndex of zero means table 0 or 1 by the sign of the color.
        draw.otIndex = s32(draw.color) < 0 ? 0 : 1;
        // sdfCorePacketGen turns the same flag into corePktBuildFaceList's backfaceSign.
        draw.reverse = suppressLinked;

        gConvert->getTrace().countNode(ModelTrace::Node::Drawn);

        // The chain walk is this function's inner loop, not a helper: each header names an ot
        // index and a type, and the type picks which of the three packet paths runs.
        // 0x2B1FC0..0x2B1FDF
        // @optout 0x2B1FE0..0x2B207F packetbuf: the node's own draw tag and transform packet
        // @optout 0x2B1FE0 packetbuf: sdfPrimAllocDrawTag carves that tag
        // @optout 0x2B1FF0 packetbuf: its sort key, which the draw carries as sortZ
        // @optout 0x2B1FFC packetbuf: its light packet
        // @optout 0x2B2000 packetbuf: sdfGrAllocPacketBuff carves the transform packet
        // @optout 0x2B2004 packetbuf: its fog packet
        // @optout 0x2B2014 packetbuf: sdfModelSetTmRefPacket writes the transform
        // @optout 0x2B2074 packetbuf: sdfDrawTagLinkRef links it
        for (u32 addr = chainAddr; addr;) {
            const sdfModelPacketHeaderPtr_t header{addr};
            if (!header) {
                gConvert->getTrace().countPacket(ModelTrace::Packet::Unknown);
                return;
            }
            NodeDraw packetDraw = draw;
            if (header->otIndex) packetDraw.otIndex = header->otIndex;

            // 0x2B2084 type 2, 0x2B2098 type 1, 0x2B20A8 type 3, each tested before any body.
            // 0x2B208C..0x2B2093, 0x2B20A0..0x2B20A7, 0x2B20B0..0x2B20B7, 0x2B20B8..0x2B20BF
            switch (header->type) {
                case sdfPacketDmaCall:
                    if (suppressLinked) {
                        gConvert->getTrace().countPacket(ModelTrace::Packet::Suppressed);
                        break;
                    }
                    gConvert->getTrace().countPacket(ModelTrace::Packet::DmaCall);
                    gConvert->dumpWords("call", addr + sizeof(sdfModelPacketHeader_t));
                    // 0x2B208C..0x2B2093, 0x2B20C0..0x2B20FF
                // @optout 0x2B20F0 dma: sdfDrawTagLinkCall links the chain as a source chain
                xsdfModelSetNormalDrawPacket(packetDraw, addr + u32(sizeof(sdfModelPacketHeader_t)),
                                             frame);
                    break;
                case sdfPacketShaped:
                    if (suppressLinked) {
                        gConvert->getTrace().countPacket(ModelTrace::Packet::Suppressed);
                        break;
                    }
                    // 0x2B2100..0x2B2107, 0x2B2108..0x2B2123
                gConvert->getTrace().countPacket(ModelTrace::Packet::Shaped);
                    xsdfModelLinkShapedDrawPacket(root, packetDraw, header->dataPtr.addr, frame);
                    break;
                case sdfPacketGenerated:
                    gConvert->getTrace().countPacket(ModelTrace::Packet::Generated);
                // @optout 0x2B2128..0x2B2163 packetbuf: the light, fog and z the packet carries
                // @optout 0x2B2128 packetbuf: sdfCoreDrawSetLight writes the light into the packet
                // @optout 0x2B2130 packetbuf: sdfCoreDrawSetFog does the same for fog
                // @optout 0x2B2164 packetbuf: sdfDrawTagLink links what it generated
                // @optout 0x2B2164..0x2B216B packetbuf: and the link that files it
                // 0x2B216C..0x2B216F, 0x2B2170..0x2B2177, 0x2B2178..0x2B217F
                gModel.currentLink = gModel.linkIndex++;
                    xsdfCorePacketGen(root, packetDraw, sdfCorePacketPtr_t{header->dataPtr.addr}, frame);
                    break;
                default:
                    gConvert->getTrace().countPacket(ModelTrace::Packet::Unknown);
                    reportUnsupported("sdfModelLinkOt packet type", header->type);
                    break;
            }
            addr = header->next.addr;
        }
        // 0x2B2194: the ot group, read here where the guest registers.
        // 0x2B21A0..0x2B21B7
        // @optout 0x2B21AC elsewhere(sdfGrOtNormalReg): the table's registrar, which the sink
        // calls once the mesh is built
        // @optout 0x2B21B0 elsewhere(sdfGrOtNormalReg): that indirect call
        // 0x2B21B8..0x2B21BF, 0x2B21C0..0x2B21C3, 0x2B21C4..0x2B21C7, 0x2B21C8..0x2B21E7,
        // 0x2B21E8..0x2B21EB, 0x2B21EC..0x2B21F3, 0x2B21F4..0x2B21FF, 0x2B2200..0x2B2237
    }
}


/// Reads the generated packet the engine would build, by packet type.
/// @fn sdfCorePacketGen
/// @note SLUS-20911 0x002B9178
void xsdfCorePacketGen(guest_ref<sdfModelRootNode_t> root, const NodeDraw& draw,
                                  sdfCorePacketPtr_t aPacket, Frame* frame) {
    if (!aPacket) return;
    const sdfCorePacket_t& packet = *aPacket;

    // corePktInitCb parses four header words into the cb every corePkt* function below reads, so
    // ours is filled the same way rather than each of them re-reading the packet.
    // @optout 0x2B91A4 vu0: the scratch corePktVu0Run stages its arguments through
    // @optout 0x2B91D0 packetbuf: node->tmPacket is the matrix in packet form, and the walk
    // already carries it as draw.model
    // @optout 0x2B91E4 elsewhere(corePktInitCb): the header parse is the cb fill just below

    corePktCb_t cb{};
    cb.unk172 = draw.reverse ? -1.0f : 1.0f;  // 0x2B9200, 0x2B9210

    // @optout 0x2B9214 elsewhere(sdfModelLinkOt): the walk resolved node->root already
    // The material is the packet's own index into the root's list, and the frame id picks which
    // of its two packets is the finished one.
    sdfMatPacketPtr_t matPacket;
    const sdfMatListPtr_t matList = root->matList;  // 0x2B9224
    const u32 matAddr =
        matList ? u32Ptr_t{matList->items.addr + 4u * u32(packet.header.matIndex)}.valueOr() : 0;
    const sdfMatPtr_t mat{matAddr};  // 0x2B9234

    // @optout 0x2B9238 vu0: corePktFaceDivParm, staged with the light constants
    cb.color = draw.color;                   // 0x2B9240
    cb.fdSoften = gModel.faceDivSoftenTerm;  // 0x2B9248
    if (mat) gConvert->getTrace().countMaterialShading(mat->id, cb.attrBits);  // 0x2B924C
    cb.fdBias = gModel.faceDivBiasTerm;      // 0x2B9258
    if (mat) {
        const u32 frameId = gModel.sdfFrameId ? (*gModel.sdfFrameId & 1) ^ 1 : 0;
        matPacket = sdfMatPacketPtr_t{mat->packet[frameId].addr};  // 0x2B925C
    }
    cb.faceCount = packet.prim.faceCount;  // 0x2B9260
    cb.matPacket2 = matPacket;             // 0x2B9268
    cb.matPacketPtr = matPacket;           // 0x2B926C
    cb.attrBits = packet.prim.attrBits & s32(draw.attrMask);
    cb.orgAttrBits = packet.prim.attrBits;
    cb.unk176 = u32(cb.attrBits);            // 0x2B9270
    cb.pointCount = packet.prim.pointCount;  // 0x2B9274
    cb.packetBits = packet.prim.packetBits;  // 0x2B9278

    // The rest of the guest's body stages matrices into VU0 data memory and runs the
    // microprograms that skin, morph, transform, light and clip the vertices. Every one of those
    // happens on the GPU here, out of the arrays the batch readers below decode.
    // @optout 0x2B927C hooked: corePktSetFaceDivParm takes the terms from $f12 and $f13, which
    // the hook reads at entry. Calling it here with the stored pair pins them to their own
    // previous value, which flattens the shading ramp.
    // @optout 0x2B9280 vu0: the VU0 vertex buffer every one of those runs works in
    // @optout 0x2B9294..0x2B935B vu0: sdf3DScreenTm, the light matrix and the texture matrix are
    // staged into VU0 data memory
    // @optout 0x2B92C0 elsewhere(corePktSkinVertFirst): and its root
    // @optout 0x2B92D0 vu0: the screen matrix, staged for the microprogram
    // @optout 0x2B92E0 vu0: the payload cursor they consume
    // @optout 0x2B92E4 elsewhere(corePktSkinVertFirst): the node list the palette is built from
    // @optout 0x2B92F4 vu0: the screen matrix's four rows
    // @optout 0x2B92FC vu0: row one
    // @optout 0x2B9304 vu0: row two
    // @optout 0x2B930C vu0: row three
    // @optout 0x2B9310 vu0: corePktCurrLight, staged next
    // @optout 0x2B9318 vu0: its first row
    // @optout 0x2B9324 vu0: its second
    // @optout 0x2B9330 vu0: its third
    // @optout 0x2B9338 elsewhere(sdfMatMakePacket): the texture matrix, which we read there
    // @optout 0x2B933C vu0: its first row staged
    // @optout 0x2B9344 vu0: its second
    // @optout 0x2B934C vu0: its third
    // @optout 0x2B935C..0x2B935F vu0: the per-node transform copy loop
    // @optout 0x2B9360..0x2B93BF vu0: its body
    // @optout 0x2B93C0..0x2B94DB vu0: the staging and microprogram body, whose four arms
    // (skinned, morphed, rigid and the type 7 and 8 VU0 paths) we replace with the batch readers
    // below and with skinning in the shader
    // @optout 0x2B93C8 vu0: the face division parameter quadword
    // @optout 0x2B93CC vu0: coreLightConst1
    // @optout 0x2B93D0 vu0: staged
    // @optout 0x2B93D4 vu0: coreLightConst2
    // @optout 0x2B93D8 vu0: staged
    // @optout 0x2B9424 vu0: MultMatrixRev builds the model-view the microprogram uses
    // @optout 0x2B942C vu0: corePktLoadWorldLightMtx stages the light matrix
    // @optout 0x2B94DC vu0: corePktVu0Run is the microprogram itself
    // @optout 0x2B94EC vu0: cb->orgAttrBits picks whether a second UV set follows
    // @optout 0x2B94F8..0x2B9ABB vu0: the rest of it, past the second UV set we do read
    // @optout 0x2B9500 vu0: where they leave the transformed arrays
    // @optout 0x2B9510 gif: corePktOpenAndEmit opens the GIF packet
    // @optout 0x2B9608 vu0: corePktSkinPointsFirst
    // @optout 0x2B9620 vu0: corePktSkinPointsAccum
    // @optout 0x2B9654 vu0: corePktSkinVertAccum
    // @optout 0x2B967C vu0: corePktNormalizeNorms
    // @optout 0x2B9738 elsewhere(sdfModelLinkShapedDrawPacket): the morph list
    // @optout 0x2B976C elsewhere(sdfModelLinkShapedDrawPacket): its count
    // @optout 0x2B9780 elsewhere(sdfModelLinkShapedDrawPacket): its items
    // @optout 0x2B97BC vu0: corePktLoadVertsNorms
    // @optout 0x2B97D8 vu0: corePktLoadVerts
    // @optout 0x2B97EC vu0: the morph weight epsilon
    // @optout 0x2B9868 vu0: corePktMorphVertsNorms
    // @optout 0x2B987C vu0: corePktMorphVerts
    // @optout 0x2B992C vu0: corePktXfmLightClipUVPB
    // @optout 0x2B9944 vu0: corePktXfmLightClipPB
    // @optout 0x2B9A4C vu0: corePktXfmLightClipUV
    // @optout 0x2B9A6C vu0: corePktXfmLightClip
    // @optout 0x2B9A7C gif: corePktClosePacket closes it

    const sdfCorePacketType_t type = sdfCorePacketType_t(packet.header.packetType);
    gConvert->getTrace().countGeom(type, cb.faceCount);
    gConvert->getTrace().countPacketType(packet.header.packetType, packet.prim.attrBits, packet.prim.packetBits);
    gConvert->dumpPayload(aPacket);

    switch (type) {
        case sdfCorePacketTypeRigid:
        case sdfCorePacketTypeWeighted:
        case sdfCorePacketTypeBlendShape: break;
        default:
            reportUnsupported("sdfCorePacketGen body type", packet.header.packetType);
            return;
    }
    if (!packet.prim.faceCount || !packet.prim.pointCount) {
        gConvert->getTrace().countReject(type, ModelTrace::Reject::Empty);
        return;
    }

    const u32 nodesAddr = aPacket.addr + u32(sizeof(sdfCorePacket_t));
    u32 at = nodesAddr;
    if (type != sdfCorePacketTypeRigid) {
        const u16Ptr_t usedNodes{nodesAddr};
        if (!usedNodes) return;
        const u16 usedNodeCount = *usedNodes;
        at += 2 + 2u * usedNodeCount;
    }
    at = align16(at);

    DrawMesh mesh;
    mesh.sortZ = draw.sortZ;
    mesh.otLocal = u8(draw.otIndex & 3);
    mesh.otIndex = u8(gModel.otTable[mesh.otLocal]);
    mesh.color = Rgba::fromPacked(draw.color);
    mesh.attrBits = packet.prim.attrBits & draw.attrMask;
    mesh.reverseFacing = draw.reverse;

    if (!xcorePktBuildFaceList(at, cb, mesh)) {
        gConvert->getTrace().countReject(type, ModelTrace::Reject::BadElems);
        return;
    }
    at = align16(at + 6u * packet.prim.faceCount);

    bool ok = false;
    switch (type) {
        case sdfCorePacketTypeRigid:
            mesh.bones.push_back(draw.model);
            ok = gConvert->readRigidBatches(at, packet, mesh);
            break;
        case sdfCorePacketTypeWeighted:
            ok = xcorePktSkinVertFirst(at, packet, nodesAddr, mesh);
            break;
        case sdfCorePacketTypeBlendShape:
            mesh.bones.push_back(draw.model);
            ok = gConvert->readBlendShapes(at, packet, root, mesh);
            break;
    }
    if (!ok) {
        gConvert->getTrace().countReject(type, ModelTrace::Reject::BadBatch);
        return;
    }

    gConvert->getTrace().countSampler(mesh.tex0, mesh.tex1, mesh.clamp);
    gConvert->getTrace().countDeterminant(mesh.bones[0]);
    gConvert->getTrace().countAttr(mesh.attrBits);
    gConvert->getTrace().countGs(mesh.gs);
    gConvert->getTrace().countOt(mesh.otIndex);
    gConvert->getTrace().countBones(u32(mesh.bones.size()));
    gConvert->finishMesh(root, packet, type, mesh, frame);
}

/// The same node walk over the root's temporary packet list, run first under flags & 4.
/// @fn sdfModelTempPacketLinkOt
/// @note SLUS-20911 0x002B1950
void xsdfModelTempPacketLinkOt(guest_ref<sdfModelRootNode_t> root, Frame* frame) {
    // @optout 0x2B1954 packetbuf: sdfFrameId picks which of the two per-frame packets to build
    // @optout 0x2B19A4 packetbuf: sdfMatListUpdatePacket rebuilds each material's DMA packet

    // 0x2B19B0..0x2B19CC: the root's own light and fog, each falling back to the engine default.
    readLighting(root->light);
    readFog(root->fog);

    // 0x2B19FC..0x2B1A17: this walk takes faceDivSoften where sdfModelLinkOt takes faceDivBias,
    // and falls back to its own global.
    // @optout 0x2B1A00 vu0: corePktSoftenBias rides in the packet's attr word
    // @optout 0x2B1A14 elsewhere(sdfModelLinkOt): sdfFaceDivBias, which that walk takes
    gConvert->reportFaceDiv("sdfModelTempPacketLinkOt faceDivSoften",
                  (root->flags & sdfModelRootOwnFaceDivBias) ? root->faceDivSoften
                                                             : gModel.sdfFaceDivSoften.valueOr());
    gConvert->getTrace().countTempPacketWalk();

    const sdfModelNodeListPtr_t list = root->nodeList;
    if (!list) return;

    // 0x2B1A2C..0x2B1A7C, 0x2B1D60: this walk draws only nodes whose shape is a format 1
    // definition carrying a group array, and whose flags hide neither it nor its children.
    for (s16 i = 0; i < list->count; i++) {
        // @optout 0x2B1A40 elsewhere(readListItem): the list header's inline items
        // @optout 0x2B1A4C elsewhere(readListItem): and the link it walks
        const sdfModelNodePtr_t node{readListItem(root->nodeList.addr, u32(i))};
        if (!node || !node->shape || node->shape->format != 1) continue;
        const u32 groups = node->shape->shape[0].addr;
        if (!groups) continue;
        if (node->flags & sdfModelNodeHidden) continue;

        NodeDraw draw{};
        std::memcpy(&draw.model, &node->tm, sizeof(Matrix4));
        // @optout 0x2B1AA0 elsewhere(readScreenTm): sdf3DScreenTm is captured once a frame
        draw.clip = xMultMatrixRev(draw.model, gModel.screenTm);
        // 0x2B1AA8..0x2B1AB8: a node without bounds is never culled.
        if (node->bounds && xsdfModelCheckBounds(node->bounds, draw.clip)) continue;

        draw.sortZ = draw.clip.m[3][3];
        reportRootDepthBias(root->zBias);
        // 0x2B1ADC..0x2B1AEB: the node's own colour, or the root's.
        draw.color = (node->flags & sdfModelNodeOwnColor) ? node->color : root->color;
        // 0x2B1AEC..0x2B1B07: a negative colour, meaning an alpha over 0x80, sorts into table 0.
        draw.attrMask = u16(root->drawAttr) & node->drawAttr;
        draw.otIndex = s32(draw.color) < 0 ? 0 : 1;
        draw.reverse = (root->flags & sdfModelRootNoLinkedPackets) != 0;

        // @optout 0x2B1B08..0x2B1B23 packetbuf: clears the four per-ot draw tags
        // @optout 0x2B1B24 packetbuf: sdfCoreDrawSetZBias writes the bias into the packet

        // 0x2B1B30..0x2B1B5F, 0x2B1CF0: an otIndex word then a null-terminated array of temp
        // packet pointers. The guest folds the attr mask and colour sign into a qword; we carry
        // both on the draw.
        for (u32 g = 0; ; g++) {
            const u32 group = guest_ptr<u32>{groups + 4u * g}.valueOr();
            if (!group) break;

            NodeDraw packetDraw = draw;
            // 0x2B1B60: the group's own ot index, when it names one.
            if (const u16 otIndex = u16(guest_ptr<u32>{group}.valueOr() >> 16)) {
                packetDraw.otIndex = u8(otIndex);
            }

            // @optout 0x2B1B88..0x2B1C53 packetbuf: allocates this ot's draw tag, writes the
            // transform as a VIF packet and links it
            // @optout 0x2B1B88 packetbuf: sdfPrimAllocDrawTag carves that tag
            // @optout 0x2B1B98 packetbuf: its sort key, which our draw carries as sortZ
            // @optout 0x2B1BA0 packetbuf: its light packet
            // @optout 0x2B1BAC packetbuf: sdfGrAllocPacketBuff carves the transform packet
            // @optout 0x2B1BB0 packetbuf: its fog packet
            // @optout 0x2B1BE4 packetbuf: sdf3DPrimSetTmPacket writes the transform into it
            // @optout 0x2B1C4C packetbuf: sdfDrawTagLink2 links both

            for (u32 e = 0; ; e++) {
                const sdfModelTempPacketPtr_t packet{guest_ptr<u32>{group + 4u + 4u * e}.valueOr()};
                // 0x2B1C58..0x2B1C70, 0x2B1CE4: the list ends at a null, and only type 1 draws.
                if (!packet) break;
                if (packet->type != 1) {
                    reportUnsupported("sdfModelTempPacketLinkOt packet type", packet->type);
                    continue;
                }
                // @optout 0x2B1C78..0x2B1CE3 dma: builds the material REF packet and links it
                // @optout 0x2B1C98 dma: sdfModelSetMatRefPacket is what writes it
                const sdfMatPtr_t mat{readListItem(root->matList.addr, packet->matIndex)};
                if (!gConvert->readVifStreamMesh(packet->dataPtr.addr,
                                       packet->dataPtr.addr + 16u * packet->qwc, packetDraw, mat,
                                       frame)) {
                    gConvert->getTrace().countStrayRef();
                }
            }
        }
        // @optout 0x2B1CF8..0x2B1D3F packetbuf: files each allocated tag into its ordering table
        // @optout 0x2B1D14 elsewhere(sdfModelLinkOt): the ot group, resolved by that walk
        // @optout 0x2B1D30 elsewhere(sdfModelLinkOt): and the registrar it calls through
        // 0x2B1D40..0x2B1D6B: every skip above lands on this same node-loop tail.
    }
    // @optout 0x2B1D6C..0x2B1D7F enginestate: the root's "drew" bit, which only the engine's own
    // linker reads back
    // @optout 0x2B1D80..0x2B1DB7 abi: epilogue
}

/// @fn sdfModelLinkShapedDrawPacket
/// @note SLUS-20911 0x002B0CB0
void xsdfModelLinkShapedDrawPacket(guest_ref<sdfModelRootNode_t> root,
                                               const NodeDraw& draw, u32 dataAddr, Frame* frame) {
    gConvert->getTrace().countShapedWalk();
    // 0x2B0CB0..0x2B0D27
    // in as our first argument
    const f32 epsilon = gModel.sdfModelShapeWeightEpsilon  // 0x2B0CB4
                            ? *gModel.sdfModelShapeWeightEpsilon
                            : 0.000001f;

    // @optout 0x2B0CF4 elsewhere(sdfModelLinkOt): node->root, handed in as our first argument
    // The low half of word 1 indexes the morph list, the high half the material list.
    const u32 word1 = guest_ptr<u32>{dataAddr + 4}.valueOr();
    const u16 morphIndex = u16(word1 & 0xFFFF);
    const u16 matIndex = u16(word1 >> 16);
    const u16 shapeLists = u16(guest_ptr<u32>{dataAddr + 8}.valueOr() & 0xFFFF);
    // 0x2B0D08: the morph list's items follow its header.
    const u32 morphItems = root->morphList ? root->morphList->items.addr : 0;
    const sdfModelMorphPtr_t morph{morphItems + u32(sizeof(sdfModelMorph_t)) * morphIndex};
    if (!morph) {
        reportUnsupported("sdfModelLinkShapedDrawPacket morph index", morphIndex);
        return;
    }

    // Only the targets the controller is actually blending towards reach the chain.
    // 0x2B0D28..0x2B0D3F, 0x2B0D40..0x2B0D57, 0x2B0D58..0x2B0D5F
    // @optout 0x2B0D60..0x2B0DB3 packetbuf: the per-target packet is built here
    // @optout 0x2B0D6C packetbuf: sdfGrAllocPacketBuff carves it
    // @optout 0x2B0D84 packetbuf: sdfModelSetMatRefPacket links the material into it
    u16 targets[kMorphTargets] = {};
    u32 used = 0;
    for (const sdfModelMorphParm_t& target : morph->target) {
        if (target.blend > epsilon) targets[used++] = u16(target.targetId);
    }

    // 0x2B0DB4..0x2B0DBF, 0x2B0DC0..0x2B0DDB, 0x2B0DDC..0x2B0DDF, 0x2B0DE0..0x2B0DEB,
    // 0x2B0DEC..0x2B0DF7, 0x2B0DF8..0x2B0E13, 0x2B0E14..0x2B0E17, 0x2B0E18..0x2B0E3F,
    // 0x2B0E40..0x2B0E47, 0x2B0E48..0x2B0E5F, 0x2B0E60..0x2B0E9B
    const sdfMatPtr_t mat{readListItem(root->matList.addr, matIndex)};
    for (u16 l = 0; l < shapeLists; l++) {
        sdfModelShapeListPtr_t list{guest_ptr<u32>{dataAddr + 12u + 4u * l}.valueOr()};
        if (!list) {
            reportUnsupported("sdfModelLinkShapedDrawPacket shape list", l);
            continue;
        }
        for (u32 t = 0; t < used; t++) {
            // 0x2B0E78: the entries follow the list header.
            const sdfModelShapeEntry_t entry = list->entry[targets[t]];
            if (!entry.dataPtr) continue;
            if (!gConvert->readVifStreamMesh(entry.dataPtr.addr, entry.dataPtr.addr + 16u * entry.qwc, draw,
                                   mat, frame)) {
                gConvert->getTrace().countStrayRef();
            }
        }
        // 0x2B0E9C..0x2B0ECB: the list's count bounds the stream below.
        // @optout 0x2B0ECC..0x2B0F07 packetbuf: the finished chain is linked here
        // @optout 0x2B0ED8 packetbuf: sdfDrawTagLink2 links it
        // The list's own stream closes the chain, which is the mesh the targets deform.
        if (!gConvert->readVifStreamMesh(list->next.addr, list->next.addr + 16u * list->count, draw, mat,
                               frame)) {
            gConvert->getTrace().countStrayRef();
        }
    }
}

/// One camera-facing quad, rotated in its own plane by the angle sdfBillBoardMake1 never sees.
/// The quad's own spin, about z, before the camera axes are applied.
/// @fn SetRotateMatrixZ
/// @note SLUS-20911 0x002B6060
Matrix4 xSetRotateMatrixZ(f32 aAngle) {
    const f32 turn = std::cos(aAngle);
    const f32 lean = std::sin(aAngle);
    Matrix4 out{};
    out.m[0][0] = turn;
    out.m[0][1] = lean;
    out.m[1][0] = -lean;
    out.m[1][1] = turn;
    out.m[2][2] = 1.0f;
    out.m[3][3] = 1.0f;
    return out;
}

/// @fn drawSprite3D
/// @note SLUS-20911 0x0012A2D0
void xdrawSprite3D(TexHandle aTexHandle, sdfSprite3DPtr_t aSprite, Frame* frame) {
    if (!frame || !aSprite) return;
    const sdfSprite3D_t& sprite = *aSprite;
    // 0x12A2D0..0x12A3EF
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(1);  // 0x12A2F4
    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);                             // 0x12A300
    // @optout 0x12A314 elsewhere(sdfTexSetEx): sdfTexGetPacket is the upload we hold by handle
    // @optout 0x12A320 elsewhere(sdfTexSetEx): sdfTexGetPacketLength sizes that upload
    // @optout 0x12A330 packetbuf: sdf3DMakeDirectRefPacket wraps it as a REF
    // @optout 0x12A33C packetbuf: sdfDrawTagLinkRef links the texture packet

    // A billboard faces the camera by construction, so the screen matrix's columns are exact.
    const auto axis = [](f32 x, f32 y, f32 z, f32 out[3]) {
        const f32 length = std::sqrt(x * x + y * y + z * z);
        const f32 scale = length > 1e-6f ? 1.0f / length : 0.0f;
        out[0] = x * scale;
        out[1] = y * scale;
        out[2] = z * scale;
    };
    f32 right[3];
    f32 up[3];
    axis(gModel.screenTm.m[0][0], gModel.screenTm.m[1][0], gModel.screenTm.m[2][0], right);
    axis(gModel.screenTm.m[0][1], gModel.screenTm.m[1][1], gModel.screenTm.m[2][1], up);

    DrawMesh mesh;
    Matrix4 identity{};
    for (int i = 0; i < 4; i++) identity.m[i][i] = 1.0f;
    mesh.bones.push_back(identity);
    mesh.color = Rgba::fromPacked(sprite.color);
    mesh.lighting = gModel.lighting;
    mesh.fog = gModel.fog;
    // Stamped alongside the state, or the row reports table 0 beside table 2's registers.
    mesh.otLocal = 2;
    mesh.otIndex = u8(gModel.otTable[2]);
    mesh.gs = sdfGsStateForOt(mesh.otLocal);
    mesh.gs.frameMask = gState.frameMask;
    mesh.attrBits = sdfCoreAttrTextured | sdfCoreAttrBlend | sdfPrimTriangle;
    mesh.lit = false;
    mesh.packetType = sdfPacketGenerated;
    mesh.texHandle = gConvert->getTextures()->getHandle(aTexHandle, frame);

    // The rotation turns the offsets in the quad's own plane before the camera axes, which is why
    // this call is the hook rather than sdfBillBoardMake1.
    const Matrix4 spin = xSetRotateMatrixZ(sprite.rotation);  // 0x12A344
    const f32 turn = spin.m[0][0];
    const f32 lean = spin.m[0][1];
    // @optout 0x12A36C packetbuf: sdfBillBoardMakeTMPacket writes the transform packet
    xsdfDrawTagLink(tag, packet);  // 0x12A378
    const TexHandle bound{mesh.texHandle.addr};
    const f32 width = bound ? f32(bound->width) : 0.0f;
    const f32 height = bound ? f32(bound->height) : 0.0f;
    const f32 uScale = width > 0.0f ? 1.0f / (16.0f * width) : 0.0f;
    const f32 vScale = height > 0.0f ? 1.0f / (16.0f * height) : 0.0f;

    mesh.vertices.resize(4);
    for (u32 c = 0; c < 4; c++) {
        MeshVertex& out = mesh.vertices[c];
        const sdfVector2_t& at = sprite.offset.corner[c];
        const f32 x = at.x * turn - at.y * lean;
        const f32 y = at.x * lean + at.y * turn;
        // 0x12A390: the centre's z, with x and y beside it.
        setRigidInfluence(out,
                          sdfVector3_t{sprite.centre.x + right[0] * x + up[0] * y,
                                       sprite.centre.y + right[1] * x + up[1] * y,
                                       sprite.centre.z + right[2] * x + up[2] * y},
                          nullptr);
        out.u = f32(sprite.uv.corner[c].u) * uScale;
        out.v = f32(sprite.uv.corner[c].v) * vScale;
    }
    // @optout 0x12A39C packetbuf: sdfBillBoardMake1 writes the quad, whose corners we built here
    static constexpr u16 kQuad[6] = {0, 2, 1, 0, 3, 2};
    mesh.indices.assign(std::begin(kQuad), std::end(kQuad));
    gConvert->getTrace().countBones(1);
    DrawSink::addMeshTo(frame, std::move(mesh), tag);

    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];  // 0x12A3B0, 0x12A3B8
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        ot->regFunc(ot, tag);  // 0x12A3C4, 0x12A3C8
    }
}

/// An indexed triangle list in world space: the vertices are quadword-strided with only xyz used,
/// and +0x0C holds one byte index per lane, three to a word, with the word count at +0x00.
/// @fn sdf3DPrimMakePoly
/// @note SLUS-20911 0x002BAAB8
void xsdf3DPrimMakePoly(sdf3DPrimPolyParmPtr_t aPoly, Frame* frame) {
    if (!frame || !aPoly) return;
    const u32 vertexCount = u32(aPoly->pointCount);
    const u32 triangles = u32(aPoly->faceCount);
    if (!vertexCount || !triangles) return;

    const sdfVector4_t* point = sdfVector4Ptr_t{aPoly->vertex}.slice(vertexCount);
    // A null face list means the faces are implicit: face t is points 3t, 3t+1, 3t+2. Measured on
    // smt3_field_unknown_realm, where every call has faceCount 1, pointCount 3 and no list.
    const u32* index = aPoly->faceIndex ? u32Ptr_t{aPoly->faceIndex}.slice(triangles) : nullptr;
    if (!point || (aPoly->faceIndex && !index)) {
        reportUnsupported("sdf3DPrimMakePoly without geometry", aPoly.addr);
        return;
    }

    DrawMesh mesh;
    Matrix4 identity{};
    for (int axis = 0; axis < 4; axis++) identity.m[axis][axis] = 1.0f;
    mesh.bones.push_back(identity);
    mesh.lighting = gModel.lighting;
    mesh.fog = gModel.fog;
    mesh.gs = sdfGsStateForOt(0);
    // fldSdwDraw sets its own blend and test through rsetAlpha and rsetTest rather than taking an
    // ordering table's pair, so the state in force at the call is the one this draw runs under.
    {
        mesh.gs.frameMask = gState.frameMask;
        mesh.gs.test = gState.test;
    }
    // 0x2BAAF4: the prim word is the descriptor's own or'd with 0x68, which is IIP, FGE and ABE.
    mesh.attrBits = u16(aPoly->prim | 0x68);
    const GsAlpha alpha = gState.alpha;
    if (alpha == GsAlpha::Opaque) {
        mesh.attrBits = u16(mesh.attrBits & ~u16(sdfCoreAttrBlend));
    } else {
        mesh.gs.alpha = u64(alpha);
    }
    mesh.color = Rgba::fromPacked(aPoly->colorWord);
    mesh.lit = false;
    mesh.packetType = sdfPacketGenerated;

    mesh.vertices.resize(vertexCount);
    for (u32 v = 0; v < vertexCount; v++) {
        setRigidInfluence(mesh.vertices[v], sdfVector3_t{point[v].x, point[v].y, point[v].z},
                          nullptr);
        mesh.vertices[v].color = mesh.color;
    }
    for (u32 t = 0; t < triangles; t++) {
        for (int corner = 0; corner < 3; corner++) {
            const u32 at = index ? ((index[t] >> (8 * corner)) & 0xFF) : (t * 3 + corner);
            if (at >= vertexCount) return;
            mesh.indices.push_back(u16(at));
        }
    }
    gConvert->getTrace().countBones(1);
    gConvert->getTrace().countStreamGeom(triangles);
    gConvert->getTrace().countStreamMesh(triangles);
    DrawSink::addMeshTo(frame, std::move(mesh));
}

/// Three world positions and three colors, no texture and no UVs.
/// @fn fldDrawPoly3
/// @note SLUS-20911 0x0012A5E0
void xfldDrawPoly3(const sdfVector3_t point[3], const u32 color[3], Frame* frame) {
    if (!frame) return;
    // 0x12A5E0..0x12A75B: one flat triangle, built and filed in a single block.
    // @optout 0x12A64C packetbuf: sdfGrAllocPacketBuff carves the prim packet
    // @optout 0x12A658 packetbuf: sdfDrawTagInit on its tag
    // @optout 0x12A664 packetbuf: sdf3DPrimLinkResetTm links an identity transform, which the
    // mesh below carries as its one bone
    // @optout 0x12A670 enginestate: fldDrawEnv is the field scene's own material
    // @optout 0x12A674 packetbuf: sdf3DPrimLinkMat links it
    xsdfPacket_t* packet = xsdfGrAllocPacketBuff(1);
    xsdfDrawTag_t* tag = xsdfAllocDrawTag();
    xsdfDrawTagInit(tag);

    // memset clears the block, then only the fields a flat triangle needs are set.
    // @optout 0x12A684 alloc: value initialisation is our memset
    sdf3DPrimPolyParm_t parm{};
    parm.faceCount = 1;
    // 0x12A6A8: one triangle, three points.
    parm.pointCount = 3;
    // 0x12A6AC, 0x12A6B4: no texture, so neither uv array is set.
    parm.uv = 0;
    parm.multiUv = 0;
    // 0x12A6B8, 0x12A6BC, 0x12A6C0: no per-vertex colour array, the default allocator, no bias.
    parm.vertexColor = 0;
    parm.allocator = 0;
    parm.zBias = 0.0f;
    // 0x12A6DC: the flat colour every corner starts from.
    parm.colorWord = 0x80808080;
    // 0x12A6E4: the caller's own three points, which we already hold.
    parm.vertex = 0;
    // 0x12A6EC: a flat triangle carries no normals.
    parm.normal = 0;

    DrawMesh mesh;
    Matrix4 identity{};
    for (int axis = 0; axis < 4; axis++) identity.m[axis][axis] = 1.0f;
    mesh.bones.push_back(identity);
    mesh.lighting = gModel.lighting;
    mesh.fog = gModel.fog;
    mesh.gs = sdfGsStateForOt(0);
    // Gouraud and blended, with no texture: the call carries no UVs, so there is nothing to sample.
    mesh.attrBits = sdfCoreAttrGouraud | sdfCoreAttrBlend | sdfPrimTriangle;
    mesh.color = Rgba::fromPacked(parm.colorWord);
    mesh.lit = false;
    mesh.packetType = sdfPacketGenerated;
    mesh.vertices.resize(3);
    for (u32 i = 0; i < 3; i++) {
        setRigidInfluence(mesh.vertices[i], point[i], nullptr);
        mesh.vertices[i].color = Rgba::fromPacked(color[i]);
    }
    static constexpr u16 kTriangle[3] = {0, 1, 2};
    mesh.indices.assign(std::begin(kTriangle), std::end(kTriangle));
    gConvert->getTrace().countBones(1);
    DrawSink::addMeshTo(frame, std::move(mesh), tag);

    // 0x12A6F4: the prim packet is linked into the tag.
    xsdfDrawTagLink(tag, packet);
    // 0x12A6FC, 0x12A704: the ot offset rsetOtOfs last set names the table.
    xsdfGrOt_t* ot = &xsdfGrOtTable[xrsetFrameMaskOtOffset];
    if (ot->regFunc) {
        tag->sequence = gSequence++;
        // 0x12A710, 0x12A714: the table's registrar files the finished tag.
        ot->regFunc(ot, tag);
    }
}

/// Self-describing, so it is read rather than rebuilt.
/// @fn fldSdwDraw
/// @note SLUS-20911 0x001193E8
bool readShadowStream(u32 aPacket, u32 aEnd, const sdfGsState_t& aState,
                             Frame* frame) {
    if (!frame || aEnd <= aPacket) return false;

    // The stream opens with a V4_8 color unpack and its settings, so the batch header is not at the
    // target the way a model batch's is. Walk the codes to it rather than assuming an offset.
    u32 at = aPacket;
    VifCode header;
    bool found = false;
    while (at + 8 <= aEnd) {
        VifCode code;
        if (!readVifCode(at, code)) return false;
        if (isUnpack(code) && code.count == 1 && unpackWidth(code) == 8) {
            header = code;
            found = true;
            break;
        }
        // Everything else is a one-word setting or a kick.
        at = isUnpack(code) ? code.next + unpackWidth(code) * code.count : code.next;
    }
    if (!found) return false;

    const u16* fields = u16Ptr_t{header.next}.slice(4);
    if (!fields) return false;
    const u32 faceCount = fields[0];
    const u32 pointCount = fields[1];
    const u16 attrBits = fields[2];
    if (!faceCount || !pointCount) return false;

    // Triangle indices follow as one V4_8 element per face, three bytes and a pad.
    VifCode indexCode;
    if (!readVifCode(header.next + 8, indexCode) || !isUnpack(indexCode) ||
        unpackWidth(indexCode) != 4 || indexCode.count != faceCount) {
        return false;
    }
    const u8* faces = u8Ptr_t{indexCode.next}.slice(faceCount * 4);
    if (!faces) return false;

    // The positions get their own unpack, V3_32, rather than trailing the indices: reading them at
    // the end of the index array takes the code word itself as the first x.
    VifCode pointCode;
    if (!readVifCode(indexCode.next + faceCount * 4, pointCode) || !isUnpack(pointCode) ||
        unpackWidth(pointCode) != 12 || pointCode.count != pointCount) {
        return false;
    }
    const sdfVector3_t* points = sdfVector3Ptr_t{pointCode.next}.slice(pointCount);
    if (!points) return false;

    DrawMesh mesh;
    Matrix4 identity{};
    for (int axis = 0; axis < 4; axis++) identity.m[axis][axis] = 1.0f;
    mesh.bones.push_back(identity);
    mesh.lighting = gModel.lighting;
    mesh.fog = gModel.fog;
    // The state the chain left in force at this packet, FBMSK included: the engine masks RGB for
    // part of this pass, and drawing that as visible color is what costs 24 points.
    mesh.gs = aState;
    mesh.attrBits = attrBits;
    mesh.lit = false;
    mesh.packetType = sdfPacketGenerated;

    mesh.vertices.resize(pointCount);
    for (u32 i = 0; i < pointCount; i++) setRigidInfluence(mesh.vertices[i], points[i], nullptr);

    mesh.indices.reserve(faceCount * 3);
    for (u32 f = 0; f < faceCount; f++) {
        for (u32 i = 0; i < 3; i++) {
            const u8 index = faces[f * 4 + i];
            if (index >= pointCount) return false;
            mesh.indices.push_back(index);
        }
    }

    gConvert->getTrace().countBones(1);
    DrawSink::addMeshTo(frame, std::move(mesh));
    return true;
}

/// Builds one batch of camera-facing quads from the centres, offsets and UVs
/// the engine is handed.
/// @fn sdfBillBoardMakeN
/// @note SLUS-20911 0x002BB0F0
void xsdfBillBoardMakeN(sdfBillBoardBatchPtr_t aBatch, u32 aNum, Frame* frame) {
    if (!frame || !aBatch || !aNum || aNum > std::size(aBatch->centre)) return;
    const sdfBillBoardBatch_t& batch = *aBatch;

    // Offsets are view-space, so they need the camera's axes to land back in world.
    // A billboard faces the camera by construction, which is what makes taking them from the
    // matrix's columns exact rather than an approximation.
    const Matrix4& tm = gModel.screenTm;
    const auto axis = [](f32 x, f32 y, f32 z, f32 out[3]) {
        const f32 length = std::sqrt(x * x + y * y + z * z);
        const f32 scale = length > 1e-6f ? 1.0f / length : 0.0f;
        out[0] = x * scale;
        out[1] = y * scale;
        out[2] = z * scale;
    };
    f32 right[3];
    f32 up[3];
    axis(tm.m[0][0], tm.m[1][0], tm.m[2][0], right);
    axis(tm.m[0][1], tm.m[1][1], tm.m[2][1], up);

    // The node above the batch names its table; the GS trace puts the specters' additive batches
    // in table 2, by its ALPHA and TEST, so a miss says so rather than silently picking one.
    // 0x2BB0F0..0x2BB143
    // @optout 0x2BB134 packetbuf: sdfBillBoardGetPacketSize sizes the GIF packet
    // @optout 0x2BB144 packetbuf: sdfGrAllocPacketBuff carves it out of the frame buffer
    // 0x2BB144..0x2BB153
    // @optout 0x2BB158 packetbuf: the per-kind quad writer reached through the table
    // 0x2BB158..0x2BB163, 0x2BB164..0x2BB1D7, 0x2BB1D8..0x2BB1FF, 0x2BB200..0x2BB2C3
    // @optout 0x2BB234 packetbuf: memcpy stamps the corners into the packet
    // 0x2BB2C4..0x2BB2C7, 0x2BB2C8..0x2BB2E3, 0x2BB2E4..0x2BB2E7, 0x2BB2E8..0x2BB317
    const auto found = gModel.billboardOt.find(aBatch.addr);
    if (found == gModel.billboardOt.end()) reportUnsupported("billboard batch with no ordering table", 0);
    const u32 otIndex = found != gModel.billboardOt.end() ? found->second : 2;

    for (u32 i = 0; i < aNum; i++) {
        const sdfVector4_t& centre = batch.centre[i];
        const sdfBillBoardQuad_t& offset = batch.offset[i];
        const sdfBillBoardUv_t& uv = batch.uv[i];

        DrawMesh mesh;
        Matrix4 identity{};
        for (int axisIndex = 0; axisIndex < 4; axisIndex++) identity.m[axisIndex][axisIndex] = 1.0f;
        mesh.bones.push_back(identity);
        mesh.color = Rgba::fromPacked(batch.color[i]);
        mesh.lighting = gModel.lighting;
        mesh.fog = gModel.fog;
        // The node names one of the model group's four, the same as any other mesh.
        mesh.otLocal = u8(otIndex & 3);
        mesh.otIndex = u8(gModel.otTable[mesh.otLocal]);
        mesh.gs = sdfGsStateForOt(mesh.otLocal);
        mesh.gs.frameMask = gState.frameMask;
        mesh.attrBits = sdfCoreAttrTextured | sdfCoreAttrBlend | sdfPrimTriangle;
        mesh.lit = false;
        mesh.billboard = true;
        mesh.packetType = sdfPacketGenerated;
        mesh.texHandle = gConvert->getTextures()->getHandle(gModel.billboardTexture, frame);
        // The batch stores texel coordinates in 12.4; the sampler wants them over the texture.
        const f32 width = gModel.billboardTexture ? f32(gModel.billboardTexture->width) : 0.0f;
        const f32 height = gModel.billboardTexture ? f32(gModel.billboardTexture->height) : 0.0f;
        const f32 uScale = width > 0.0f ? 1.0f / (16.0f * width) : 0.0f;
        const f32 vScale = height > 0.0f ? 1.0f / (16.0f * height) : 0.0f;
        mesh.vertices.resize(4);
        for (u32 c = 0; c < 4; c++) {
            MeshVertex& out = mesh.vertices[c];
            const sdfVector2_t& at = offset.corner[c];
            const sdfVector3_t world{centre.x + right[0] * at.x + up[0] * at.y,
                                     centre.y + right[1] * at.x + up[1] * at.y,
                                     centre.z + right[2] * at.x + up[2] * at.y};
            setRigidInfluence(out, world, nullptr);
            out.u = f32(uv.corner[c].u) * uScale;
            out.v = f32(uv.corner[c].v) * vScale;
        }
        // Wound the way the engine lists the corners, which is the reverse of a GL front face.
        static constexpr u16 kQuad[6] = {0, 2, 1, 0, 3, 2};
        mesh.indices.assign(std::begin(kQuad), std::end(kQuad));
        gConvert->getTrace().countBones(1);
        DrawSink::addMeshTo(frame, std::move(mesh));
    }
}

/// Reads a type 1 DMA call chain, taking the material from the shape
/// record's matIndex and the stream from extData.
/// @fn sdfModelSetNormalDrawPacket
/// @note SLUS-20911 0x002B0AC0
void xsdfModelSetNormalDrawPacket(const NodeDraw& draw, u32 chainAddr, Frame* frame) {
    // 0x2B0AC0..0x2B0B2B
    // @optout 0x2B0AD8 elsewhere(sdfModelLinkOt): node->root, which the walk resolved
    // @optout 0x2B0AE0 packetbuf: sdfModelSetMatRefPacket links node->mat as a REF, and the chain
    // walk below reads that same material out of the root's packet map
    gModel.currentChain = chainAddr;
    sdfMatPtr_t mat;
    u32 walked = 0;
    for (u32 at = chainAddr; ; at += 16) {
        if (++walked > kMaxChainPackets) {
            reportUnsupported("sdfModelSetNormalDrawPacket chain longer than", kMaxChainPackets);
            return;
        }
        const u32* tag = u32Ptr_t{at}.slice(2);
        if (!tag) return;

        const u32 id = (tag[0] >> 28) & 7;
        if (id != sdfDmaTagRef) return;

        const u32 target = tag[1];
        const auto found = gModel.matByPacket.find(target);
        if (found != gModel.matByPacket.end()) {
            mat = {found->second};
            continue;
        }

        // A mesh stream opens with the batch header unpack. Anything else in the chain is setup
        // the hardware consumes and we already have by other means, so it is counted, not read.
        VifCode code;
        if (!readVifCode(target, code) || !isUnpack(code) || code.count != 1 ||
            unpackWidth(code) != 8) {
            gConvert->getTrace().countSetupRef();
            continue;
        }
        if (!gConvert->readVifStreamMesh(target, target + 16u * (tag[0] & 0xFFFF), draw, mat, frame)) {
            gConvert->getTrace().countStrayRef();
        }
    }
}

/// Keeps a triangle when its signed screen area times the packet's backface
/// sign is non-negative.
/// @fn corePktBuildFaceList
/// @note SLUS-20911 0x002B7A80
bool xcorePktBuildFaceList(u32 at, const corePktCb_t& cb, DrawMesh& mesh) {
    // The guest assembles triangles here and subdivides any that straddle the face-division
    // boundary or a clip plane, generating vertices as it goes. GL clips, and the shader takes
    // the ramp across the whole triangle, so all that survives is the index list.
    // @optout 0x2B7A84 vu0: corePktZBias is staged into vf2 for the test below
    // @optout 0x2B7AA4 vu0: sdf3DEyeOffset into vf21
    // @optout 0x2B7AAC vu0: sdf3DScreenParam, read for the pair that follows it
    // @optout 0x2B7B04 vu0: corePktSoftenBias forces every triangle through the split path
    // @optout 0x2B7AD8 vu0: sdfDrawFaceListParam into vf22
    // @optout 0x2B7ADC vu0: sdfDrawFaceListParam2 into vf23
    // @optout 0x2B7ACC vu0: cb->unk172 is the winding sign the area is tested against
    // @optout 0x2B7AE4 vu0: cb->attrBits picks whether corePktClipFace runs first
    // @optout 0x2B7AF4 vu0: corePktClipFace, which GL's own clipping replaces
    // @optout 0x2B7B08 vu0: corePktFaceList is the buffer it all lands in
    // @optout 0x2B7B20 vu0: cb->pointBuf0 holds the transformed vertices it indexes
    // @optout 0x2B7C58 vu0: corePktMakeEdgePoint interpolates a vertex at the boundary
    // @optout 0x2B7C84 vu0: cb->pointCount0 counts the room left for those
    // @optout 0x2B7C88 vu0: cb->pointBuf1 is where the next one goes
    // @optout 0x2B7C98 vu0: cb->pointBuf2 is the wrap
    // @optout 0x2B7C9C vu0: cb->pointCount1 its size
    // @optout 0x2B7CB8 vu0: cb->unk124 is the cursor over them
    // @optout 0x2B8040 vu0: cb->drawFaceCount is the count corePktFaceList ended up with
    // @optout 0x2B7AF4..0x2B7AFF vu0: the clip-face arm
    // @optout 0x2B7B2C..0x2B8067 vu0: the assembly and subdivision body
    const u32 bytes = 6u * u32(cb.faceCount);  // 0x2B7AF0
    const u8* source = u8Ptr_t{at + cb.unk148}.slice(bytes);  // 0x2B7B18
    if (!source) return false;

    // 0x2B7AF0, 0x2B7B18: the element list is three u16 indices a face, read from the cursor the
    // cb carries.
    mesh.indices.resize(3u * u32(cb.faceCount));
    std::memcpy(mesh.indices.data(), source, bytes);
    for (u16 index : mesh.indices) {
        if (index >= cb.pointCount) return false;
    }
    return true;
}

/// @fn corePktSkinVertFirst
/// @note SLUS-20911 0x002B6BC0
bool xcorePktSkinVertFirst(u32 at, const sdfCorePacket_t& packet, u32 nodesAddr, DrawMesh& mesh) {
    // 0x2B6BC0..0x2B6BCF, 0x2B6BD0..0x2B6C0F, 0x2B6C10..0x2B6C17
    const u16 usedNodeCount = u16Ptr_t{nodesAddr}.valueOr();
    if (!usedNodeCount || usedNodeCount > kMaxBones) return false;

    // Every batch blends the same node set, so the palette is built once from the mesh header.
    const u16* nodeIndices = u16Ptr_t{nodesAddr + 2}.slice(usedNodeCount);
    if (!nodeIndices) return false;
    mesh.bones.resize(usedNodeCount);
    for (u16 n = 0; n < usedNodeCount; n++) {
        const u32 nodeAddr = readListItem(gModel.nodeListAddr, nodeIndices[n]);
        const Matrix4Ptr_t tm{nodeAddr + u32(offsetof(sdfModelNode_t, tm))};
        if (!tm) return false;
        mesh.bones[n] = *tm;
    }

    while (mesh.vertices.size() < packet.prim.pointCount) {
        u16 extraNodes = 0;
        u16 count = 0;
        u32 next = 0;
        if (!readBatchHeader(at, extraNodes, count, next) || !count) return false;
        if (usize(extraNodes) + 1 != usedNodeCount) return false;

        const usize base = mesh.vertices.size();
        mesh.vertices.resize(base + count);
        // A node's whole vertex array arrives before the next node's, so every vertex collects one
        // influence per pass rather than all of its own at once.
        for (u16 n = 0; n < usedNodeCount; n++) {
            u32 positions = 0;
            u32 normals = 0;
            if (!readArray(next, 16, count, positions, next)) return false;
            if (!readArray(next, 12, count, normals, next)) return false;

            // w is the node's weight for that vertex, which is why a blended position is a vec4.
            const sdfVector4_t* position = sdfVector4Ptr_t{positions}.slice(count);
            const sdfVector3_t* normal = sdfVector3Ptr_t{normals}.slice(count);
            if (!position || !normal) return false;

            for (u16 v = 0; v < count; v++) {
                const f32 weight = position[v].w;
                if (weight == 0.0f) continue;

                MeshVertex& out = mesh.vertices[base + v];
                usize slot = 0;
                while (slot < kMaxBoneInfluences && out.influence[slot].weight != 0.0f) slot++;
                if (slot == kMaxBoneInfluences) {
                    gConvert->getTrace().countInfluences(kMaxBoneInfluences + 1);
                    continue;
                }
                MeshInfluence& influence = out.influence[slot];
                influence.x = position[v].x;
                influence.y = position[v].y;
                influence.z = position[v].z;
                influence.nx = normal[v].x;
                influence.ny = normal[v].y;
                influence.nz = normal[v].z;
                influence.weight = weight;
                influence.bone = f32(n);
            }
            VifCode kick;
            if (!readVifCode(next, kick)) return false;
            next = kick.next;
        }

        u32 texcoords = 0;
        if (!readArray(next, 8, count, texcoords, next)) return false;
        const sdfVector2_t* uv = sdfVector2Ptr_t{texcoords}.slice(count);
        if (!uv) return false;
        for (u16 v = 0; v < count; v++) {
            mesh.vertices[base + v].u = uv[v].x;
            mesh.vertices[base + v].v = uv[v].y;
        }

        at = skipBatchEnd(next);
        if (!at) return false;
    }
    if (mesh.vertices.size() != packet.prim.pointCount) return false;

    for (const MeshVertex& vertex : mesh.vertices) {
        u32 used = 0;
        for (const MeshInfluence& influence : vertex.influence) {
            used += influence.weight != 0.0f ? 1 : 0;
        }
        gConvert->getTrace().countInfluences(used);
    }
    gConvert->readSecondUv(at, packet, mesh);
    return true;
}

/// The one moment an asset's pixels are in main memory, so it is where the header is read.
/// @fn sdfTexSetEx
/// @note SLUS-20911 0x002ABC90
void xsdfTexSetEx(sdfTexFilePtr_t aTex, sdfTexHandlePtr_t aHandle, Frame* frame) {
    // 0x2ABC90..0x2ABD2B, 0x2ABD2C..0x2ABD63, 0x2ABD64..0x2ABD87, 0x2ABD88..0x2ABDCB,
    // 0x2ABDCC..0x2ABE07
    if (!aTex || !aHandle || !gSink.textures) return;
    const sdfTexFile_t& file = *aTex;
    sdfTexHandle_t& handle = *aHandle;

    // 0x2ABCC4..0x2ABD0C copies the header's geometry and sampler state field for field into the
    // handle, which is what makes the handle the key everything downstream is given. We read the
    // handle, so a disagreement would silently pick the wrong size or format.
    const bool matches = file.width == handle.width && file.height == handle.height
                         && file.pixelFormat == handle.pixelFormat
                         && file.numOfMipmap == handle.numOfMipmap
                         && file.mipmapParam == handle.mipmapParam && file.flags == handle.flags
                         && file.texWrapMode == handle.texWrapMode && file.texId == handle.texId
                         && file.clutId == handle.clutId;
    if (!matches) reportUnsupported("sdfTexSetEx handle disagrees with its file", file.pixelFormat);
    if (file.version != 2) reportUnsupported("sdfTexFile version", file.version);

    // @optout 0x2ABCD8 alloc: sdfTexAlloc places the texture in GS memory
    // @optout 0x2ABD1C gs: sdfTexGetCLUTCount sizes the palette upload
    // @optout 0x2ABD2C alloc: sdfTexAllocClutBuff carves its buffer
    // @optout 0x2ABD38 gs: sdfTexCopyClutData fills it
    // @optout 0x2ABD40 gs: sdfTexGetCLUTAddr is its GS address
    // @optout 0x2ABD54 gs: sdfTexTransCLUT uploads it
    // @optout 0x2ABD70 gs: sdfTexGetPixelAddr is where the pixels land
    // @optout 0x2ABD7C gs: sdfTexGetTexelBitCount sizes that transfer
    // @optout 0x2ABD9C gs: sdfTexTransData runs it, and we read the asset instead
    // @optout 0x2ABDCC gif: sdfTexMakeGIFPacket builds the upload packet
    TextureCache::Asset asset;
    asset.width = u32(file.width);
    asset.height = u32(file.height);
    asset.pixelFormat = file.pixelFormat;
    asset.clutFormat = file.CLUTFormat;
    asset.numOfCLUT = file.numOfCLUT;

    // The engine skips a fixed gap past the header's own data offset before the CLUT.
    asset.dataStart = aTex.addr + (file.dataOffset & 0xF0) + sdfTmxDataGap;
    gSink.textures->add(asset, TexHandle{aHandle.addr}, frame);
}

/// The guest is done with the handle, which does not mean nothing is drawing it: the GL object
/// goes at its next upload, never here.
/// @fn sdfTexFree
/// @note SLUS-20911 0x002AB860
void xsdfTexFree(sdfTexHandlePtr_t aHandle) {
    // 0x2AB860..0x2AB873, 0x2AB874..0x2AB87F, 0x2AB880..0x2AB887, 0x2AB888..0x2AB8AB,
    // 0x2AB8AC..0x2AB8AF, 0x2AB8B0..0x2AB8B7, 0x2AB8B8..0x2AB8BB, 0x2AB8BC..0x2AB8DF,
    // 0x2AB8E0..0x2AB8EF
    // @optout 0x2AB874 alloc: every read here names a block to release, and our allocation model
    // is not the guest's
    // @optout 0x2AB880 gs: sdfGsMemFree returns the texture's GS memory
    // @optout 0x2AB884 alloc: the pixel block it frees
    // @optout 0x2AB88C alloc: the CLUT block
    // @optout 0x2AB890 alloc: sdfChipFreeDirect
    // @optout 0x2AB894 alloc: its argument
    // @optout 0x2AB89C alloc: and its size
    // @optout 0x2AB8A0 alloc: the handle's own block
    // @optout 0x2AB8A8 alloc: its link field, unlinked below
    // @optout 0x2AB8B4 enginestate: sdfTexHandleLink is the engine's handle list
    // @optout 0x2AB8BC alloc: sdfFreeAuto
    // @optout 0x2AB8C0 alloc: its argument
    // @optout 0x2AB8C8 alloc: and its size
    if (!gSink.textures) return;
    gSink.textures->onFree(TexHandle{aHandle.addr});
}

/// A second handle over one asset's pixels, so the copy resolves to what the source decoded.
/// @fn sdfTexDup
/// @note SLUS-20911 0x002AB448
void xsdfTexDup(sdfTexHandlePtr_t aSource, sdfTexHandlePtr_t aCopy) {
    // 0x2AB448..0x2AB4F7, 0x2AB4F8..0x2AB4FF, 0x2AB500..0x2AB51F, 0x2AB520..0x2AB553,
    // 0x2AB554..0x2AB57F
    // @optout 0x2AB460 alloc: sdfChipAllocDirect carves the new handle
    // @optout 0x2AB474 alloc: the fields copied into it are the source's own allocation
    // @optout 0x2AB47C alloc: its pixel block
    // @optout 0x2AB484 alloc: its CLUT block
    // @optout 0x2AB4AC alloc: its size
    // @optout 0x2AB4B4 alloc: its mode
    // @optout 0x2AB4BC alloc: its GS address
    // @optout 0x2AB4C4 alloc: and its packet
    // @optout 0x2AB50C enginestate: sdfTexLink puts the copy on the handle list
    // @optout 0x2AB528 alloc: sdfTexAllocClut
    // @optout 0x2AB534 alloc: sdfTexAllocClutBuff
    // @optout 0x2AB540 gs: sdfTexCopyClutData
    // @optout 0x2AB554 gif: sdfTexMakeGIFPacket
    // @optout 0x2AB55C gs: sdfTexClutToGs uploads the copy's palette
    if (!gSink.textures) return;
    gSink.textures->onDuplicate(TexHandle{aSource.addr}, TexHandle{aCopy.addr});
}

/// A render target: a frame buffer wrapped as a texture, with no main-memory pixels by
/// construction.
/// @fn sdfGsAllocFrameBuff
/// @note SLUS-20911 0x002AA8D0
void xsdfGsAllocFrameBuff(sdfGsMemSlicePtr_t aSlice) {
    // 0x2AA8D0..0x2AA90F, 0x2AA910..0x2AA927, 0x2AA928..0x2AA92B, 0x2AA92C..0x2AA96B
    // @optout 0x2AA93C alloc: sdfGsMemAllocFromHead places the buffer
    if (!gSink.textures || !aSlice) return;
    const sdfGsMemSlice_t& slice = *aSlice;
    // 0x2AA94C, 0x2AA950, 0x2AA954: the buffer's format and size are what make it a texture.
    if (slice.pixelFormat != sdfPsmct32 && slice.pixelFormat != sdfPsmct24) {
        reportUnsupported("frame buffer pixelFormat", slice.pixelFormat);
    }
    if (!slice.w || !slice.h) reportUnsupported("frame buffer with no size", slice.w);
    gSink.textures->onFrameBuffer(aSlice);
}

/// The glyph sheet's sixteen palette entries, uploaded straight to GS memory.
/// @fn frCopyFontClutToVram
/// @note SLUS-20911 0x001C6700
void xfrCopyFontClutToVram(u32 aData) {
    // 0x1C6700..0x1C677F
    // @optout 0x1C6730 gs: sceGsSetDefLoadImage builds the transfer
    // @optout 0x1C6738 abi: dds3GsSema guards the GS path against the other thread
    // @optout 0x1C673C abi: WaitSema takes it
    // @optout 0x1C6744 alloc: FlushCache writes the data back for the DMA
    // @optout 0x1C6750 gs: sceGsExecLoadImage runs the transfer
    // @optout 0x1C675C gs: sceGsSyncPath waits for it
    // @optout 0x1C6768 abi: SignalSema releases the semaphore
    if (!gSink.textures) return;
    gSink.textures->onFontClut(aData);
}

}  // namespace sdfhook
