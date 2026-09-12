#pragma once

#include "guest_ptr.h"
#include "types.h"

// Names and offsets are the game's own, from the SLUS_209.11 IDB, so a field compares against the
// database directly.

using sdfhook::guest_ptr;

using sdfhook::f32;
using sdfhook::s16;
using sdfhook::s32;
using sdfhook::u16;
using sdfhook::u32;
using sdfhook::u64;
using sdfhook::u8;
using sdfhook::usize;

// Declared ahead so the pointer aliases below can name them.
struct sdfModelNode_t;
struct sdfModelNodeDef_t;
struct sdfModelPacketHeader_t;
struct sdfModelRootNode_t;
struct sdfDrawTag_t;
struct sdfModelNodeList_t;
struct sdfShape_t;
struct sdfModelTexList_t;
struct sdfMatList_t;
struct sdfModelMorphList_t;
struct sdfMotionCtrlr_t;
struct sdfMemHandle_t;
struct sdfGrOt_t;
struct sdfGrPacketHeader_t;
struct sdf3DLightParallelPacket_t;
struct sdf3DFogPacket_t;
struct sdfMat_t;
struct sdfModelMorph_t;

// A size on its own passes even when a field has moved, because padding absorbs the difference.
#define SDF_STATIC_ASSERT_SIZE(type, bytes) \
    static_assert(sizeof(type) == (bytes), #type " is not the size the database reports")

#define SDF_STATIC_ASSERT_OFFSETOF(type, field, offset) \
    static_assert(offsetof(type, field) == (offset), #type "::" #field " is not where the database puts it")

using sdfModelNodePtr_t = guest_ptr<sdfModelNode_t>;
using sdfModelNodeDefPtr_t = guest_ptr<sdfModelNodeDef_t>;
using sdfModelNodeListPtr_t = guest_ptr<sdfModelNodeList_t>;
using sdfModelRootNodePtr_t = guest_ptr<sdfModelRootNode_t>;
using sdfModelPacketHeaderPtr_t = guest_ptr<sdfModelPacketHeader_t>;
using sdfModelTexListPtr_t = guest_ptr<sdfModelTexList_t>;
using sdfModelMorphListPtr_t = guest_ptr<sdfModelMorphList_t>;
using sdfMatListPtr_t = guest_ptr<sdfMatList_t>;
using sdfMotionCtrlrPtr_t = guest_ptr<sdfMotionCtrlr_t>;
using sdfMemHandlePtr_t = guest_ptr<sdfMemHandle_t>;
using sdfGrOtPtr_t = guest_ptr<sdfGrOt_t>;
using sdfGrPacketHeaderPtr_t = guest_ptr<sdfGrPacketHeader_t>;
using sdf3DLightParallelPacketPtr_t = guest_ptr<sdf3DLightParallelPacket_t>;
using sdf3DLightDefaultPtr_t = guest_ptr<sdf3DLightParallelPacketPtr_t>;
using sdf3DFogPacketPtr_t = guest_ptr<sdf3DFogPacket_t>;
using sdf3DFogDefaultPtr_t = guest_ptr<sdf3DFogPacketPtr_t>;
using sdfShapePtr_t = guest_ptr<sdfShape_t>;
using sdfDrawTagPtr_t = guest_ptr<sdfDrawTag_t>;
using sdfMatPtr_t = guest_ptr<sdfMat_t>;
using sdfMatPacketPtr_t = guest_ptr<struct sdfMatPacket_t>;
using sdfModelMorphPtr_t = guest_ptr<sdfModelMorph_t>;
using sdfVoidPtr_t = guest_ptr<void>;

struct sdfVector2_t {
    f32 x, y;

    f32 operator[](usize axis) const { return (&x)[axis]; }
};

struct sdfVector3_t {
    f32 x, y, z;

    f32 operator[](usize axis) const { return (&x)[axis]; }
};

struct sdfVector4_t {
    f32 x, y, z, w;

    f32 operator[](usize axis) const { return (&x)[axis]; }
};

struct sdfMatrix4_t {
    sdfVector4_t row[4];
};

using sdfVector2Ptr_t = guest_ptr<sdfVector2_t>;
using sdfVector3Ptr_t = guest_ptr<sdfVector3_t>;
using sdfVector4Ptr_t = guest_ptr<sdfVector4_t>;
using sdfMatrix4Ptr_t = guest_ptr<sdfMatrix4_t>;

/// The axis-aligned box sdfModelCheckBounds tests, min then max.
/// @fn sdfModelCheckBounds
/// @note SLUS-20911 0x002B1838
struct sdfModelBounds_t {
    sdfVector3_t min;
    sdfVector3_t max;
};
using sdfModelBoundsPtr_t = guest_ptr<sdfModelBounds_t>;

/// What sdfModelLinkOt reads out of a root's flags, at 0x002B1DB8.
enum sdfModelRootFlag_t : u8 {
    sdfModelRootTempPackets = 0x04,      // link the temp packets first
    sdfModelRootNoLinkedPackets = 0x08,  // suppress the DMA call and shaped paths
    sdfModelRootOwnFaceDivBias = 0x02,
    sdfModelRootDrew = 0x10,             // cleared on entry, set if anything reached an OT
};

struct sdfModelRootNode_t {
    sdfModelNodeListPtr_t nodeList;
    sdfModelNodePtr_t headNode;
    sdfModelTexListPtr_t texList;
    sdfMatListPtr_t matList;
    sdfModelMorphListPtr_t morphList;
    sdfMotionCtrlrPtr_t motion;
    u8 complexNodeId;
    u8 flags;
    u16 drawAttr;
    u32 color;
    sdfMatrix4_t tm;
    sdfVector4_t rotate;
    sdfVector4_t scale;
    sdf3DLightParallelPacketPtr_t light;
    sdf3DFogPacketPtr_t fog;
    f32 faceDivSoften;
    f32 faceDivBias;
    sdfVoidPtr_t extraData;
    f32 zBias;
    sdfVoidPtr_t modelobj;
    sdfVoidPtr_t oData;
    s32 effpos_type;
    s32 reversePB;
};
SDF_STATIC_ASSERT_SIZE(sdfModelRootNode_t, 168);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, nodeList, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, headNode, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, texList, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, matList, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, morphList, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, motion, 0x14);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, complexNodeId, 0x18);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, flags, 0x19);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, drawAttr, 0x1A);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, color, 0x1C);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, tm, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, rotate, 0x60);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, scale, 0x70);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, light, 0x80);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, fog, 0x84);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, faceDivSoften, 0x88);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, faceDivBias, 0x8C);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, extraData, 0x90);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, zBias, 0x94);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, modelobj, 0x98);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, oData, 0x9C);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, effpos_type, 0xA0);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelRootNode_t, reversePB, 0xA4);

using sdfModelOtPtr_t = guest_ptr<struct sdfModelOt_t>;

struct sdfModelOt_t {
    sdfGrOtPtr_t ot[4];
};
SDF_STATIC_ASSERT_SIZE(sdfModelOt_t, 16);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelOt_t, ot, 0x00);

// Every sdf growable list is this header over an item array allocated on its own.
struct sdfModelNodeList_t {
    sdfMemHandlePtr_t memHandle;
    s16 count;
    s16 capacity;
    s16 elemSize;
    s16 growStep;
    guest_ptr<sdfModelNodePtr_t> items;
};
SDF_STATIC_ASSERT_SIZE(sdfModelNodeList_t, 16);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, memHandle, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, count, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, capacity, 0x06);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, elemSize, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, growStep, 0x0A);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, items, 0x0C);

struct sdfMatList_t {
    sdfMemHandlePtr_t memHandle;
    s16 count;
    s16 capacity;
    s16 elemSize;
    s16 growStep;
    guest_ptr<sdfMatPtr_t> items;
};
SDF_STATIC_ASSERT_SIZE(sdfMatList_t, 16);
SDF_STATIC_ASSERT_OFFSETOF(sdfMatList_t, count, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfMatList_t, items, 0x0C);

struct sdfModelMorphList_t {
    sdfMemHandlePtr_t memHandle;
    s16 count;
    s16 capacity;
    s16 elemSize;
    s16 growStep;
    sdfModelMorphPtr_t items;
};
SDF_STATIC_ASSERT_SIZE(sdfModelMorphList_t, 16);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelMorphList_t, count, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelMorphList_t, items, 0x0C);

/// The transfer unit everything on this hardware is counted in.
constexpr u32 sdfQwordBytes = 16;

/// The GS modulates and tests alpha with 128 as one, not 255.
constexpr f32 sdfGsUnit = 128.0f;

/// DMA source-chain tag ids. REFE, REF and REFS name their data elsewhere; the rest carry it.
enum sdfDmaTagId_t : u32 {
    sdfDmaTagRefe = 0,
    sdfDmaTagCnt = 1,
    sdfDmaTagNext = 2,
    sdfDmaTagRef = 3,
    sdfDmaTagRefs = 4,
    sdfDmaTagCall = 5,
    sdfDmaTagRet = 6,
    sdfDmaTagEnd = 7,
};

// The packets the engine links, as its builders lay them out.

/// A DMA source-chain tag. `id` holds PCE, ID and IRQ; channel 1 hands dat0 and dat1 to VIF as
/// codes.
struct sdfDmaTag_t {
    u16 qwc;
    u8 pad;
    u8 id;
    u32 addr;
    u32 dat0;
    u32 dat1;
};
SDF_STATIC_ASSERT_SIZE(sdfDmaTag_t, 16);
SDF_STATIC_ASSERT_OFFSETOF(sdfDmaTag_t, qwc, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfDmaTag_t, pad, 0x02);
SDF_STATIC_ASSERT_OFFSETOF(sdfDmaTag_t, id, 0x03);
SDF_STATIC_ASSERT_OFFSETOF(sdfDmaTag_t, addr, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfDmaTag_t, dat0, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfDmaTag_t, dat1, 0x0C);

using sdfDmaTagPtr_t = guest_ptr<sdfDmaTag_t>;

constexpr u32 sdfGetDmaId(const sdfDmaTag_t& t) { return (t.id >> 4) & 7; }

/// REFE, REF and REFS name their data at `addr`; the rest carry it after the tag.
constexpr bool sdfDmaIsRef(const sdfDmaTag_t& t) {
    const u32 id = sdfGetDmaId(t);
    return id == sdfDmaTagRef || id == sdfDmaTagRefs || id == sdfDmaTagRefe;
}

struct sdfGifTag_t {
    u64 parm;
    u64 regs;
};
SDF_STATIC_ASSERT_SIZE(sdfGifTag_t, 16);
SDF_STATIC_ASSERT_OFFSETOF(sdfGifTag_t, parm, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGifTag_t, regs, 0x08);

using sdfGifTagPtr_t = guest_ptr<sdfGifTag_t>;

constexpr u32 sdfGetGifNloop(const sdfGifTag_t& t) { return u32(t.parm & 0x7FFF); }
constexpr u32 sdfGetGifPre(const sdfGifTag_t& t) { return u32((t.parm >> 46) & 1); }
constexpr u32 sdfGetGifPrim(const sdfGifTag_t& t) { return u32((t.parm >> 47) & 0x7FF); }
constexpr u32 sdfGetGifFlg(const sdfGifTag_t& t) { return u32((t.parm >> 58) & 3); }
constexpr u32 sdfGetGifNreg(const sdfGifTag_t& t) { return u32((t.parm >> 60) & 0xF); }

/// One A+D write: the value, then the GS register it names in the low byte.
struct sdfGrTagStdReg_t {
    u64 data;
    u64 reg;
};
SDF_STATIC_ASSERT_SIZE(sdfGrTagStdReg_t, 16);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrTagStdReg_t, data, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrTagStdReg_t, reg, 0x08);

