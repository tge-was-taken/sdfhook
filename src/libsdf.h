#pragma once

#include "types.h"
#include "guest_ptr.h"
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

// Named by the database, not mapped here yet. A pointer to an incomplete type is still four bytes.
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

struct sdfModelNodeList_t {
    sdfMemHandlePtr_t memHandle;
    s16 count;
    s16 capacity;
    s16 elemSize;
    s16 growStep;
    /// A pointer to the item array, not the array: readListItem takes both hops.
    sdfVoidPtr_t items;
};
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, count, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, items, 0x0C);
// The header is 12 bytes and the node pointers follow it, which is what +0x0C reads.
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, memHandle, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, count, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, capacity, 0x06);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, elemSize, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, growStep, 0x0A);

// Every sdf growable list has this header and keeps its items inline from sdfListItems.
struct sdfMatList_t {
    sdfMemHandlePtr_t memHandle;
    s16 count;
    s16 capacity;
    s16 elemSize;
    s16 growStep;
    /// A pointer to the item array, not the array.
    sdfVoidPtr_t items;
};
SDF_STATIC_ASSERT_OFFSETOF(sdfMatList_t, count, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfMatList_t, items, 0x0C);
// The header is 12 bytes and the material pointers follow it, which is what +0x0C reads.

constexpr u32 sdfListItems = 0x0C;

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

// sdfGrOtInit installs 85 of these at sdfGrOtTable. regFunc is what a producer calls to register
// its tag; noSortTag holds the ones sdfGrSortOtReg diverts on the 0xFFFF sentinel.
struct sdfGrOt_t {
    sdfGrOtPtr_t next;
    sdfDrawTagPtr_t tagHead;
    sdfDrawTagPtr_t tagTail;
    sdfDrawTagPtr_t noSortTag;
    sdfVoidPtr_t regFunc;
    sdfVoidPtr_t callbackFunc;
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

// The per-layer GS state dds3InitDrawEnvSub fills. Every field is a packet pointer, so the
// mirror is offsets: nothing here is dereferenced on our side.
struct sdfGrStdDrawEnv_t {
    sdfVoidPtr_t worker;
    sdfGrPacketHeaderPtr_t header;
    sdfVoidPtr_t defParm;
    sdfVoidPtr_t frame1;
    sdfVoidPtr_t frame2;
    sdfVoidPtr_t clear;
    sdfVoidPtr_t test1;
    sdfVoidPtr_t alpha1;
    sdfVoidPtr_t test2;
    sdfVoidPtr_t alpha2;
    sdfVoidPtr_t ext;
};
SDF_STATIC_ASSERT_SIZE(sdfGrStdDrawEnv_t, 44);

// Two of these, stride 7136. Only stdDrawEnv is modelled: it is the one field the frame-sprite
// producers read, and the spans either side are unreferenced here.
struct dds3DrawEnv_t {
    u8 beforeStdDrawEnv[0x1300];
    sdfGrStdDrawEnv_t stdDrawEnv;
    u8 afterStdDrawEnv[7136 - 0x1300 - 44];
};
SDF_STATIC_ASSERT_SIZE(dds3DrawEnv_t, 7136);
SDF_STATIC_ASSERT_OFFSETOF(dds3DrawEnv_t, stdDrawEnv, 0x1300);

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

struct sdfModelShapeList_t {
    guest_ptr<struct sdfModelShapeList_t> next;
    u16 count;
    u16 pad;
    sdfModelShapeEntry_t entry[1];
};
SDF_STATIC_ASSERT_OFFSETOF(sdfModelShapeList_t, count, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelShapeList_t, entry, 0x08);
using sdfModelShapeListPtr_t = guest_ptr<sdfModelShapeList_t>;

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

using sdfMatPtr_t = guest_ptr<struct sdfMat_t>;
using sdfTexHandlePtr_t = guest_ptr<struct sdfTexHandle_t>;
using sdfGsMemSlicePtr_t = guest_ptr<struct sdfGsMemSlice_t>;

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
    sdfVoidPtr_t packet[2];
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
constexpr u16 sdfCoreShapeCount(const sdfCorePacket_t& p) { return u16(p.header.extData[0]); }
constexpr u16 sdfCoreMorphIndex(const sdfCorePacket_t& p) { return u16(p.header.extData[0] >> 16); }

// attrBits is the GS PRIM word in its low bits, with the engine's own flags above it. The three
// pass bits are what corePktEmitAllPasses and corePktOpenAndEmit branch on.
using sdfBillBoardBatchPtr_t = guest_ptr<struct sdfBillBoardBatch_t>;
using dds3BillResourcePtr_t = guest_ptr<struct dds3BillResource_t>;

/// The resource a billboard batch draws from: its texture, its quad size and the ordering tag the
/// batch is linked into.
struct dds3BillResource_t {
    sdfTexHandlePtr_t hTex;
    u16 type;
    u16 pad;
    u32 count;
    sdfVector2_t uv[2];
    f32 cx;
    f32 cy;
    f32 width;
    f32 height;
};

using dds3BillboardNodePtr_t = guest_ptr<struct dds3BillboardNode_t>;

/// One entry of the list dds3BillboardLinkOt walks. Only the four fields it reads are modelled:
/// the tag it links into is `tag[otIndex]`, and the batch is what sdfBillBoardMakeN is handed.
struct dds3BillboardNode_t {
    u8 unknown[44];
    u32 tag[4];
    u16 otIndex;
    u16 pad;
    u32 pad2;
    sdfBillBoardBatchPtr_t batch;
    dds3BillboardNodePtr_t next;
};
SDF_STATIC_ASSERT_OFFSETOF(dds3BillboardNode_t, tag, 44);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillboardNode_t, otIndex, 60);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillboardNode_t, batch, 68);
SDF_STATIC_ASSERT_OFFSETOF(dds3BillboardNode_t, next, 72);

