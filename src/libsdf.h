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
using sdf3DFogPacketPtr_t = guest_ptr<sdf3DFogPacket_t>;
using sdfShapePtr_t = guest_ptr<sdfShape_t>;
using sdfDrawTagPtr_t = guest_ptr<sdfDrawTag_t>;
using sdfVoidPtr_t = guest_ptr<void>;

struct sdfVector2_t {
    f32 x, y;
};

struct sdfVector4_t {
    f32 x, y, z, w;
};

struct sdfMatrix4_t {
    sdfVector4_t row[4];
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
};
SDF_STATIC_ASSERT_SIZE(sdfModelNodeList_t, 12);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, memHandle, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, count, 0x04);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, capacity, 0x06);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, elemSize, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNodeList_t, growStep, 0x0A);

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
    sdfModelPacketHeader_t drawPacket[1];
    sdfVoidPtr_t tmPacket;
    f32 bounds;
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
SDF_STATIC_ASSERT_SIZE(sdfModelNode_t, 264);
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
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, tmPacket, 0x38);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, bounds, 0x3C);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, shape, 0x40);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, userData, 0x48);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, rotate, 0x58);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, pos, 0x68);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, scale, 0x78);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, localNoScaledTm, 0x88);
SDF_STATIC_ASSERT_OFFSETOF(sdfModelNode_t, tm, 0xC8);

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
    sdfVoidPtr_t texHandle;
    sdfVoidPtr_t multiTexHandle;
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

// Header only; the payload after it is vertex data shaped by geomFlags. Offsets are from
// sdfCorePacketGen and corePktInitCb.
struct sdfCorePacket_t {
    u8 type;
    u8 pad0;
    u16 pad1;
    u16 pad2;
    u16 matIndex;
    u32 morph;
    u32 pad3;
    u16 elemCount;
    u16 vertexCount;
    u16 attrFlags;
    u16 geomFlags;
};
SDF_STATIC_ASSERT_SIZE(sdfCorePacket_t, 24);
SDF_STATIC_ASSERT_OFFSETOF(sdfCorePacket_t, type, 0x00);
SDF_STATIC_ASSERT_OFFSETOF(sdfCorePacket_t, matIndex, 0x06);
SDF_STATIC_ASSERT_OFFSETOF(sdfCorePacket_t, morph, 0x08);
SDF_STATIC_ASSERT_OFFSETOF(sdfCorePacket_t, elemCount, 0x10);
SDF_STATIC_ASSERT_OFFSETOF(sdfCorePacket_t, vertexCount, 0x12);
SDF_STATIC_ASSERT_OFFSETOF(sdfCorePacket_t, attrFlags, 0x14);
SDF_STATIC_ASSERT_OFFSETOF(sdfCorePacket_t, geomFlags, 0x16);

// From the branches sdfCorePacketGen takes on them.
enum sdfCoreGeomFlag_t : u16 {
    sdfCoreGeomColour = 0x0020,
    sdfCoreGeomNormals = 0x0080,
    sdfCoreGeomUv = 0x0100,
    sdfCoreGeomSkinned = 0x0800,
    sdfCoreGeomMorph = 0x1000,
};

// These two bypass the flag dispatch and run VU0 directly.
enum sdfCorePacketType_t : u8 {
    sdfCorePacketMultiNode = 7,
    sdfCorePacketSingleNode = 8,
};

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

// The GIF tag every linked packet carries, at +16 past its DMA tag. Says what the packet draws,
// which is what makes sdfDrawTagLink a complete inventory of the frame rather than a call count.
struct sdfGifTagWords_t {
    u64 tag;
    u64 regs;
};
SDF_STATIC_ASSERT_SIZE(sdfGifTagWords_t, 16);

constexpr u32 sdfGifTagOffset = 16;

constexpr u32 sdfGifNloop(const sdfGifTagWords_t& t) { return u32(t.tag & 0x7FFF); }
constexpr u32 sdfGifPre(const sdfGifTagWords_t& t) { return u32((t.tag >> 46) & 1); }
constexpr u32 sdfGifPrim(const sdfGifTagWords_t& t) { return u32((t.tag >> 47) & 0x7FF); }
constexpr u32 sdfGifFlg(const sdfGifTagWords_t& t) { return u32((t.tag >> 58) & 3); }
constexpr u32 sdfGifNreg(const sdfGifTagWords_t& t) { return u32((t.tag >> 60) & 0xF); }

// PRIM's own fields, the ones that decide how a packet is read back.
constexpr u32 sdfPrimType(u32 prim) { return prim & 7; }
constexpr u32 sdfPrimTme(u32 prim) { return (prim >> 4) & 1; }
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
    u32 tfx;   // how the texel combines with the vertex colour
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
    guest_ptr<sdfTexHandle_t> prev;
    guest_ptr<sdfTexHandle_t> next;
    guest_ptr<sdfTexHandle_t> origin;
    s16 width;
    s16 height;
    sdfVoidPtr_t pixelGsHandle;
    sdfVoidPtr_t clutGsHandle;
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
struct FontMem_t {
    u32 id;
    s32 uv[4];  // the slot's position in the sheet, 12.4
    sdfVoidPtr_t user_data;
    guest_ptr<FontMem_t> prev;
    guest_ptr<FontMem_t> next;
};
SDF_STATIC_ASSERT_SIZE(FontMem_t, 32);
SDF_STATIC_ASSERT_OFFSETOF(FontMem_t, uv, 0x04);

struct GsFont_t {
    u16 font_no;
    u16 y_shift;
    u16 used_count;
    u16 pad;
    guest_ptr<FontMem_t> font_mem;
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

// What fclDrawSpriteCore draws: a texture, a UV rectangle packed two per word, and a colour.
struct fclSprite_t {
    guest_ptr<sdfTexHandle_t> TexHandle;
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
struct frGsTex_t {
    u32 Width;
    u32 Width2;
    u32 Height;
    u32 Height2;
    u32 Addr;
    u32 BufWidth;
};
SDF_STATIC_ASSERT_SIZE(frGsTex_t, 24);