using sdfGrTagStdRegPtr_t = guest_ptr<sdfGrTagStdReg_t>;

/// A GIF packet's opening, as sdf2DMakePrimPacketHeader writes it: the DMA tag, then the GIF tag.
struct sdfGrPacketHeader_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    // u64 data[] follows
};
SDF_STATIC_ASSERT_SIZE(sdfGrPacketHeader_t, 32);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrPacketHeader_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrPacketHeader_t, giftag, 0x10);

/// sdfGrSetRegsPacketHeader: NLOOP A+D writes. The sized variants are what the callers allocate.
struct sdfGrRegPacket_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    // sdfGrTagStdReg_t reg[] follows
};
SDF_STATIC_ASSERT_SIZE(sdfGrRegPacket_t, 32);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrRegPacket_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrRegPacket_t, giftag, 0x10);

struct sdfGrReg1Packet_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    sdfGrTagStdReg_t reg[1];
};
SDF_STATIC_ASSERT_SIZE(sdfGrReg1Packet_t, 48);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrReg1Packet_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrReg1Packet_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrReg1Packet_t, reg, 0x20);

struct sdfGrReg2Packet_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    sdfGrTagStdReg_t reg[2];
};
SDF_STATIC_ASSERT_SIZE(sdfGrReg2Packet_t, 64);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrReg2Packet_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrReg2Packet_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrReg2Packet_t, reg, 0x20);

struct sdfGrReg9Packet_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    sdfGrTagStdReg_t reg[9];
};
SDF_STATIC_ASSERT_SIZE(sdfGrReg9Packet_t, 176);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrReg9Packet_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrReg9Packet_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrReg9Packet_t, reg, 0x20);

struct sdfGrReg15Packet_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    sdfGrTagStdReg_t reg[15];
};
SDF_STATIC_ASSERT_SIZE(sdfGrReg15Packet_t, 272);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrReg15Packet_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrReg15Packet_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrReg15Packet_t, reg, 0x20);

/// A REF to data held elsewhere, then the tag that continues the chain.
struct sdfGrRefPacketHeader_t {
    sdfDmaTag_t reftag;
    sdfDmaTag_t linktag;
};
SDF_STATIC_ASSERT_SIZE(sdfGrRefPacketHeader_t, 32);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrRefPacketHeader_t, reftag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrRefPacketHeader_t, linktag, 0x10);

struct sdfGrRefGifPacketHeader_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    sdfDmaTag_t reftag;
    sdfDmaTag_t linktag;
};
SDF_STATIC_ASSERT_SIZE(sdfGrRefGifPacketHeader_t, 64);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrRefGifPacketHeader_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrRefGifPacketHeader_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrRefGifPacketHeader_t, reftag, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrRefGifPacketHeader_t, linktag, 0x30);

/// sdfDrawTagLinkCall: a DMA call into a chain the caller built, and the tag after it.
struct sdfGrCallPacket_t {
    sdfDmaTag_t calltag;
    sdfDmaTag_t linktag;
};
SDF_STATIC_ASSERT_SIZE(sdfGrCallPacket_t, 32);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrCallPacket_t, calltag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrCallPacket_t, linktag, 0x10);

/// A VIF stream straight after its tag, what the VU1 routines' builders return.
struct sdfGrVifPacket_t {
    sdfDmaTag_t tag;
    // u32 vif[] follows
};
SDF_STATIC_ASSERT_SIZE(sdfGrVifPacket_t, 16);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrVifPacket_t, tag, 0x00);

using sdfPreDrawHookPtr_t = guest_ptr<struct sdfPreDrawHook_t>;
using sdfPreDrawTagPtr_t = guest_ptr<struct sdfPreDrawTag_t>;

/// A callback sdfGrDrawFrame runs before it sends the chain, with two words of its own.
struct sdfPreDrawHook_t {
    sdfPreDrawHookPtr_t next;
    u32 predrawFunc;
    u32 option0;
    u32 option1;
};
SDF_STATIC_ASSERT_SIZE(sdfPreDrawHook_t, 16);
SDF_STATIC_ASSERT_OFFSETOF(sdfPreDrawHook_t, next, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfPreDrawHook_t, predrawFunc, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfPreDrawHook_t, option0, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfPreDrawHook_t, option1, 0x0C);

struct sdfPreDrawTag_t {
    sdfPreDrawTagPtr_t next;
    sdfPreDrawHookPtr_t head;
    sdfPreDrawHookPtr_t tail;
    u32 pad;
};
SDF_STATIC_ASSERT_SIZE(sdfPreDrawTag_t, 16);
SDF_STATIC_ASSERT_OFFSETOF(sdfPreDrawTag_t, next, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfPreDrawTag_t, head, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfPreDrawTag_t, tail, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfPreDrawTag_t, pad, 0x0C);

using sdfGsMemSlicePtr_t = guest_ptr<struct sdfGsMemSlice_t>;

struct sdfGrFrameSet_t {
    s16 w;
    s16 h;
    s16 buffH;
    u8 pixelFormat;
    u8 zbuffFormat;
    sdfGsMemSlicePtr_t frameHandle[2];
    sdfGsMemSlicePtr_t zbuffHandle;
};
SDF_STATIC_ASSERT_SIZE(sdfGrFrameSet_t, 20);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFrameSet_t, w, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFrameSet_t, h, 0x02);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFrameSet_t, buffH, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFrameSet_t, pixelFormat, 0x06);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFrameSet_t, zbuffFormat, 0x07);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFrameSet_t, frameHandle, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFrameSet_t, zbuffHandle, 0x10);

using sdfGrFrameSetPtr_t = guest_ptr<sdfGrFrameSet_t>;

struct sdfGrOt_t {
    sdfGrOtPtr_t next;
    sdfDrawTagPtr_t tagHead;
    sdfDrawTagPtr_t tagTail;
    sdfDrawTagPtr_t noSortTag;
    u32 regFunc;
    u32 callbackFunc;
    sdfVoidPtr_t data;
    u32 pad;
};
SDF_STATIC_ASSERT_SIZE(sdfGrOt_t, 32);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrOt_t, next, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrOt_t, tagHead, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrOt_t, tagTail, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrOt_t, noSortTag, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrOt_t, regFunc, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrOt_t, callbackFunc, 0x14);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrOt_t, data, 0x18);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrOt_t, pad, 0x1C);

/// One queued frame, sdfGrDrawState[sdfDrawReqQueueIndex]: sdfGrDrawFrame sends `tag->head`.
struct sdfGrDrawState_t {
    sdfDrawTagPtr_t tag;
    sdfPreDrawHookPtr_t preDrawHead;
    u8 mode;
    u8 buffId;
    u8 frameId;
    u8 halfOffset;
    s32 field_C;
};
SDF_STATIC_ASSERT_SIZE(sdfGrDrawState_t, 16);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawState_t, tag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawState_t, preDrawHead, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawState_t, mode, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawState_t, buffId, 0x09);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawState_t, frameId, 0x0A);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawState_t, halfOffset, 0x0B);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawState_t, field_C, 0x0C);

/// sdfGrSetClearPacket: a sprite over the whole buffer, under a TEST that passes everything.
struct sdfGrClearPacket_t {
    sdfGrTagStdReg_t test;
    sdfGrTagStdReg_t prim;
    sdfGrTagStdReg_t rgbaq;
    sdfGrTagStdReg_t xyzA;
    sdfGrTagStdReg_t xyzB;
};
SDF_STATIC_ASSERT_SIZE(sdfGrClearPacket_t, 80);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrClearPacket_t, test, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrClearPacket_t, prim, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrClearPacket_t, rgbaq, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrClearPacket_t, xyzA, 0x30);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrClearPacket_t, xyzB, 0x40);

/// sdfGrSetDrawFramePacket
struct sdfGrDrawFramePacket_t {
    sdfGrTagStdReg_t frame;
    sdfGrTagStdReg_t zbuf;
    sdfGrTagStdReg_t xyoffset;
    sdfGrTagStdReg_t scissor;
};
SDF_STATIC_ASSERT_SIZE(sdfGrDrawFramePacket_t, 64);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawFramePacket_t, frame, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawFramePacket_t, zbuf, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawFramePacket_t, xyoffset, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawFramePacket_t, scissor, 0x30);

struct sdfGrDrawDefParmPacket_t {
    sdfGrTagStdReg_t prmodecont;
    sdfGrTagStdReg_t colclamp;
    sdfGrTagStdReg_t dthe;
    sdfGrTagStdReg_t texa;
};
SDF_STATIC_ASSERT_SIZE(sdfGrDrawDefParmPacket_t, 64);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawDefParmPacket_t, prmodecont, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawDefParmPacket_t, colclamp, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawDefParmPacket_t, dthe, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawDefParmPacket_t, texa, 0x30);

/// sdfTexSetPacket: a texture handle's registers, which sdfTexGetTex0Reg reads TEX0 back out of.
struct sdfTexPacket_t {
    sdfGifTag_t giftag;
    sdfGrTagStdReg_t tex1;
    sdfGrTagStdReg_t tex0;
    sdfGrTagStdReg_t clamp;
};
SDF_STATIC_ASSERT_SIZE(sdfTexPacket_t, 64);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexPacket_t, giftag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexPacket_t, tex1, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexPacket_t, tex0, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexPacket_t, clamp, 0x30);

using sdfTexPacketPtr_t = guest_ptr<sdfTexPacket_t>;

/// The masked FRAME writes sdfGrStdDrawEnvPredraw builds, which sdfGrSetFrameMaskRGB REFs.
struct sdfGrAlphaMaskPacket_t {
    sdfGrTagStdReg_t frame1;
    sdfGrTagStdReg_t frame2;
};
SDF_STATIC_ASSERT_SIZE(sdfGrAlphaMaskPacket_t, 32);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrAlphaMaskPacket_t, frame1, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrAlphaMaskPacket_t, frame2, 0x10);

/// sdfGrLinkDispFrameTex REFs dispTex and sdfGrLinkDrawFrameTex drawTex: the two frame buffers as
/// textures.
struct sdfGrStdDrawEnvExt_t {
    sdfGrAlphaMaskPacket_t alphaMask;
    sdfTexPacket_t dispTex;
    sdfTexPacket_t drawTex;
};
SDF_STATIC_ASSERT_SIZE(sdfGrStdDrawEnvExt_t, 160);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrStdDrawEnvExt_t, alphaMask, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrStdDrawEnvExt_t, dispTex, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrStdDrawEnvExt_t, drawTex, 0x60);

/// sdfGrSetStdDrawEnv. `worker` is its predraw hook, sdfGrStdDrawEnvPredraw, which fills `ext`.
struct sdfGrStdDrawEnv_t {
    sdfPreDrawHook_t worker;
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    sdfGrDrawDefParmPacket_t defParm;
    sdfGrDrawFramePacket_t frame1;
    sdfGrDrawFramePacket_t frame2;
    sdfGrClearPacket_t clear;
    sdfGrTagStdReg_t test1;
    sdfGrTagStdReg_t alpha1;
    sdfGrTagStdReg_t test2;
    sdfGrTagStdReg_t alpha2;
    sdfGrStdDrawEnvExt_t ext;
};
SDF_STATIC_ASSERT_SIZE(sdfGrStdDrawEnv_t, 544);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrStdDrawEnv_t, worker, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrStdDrawEnv_t, dmatag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrStdDrawEnv_t, giftag, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrStdDrawEnv_t, defParm, 0x30);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrStdDrawEnv_t, frame1, 0x70);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrStdDrawEnv_t, frame2, 0xB0);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrStdDrawEnv_t, clear, 0xF0);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrStdDrawEnv_t, test1, 0x140);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrStdDrawEnv_t, alpha1, 0x150);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrStdDrawEnv_t, test2, 0x160);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrStdDrawEnv_t, alpha2, 0x170);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrStdDrawEnv_t, ext, 0x180);