/// A quad's texture coordinates, one pair per corner in 12.4 fixed point.
struct sdfBillBoardUv_t {
    struct {
        s16 u;
        s16 v;
    } corner[4];
};

/// A quad's corner offsets from its centre, which dds3BillboardTagLink builds from the resource's
/// width, height and centre and then rotates: the pair is a rotated basis rather than a box.
struct sdfBillBoardQuad_t {
    sdfVector2_t corner[4];
};


using sdfSprite3DPtr_t = guest_ptr<struct sdfSprite3D_t>;

/// One camera-facing quad with its orientation, which sdfBillBoardMake1 never sees: drawSprite3D
/// feeds the rotation to SetRotateMatrixZ and only the centre, uvs and offsets go on.
/// @fn drawSprite3D
/// @note SLUS-20911 0x0012A2D0
struct sdfSprite3D_t {
    // A quadword, the way drawSprite3D reads it.
    sdfVector4_t centre;
    sdfBillBoardUv_t uv;
    sdfBillBoardQuad_t offset;
    u32 color;
    f32 rotation;
};
SDF_STATIC_ASSERT_SIZE(sdfSprite3D_t, 0x48);
SDF_STATIC_ASSERT_OFFSETOF(sdfSprite3D_t, uv, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfSprite3D_t, offset, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfSprite3D_t, color, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(sdfSprite3D_t, rotation, 0x44);

/// One batch of camera-facing quads, filled a quad at a time by dds3BillboardTagLink. The arrays
/// are one allocation, which is why sdfBillBoardMakeN takes four pointers into it.
/// @note SLUS-20911, filled at 0x0015B320
struct sdfBillBoardBatch_t {
    sdfVector4_t centre[15];
    u32 color[15];
    sdfBillBoardUv_t uv[15];
    sdfBillBoardQuad_t offset[15];
    s32 count;
};

SDF_STATIC_ASSERT_SIZE(sdfBillBoardBatch_t, 1024);

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
    u16 prim;
    u16 mask;

    /// Measured 0x8080F080 and 0x408080F0, so a packed color rather than a pointer.
    u32 colorWord;

    /// Null when the faces are implicit, which is every call measured on the field scenes.
    u32 faceIndex;

    /// A pointer, though the database types this slot and normal as inline sdfVector4_t arrays,
    /// which is what makes its struct 0x4C. Measured at 0x555FA0: vertex addresses this block
    /// + 0x30 and normal + 0x60, so the header is 0x30 and the arrays follow it.
    u32 vertex;
    u32 vertexRest[3];
    u32 normal;
    u32 normalRest[3];

    /// Read only when prim says the attribute is present.
    u32 uv;
    u32 uvRest;
    u32 multiUv;
    u32 multiUvRest;
    u32 vertexColor;

    /// Called instead of sdfGrAllocPacketBuff when it is set.
    u32 allocator;
    f32 zBias;
};
SDF_STATIC_ASSERT_SIZE(sdf3DPrimPolyParm_t, 0x4C);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, faceCount, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, pointCount, 0x02);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, prim, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, mask, 0x06);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, colorWord, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, faceIndex, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, vertex, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, normal, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, uv, 0x30);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, multiUv, 0x38);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, vertexColor, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, allocator, 0x44);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, zBias, 0x48);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, faceCount, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, pointCount, 0x02);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, prim, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, mask, 0x06);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, colorWord, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, faceIndex, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdf3DPrimPolyParm_t, vertex, 0x10);

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