/// sdfGrSetDrawEnv, for dds3DrawEnv_t's RenderDrawEnv.
struct sdfGrDrawEnv_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    sdfGrDrawDefParmPacket_t defParm;
    sdfGrDrawFramePacket_t frame1;
    sdfGrDrawFramePacket_t frame2;
    sdfGrClearPacket_t clear;
    sdfGrTagStdReg_t test1;
    sdfGrTagStdReg_t alpha1;
    sdfGrTagStdReg_t test2;
    sdfGrTagStdReg_t alpha2;
};
SDF_STATIC_ASSERT_SIZE(sdfGrDrawEnv_t, 368);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawEnv_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawEnv_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawEnv_t, defParm, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawEnv_t, frame1, 0x60);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawEnv_t, frame2, 0xA0);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawEnv_t, clear, 0xE0);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawEnv_t, test1, 0x130);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawEnv_t, alpha1, 0x140);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawEnv_t, test2, 0x150);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawEnv_t, alpha2, 0x160);

/// sdfGrSetDrawFrame
struct sdfGrDrawFrame_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    sdfGrDrawFramePacket_t packet;
};
SDF_STATIC_ASSERT_SIZE(sdfGrDrawFrame_t, 96);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawFrame_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawFrame_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrDrawFrame_t, packet, 0x20);

/// sdfGrSetFramePacket. `worker` is its predraw hook, sdfGrFramePacketPredraw.
struct sdfGrFramePacket_t {
    sdfPreDrawHook_t worker;
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    sdfGrTagStdReg_t frame;
    sdfGrTagStdReg_t zbuf;
    sdfGrFrameSetPtr_t frameSet;
    u32 frameMask;
    u32 zbuffMask;
    s32 context;
};
SDF_STATIC_ASSERT_SIZE(sdfGrFramePacket_t, 96);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFramePacket_t, worker, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFramePacket_t, dmatag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFramePacket_t, giftag, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFramePacket_t, frame, 0x30);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFramePacket_t, zbuf, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFramePacket_t, frameSet, 0x50);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFramePacket_t, frameMask, 0x54);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFramePacket_t, zbuffMask, 0x58);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrFramePacket_t, context, 0x5C);

struct frDrawEnvPkt_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    sdfGrTagStdReg_t texflush;
    sdfGrTagStdReg_t alpha1;
    sdfGrTagStdReg_t pabe;
    sdfGrTagStdReg_t texa;
    sdfGrTagStdReg_t fba1;
    sdfGrTagStdReg_t test1;
};
SDF_STATIC_ASSERT_SIZE(frDrawEnvPkt_t, 128);
SDF_STATIC_ASSERT_OFFSETOF(frDrawEnvPkt_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(frDrawEnvPkt_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(frDrawEnvPkt_t, texflush, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(frDrawEnvPkt_t, alpha1, 0x30);
SDF_STATIC_ASSERT_OFFSETOF(frDrawEnvPkt_t, pabe, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(frDrawEnvPkt_t, texa, 0x50);
SDF_STATIC_ASSERT_OFFSETOF(frDrawEnvPkt_t, fba1, 0x60);
SDF_STATIC_ASSERT_OFFSETOF(frDrawEnvPkt_t, test1, 0x70);

struct sdfPrimBitBltElem_t {
    sdfGrTagStdReg_t bitbltbuf;
    sdfGrTagStdReg_t trxpos;
    sdfGrTagStdReg_t trxreg;
    sdfGrTagStdReg_t trxdir;
};
SDF_STATIC_ASSERT_SIZE(sdfPrimBitBltElem_t, 64);
SDF_STATIC_ASSERT_OFFSETOF(sdfPrimBitBltElem_t, bitbltbuf, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfPrimBitBltElem_t, trxpos, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfPrimBitBltElem_t, trxreg, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfPrimBitBltElem_t, trxdir, 0x30);

/// sdfPrimSetBitBltPacket
struct sdfPrimBitBltPacket_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    sdfPrimBitBltElem_t bitblt;
};
SDF_STATIC_ASSERT_SIZE(sdfPrimBitBltPacket_t, 96);
SDF_STATIC_ASSERT_OFFSETOF(sdfPrimBitBltPacket_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfPrimBitBltPacket_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfPrimBitBltPacket_t, bitblt, 0x20);

/// sdfSetLoadImage: a host-to-GS transfer.
struct sdfLoadImagePacket_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    sdfPrimBitBltElem_t bitblt;
    sdfGifTag_t giftagData;
    sdfDmaTag_t dmatagData;
    sdfDmaTag_t dmatagFlush;
    sdfGifTag_t giftagFlush;
    sdfGrTagStdReg_t texflush;
};
SDF_STATIC_ASSERT_SIZE(sdfLoadImagePacket_t, 176);
SDF_STATIC_ASSERT_OFFSETOF(sdfLoadImagePacket_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfLoadImagePacket_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfLoadImagePacket_t, bitblt, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfLoadImagePacket_t, giftagData, 0x60);
SDF_STATIC_ASSERT_OFFSETOF(sdfLoadImagePacket_t, dmatagData, 0x70);
SDF_STATIC_ASSERT_OFFSETOF(sdfLoadImagePacket_t, dmatagFlush, 0x80);
SDF_STATIC_ASSERT_OFFSETOF(sdfLoadImagePacket_t, giftagFlush, 0x90);
SDF_STATIC_ASSERT_OFFSETOF(sdfLoadImagePacket_t, texflush, 0xA0);

/// sdfSetStoreImage: a GS-to-host readback, with where it lands and the callback to run.
struct sdfStoreImagePacket_t {
    sdfDmaTag_t dmaAnchor;
    u64 anchor[2];
    sdfDmaTag_t dmaFinish;
    sdfGifTag_t gifFinish;
    sdfGrTagStdReg_t finish;
    sdfDmaTag_t dmaBitblt;
    sdfGifTag_t gifBitblt;
    sdfPrimBitBltElem_t bitblt;
    u32 qwc;
    sdfVoidPtr_t addr;
    u32 callback;
    sdfVoidPtr_t callbackParam;
    sdfDmaTag_t dmaFooter;
    sdfGifTag_t gifFooter;
    sdfGrTagStdReg_t footer;
};
SDF_STATIC_ASSERT_SIZE(sdfStoreImagePacket_t, 240);
SDF_STATIC_ASSERT_OFFSETOF(sdfStoreImagePacket_t, dmaAnchor, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfStoreImagePacket_t, anchor, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfStoreImagePacket_t, dmaFinish, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfStoreImagePacket_t, gifFinish, 0x30);
SDF_STATIC_ASSERT_OFFSETOF(sdfStoreImagePacket_t, finish, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(sdfStoreImagePacket_t, dmaBitblt, 0x50);
SDF_STATIC_ASSERT_OFFSETOF(sdfStoreImagePacket_t, gifBitblt, 0x60);
SDF_STATIC_ASSERT_OFFSETOF(sdfStoreImagePacket_t, bitblt, 0x70);
SDF_STATIC_ASSERT_OFFSETOF(sdfStoreImagePacket_t, qwc, 0xB0);
SDF_STATIC_ASSERT_OFFSETOF(sdfStoreImagePacket_t, addr, 0xB4);
SDF_STATIC_ASSERT_OFFSETOF(sdfStoreImagePacket_t, callback, 0xB8);
SDF_STATIC_ASSERT_OFFSETOF(sdfStoreImagePacket_t, callbackParam, 0xBC);
SDF_STATIC_ASSERT_OFFSETOF(sdfStoreImagePacket_t, dmaFooter, 0xC0);
SDF_STATIC_ASSERT_OFFSETOF(sdfStoreImagePacket_t, gifFooter, 0xD0);
SDF_STATIC_ASSERT_OFFSETOF(sdfStoreImagePacket_t, footer, 0xE0);

/// sdf2DMakeTexPacket
struct sdf2DTexPacket_t {
    sdfDmaTag_t tag;
    sdfGifTag_t giftag;
    sdfGrTagStdReg_t tex1;
    sdfGrTagStdReg_t tex0;
    sdfGrTagStdReg_t clamp;
};
SDF_STATIC_ASSERT_SIZE(sdf2DTexPacket_t, 80);
SDF_STATIC_ASSERT_OFFSETOF(sdf2DTexPacket_t, tag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdf2DTexPacket_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdf2DTexPacket_t, tex1, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdf2DTexPacket_t, tex0, 0x30);
SDF_STATIC_ASSERT_OFFSETOF(sdf2DTexPacket_t, clamp, 0x40);

struct sdfPrimTransElem_t {
    sdfGrTagStdReg_t test;
    sdfGrTagStdReg_t alpha;
};
SDF_STATIC_ASSERT_SIZE(sdfPrimTransElem_t, 32);
SDF_STATIC_ASSERT_OFFSETOF(sdfPrimTransElem_t, test, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfPrimTransElem_t, alpha, 0x10);

/// sdfPrimSetOpaquePacket
struct sdfPrimTransPacket_t {
    sdfDmaTag_t tag;
    sdfGifTag_t giftag;
    sdfPrimTransElem_t elem;
};
SDF_STATIC_ASSERT_SIZE(sdfPrimTransPacket_t, 64);
SDF_STATIC_ASSERT_OFFSETOF(sdfPrimTransPacket_t, tag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfPrimTransPacket_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfPrimTransPacket_t, elem, 0x20);

/// frCreateDrawFontPacket_20550848
struct frGlyphPacket_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    sdfGrTagStdReg_t reg[3];
    sdfGifTag_t primtag;
    u64 prim[6];
};
SDF_STATIC_ASSERT_SIZE(frGlyphPacket_t, 144);
SDF_STATIC_ASSERT_OFFSETOF(frGlyphPacket_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(frGlyphPacket_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(frGlyphPacket_t, reg, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(frGlyphPacket_t, primtag, 0x50);
SDF_STATIC_ASSERT_OFFSETOF(frGlyphPacket_t, prim, 0x60);

/// dds3LinkZClearDrawTag
struct dds3ZClearPacket_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t testTag;
    sdfGrTagStdReg_t test;
    sdfGifTag_t spriteTag;
    sdfGrTagStdReg_t vertex[4];
    sdfGifTag_t restoreTag;
    sdfGrTagStdReg_t restore[2];
};
SDF_STATIC_ASSERT_SIZE(dds3ZClearPacket_t, 176);
SDF_STATIC_ASSERT_OFFSETOF(dds3ZClearPacket_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(dds3ZClearPacket_t, testTag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(dds3ZClearPacket_t, test, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(dds3ZClearPacket_t, spriteTag, 0x30);
SDF_STATIC_ASSERT_OFFSETOF(dds3ZClearPacket_t, vertex, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(dds3ZClearPacket_t, restoreTag, 0x80);
SDF_STATIC_ASSERT_OFFSETOF(dds3ZClearPacket_t, restore, 0x90);

/// REGLIST primitives, typed by the function that allocates them. `data` is the register list.
struct dds3PrimFrameRectPacket_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    u64 data[12];
};
SDF_STATIC_ASSERT_SIZE(dds3PrimFrameRectPacket_t, 128);
SDF_STATIC_ASSERT_OFFSETOF(dds3PrimFrameRectPacket_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(dds3PrimFrameRectPacket_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(dds3PrimFrameRectPacket_t, data, 0x20);

struct dds3PrimLinePacket_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    u64 data[6];
};
SDF_STATIC_ASSERT_SIZE(dds3PrimLinePacket_t, 80);
SDF_STATIC_ASSERT_OFFSETOF(dds3PrimLinePacket_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(dds3PrimLinePacket_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(dds3PrimLinePacket_t, data, 0x20);

struct dds3PrimLine3DPacket_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    u64 data[84];
};
SDF_STATIC_ASSERT_SIZE(dds3PrimLine3DPacket_t, 704);
SDF_STATIC_ASSERT_OFFSETOF(dds3PrimLine3DPacket_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(dds3PrimLine3DPacket_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(dds3PrimLine3DPacket_t, data, 0x20);

struct effPkt2DAllocF4Packet_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    u64 data[12];
};
SDF_STATIC_ASSERT_SIZE(effPkt2DAllocF4Packet_t, 128);
SDF_STATIC_ASSERT_OFFSETOF(effPkt2DAllocF4Packet_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(effPkt2DAllocF4Packet_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(effPkt2DAllocF4Packet_t, data, 0x20);

struct pbAllocPrimPacketPacket_t {
    sdfDmaTag_t dmatag;
    sdfGifTag_t giftag;
    u64 data[12];
};
SDF_STATIC_ASSERT_SIZE(pbAllocPrimPacketPacket_t, 128);
SDF_STATIC_ASSERT_OFFSETOF(pbAllocPrimPacketPacket_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(pbAllocPrimPacketPacket_t, giftag, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(pbAllocPrimPacketPacket_t, data, 0x20);

/// sdf3DMakeViewportPacket: VU1's camera, uploaded by the UNPACK in the tag's upper words.
struct sdf3DViewportPacket_t {
    sdfDmaTag_t dmatag;
    sdfMatrix4_t cameraTm;
    sdfMatrix4_t TM;
    sdfVector4_t scale;
    sdfVector4_t trans;
    sdfVector4_t cameraPos;
    u32 itop;
    u32 mscal;
    u32 pad[2];
};
SDF_STATIC_ASSERT_SIZE(sdf3DViewportPacket_t, 208);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DViewportPacket_t, dmatag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DViewportPacket_t, cameraTm, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DViewportPacket_t, TM, 0x50);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DViewportPacket_t, scale, 0x90);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DViewportPacket_t, trans, 0xA0);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DViewportPacket_t, cameraPos, 0xB0);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DViewportPacket_t, itop, 0xC0);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DViewportPacket_t, mscal, 0xC4);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DViewportPacket_t, pad, 0xC8);

/// sdf3DPrimSetTmPacket and sdfModelAllocTmPacket
struct sdf3DTmPacketData_t {
    sdfVector4_t tm[7];
    u32 cmd[4];
};
SDF_STATIC_ASSERT_SIZE(sdf3DTmPacketData_t, 128);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DTmPacketData_t, tm, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DTmPacketData_t, cmd, 0x70);

/// The microcode upload. Untyped in the database too, since a type would swallow the 23
/// vu1_entry* names inside it.
struct sdf3DVu1MicroPacket_t {
    u64 data[1956];
};
SDF_STATIC_ASSERT_SIZE(sdf3DVu1MicroPacket_t, 15648);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DVu1MicroPacket_t, data, 0x00);

/// Model packets that are one REF tag: a material, a transform, the chain's end.
using sdfMatRefPacket_t = sdfDmaTag_t;

using sdfModelTmRefPacket_t = sdfDmaTag_t;

using sdfModelTermPacket_t = sdfDmaTag_t;

/// The per-draw colour and face-division terms VU1 reads, closing a node's or a temp packet's draw.
struct sdfModelDrawParmPacket_t {
    sdfDmaTag_t next;
    u32 unpackColor;
    u32 color;
    u32 unpackParm;
    f32 zBias;
    f32 faceDivSoften;
    f32 faceDivBias;
    u32 itop;
    u32 mscal;
};
SDF_STATIC_ASSERT_SIZE(sdfModelDrawParmPacket_t, 48);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelDrawParmPacket_t, next, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelDrawParmPacket_t, unpackColor, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelDrawParmPacket_t, color, 0x14);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelDrawParmPacket_t, unpackParm, 0x18);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelDrawParmPacket_t, zBias, 0x1C);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelDrawParmPacket_t, faceDivSoften, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelDrawParmPacket_t, faceDivBias, 0x24);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelDrawParmPacket_t, itop, 0x28);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelDrawParmPacket_t, mscal, 0x2C);

/// sdfModelLinkOt, per node
struct sdfModelNodeDrawPacket_t {
    sdfModelTmRefPacket_t tmRef;
    sdfModelDrawParmPacket_t parm;
};
SDF_STATIC_ASSERT_SIZE(sdfModelNodeDrawPacket_t, 64);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeDrawPacket_t, tmRef, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeDrawPacket_t, parm, 0x10);

/// sdfModelSetNormalDrawPacket: one material REF and one mesh body REF.
struct sdfModelNormalDrawPacket_t {
    sdfDmaTag_t matRef;
    sdfDmaTag_t dataRef;
};
SDF_STATIC_ASSERT_SIZE(sdfModelNormalDrawPacket_t, 32);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNormalDrawPacket_t, matRef, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNormalDrawPacket_t, dataRef, 0x10);

/// sdfModelSetOneSkinDrawPacket
struct sdfModelOneSkinDrawPacket_t {
    sdfDmaTag_t matRef;
    // sdfDmaTag_t ref[] follows
};
SDF_STATIC_ASSERT_SIZE(sdfModelOneSkinDrawPacket_t, 16);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelOneSkinDrawPacket_t, matRef, 0x00);

/// sdfModelTempPacketLinkOt, per entry
struct sdfModelTempDrawPacket_t {
    sdfDmaTag_t matRef;
    sdfDmaTag_t dataRef;
    sdfDmaTag_t next;
};
SDF_STATIC_ASSERT_SIZE(sdfModelTempDrawPacket_t, 48);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelTempDrawPacket_t, matRef, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelTempDrawPacket_t, dataRef, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelTempDrawPacket_t, next, 0x20);

/// sdfModelTempPacketLinkOt, once per group
struct sdfModelTempTmPacket_t {
    sdfDmaTag_t tag;
    sdf3DTmPacketData_t tm;
    sdfModelDrawParmPacket_t parm;
};
SDF_STATIC_ASSERT_SIZE(sdfModelTempTmPacket_t, 192);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelTempTmPacket_t, tag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelTempTmPacket_t, tm, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelTempTmPacket_t, parm, 0x90);

struct sdfDrawTag_t {
    sdfDrawTagPtr_t next;
    sdfGrPacketHeaderPtr_t head;
    sdfGrPacketHeaderPtr_t tail;
    f32 z;
    sdf3DLightParallelPacketPtr_t lightPacket;
    sdf3DFogPacketPtr_t fogPacket;
    sdfVoidPtr_t texPacket[2];
};
SDF_STATIC_ASSERT_SIZE(sdfDrawTag_t, 32);
SDF_STATIC_ASSERT_OFFSETOF(sdfDrawTag_t, next, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfDrawTag_t, head, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfDrawTag_t, tail, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfDrawTag_t, z, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdfDrawTag_t, lightPacket, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfDrawTag_t, fogPacket, 0x14);
SDF_STATIC_ASSERT_OFFSETOF(sdfDrawTag_t, texPacket, 0x18);

struct sdfModelPacketHeader_t {
    sdfModelPacketHeaderPtr_t next;
    u8 type;
    u8 otIndex;
    u16 size;
    sdfVoidPtr_t dataPtr;
    sdfMemHandlePtr_t memHandle;
};
SDF_STATIC_ASSERT_SIZE(sdfModelPacketHeader_t, 16);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelPacketHeader_t, next, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelPacketHeader_t, type, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelPacketHeader_t, otIndex, 0x05);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelPacketHeader_t, size, 0x06);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelPacketHeader_t, dataPtr, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelPacketHeader_t, memHandle, 0x0C);

/// What sdfModelLinkOt reads out of a node's flags. Either hidden bit suppresses the node.
enum sdfModelNodeFlag_t : u16 {
    sdfModelNodeHidden = 0x11,
    sdfModelNodeOwnColor = 0x02,
};

struct sdfModelNode_t {
    sdfModelNodePtr_t prev;
    sdfModelNodePtr_t next;
    sdfModelNodePtr_t parent;
    sdfModelNodePtr_t children;
    sdfModelRootNodePtr_t root;
    u16 flags;
    u16 drawAttr;
    u32 id;
    u32 color;
    sdfMemHandlePtr_t drawPacketMemHandle[2];
    // One chain per frame buffer, indexed by sdfFrameId.
    sdfModelPacketHeaderPtr_t drawPacket[2];
    sdfVoidPtr_t tmPacket;
    sdfModelBoundsPtr_t bounds;
    sdfModelNodeDefPtr_t shape;
    u32 pad0;
    sdfVoidPtr_t userData;
    u32 pad1[3];
    sdfVector4_t rotate;
    sdfVector4_t pos;
    sdfVector4_t scale;
    sdfMatrix4_t localNoScaledTm;
    sdfMatrix4_t tm;
};
SDF_STATIC_ASSERT_SIZE(sdfModelNode_t, 256);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, prev, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, next, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, parent, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, children, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, root, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, flags, 0x14);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, drawAttr, 0x16);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, id, 0x18);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, color, 0x1C);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, drawPacketMemHandle, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, drawPacket, 0x28);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, tmPacket, 0x30);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, bounds, 0x34);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, shape, 0x38);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, userData, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, rotate, 0x50);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, pos, 0x60);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, scale, 0x70);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, localNoScaledTm, 0x80);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, tm, 0xC0);

/// One entry of a temporary packet group: a DMA REF's worth of VIF stream, and the material it
/// draws under. sdfModelTempPacketLinkOt links these instead of the node's own draw packet chain.
struct sdfModelTempPacket_t {
    u8 type;
    u8 pad[3];
    u16 qwc;
    u16 matIndex;
    sdfVoidPtr_t dataPtr;
};
SDF_STATIC_ASSERT_SIZE(sdfModelTempPacket_t, 12);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelTempPacket_t, type, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelTempPacket_t, qwc, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelTempPacket_t, matIndex, 0x06);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelTempPacket_t, dataPtr, 0x08);
using sdfModelTempPacketPtr_t = guest_ptr<sdfModelTempPacket_t>;

/// One blend target's stream, as sdfModelLinkShapedDrawPacket chains them.
struct sdfModelShapeEntry_t {
    sdfVoidPtr_t dataPtr;
    u16 qwc;
    u16 pad;
};
SDF_STATIC_ASSERT_SIZE(sdfModelShapeEntry_t, 8);

using sdfModelShapeListPtr_t = guest_ptr<struct sdfModelShapeList_t>;

struct sdfModelShapeList_t {
    sdfModelShapeListPtr_t next;
    u16 count;
    u16 pad;
    sdfModelShapeEntry_t entry[1];
};
SDF_STATIC_ASSERT_SIZE(sdfModelShapeList_t, 16);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelShapeList_t, count, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelShapeList_t, entry, 0x08);

struct sdfModelNodeDef_t {
    u16 format;
    u16 pad0;
    u32 pad1;
    u32 id;
    s32 parentIndex;
    sdfVector4_t rotate;
    sdfVector4_t pos;
    sdfVector4_t scale;
    f32 bounds;
    sdfShapePtr_t shape[3];
};
SDF_STATIC_ASSERT_SIZE(sdfModelNodeDef_t, 80);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeDef_t, format, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeDef_t, id, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeDef_t, parentIndex, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeDef_t, rotate, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeDef_t, pos, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeDef_t, scale, 0x30);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeDef_t, bounds, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeDef_t, shape, 0x44);