constexpr u64 sdfFrameMask(u64 frame) { return (frame >> 32) & 0xFFFFFFFF; }

/// What sdfGrSetFrameMaskRGB writes: color masked, alpha writable.
constexpr u64 sdfFrameMaskRgb = 0x00FFFFFF;

/// sdfModelDrawEnvLinkOt links one env set per ordering table, 96 bytes apart, and puts the
/// terminator at aEnv[19].set[1], which is 96 * 4.
constexpr u32 sdfModelOrderingTables = 4;
constexpr u32 sdfModelDrawEnvStride = 96;

/// The four the engine sets up, in ordering-table order.
constexpr sdfGsState_t sdfGsStateForOt(u32 otIndex) {
    switch (otIndex) {
        case 2: return {0x48, 0x71801};  // additive
        case 3: return {0x42, 0x71801};  // subtractive
        default: return {0x44, 0x717FB};  // opaque and semi-transparent share a pair
    }
}

enum sdfGsRegisterAddr_t : u32 {
    sdfGsAddrAlpha1 = 0x42,
    sdfGsAddrAlpha2 = 0x43,
    sdfGsAddrFrame1 = 0x4C,
    sdfGsAddrFrame2 = 0x4D,
    sdfGsAddrTest1 = 0x47,
    sdfGsAddrTest2 = 0x48,
};

// ALPHA: Cv = (A - B) * C + D, over the source, the destination and zero.
constexpr u32 sdfAlphaA(u64 r) { return u32(r & 3); }
constexpr u32 sdfAlphaB(u64 r) { return u32((r >> 2) & 3); }
constexpr u32 sdfAlphaC(u64 r) { return u32((r >> 4) & 3); }
constexpr u32 sdfAlphaD(u64 r) { return u32((r >> 6) & 3); }
constexpr u32 sdfAlphaFix(u64 r) { return u32((r >> 32) & 0xFF); }
enum sdfAlphaTerm_t : u32 { sdfAlphaSource = 0, sdfAlphaDest = 1, sdfAlphaZero = 2 };
enum sdfAlphaCoef_t : u32 { sdfAlphaAs = 0, sdfAlphaAd = 1, sdfAlphaFixed = 2 };

// TEST: the alpha test, what a failing fragment still writes, and the depth test.
constexpr u32 sdfTestAte(u64 r) { return u32(r & 1); }
constexpr u32 sdfTestAtst(u64 r) { return u32((r >> 1) & 7); }
constexpr u32 sdfTestAref(u64 r) { return u32((r >> 4) & 0xFF); }
constexpr u32 sdfTestAfail(u64 r) { return u32((r >> 12) & 3); }
constexpr u32 sdfTestZte(u64 r) { return u32((r >> 16) & 1); }
constexpr u32 sdfTestZtst(u64 r) { return u32((r >> 17) & 3); }

enum sdfZtst_t : u32 { sdfZtstNever = 0, sdfZtstAlways = 1, sdfZtstGequal = 2, sdfZtstGreater = 3 };
enum sdfAfail_t : u32 { sdfAfailKeep = 0, sdfAfailFbOnly = 1, sdfAfailZbOnly = 2, sdfAfailRgbOnly = 3 };

// The GS registers the material packet carries, which sdfMatUpdatePacketBasic copies out of the
// texture handle rather than the material.
using sdfMatPacketPtr_t = guest_ptr<struct sdfMatPacket_t>;

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

// This build carries 64 bytes ahead of faceCount where the reconstructed HD header has only
// tmSave, so every field from matPacketPtr on sits 0x3C past its HD offset.
struct corePktCb_t {
    u8 header[0x40];
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
    u32 pointBuf0;
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
    u32 payload;
    u32 unk148;
    u8 unk152[20];
    f32 unk172;
    u32 unk176;
    sdfMatPacketPtr_t matPacket2;
    u8 unk184[136];
};
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, faceCount, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, pointCount, 0x42);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, attrBits, 0x44);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, packetBits, 0x48);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, orgAttrBits, 0x4C);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, drawFaceCount, 0x50);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, matPacketPtr, 0x54);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, color, 0x58);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, fdSoften, 0x5C);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, fdBias, 0x60);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, pointBuf0, 0x64);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, pointBuf1, 0x68);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, pointBuf2, 0x6C);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, pointCount0, 0x70);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, pointCount1, 0x74);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk120, 0x78);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk124, 0x7C);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, payload, 0x90);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk148, 0x94);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk152, 0x98);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk172, 0xAC);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk176, 0xB0);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, matPacket2, 0xB4);
SDF_STATIC_ASSERT_OFFSETOF(corePktCb_t, unk184, 0xB8);
static_assert(sizeof(corePktCb_t) == 0x140, "corePktCb_t is 0x140 in the database");