struct sdfShape_t {
    u32 flags;
    s32 vertexCount;
    s32 faceCount;
    s16 pointElemCount;
    s16 packetPointMax;
    sdfMemHandlePtr_t vertexListHandle;
    sdfVector4_t vertexList;
    sdfMemHandlePtr_t normalListHandle;
    sdfVector4_t normalList;
    sdfMemHandlePtr_t uvListHandles[2];
    sdfVector2_t uvList[2];
    sdfMemHandlePtr_t vertColorListHandle;
    u32 vertColorList;
    sdfMemHandlePtr_t faceListHandle;
    s32 faceList[4];
    sdfMemHandlePtr_t faceNormalListHandle;
    sdfVector4_t faceNormalList;
    sdfMemHandlePtr_t faceColorListHandle;
    s32 faceColorList[4];
    sdfVoidPtr_t packetPtr;
};
SDF_STATIC_ASSERT_SIZE(sdfShape_t, 152);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, flags, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, vertexCount, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, faceCount, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, pointElemCount, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, packetPointMax, 0x0E);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, vertexListHandle, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, vertexList, 0x14);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, normalListHandle, 0x24);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, normalList, 0x28);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, uvListHandles, 0x38);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, uvList, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, vertColorListHandle, 0x50);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, vertColorList, 0x54);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, faceListHandle, 0x58);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, faceList, 0x5C);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, faceNormalListHandle, 0x6C);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, faceNormalList, 0x70);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, faceColorListHandle, 0x80);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, faceColorList, 0x84);
SDF_STATIC_ASSERT_OFFSETOF(sdfShape_t, packetPtr, 0x94);

using sdfTexHandlePtr_t = guest_ptr<struct sdfTexHandle_t>;

/// One allocation in GS memory, in the linked list sdfGsMemAllocFromHead keeps. A frame buffer and
/// a texture are the same kind of slice, which is why a texture's region can end up under one.
struct sdfGsMemSlice_t {
    sdfGsMemSlicePtr_t prev;
    sdfGsMemSlicePtr_t next;
    s32 type;
    u32 addr;
    s32 size;
    s16 w;
    s16 h;
    u32 pixelFormat;
};

static_assert(sizeof(sdfGsMemSlice_t) == 28);

struct sdfMat_t {
    u32 id;
    u16 flags;
    u8 duty;
    u8 pad;
    sdfMatPacketPtr_t packet[2];
    u32 ambient;
    u32 diffuse;
    u32 multiTexDiffuse;
    f32 constant;
    u32 specular;
    u32 specularParm;
    u32 refmap;
    sdfTexHandlePtr_t texHandle;
    sdfTexHandlePtr_t multiTexHandle;
    u32 multiTexMethod;
    sdfVoidPtr_t uvwParm;
    sdfVoidPtr_t multiTexUvwParm;
    f32 fdBias;
    f32 fdSoften;
    u8 reserved[88];
};
SDF_STATIC_ASSERT_SIZE(sdfMat_t, 160);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, id, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, flags, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, duty, 0x06);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, packet, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, ambient, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, diffuse, 0x14);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, multiTexDiffuse, 0x18);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, constant, 0x1C);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, specular, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, specularParm, 0x24);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, refmap, 0x28);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, texHandle, 0x2C);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, multiTexHandle, 0x30);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, multiTexMethod, 0x34);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, uvwParm, 0x38);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, multiTexUvwParm, 0x3C);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, fdBias, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, fdSoften, 0x44);
SDF_STATIC_ASSERT_OFFSETOF(sdfMat_t, reserved, 0x48);

// The data a generated draw packet points at, named as the engine's own headers do. The prim
// header follows inline here, where the HD build keeps a pointer to it.
struct sdfCorePacketHeader_t {
    u8 packetType;
    u8 mpgType;
    s16 priority;
    u16 size;
    u16 matIndex;
    u32 extData[2];
};
SDF_STATIC_ASSERT_SIZE(sdfCorePacketHeader_t, 16);

struct sdfCorePrimHeader_t {
    u16 faceCount;
    u16 pointCount;
    u16 attrBits;
    u16 packetBits;
};
SDF_STATIC_ASSERT_SIZE(sdfCorePrimHeader_t, 8);

using sdfCorePacketPtr_t = guest_ptr<struct sdfCorePacket_t>;

struct sdfCorePacket_t {
    sdfCorePacketHeader_t header;
    sdfCorePrimHeader_t prim;
};
SDF_STATIC_ASSERT_SIZE(sdfCorePacket_t, 24);
SDF_STATIC_ASSERT_OFFSETOF(sdfCorePacket_t, prim, 0x10);

// A blend-shape packet keeps its shape count and its morph controller in the first extra word.
constexpr u16 sdfGetCoreShapeCount(const sdfCorePacket_t& p) { return u16(p.header.extData[0]); }
constexpr u16 sdfGetCoreMorphIndex(const sdfCorePacket_t& p) { return u16(p.header.extData[0] >> 16); }

/// A texel coordinate in 12.4 fixed point.
struct UV_t {
    u16 u;
    u16 v;
};
SDF_STATIC_ASSERT_SIZE(UV_t, 4);
SDF_STATIC_ASSERT_OFFSETOF(UV_t, u, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(UV_t, v, 0x02);

struct VTX_t {
    f32 x;
    f32 y;
};
SDF_STATIC_ASSERT_SIZE(VTX_t, 8);
SDF_STATIC_ASSERT_OFFSETOF(VTX_t, x, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(VTX_t, y, 0x04);

using sdfBillBoardBatchPtr_t = guest_ptr<struct sdfBillBoardBatch_t>;
using dds3BillResourcePtr_t = guest_ptr<struct dds3BillResource_tag>;
using dds3BillboardTagPtr_t = guest_ptr<dds3BillResourcePtr_t>;

/// What a billboard batch draws from: its texture, its quad, and a tag per ordering table. The
/// tail of BillboardLoadTex's 1100-byte allocation, whose first 1024 bytes are `batch`.
struct dds3BillResource_tag {
    sdfTexHandlePtr_t hTex;
    u16 type;
    u32 count;
    UV_t uv[4];
    f32 cx;
    f32 cy;
    f32 width;
    f32 height;

    /// One tag per ordering table; current_mode picks the one a batch links into.
    sdfDrawTagPtr_t pBillTag[4];
    u16 current_mode;

    sdfMemHandlePtr_t hMem;
    sdfBillBoardBatchPtr_t batch;

    /// The pending list dds3BillboardLinkOt walks from dds3BillboardTag, self while unqueued.
    dds3BillResourcePtr_t next;
};
SDF_STATIC_ASSERT_SIZE(dds3BillResource_tag, 76);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillResource_tag, hTex, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillResource_tag, type, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillResource_tag, count, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillResource_tag, uv, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillResource_tag, cx, 0x1C);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillResource_tag, cy, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillResource_tag, width, 0x24);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillResource_tag, height, 0x28);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillResource_tag, pBillTag, 0x2C);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillResource_tag, current_mode, 0x3C);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillResource_tag, hMem, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillResource_tag, batch, 0x44);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillResource_tag, next, 0x48);

/// sdfBillBoardMake1
struct sdfBillBoard1Packet_t {
    u16 dma_qwc;
    u8 dma_pad;
    u8 dma_id;
    u32 dma_addr;
    u32 vif_unpack1;
    u32 count;
    u32 giftag_h;
    u32 giftag_reg_l;
    u32 giftag_reg_h;
    u32 vif_unpack2;
    u32 prim;
    u32 vif_unpack3;
    f32 center[3];
    u32 vif_unpack4;
    u32 color;
    u32 vif_unpack5;
    u32 uv[4];
    u32 vif_unpack6;
    f32 xyOffset[8];
    u32 vif_itop;
    u32 vif_mscal;
    u32 pad;
};
SDF_STATIC_ASSERT_SIZE(sdfBillBoard1Packet_t, 128);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, dma_qwc, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, dma_pad, 0x02);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, dma_id, 0x03);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, dma_addr, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, vif_unpack1, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, count, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, giftag_h, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, giftag_reg_l, 0x14);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, giftag_reg_h, 0x18);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, vif_unpack2, 0x1C);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, prim, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, vif_unpack3, 0x24);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, center, 0x28);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, vif_unpack4, 0x34);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, color, 0x38);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, vif_unpack5, 0x3C);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, uv, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, vif_unpack6, 0x50);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, xyOffset, 0x54);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, vif_itop, 0x74);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, vif_mscal, 0x78);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoard1Packet_t, pad, 0x7C);

/// sdfBillBoardVcMake1
struct sdfBillBoardVc1Packet_t {
    u16 dma_qwc;
    u8 dma_pad;
    u8 dma_id;
    u32 dma_addr;
    u32 vif_unpack1;
    u32 count;
    u32 giftag_h;
    u32 giftag_reg_l;
    u32 giftag_reg_h;
    u32 vif_unpack2;
    u32 prim;
    u32 vif_unpack3;
    f32 center[3];
    u32 vif_unpack4;
    u32 color[4];
    u32 vif_unpack5;
    u32 uv[4];
    u32 vif_unpack6;
    f32 xyOffset[8];
    u32 vif_itop;
    u32 vif_mscal;
    u32 pad[2];
};
SDF_STATIC_ASSERT_SIZE(sdfBillBoardVc1Packet_t, 144);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, dma_qwc, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, dma_pad, 0x02);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, dma_id, 0x03);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, dma_addr, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, vif_unpack1, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, count, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, giftag_h, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, giftag_reg_l, 0x14);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, giftag_reg_h, 0x18);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, vif_unpack2, 0x1C);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, prim, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, vif_unpack3, 0x24);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, center, 0x28);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, vif_unpack4, 0x34);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, color, 0x38);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, vif_unpack5, 0x48);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, uv, 0x4C);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, vif_unpack6, 0x5C);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, xyOffset, 0x60);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, vif_itop, 0x80);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, vif_mscal, 0x84);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardVc1Packet_t, pad, 0x88);

/// sdfBillBoardMakeTMPacket
struct sdfBillBoardTMPacket_t {
    u16 dma_qwc;
    u8 dma_pad;
    u8 dma_id;
    u32 dma_addr;
    u32 vif_unpack;
    f32 tm[6];
    u32 vif_itop;
    u32 vif_mscal;
    u32 pad[3];
};
SDF_STATIC_ASSERT_SIZE(sdfBillBoardTMPacket_t, 56);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardTMPacket_t, dma_qwc, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardTMPacket_t, dma_pad, 0x02);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardTMPacket_t, dma_id, 0x03);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardTMPacket_t, dma_addr, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardTMPacket_t, vif_unpack, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardTMPacket_t, tm, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardTMPacket_t, vif_itop, 0x24);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardTMPacket_t, vif_mscal, 0x28);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardTMPacket_t, pad, 0x2C);

using Sprite3DPtr_t = guest_ptr<struct Sprite3D_t>;

/// One camera-facing quad with its orientation, which sdfBillBoardMake1 never sees: drawSprite3D
/// feeds the rotation to SetRotateMatrixZ and only the centre, uvs and offsets go on.
/// @fn drawSprite3D
/// @note SLUS-20911 0x0012A2D0
struct Sprite3D_t {
    sdfVector4_t pos;
    UV_t uv[4];
    VTX_t vt[4];
    s32 col;
    f32 rot;
};
SDF_STATIC_ASSERT_SIZE(Sprite3D_t, 72);
SDF_STATIC_ASSERT_OFFSETOF(Sprite3D_t, pos, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(Sprite3D_t, uv, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(Sprite3D_t, vt, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(Sprite3D_t, col, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(Sprite3D_t, rot, 0x44);

/// One batch of camera-facing quads, filled a quad at a time by dds3BillboardTagLink. The arrays
/// are one allocation, which is why sdfBillBoardMakeN takes four pointers into it.
/// @note SLUS-20911, filled at 0x0015B320
struct sdfBillBoardBatch_t {
    sdfVector4_t centre[15];
    u32 color[15];
    UV_t uv[15][4];
    VTX_t offset[15][4];
    s32 count;
};
SDF_STATIC_ASSERT_SIZE(sdfBillBoardBatch_t, 1024);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardBatch_t, centre, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardBatch_t, color, 0xF0);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardBatch_t, uv, 0x12C);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardBatch_t, offset, 0x21C);
SDF_STATIC_ASSERT_OFFSETOF(sdfBillBoardBatch_t, count, 0x3FC);

using sdf3DPrimPolyParmPtr_t = guest_ptr<struct sdf3DPrimPolyParm_t>;

/// A polygon as its counts, PRIM word and one array per attribute, which is what the packet it
/// builds is derived from rather than read back.
/// @fn sdf3DPrimMakePoly
/// @note SLUS-20911 0x002BAAB8
struct sdf3DPrimPolyParm_t {
    s16 faceCount;
    s16 pointCount;

    /// The GS PRIM word, or'd with 0x68, and gaining 0x10 for UVs, 0x1000 for a second UV set and
    /// 0x800 for colors as each array below turns out non-null.
    u16 attr;
    u16 attrInhibit;

    u32 color;
    guest_ptr<u32> faceIndex;
    sdfVector4Ptr_t vertex;

    /// Non-null takes the per-vertex stride from 96 to 224 and the base PRIM from 18 to 19. Null on
    /// every call measured on the field scenes.
    sdfVector4Ptr_t normal;

    sdfVector2Ptr_t uv;
    sdfVector2Ptr_t multiUv;
    guest_ptr<u32> vertexColor;

    /// Called instead of sdfGrAllocPacketBuff when it is set.
    u32 allocator;
    f32 zBias;
};
SDF_STATIC_ASSERT_SIZE(sdf3DPrimPolyParm_t, 44);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, faceCount, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, pointCount, 0x02);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, attr, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, attrInhibit, 0x06);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, color, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, faceIndex, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, vertex, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, normal, 0x14);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, uv, 0x18);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, multiUv, 0x1C);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, vertexColor, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, allocator, 0x24);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, zBias, 0x28);

// attrBits is the GS PRIM word in its low bits, with the engine's own flags above it. The three
// pass bits are what corePktEmitAllPasses and corePktOpenAndEmit branch on.
enum sdfCoreAttrBit_t : u16 {
    sdfCoreAttrRefMapPass = 0x0001,

    /// A second textured pass through sdf3DSpecularTex, a texture handle where sdf3DRefMapParam
    /// holds a raw TEX0. Drawn in GS context 2, writing color and never depth. See TODO V7.
    sdfCoreAttrSpecularPass = 0x0002,
    sdfCoreAttrGouraud = 0x0008,
    sdfCoreAttrTextured = 0x0010,
    sdfCoreAttrFog = 0x0020,
    sdfCoreAttrBlend = 0x0040,
    sdfCoreAttrColor = 0x0800,
    /// A second UV set travels with the mesh and a second textured pass draws through it.
    sdfCoreAttrMultiTexPass = 0x1000,
    sdfCoreAttrTwoSided = 0x4000,
};

// packetBits says which arrays the body carries.
enum sdfCorePacketBit_t : u16 {
    sdfCorePacketFaces = 0x0020,
    sdfCorePacketNormals = 0x0080,
    sdfCorePacketUv = 0x0100,
    sdfCorePacketWeights = 0x0800,
    sdfCorePacketBlendShapes = 0x1000,
};

// The body layouts sdfCorePacketGen dispatches on before it looks at packetBits at all.
/// The header every VIF-stream batch opens with, as its own UNPACK.
struct sdfCoreBatchHeader_t {
    u16 faceCount;
    u16 pointCount;
    u16 attrBits;
    u16 packetBits;
};
using sdfCoreBatchHeaderPtr_t = guest_ptr<sdfCoreBatchHeader_t>;

enum sdfCorePacketType_t : u8 {
    sdfCorePacketTypeBlendShape = 5,
    sdfCorePacketTypeWeighted = 7,
    sdfCorePacketTypeRigid = 8,
};

// The ALPHA and TEST a draw runs under, from neither its mesh nor its material: sdfModelSetDrawEnv
// builds one pair per ordering table, so the packet's own otIndex picks them.
struct sdfGsState_t {
    u64 alpha = 0x44;
    u64 test = 0x717FB;

    /// FRAME's write mask, one bit per masked bit of the pixel. The frame-mask pair sets it to
    /// 0x00FFFFFF so a pass writes alpha and no color, which is how the game builds its bloom mask.
    u64 frameMask = 0;
};

constexpr u64 sdfGetFrameMask(u64 frame) { return (frame >> 32) & 0xFFFFFFFF; }

/// What sdfGrSetFrameMaskRGB writes: color masked, alpha writable.
constexpr u64 sdfFrameMaskRgb = 0x00FFFFFF;

/// The four the engine sets up, in ordering-table order.
constexpr sdfGsState_t sdfGetGsStateForOt(u32 otIndex) {
    switch (otIndex) {
        case 2: return {0x48, 0x71801};  // additive
        case 3: return {0x42, 0x71801};  // subtractive
        default: return {0x44, 0x717FB};  // opaque and semi-transparent share a pair
    }
}

/// Every GS register an A+D or REGLIST write can name. A context-2 register sits one address
/// above its context-1 twin.
enum sdfGsRegisterAddr_t : u32 {
    sdfGsAddrPrim = 0x00,
    sdfGsAddrRgbaq = 0x01,
    sdfGsAddrSt = 0x02,
    sdfGsAddrUv = 0x03,
    sdfGsAddrXyzf2 = 0x04,
    sdfGsAddrXyz2 = 0x05,
    sdfGsAddrTex01 = 0x06,
    sdfGsAddrTex02 = 0x07,
    sdfGsAddrClamp1 = 0x08,
    sdfGsAddrClamp2 = 0x09,
    sdfGsAddrFog = 0x0A,
    sdfGsAddrXyzf3 = 0x0C,
    sdfGsAddrXyz3 = 0x0D,
    sdfGsAddrTex11 = 0x14,
    sdfGsAddrTex12 = 0x15,
    sdfGsAddrTex21 = 0x16,
    sdfGsAddrTex22 = 0x17,
    sdfGsAddrXyoffset1 = 0x18,
    sdfGsAddrXyoffset2 = 0x19,
    sdfGsAddrPrmodecont = 0x1A,
    sdfGsAddrPrmode = 0x1B,
    sdfGsAddrTexclut = 0x1C,
    sdfGsAddrScanmsk = 0x22,
    sdfGsAddrMiptbp11 = 0x34,
    sdfGsAddrMiptbp12 = 0x35,
    sdfGsAddrMiptbp21 = 0x36,
    sdfGsAddrMiptbp22 = 0x37,
    sdfGsAddrTexa = 0x3B,
    sdfGsAddrFogcol = 0x3D,
    sdfGsAddrTexflush = 0x3F,
    sdfGsAddrScissor1 = 0x40,
    sdfGsAddrScissor2 = 0x41,
    sdfGsAddrAlpha1 = 0x42,
    sdfGsAddrAlpha2 = 0x43,
    sdfGsAddrDimx = 0x44,
    sdfGsAddrDthe = 0x45,
    sdfGsAddrColclamp = 0x46,
    sdfGsAddrTest1 = 0x47,
    sdfGsAddrTest2 = 0x48,
    sdfGsAddrPabe = 0x49,
    sdfGsAddrFba1 = 0x4A,
    sdfGsAddrFba2 = 0x4B,
    sdfGsAddrFrame1 = 0x4C,
    sdfGsAddrFrame2 = 0x4D,
    sdfGsAddrZbuf1 = 0x4E,
    sdfGsAddrZbuf2 = 0x4F,
    sdfGsAddrBitbltbuf = 0x50,
    sdfGsAddrTrxpos = 0x51,
    sdfGsAddrTrxreg = 0x52,
    sdfGsAddrTrxdir = 0x53,
    sdfGsAddrHwreg = 0x54,
    sdfGsAddrSignal = 0x60,
    sdfGsAddrFinish = 0x61,
    sdfGsAddrLabel = 0x62,
};


// ALPHA: Cv = (A - B) * C + D, over the source, the destination and zero.
constexpr u32 sdfGetAlphaA(u64 r) { return u32(r & 3); }
constexpr u32 sdfGetAlphaB(u64 r) { return u32((r >> 2) & 3); }
constexpr u32 sdfGetAlphaC(u64 r) { return u32((r >> 4) & 3); }
constexpr u32 sdfGetAlphaD(u64 r) { return u32((r >> 6) & 3); }
constexpr u32 sdfGetAlphaFix(u64 r) { return u32((r >> 32) & 0xFF); }
enum sdfAlphaTerm_t : u32 { sdfAlphaSource = 0, sdfAlphaDest = 1, sdfAlphaZero = 2 };
enum sdfAlphaCoef_t : u32 { sdfAlphaAs = 0, sdfAlphaAd = 1, sdfAlphaFixed = 2 };

// TEST: the alpha test, what a failing fragment still writes, and the depth test.
constexpr u32 sdfGetTestAte(u64 r) { return u32(r & 1); }
constexpr u32 sdfGetTestAtst(u64 r) { return u32((r >> 1) & 7); }
constexpr u32 sdfGetTestAref(u64 r) { return u32((r >> 4) & 0xFF); }
constexpr u32 sdfGetTestAfail(u64 r) { return u32((r >> 12) & 3); }
constexpr u32 sdfGetTestDate(u64 r) { return u32((r >> 14) & 1); }
constexpr u32 sdfGetTestDatm(u64 r) { return u32((r >> 15) & 1); }
constexpr u32 sdfGetTestZte(u64 r) { return u32((r >> 16) & 1); }
constexpr u32 sdfGetTestZtst(u64 r) { return u32((r >> 17) & 3); }

enum sdfAtst_t : u32 { sdfAtstNever = 0, sdfAtstAlways = 1 };
enum sdfZtst_t : u32 { sdfZtstNever = 0, sdfZtstAlways = 1, sdfZtstGequal = 2, sdfZtstGreater = 3 };
enum sdfAfail_t : u32 { sdfAfailKeep = 0, sdfAfailFbOnly = 1, sdfAfailZbOnly = 2, sdfAfailRgbOnly = 3 };

// The GS registers the material packet carries, which sdfMatUpdatePacketBasic copies out of the
// texture handle rather than the material.
struct sdfMatPacket_t {
    u32 vifcode0;
    u32 ambient;
    u32 diffuse;
    u32 mtexDiffuse;
    u32 specularColor;
    u32 refmapColor;
    u32 vifcode1;
    f32 constant;
    u32 mtexAlpha;
    u32 mtexAlgo;
    f32 fdBias;
    f32 fdSoften;
    u32 pad;
    u32 vifcode2;
    u64 tex1Data;
    u64 tex0Data;
    u64 clampData;
    u64 mtexTex1Data;
    u64 mtexTex0Data;
    u64 mtexClampData;