// TEX0's texture function and whether the texture carries alpha.
constexpr u32 sdfTex0Tfx(u64 r) { return u32((r >> 35) & 3); }
constexpr u32 sdfTex0Tcc(u64 r) { return u32((r >> 34) & 1); }
enum sdfTexFunction_t : u32 {
    sdfTexModulate = 0,
    sdfTexDecal = 1,
    sdfTexHighlight = 2,
    sdfTexHighlight2 = 3,
};

// TEX1's filters. LINEAR is 1; the mipmap minification modes are 2 and above.
constexpr u32 sdfTex1Mmag(u64 r) { return u32((r >> 5) & 1); }
constexpr u32 sdfTex1Mmin(u64 r) { return u32((r >> 6) & 7); }

// CLAMP's wrap mode per axis: 0 repeat, 1 clamp, 2 region repeat, 3 region clamp.
constexpr u32 sdfClampWms(u64 r) { return u32(r & 3); }
constexpr u32 sdfClampWmt(u64 r) { return u32((r >> 2) & 3); }
enum sdfWrapMode_t : u32 { sdfWrapRepeat = 0, sdfWrapClamp = 1 };

// A VIF code, which is how a mesh body names each of its vertex arrays.
enum sdfVifCommand_t : u32 {
    sdfVifFlushEnd = 0x10,
    sdfVifActMicro = 0x14,
    sdfVifCntMicro = 0x17,
    sdfVifUnpack = 0x60,
};

// One blend shape a morph controller drives, and how far towards it the model currently is.

// The parallel-light packet VU1 shades with: three directions, a base term and a color each, all
// in the GS's own 0..255 units.

/// sdfCorePacketGen's own threshold for a blend shape being worth applying.

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

struct sdfModelMorphParm_t {
    u32 targetId;
    f32 blend;
};
SDF_STATIC_ASSERT_SIZE(sdfModelMorphParm_t, 8);

// Two at a time, which is all sdfCorePacketGen applies.
using sdfModelMorphPtr_t = guest_ptr<struct sdfModelMorph_t>;

/// A morph controller mixes at most two targets, which is what bounds the shaped packet's chain.
constexpr u32 kMorphTargets = 2;

struct sdfModelMorph_t {
    sdfModelMorphParm_t target[kMorphTargets];
};
SDF_STATIC_ASSERT_SIZE(sdfModelMorph_t, 16);

struct sdfModelMorphList_t {
    sdfMemHandlePtr_t memHandle;
    s16 count;
    s16 capacity;
    s16 elemSize;
    s16 growStep;
    guest_ptr<struct sdfModelMorph_t> items;
};
SDF_STATIC_ASSERT_OFFSETOF(sdfModelMorphList_t, count, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelMorphList_t, items, 0x0C);

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
    sdfGifRegFog = 0xA,
    sdfGifRegAD = 0xE,
    sdfGifRegNop = 0xF,
};

constexpr u32 sdfGifRegAt(u64 regs, u32 index) { return u32((regs >> (4 * index)) & 0xF); }
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

// What sdf2DGetPrimPtr skips: the DMA tag and the GIF tag sdf2DMakePrimPacketHeader wrote.
constexpr u32 sdf2DPrimOffset = 32;

// Where sdfTexGetTex0Reg finds the register inside a texture's own upload packet.
constexpr u32 kTex0InPacket = 32;

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

    static constexpr GsTex0 fromReg(u64 reg) {
        return {u32(reg & 0x3FFF),        u32((reg >> 14) & 0x3F), u32((reg >> 20) & 0x3F),
                u32((reg >> 26) & 0xF),   u32((reg >> 30) & 0xF),  u32((reg >> 37) & 0x3FFF),
                u32((reg >> 51) & 0xF),   u32((reg >> 56) & 0x1F)};
    }

    constexpr bool isPaletted() const { return (psm & 0x13) == 0x13 || psm == 0x14 || psm == 0x24 || psm == 0x2C; }
    constexpr bool isFourBit() const { return psm == 0x14 || psm == 0x24 || psm == 0x2C; }
};