    /// sdfMatSetUvwParm writes six floats here, a 2x2 followed by a 2-vector offset, and the field's
    /// own address is what corePktXfmLightClipUV is handed.
    f32 texTm[6];
};
SDF_STATIC_ASSERT_SIZE(sdfMatPacket_t, 128);
SDF_STATIC_ASSERT_OFFSETOF(sdfMatPacket_t, tex1Data, 0x38);
SDF_STATIC_ASSERT_OFFSETOF(sdfMatPacket_t, tex0Data, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(sdfMatPacket_t, clampData, 0x48);
SDF_STATIC_ASSERT_OFFSETOF(sdfMatPacket_t, texTm, 0x68);

using corePktCbPtr_t = guest_ptr<struct corePktCb_t>;

// This build carries 64 bytes ahead of faceCount where the reconstructed HD sdfCorePacketCb_t has
// only tmSave, so every field from faceCount to pointBuff sits 0x3C past its HD offset.
struct corePktCb_t {
    u8 head[64];
    s16 faceCount;
    s16 pointCount;
    s32 attrBits;
    s32 packetBits;
    s32 orgAttrBits;
    s16 drawFaceCount;
    s16 backfaceCount;
    sdfMatPacketPtr_t matPacketPtr;
    u32 color;
    f32 fdSoften;
    f32 fdBias;
    u32 pointBuff;
    u32 pointBuf1;
    u32 pointBuf2;
    u32 pointCount0;
    u32 pointCount1;
    u32 unk120;
    u32 unk124;
    u32 unk128;
    u32 unk132;
    u32 unk136;
    u32 unk140;
    sdfVoidPtr_t payload;
    u32 unk148;
    u8 unk152[20];
    f32 unk172;
    u32 unk176;
    sdfMatPacketPtr_t matPacket2;
    u8 unk184[136];
};
SDF_STATIC_ASSERT_SIZE(corePktCb_t, 320);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, head, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, faceCount, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, pointCount, 0x42);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, attrBits, 0x44);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, packetBits, 0x48);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, orgAttrBits, 0x4C);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, drawFaceCount, 0x50);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, backfaceCount, 0x52);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, matPacketPtr, 0x54);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, color, 0x58);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, fdSoften, 0x5C);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, fdBias, 0x60);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, pointBuff, 0x64);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, pointBuf1, 0x68);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, pointBuf2, 0x6C);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, pointCount0, 0x70);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, pointCount1, 0x74);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk120, 0x78);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk124, 0x7C);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk128, 0x80);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk132, 0x84);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk136, 0x88);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk140, 0x8C);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, payload, 0x90);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk148, 0x94);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk152, 0x98);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk172, 0xAC);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk176, 0xB0);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, matPacket2, 0xB4);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk184, 0xB8);

// TEX0's texture function and whether the texture carries alpha.
constexpr u32 sdfGetTex0Tfx(u64 r) { return u32((r >> 35) & 3); }
constexpr u32 sdfGetTex0Tcc(u64 r) { return u32((r >> 34) & 1); }
enum sdfTexFunction_t : u32 {
    sdfTexModulate = 0,
    sdfTexDecal = 1,
    sdfTexHighlight = 2,
    sdfTexHighlight2 = 3,
};

// TEX1's filters. LINEAR is 1; the mipmap minification modes are 2 and above.
constexpr u32 sdfGetTex1Mmag(u64 r) { return u32((r >> 5) & 1); }
constexpr u32 sdfGetTex1Mmin(u64 r) { return u32((r >> 6) & 7); }

// CLAMP's wrap mode per axis: 0 repeat, 1 clamp, 2 region repeat, 3 region clamp.
constexpr u32 sdfGetClampWms(u64 r) { return u32(r & 3); }
constexpr u32 sdfGetClampWmt(u64 r) { return u32((r >> 2) & 3); }
enum sdfWrapMode_t : u32 { sdfWrapRepeat = 0, sdfWrapClamp = 1 };

// A VIF code, which is how a mesh body names each of its vertex arrays.
enum sdfVifCommand_t : u32 {
    sdfVifNop = 0x00,
    sdfVifFlushEnd = 0x10,
    sdfVifActMicro = 0x14,
    sdfVifCntMicro = 0x17,
    sdfVifUnpack = 0x60,
};

/// sdfCorePacketGen's own threshold for a blend shape being worth applying.
constexpr f32 sdfMorphEpsilon = 0.000001f;

/// The viewport sdf3DMakeViewportPacket copies into the camera packet VU1 loads. TM is the
/// projection; scale and trans map clip space onto GS coordinates at 0x02BD.
struct sdf3DScreenParam_t {
    u32 flags;
    f32 aspect;
    f32 projDist;
    f32 fovy;
    f32 ox;
    f32 oy;
    f32 width;
    f32 height;
    f32 zMin;
    f32 zMax;
    f32 nearClip;
    f32 farClip;
    sdfMatrix4_t TM;
    sdfVector4_t scale;
    sdfVector4_t trans;
    sdfVector4_t cameraPos;
};
SDF_STATIC_ASSERT_SIZE(sdf3DScreenParam_t, 160);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DScreenParam_t, zMin, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DScreenParam_t, TM, 0x30);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DScreenParam_t, scale, 0x70);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DScreenParam_t, trans, 0x80);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DScreenParam_t, cameraPos, 0x90);

using sdf3DScreenParamPtr_t = guest_ptr<sdf3DScreenParam_t>;

/// The parallel-light packet VU1 shades with: three directions, a base term and a color each, all
/// in the GS's own 0..255 units.
struct sdf3DLightParallelPacket_t {
    u64 dmatag;
    u64 vifcode;

    /// A matrix whose columns are the light vectors: VU1 multiplies the normal through it as rows
    /// and reads the three dot products out of the result.
    sdfVector4_t normal[3];

    sdfVector4_t colorBase;
    sdfVector4_t color[5];

    /// corePktEmitRefMapPass builds its specular axis from the last row.
    sdfMatrix4_t TM;
};
SDF_STATIC_ASSERT_SIZE(sdf3DLightParallelPacket_t, 224);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DLightParallelPacket_t, normal, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DLightParallelPacket_t, colorBase, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DLightParallelPacket_t, color, 0x50);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DLightParallelPacket_t, TM, 0xA0);

struct sdf3DFogPacket_t {
    u64 dmatag;
    u64 vifcode;

    /// Minimum, maximum, the midpoint VU1 works from, and the reciprocal-depth scale.
    f32 fogParm[4];

    u64 giftag[2];

    /// One GIF A+D pair: the color, then the GS register it names.
    u64 fogColor;
    u64 fogColorReg;

    u32 mscal;
    u32 pad[3];
};
SDF_STATIC_ASSERT_SIZE(sdf3DFogPacket_t, 80);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DFogPacket_t, fogParm, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DFogPacket_t, fogColor, 0x30);

/// sdf3DLinkInitPacket: a REF to vu1MicroPacket and the tag after it.
struct sdf3DInitPacket_t {
    sdfDmaTag_t reftag;
    sdfDmaTag_t linktag;
};
SDF_STATIC_ASSERT_SIZE(sdf3DInitPacket_t, 32);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DInitPacket_t, reftag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DInitPacket_t, linktag, 0x10);

/// One ordering table's alpha and test state, which sdfModelDrawEnvLinkOt files ahead of its draws.
struct sdfModelDrawEnvSet_t {
    sdfDrawTag_t drawTag;
    sdfPrimTransPacket_t transPacket;
};
SDF_STATIC_ASSERT_SIZE(sdfModelDrawEnvSet_t, 96);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelDrawEnvSet_t, drawTag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelDrawEnvSet_t, transPacket, 0x20);

struct sdfModelDrawEnvTerm_t {
    sdfDrawTag_t drawTag;
    sdfDmaTag_t footer;
};
SDF_STATIC_ASSERT_SIZE(sdfModelDrawEnvTerm_t, 48);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelDrawEnvTerm_t, drawTag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelDrawEnvTerm_t, footer, 0x20);

using sdfModelDrawEnvPtr_t = guest_ptr<struct sdfModelDrawEnv_t>;

/// sdfModelSetDrawEnv: one set per ordering table, then the terminator linked into the last.
struct sdfModelDrawEnv_t {
    sdfModelDrawEnvSet_t set[4];
    sdfModelDrawEnvTerm_t term;
};
SDF_STATIC_ASSERT_SIZE(sdfModelDrawEnv_t, 432);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelDrawEnv_t, set, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelDrawEnv_t, term, 0x180);

/// The kernel's per-frame draw state, dds3DrawEnv[2], as dds3InitDrawEnvSub and dds3InitRenderDrawEnv
/// build it. Names are HD's; every member is inline here where HD keeps a pointer.
struct dds3DrawEnv_t {
    sdfDrawTag_t SysDrawTag;
    sdf3DInitPacket_t TdPacket;
    sdfModelDrawEnv_t ModelDrawEnv[11];
    sdfDrawTag_t DrawTag;
    sdfPreDrawTag_t PreDrawTag;
    sdfGrStdDrawEnv_t DrawEnv;
    sdf3DViewportPacket_t ViewportPacket;
    sdf3DLightParallelPacket_t DirLightPacket;
    sdfDrawTag_t RenderDrawTag;
    sdfGrDrawEnv_t RenderDrawEnv;
    sdf3DViewportPacket_t RenderViewportPacket;
    sdf3DLightParallelPacket_t RenderDirLightPacket;
    sdfDrawTag_t HighDrawTag;
    sdf3DViewportPacket_t HighViewportPacket;
    sdf3DLightParallelPacket_t HighDirLightPacket;
};
SDF_STATIC_ASSERT_SIZE(dds3DrawEnv_t, 7136);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, SysDrawTag, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, TdPacket, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, ModelDrawEnv, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, DrawTag, 0x12D0);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, PreDrawTag, 0x12F0);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, DrawEnv, 0x1300);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, ViewportPacket, 0x1520);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, DirLightPacket, 0x15F0);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, RenderDrawTag, 0x16D0);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, RenderDrawEnv, 0x16F0);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, RenderViewportPacket, 0x1860);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, RenderDirLightPacket, 0x1930);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, HighDrawTag, 0x1A10);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, HighViewportPacket, 0x1A30);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, HighDirLightPacket, 0x1B00);

/// One blend shape a morph controller drives, and how far towards it the model currently is.
struct sdfModelMorphParm_t {
    u32 targetId;
    f32 blend;
};
SDF_STATIC_ASSERT_SIZE(sdfModelMorphParm_t, 8);

/// A morph controller mixes at most two targets, which is what bounds the shaped packet's chain.
constexpr u32 kMorphTargets = 2;

struct sdfModelMorph_t {
    sdfModelMorphParm_t target[kMorphTargets];
};
SDF_STATIC_ASSERT_SIZE(sdfModelMorph_t, 16);

// What sdfModelLinkOt switches on to pick a geometry path.
enum sdfPacketKind_t : u8 {
    sdfPacketEnd = 0,
    sdfPacketDmaCall = 1,
    sdfPacketShaped = 2,
    sdfPacketGenerated = 3,
};

// GIF PACKED registers. Each occupies a whole quadword whatever it uses of it. A GIF tag's REGS
// field names them one nibble each, so a packet's layout is read rather than assumed.
enum sdfGifReg_t : u32 {
    sdfGifRegPrim = 0x0,
    sdfGifRegRgbaq = 0x1,
    sdfGifRegSt = 0x2,
    sdfGifRegUv = 0x3,
    sdfGifRegXyzf2 = 0x4,
    sdfGifRegXyz2 = 0x5,
    sdfGifRegTex01 = 0x6,
    sdfGifRegTex02 = 0x7,
    sdfGifRegClamp1 = 0x8,
    sdfGifRegClamp2 = 0x9,
    sdfGifRegFog = 0xA,
    sdfGifRegReserved = 0xB,
    sdfGifRegXyzf3 = 0xC,
    sdfGifRegXyz3 = 0xD,
    sdfGifRegAD = 0xE,
    sdfGifRegNop = 0xF,
};

constexpr u32 sdfGetGifRegAt(u64 regs, u32 index) { return u32((regs >> (4 * index)) & 0xF); }
struct sdfGifRgbaq_t {
    u32 r, g, b, a;
};
SDF_STATIC_ASSERT_SIZE(sdfGifRgbaq_t, 16);

struct sdfGifUv_t {
    u32 u, v;  // 12.4 fixed texels
    u32 pad[2];
};
SDF_STATIC_ASSERT_SIZE(sdfGifUv_t, 16);

struct sdfGifXyz2_t {
    u32 x, y;  // 12.4 fixed, XYOFFSET included
    u32 z;
    u32 unused;
};
SDF_STATIC_ASSERT_SIZE(sdfGifXyz2_t, 16);

// One sprite: two opposite corners, flat-shaded from a single RGBAQ.
struct sdfGrSpritePrim_t {
    sdfGifRgbaq_t rgbaq;
    sdfGifUv_t uv0;
    sdfGifXyz2_t xyz0;
    sdfGifUv_t uv1;
    sdfGifXyz2_t xyz1;
};
SDF_STATIC_ASSERT_SIZE(sdfGrSpritePrim_t, 80);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrSpritePrim_t, uv0, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrSpritePrim_t, xyz0, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrSpritePrim_t, uv1, 0x30);
SDF_STATIC_ASSERT_OFFSETOF(sdfGrSpritePrim_t, xyz1, 0x40);

/// The GS TEX0 register, which is all that is left describing a texture once the engine has dropped
/// its main-memory copy.
struct GsTex0 {
    u32 tbp0;
    u32 tbw;
    u32 psm;
    u32 tw;
    u32 th;
    u32 cbp;
    u32 cpsm;
    u32 csa;

    static constexpr GsTex0 decode(u64 reg) {
        return {u32(reg & 0x3FFF),        u32((reg >> 14) & 0x3F), u32((reg >> 20) & 0x3F),
                u32((reg >> 26) & 0xF),   u32((reg >> 30) & 0xF),  u32((reg >> 37) & 0x3FFF),
                u32((reg >> 51) & 0xF),   u32((reg >> 56) & 0x1F)};
    }

    /// The GS's own limit: TW and TH cap at 1024, so a register past it is not a texture and
    /// reading it walks off GS memory. TBW 0 is left alone, the GS reads it as one page.
    constexpr bool isReadable() const { return tw <= 10 && th <= 10; }

    constexpr bool isPaletted() const { return (psm & 0x13) == 0x13 || psm == 0x14 || psm == 0x24 || psm == 0x2C; }
    constexpr bool isFourBit() const { return psm == 0x14 || psm == 0x24 || psm == 0x2C; }
};

// PRIM's own fields, the ones that decide how a packet is read back.
constexpr u32 sdfGetPrimType(u32 prim) { return prim & 7; }
constexpr u32 sdfGetPrimIip(u32 prim) { return (prim >> 3) & 1; }
constexpr u32 sdfGetPrimTme(u32 prim) { return (prim >> 4) & 1; }
constexpr u32 sdfGetPrimFge(u32 prim) { return (prim >> 5) & 1; }
constexpr u32 sdfGetPrimAbe(u32 prim) { return (prim >> 6) & 1; }
constexpr u32 sdfGetPrimAa1(u32 prim) { return (prim >> 7) & 1; }
constexpr u32 sdfGetPrimFst(u32 prim) { return (prim >> 8) & 1; }

// The GS register addresses an A+D write can name, for the ones that decide how a draw looks.
enum sdfGsReg_t : u32 {
    sdfGsRegPrim = 0x00,
    sdfGsRegTex0_1 = 0x06,
    sdfGsRegTex0_2 = 0x07,
    sdfGsRegClamp1 = 0x08,
    sdfGsRegTex1_1 = 0x14,
    sdfGsRegAlpha1 = 0x42,
    sdfGsRegAlpha2 = 0x43,
    sdfGsRegTest1 = 0x47,
    sdfGsRegTest2 = 0x48,
};

// TEX0 names where a texture is in GS memory and how it is laid out there.
struct sdfTex0_t {
    u32 tbp;   // base pointer, in 64-quadword blocks
    u32 tbw;   // buffer width, in 64-texel units
    u32 psm;   // pixel storage mode
    u32 width;
    u32 height;
    u32 tcc;   // 0 = RGB, 1 = RGBA
    u32 tfx;   // how the texel combines with the vertex color
    u32 cbp;   // CLUT base pointer
    u32 cpsm;  // CLUT pixel storage mode
    u32 csm;   // CLUT storage mode
    u32 csa;   // CLUT entry offset
};

constexpr sdfTex0_t sdfDecodeTex0(u64 value) {
    return sdfTex0_t{u32(value & 0x3FFF),
                     u32((value >> 14) & 0x3F),
                     u32((value >> 20) & 0x3F),
                     1u << ((value >> 26) & 0xF),
                     1u << ((value >> 30) & 0xF),
                     u32((value >> 34) & 1),
                     u32((value >> 35) & 3),
                     u32((value >> 37) & 0x3FFF),
                     u32((value >> 51) & 0xF),
                     u32((value >> 55) & 1),
                     u32((value >> 56) & 0x1F)};
}

// GS pixel storage modes, the ones this game's textures use.
enum sdfPsm_t : u32 {
    sdfPsmct32 = 0x00,
    sdfPsmct24 = 0x01,
    sdfPsmct16 = 0x02,
    sdfPsmct16s = 0x0A,
    sdfPsmt8 = 0x13,
    sdfPsmt4 = 0x14,
    sdfPsmt8h = 0x1B,
    sdfPsmt4hl = 0x24,
    sdfPsmt4hh = 0x2C,
};

enum sdfPrimType_t : u32 {
    sdfPrimPoint = 0,
    sdfPrimLine = 1,
    sdfPrimLineStrip = 2,
    sdfPrimTriangle = 3,
    sdfPrimTriStrip = 4,
    sdfPrimTriFan = 5,
    sdfPrimSprite = 6,
};

// A loaded texture: the engine's own descriptor, with its pixels and CLUT in main memory. This is
// the level a native renderer wants, rather than whatever copy of them ended up in GS memory.
struct sdfTexHandle_t {
    sdfTexHandlePtr_t prev;
    sdfTexHandlePtr_t next;
    sdfTexHandlePtr_t origin;
    s16 width;
    s16 height;
    sdfGsMemSlicePtr_t pixelGsHandle;
    sdfGsMemSlicePtr_t clutGsHandle;
    u8 numOfCLUT;
    u8 CLUTFormat;
    u8 pixelFormat;
    u8 numOfMipmap;
    u16 mipmapParam;
    u8 flags;
    u8 texWrapMode;
    u32 texId;
    u32 clutId;
    sdfTexPacketPtr_t packet;
    sdfTexPacketPtr_t packetCtx2;
    sdfVoidPtr_t clutData;
    u32 clutDataSize;
    u32 clutColor;
    u8 monoClutData;
    u8 pad[3];
    guest_ptr<char> hashname;
    // Not an EE pointer on a loaded texture: it reads 0x10000000, and pixelGsHandle is what names
    // where the pixels actually are. Only clutData stays in main memory.
    u32 image;
};
SDF_STATIC_ASSERT_SIZE(sdfTexHandle_t, 72);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexHandle_t, width, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexHandle_t, pixelFormat, 0x1A);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexHandle_t, texId, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexHandle_t, packet, 0x28);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexHandle_t, clutData, 0x30);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexHandle_t, hashname, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexHandle_t, image, 0x44);

// The texture asset sdfTexSetEx is handed, and never writes to. It describes itself, so nothing has
// to be read back out of the handle the call returns.
using TMX_HEADER_PTR_t = guest_ptr<struct TMX_HEADER_t>;

struct TMX_HEADER_t {
    u8 fileType;
    u8 compression;  // the low nibble is unused; the high nibble is where the data starts
    s16 userId;
    s32 fileSize;
    u32 formatId;
    s32 realSize;
    u8 numOfCLUT;
    u8 CLUTFormat;
    s16 width;
    s16 height;
    u8 pixelFormat;
    u8 numOfMipmap;
    u16 mipmapParam;
    u8 flags;
    u8 texWrapMode;
    u8 userArea[1];
    u8 comment[1];
    u8 pad[2];
};
SDF_STATIC_ASSERT_SIZE(TMX_HEADER_t, 32);
SDF_STATIC_ASSERT_OFFSETOF(TMX_HEADER_t, userArea, 0x1C);
SDF_STATIC_ASSERT_OFFSETOF(TMX_HEADER_t, numOfCLUT, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(TMX_HEADER_t, width, 0x12);
SDF_STATIC_ASSERT_OFFSETOF(TMX_HEADER_t, pixelFormat, 0x16);

// The engine skips this much past the header's own data offset before the CLUT.
constexpr u32 sdfTmxDataGap = 64;

// A glyph's slot in the font sheet. frTranslateUserData rents one, uploads into it, and returns
// the GsFont_t that points back at it.
using FontMem_PTR_t = guest_ptr<struct FontMem_t>;

struct FontMem_t {
    u32 id;
    s32 uv[4];  // the slot's position in the sheet, 12.4
    sdfVoidPtr_t user_data;
    FontMem_PTR_t prev;
    FontMem_PTR_t next;
};
SDF_STATIC_ASSERT_SIZE(FontMem_t, 32);
SDF_STATIC_ASSERT_OFFSETOF(FontMem_t, uv, 0x04);

using GsFont_PTR_t = guest_ptr<struct GsFont_t>;

struct GsFont_t {
    u16 font_no;
    u16 y_shift;
    u16 used_count;
    u16 pad;
    FontMem_PTR_t font_mem;
};
SDF_STATIC_ASSERT_SIZE(GsFont_t, 12);
SDF_STATIC_ASSERT_OFFSETOF(GsFont_t, font_mem, 0x08);

// One image on its way into GS memory: source in main memory, destination a rectangle inside a
// buffer already there. Every upload passes through sdfImageSetReq with one of these.
struct sdfImageSetParm_t {
    sdfVoidPtr_t data;
    sdfVoidPtr_t dataMemHandle;
    u8 dataMemMode;
    u8 pixelFormat;
    u16 dstBuffWidth;
    u32 gsAddr;
    u16 dstX;
    u16 dstY;
    u16 width;
    u16 height;
};
SDF_STATIC_ASSERT_SIZE(sdfImageSetParm_t, 24);
SDF_STATIC_ASSERT_OFFSETOF(sdfImageSetParm_t, gsAddr, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdfImageSetParm_t, width, 0x14);

// What fclDrawSpriteCore draws: a texture, a UV rectangle packed two per word, and a color.
using fclSprite_PTR_t = guest_ptr<struct fclSprite_t>;

struct fclSprite_t {
    sdfTexHandlePtr_t TexHandle;
    sdfVoidPtr_t Base;
    s32 X;  // u0 in the low half, u1 in the high
    s32 Y;  // v0 and v1 likewise
    s32 Z;
    u8 Col[4];
    u8 Flag;
    u8 pad[3];
};
SDF_STATIC_ASSERT_SIZE(fclSprite_t, 28);
SDF_STATIC_ASSERT_OFFSETOF(fclSprite_t, X, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(fclSprite_t, Col, 0x14);

// What a font glyph is drawn from, in place of a texture handle.
using frGsTex_PTR_t = guest_ptr<struct frGsTex_t>;

struct frGsTex_t {
    u32 Width;
    u32 WidthLog2;
    u32 Height;
    u32 HeightLog2;
    u32 Addr;
    u32 BufWidth;
};
SDF_STATIC_ASSERT_SIZE(frGsTex_t, 24);