// The GIF tag every linked packet carries, at +16 past its DMA tag. Says what the packet draws,
// which is what makes sdfDrawTagLink a complete inventory of the frame rather than a call count.
using sdfGifTagWordsPtr_t = guest_ptr<struct sdfGifTagWords_t>;

struct sdfGifTagWords_t {
    u64 tag;
    u64 regs;
};
SDF_STATIC_ASSERT_SIZE(sdfGifTagWords_t, 16);

constexpr u32 sdfGifTagOffset = 16;

/// One A+D write: the value, then the GS register it names in the low byte.
struct sdfGifAdWrite_t {
    u64 value;
    u64 reg;
};
using sdfGifAdWritePtr_t = guest_ptr<sdfGifAdWrite_t>;

constexpr u32 sdfGifNloop(const sdfGifTagWords_t& t) { return u32(t.tag & 0x7FFF); }
constexpr u32 sdfGifPre(const sdfGifTagWords_t& t) { return u32((t.tag >> 46) & 1); }
constexpr u32 sdfGifPrim(const sdfGifTagWords_t& t) { return u32((t.tag >> 47) & 0x7FF); }
constexpr u32 sdfGifFlg(const sdfGifTagWords_t& t) { return u32((t.tag >> 58) & 3); }
constexpr u32 sdfGifNreg(const sdfGifTagWords_t& t) { return u32((t.tag >> 60) & 0xF); }

// PRIM's own fields, the ones that decide how a packet is read back.
constexpr u32 sdfPrimType(u32 prim) { return prim & 7; }
constexpr u32 sdfPrimTme(u32 prim) { return (prim >> 4) & 1; }
constexpr u32 sdfPrimAbe(u32 prim) { return (prim >> 6) & 1; }
// cppcheck-suppress unusedFunction  ; one of the PRIM field family, complete by design
constexpr u32 sdfPrimFst(u32 prim) { return (prim >> 8) & 1; }

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
    sdfVoidPtr_t packet;
    sdfVoidPtr_t packetCtx2;
    sdfVoidPtr_t clutData;
    u32 clutDataSize;
    u32 clutColor;
    u8 monoClutData;
    u8 pad[3];
    u32 hashname;
    // Not an EE pointer on a loaded texture: it reads 0x10000000, and pixelGsHandle is what names
    // where the pixels actually are. Only clutData stays in main memory.
    u32 image;
};
SDF_STATIC_ASSERT_SIZE(sdfTexHandle_t, 72);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexHandle_t, width, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexHandle_t, pixelFormat, 0x1A);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexHandle_t, texId, 0x20);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexHandle_t, clutData, 0x30);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexHandle_t, hashname, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexHandle_t, image, 0x44);

// The texture asset sdfTexSetEx is handed, and never writes to. It describes itself, so nothing has
// to be read back out of the handle the call returns.
using sdfTexFilePtr_t = guest_ptr<struct sdfTexFile_t>;

struct sdfTexFile_t {
    u8 version;
    u8 dataOffset;  // the low nibble is unused; the high nibble is where the data starts
    u16 userId;
    u32 fileSize;
    u32 magic;
    u32 reserved;
    u8 numOfCLUT;
    u8 CLUTFormat;
    s16 width;
    s16 height;
    u8 pixelFormat;
    u8 numOfMipmap;
    u16 mipmapParam;
    u8 flags;
    u8 texWrapMode;
    u32 texId;
    u32 clutId;
};
SDF_STATIC_ASSERT_SIZE(sdfTexFile_t, 0x24);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, version, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, dataOffset, 0x01);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, userId, 0x02);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, fileSize, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, magic, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, reserved, 0x0C);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, numOfCLUT, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, CLUTFormat, 0x11);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, width, 0x12);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, height, 0x14);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, pixelFormat, 0x16);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, numOfMipmap, 0x17);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, mipmapParam, 0x18);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, flags, 0x1A);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, texWrapMode, 0x1B);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, texId, 0x1C);
SDF_STATIC_ASSERT_OFFSETOF(sdfTexFile_t, clutId, 0x20);

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

// Width2 and Height2 are TEX0's TW and TH, the log2 of the two above: measured 0x200 against 9.
struct frGsTex_t {
    u32 Width;
    u32 WidthLog2;
    u32 Height;
    u32 HeightLog2;
    u32 Addr;
    u32 BufWidth;
};
SDF_STATIC_ASSERT_SIZE(frGsTex_t, 24);
