#pragma once

#include <vector>

#include "libsdf.h"
#include "model.h"
#include "types.h"

// SDF's own functions reimplemented, `x` prefixed so ours and the guest's never read as the same
// symbol. Structures keep the guest's field names and offsets.

namespace sdfhook {

/// 85 tables, 32 bytes each, next-linked in address order.
/// @fn sdfGrOtInit
/// @note SLUS-20911 0x002ACDC0
constexpr u32 kXsdfOrderingTables = 0x55;

// The packet buffer and the prim inside it, opaque here as they are to the guest's own callers.
// libsdf.cpp holds the definitions.
struct xsdf2DPrim_t;
struct xsdfPacket_t;

/// Our shadow of sdfDrawTag_t, carrying an index into the frame's draws where the guest carries a
/// DMA chain. Offsets are the guest's, so a field compares against the database directly.
struct xsdfDrawTag_t {
    xsdfDrawTag_t* next = nullptr;  // +0x00
    bool head = false;              // +0x04, the guest's chain head: zero means nothing to draw
    f32 z = 0.0f;                   // +0x0C

    /// What sdfDrawTagLink attached, and what sdf2DLinkTexPacket bound to it.
    xsdfPacket_t* packet = nullptr;   // +0x04's chain, as one decoded draw
    xsdfPacket_t* tail = nullptr;     // +0x08
    u32 texPacket[2] = {0, 0};        // +0x18
    u32 texHandle = 0;
    void* lightPacket = nullptr;  // +0x10
    void* fogPacket = nullptr;    // +0x14

    /// Where sdfGrJoinDrawTag found the two tags disagreeing, which is where the engine splices.
    bool lightSeam = false;
    bool fogSeam = false;

    /// Registration order within its table, which is what the unsorted 81 emit by.
    u32 sequence = 0;

    /// Which draw of the frame this tag stands for, where the guest's chain holds quadwords.
    enum class Kind : u8 { Sprite, Mesh };
    Kind kind = Kind::Sprite;
    u32 draw = 0;

    /// A state packet registers a tag carrying no geometry, so only the ones the sink stamped
    /// name a draw.
    bool hasDraw = false;
};

using xsdfRegFunc_t = void (*)(struct xsdfGrOt_t* aOt, xsdfDrawTag_t* aTag);
using xsdfOtCallback_t = void (*)(struct xsdfGrOt_t* aOt);

/// Our shadow of sdfGrOt_t. regFunc at +0x10 is what every producer calls.
struct xsdfGrOt_t {
    xsdfDrawTag_t* tagHead = nullptr;    // +0x04
    xsdfDrawTag_t* tagTail = nullptr;    // +0x08
    xsdfDrawTag_t* noSortTag = nullptr;  // +0x0C
    xsdfGrOt_t* next = nullptr;          // +0x00
    xsdfRegFunc_t regFunc = nullptr;     // +0x10
    xsdfOtCallback_t callbackFunc = nullptr;  // +0x14
    xsdfDrawTag_t* data = nullptr;       // +0x18, sdfGrSortOtReg's AVL root
};

/// sdfDrawTagInit stores the integer 0xFFFF into a float field, so the sentinel is that bit
/// pattern rather than the value 65535.
constexpr u32 kXsdfNoSortZ = 0xFFFF;

extern u32 xrsetFrameMaskOtOffset;
extern u8 xdds3FadeState;
extern u8 xdds3FadeColorB;
extern u8 xdds3FadeColorG;
extern u32 xfadergba;
extern u8 xdds3FadePacket[352];
extern u32 xfrFontEntryTable[1];

/// Two draw environments, as dds3InitDrawEnvSub builds them.
extern dds3DrawEnv_t xdds3DrawEnv[2];

void xsdf2DMakeStdPacket(xsdfPacket_t* aPacket, u32 aReg, u64 aValue);
[[nodiscard]] u32 xsdf2DGetTexPacketSize();
void xsdf2DMakeTexPacket(xsdfPacket_t* aPacket, u32 aTexHandle);
void xsdfGrSetFrameMaskRGB(sdfGrStdDrawEnv_t* aEnv, xsdfPacket_t* aPacket);
void xsdfGrSetFrameMaskReset(sdfGrStdDrawEnv_t* aEnv, xsdfPacket_t* aPacket);
void xsdfDrawTagLinkRefGif(xsdfDrawTag_t* aTag, xsdfPacket_t* aPacket);
void xrsetFrameMaskRgbOn();
void xrsetFrameMaskRgbOff();
void xrsetOtOfs(u32 aOtOfs);
void xrsetAlpha(u32 aAlpha);
void xrsetTest(u32 aAte, u32 aAtst, u32 aAref, u32 aAfail, u32 aDate, u32 aDatm,
               u32 aZtst);
void xsdfGrInitExPacket(xsdfGrOt_t* aOt);
void xsdfGrOtFuncLinkToHead(xsdfGrOt_t* aOt);
void xsdfGrOtNormalCallback(xsdfGrOt_t* aOt);
void xsdfGrOtInit();
void xsdfGrOtReset();

/// A tag from the frame's pool, live until xsdfGrOtInit resets it.
[[nodiscard]] xsdfDrawTag_t* xsdfAllocDrawTag();

/// The global 0..84 index a table pointer names, or ~0 when it is outside the array.
[[nodiscard]] u32 xsdfGrOtIndexOf(u32 aGuestAddr);

/// Walks the 85 tables in index order and concatenates their tag lists, which is the frame's
/// draw order.
/// @fn sdfGrOtClose
/// @note SLUS-20911 0x002AD010
void xsdfGrOtClose(std::vector<xsdfDrawTag_t*>& aOut);
void xsdfGrJoinDrawTag(xsdfDrawTag_t* aFore, xsdfDrawTag_t* aAft);
void xsdfGrOtNormalReg(xsdfGrOt_t* aOt, xsdfDrawTag_t* aTag);
void xsdfGrSortOtReg(xsdfGrOt_t* aOt, xsdfDrawTag_t* aTag);
void xsdfDrawTagInit(xsdfDrawTag_t* aTag);

[[nodiscard]] xsdfPacket_t* xsdfGrAllocPacketBuff(u32 aSize);

/// The allocator a sdfPrimLink* caller may pass in place of sdfGrAllocPacketBuff.
using xsdfAllocFn = xsdfPacket_t* (*)(u32);
[[nodiscard]] u32 xsdf2DGetPrimPacketSize(u32 aCount);
void xsdf2DMakePrimPacketHeader(xsdfPacket_t* aPacket, u32 aPrim);
[[nodiscard]] xsdf2DPrim_t* xsdf2DGetPrimPtr(xsdfPacket_t* aPacket);
[[nodiscard]] xsdfPacket_t* xsdf2DAllocSprite();
void xsdfPrimSetFt4Elem(xsdf2DPrim_t* aPrim, int aCorner, f32 aX, f32 aY, f32 aU, f32 aV,
                        Rgba aColor);
void xsdfPrimSetG4Elem(xsdf2DPrim_t* aPrim, int aCorner, f32 aX, f32 aY, Rgba aColor);
void xsdfTexSetFilter(sdfTexHandlePtr_t aHandle, u32 aFilter);
void xsdf2DLinkTexPacket(xsdfDrawTag_t* aTag, u32 aTexHandle);
void xsdfDrawTagLink(xsdfDrawTag_t* aTag, xsdfPacket_t* aPacket);
void xfclDrawPanelSprite2(xsdf2DPrim_t* aPrim, f32 aX, f32 aY, f32 aW, f32 aH, f32 aU0, f32 aV0,
                          f32 aU1, f32 aV1);
[[nodiscard]] xsdfDrawTag_t* xsdfPrimAllocDrawTag();
void xsdfGrLinkDispFrameTex(xsdfDrawTag_t* aTag, sdfGrStdDrawEnv_t* aEnv);
void xfldDrawFrameSprite(u8 aAlpha);
[[nodiscard]] u32 xdds3GetFrameID();
[[nodiscard]] xsdfDrawTag_t* xdds3FontReqAllocDrawTag();

/// The table an index names, clamped so a bad index reports rather than running off the array.
[[nodiscard]] xsdfGrOt_t* xsdfGrOtGet(u32 aIndex);

/// Registers the tag into the table `aIndex` names, through that table's own regFunc.
void xsdfGrOtRegister(u32 aIndex, xsdfDrawTag_t* aTag);

extern xsdfGrOt_t xsdfGrOtTable[kXsdfOrderingTables];

/// A full 32-bit byte reverse.
/// @fn frSwapColor
/// @note SLUS-20911 0x001C6338
[[nodiscard]] u32 xfrSwapColor(u32 aColor);

/// Splits a packed color into four words, most significant byte first.
/// @fn cmpExpandColor
/// @note SLUS-20911 0x002399A0
void xcmpExpandColor(u32 (&aOut)[4], u32 aColor);

/// One corner as the sdfPrimLink* calls carry it: 12.4 screen, 12.4 UV, a packed color.
struct PrimVertex {
    s32 x, y;
    s32 u, v;
    u32 color;
};

void xDrwSprite2D(f32 aX, f32 aY, f32 aW, f32 aH, f32 aU, f32 aV, f32 aUw, f32 aVh, Rgba aRgba,
                 u32 aTexHandle);
void xaDrawBoxG(s32 aX, s32 aY, s32 aW, s32 aH, const u32 (&aRgba)[16]);
void xaDrawBoxF(s32 aX, s32 aY, s32 aW, s32 aH, u32 aR, u32 aG, u32 aB, u32 aA);
void xaDrawBoxF_Direct(s32 aX0, s32 aY0, s32 aX1, s32 aY1, u32 aR, u32 aG, u32 aB, u32 aA);
void xfrCreateDrawFontPacket(s32 aX, s32 aY, s32 aWidth, s32 aHeight, u32 aCol, const f32 (&aUv)[4],
                            frGsTex_t* aGsTex);
void xsdfPrimLinkFt4(u32 aColor, u32 aPrim, const PrimVertex (&aVertex)[4],
                     xsdfAllocFn aAllocator);
void xsdfPrimLinkG4(u32 aPrim, const PrimVertex (&aVertex)[4], xsdfAllocFn aAllocator);
void xfclDrawSpriteCore(s32 aX, s32 aY, s32 aW, s32 aH, u32 aCol, fclSprite_PTR_t aSprite,
                       u32 aFlipFlag, u32 aAlphaFlag);
void xdds3DrawFade(u8 aR, u8 aG, u8 aB, u8 aState);
void xcmpDrawBoxG(s32 aX, s32 aY, s32 aW, s32 aH, u32 aPCol);
void xcmpDrawTriG(u32 aPX, u32 aPY, u32 aPCol);

class ModelParser;

// Guest format readers and the matrix helpers, shared with the decoders in model.cpp.
constexpr u16 kMaxBones = 64;

/// A model's packet chain is short; past this it is a cycle, not a chain.
constexpr u32 kMaxChainPackets = 4096;
constexpr f32 kZero3[3] = {};

struct VifCode {
    u32 command = 0;
    u32 count = 0;
    u32 immediate = 0;
    u32 next = 0;
};

void reportRootDepthBias(f32 bias);
void setRigidInfluence(MeshVertex& vertex, const sdfVector3_t& position,
                       const sdfVector3_t* normal);
u32 align16(u32 address);

/// @fn MultMatrixRev
/// @note SLUS-20911 0x002C4188
[[nodiscard]] Matrix4 xMultMatrixRev(const Matrix4& aLeft, const Matrix4& aMatrix);

u32 clipCode(const f32 v[4]);
void transformPoint(const Matrix4& m, f32 x, f32 y, f32 z, f32 out[4]);
u32 readListItem(u32 listAddr, u32 index);
bool readVifCode(u32 at, VifCode& out);
bool isUnpack(const VifCode& code);
u32 unpackWidth(const VifCode& code);
bool readArray(u32 at, u32 width, u32 count, u32& data, u32& next);
bool readBatchHeader(u32 at, u16& first, u16& second, u32& next);
u32 skipBatchEnd(u32 at);


/// The engine's own model globals, read and written where the guest does. Global for the same
/// reason the packet buffer is: a reimplementation takes no parameter for them.
struct XsdfModel {
    /// The four global table indices the root's ot group names, so a mesh's 0..3 becomes the
    /// 0..84 the shadow queue registers into.
    u32 otTable[4] = {0, 0, 0, 0};

    // Resolved once from the symbol map and named for it.
    Matrix4Ptr_t sdf3DScreenTm;
    guest_ptr<u32> sdf3DSpecularTex;
    guest_ptr<sdfVector4_t> sdf3DScreenParam;
    f32 projectionScale[2] = {1.0f, 1.0f};
    u32Ptr_t sdfFrameId;
    u32Ptr_t dds3FrameID;
    f32Ptr_t sdfModelShapeWeightEpsilon;
    u32 frFontClutTable = 0;
    u32Ptr_t sdfGrPacketBuffPtr;
    u32Ptr_t corePktCurrLight;
    f32Ptr_t sdfFaceDivBias;
    f32Ptr_t sdfFaceDivSoften;
    u8Ptr_t corePktSoftenBias;
    u64Ptr_t sdf3DRefMapParam;
    sdf3DLightDefaultPtr_t sdf3DLightDefault;
    sdf3DFogDefaultPtr_t sdf3DFogDefault;

    // $f12 and $f13 into sdfCorePacketGen. The walk runs ahead of generation, so a frame uses
    // the previous frame's pair and only the first falls back to the fitted config values.
    f32 faceDivSoftenTerm = 0.0f;
    f32 faceDivBiasTerm = 0.0f;
    bool faceDivTermsSeen = false;
    bool faceDivParmSeen = false;
    /// Per material packet, so the second pass interpolates the engine's endpoints, not ours.
    std::unordered_map<u32, MeshRamp> faceDivRamps;

    u32 nodeListAddr = 0;
    MeshLighting lighting{};
    MeshFog fog{};
    sdfGsState_t gsState{};

    /// sdfBillBoardMakeN gets the geometry alone, so the handle comes from the call that fills.
    sdfTexHandlePtr_t billboardTexture;
    std::unordered_map<u32, u32> billboardOt;
    u32 linkIndex = 0;
    u32 currentChain = 0;
    u32 currentLink = 0;

    /// So a chain address names its material rather than being guessed from its size.
    std::unordered_map<u32, u32> matByPacket;
    Matrix4 screenTm{};
};

extern XsdfModel gModel;

/// The format decoders, which are ours rather than a reimplementation of anything.
extern ModelParser* gConvert;

void xsdfTexSetEx(sdfTexFilePtr_t aTex, sdfTexHandlePtr_t aHandle, Frame* frame);
void xsdfTexFree(sdfTexHandlePtr_t aHandle);
void xsdfTexDup(sdfTexHandlePtr_t aSource, sdfTexHandlePtr_t aCopy);
void xsdfGsMemAllocFromHead(sdfGsMemSlicePtr_t aSlice);
void xsdfModelSetMatRefPacket(u32 aChainAddr);
void xsdfGsAllocFrameBuff(sdfGsMemSlicePtr_t aSlice);
void xfrCopyFontClutToVram(u32 aData);

void xcorePktSetFaceDivParm(f32 soften, f32 bias);
void xcorePktApplyFaceDivColor(corePktCbPtr_t aCb, sdfVector4Ptr_t aParm);
void readLighting(sdf3DLightParallelPacketPtr_t aLight);
void readFog(sdf3DFogPacketPtr_t aFog);
void xsdfMatMakePacket(const sdfMat_t& material, DrawMesh& mesh,
                                   Frame* frame);
void xcorePktSetConstColor(DrawMesh& mesh, u32 aColor);
void xcorePktEmitMultiTexPass(const sdfMat_t& material, sdfMatPacketPtr_t packet,
                                         DrawMesh& mesh, Frame* frame);
bool readScreenTm();
[[nodiscard]] bool xsdfModelCheckBounds(sdfModelBoundsPtr_t aBounds, const Matrix4& clip);
void xsdfModelLinkOt(sdfModelOtPtr_t aOt, sdfModelRootNodePtr_t aRoot, Frame* frame);
void xsdfCorePacketGen(guest_ref<sdfModelRootNode_t> root, const NodeDraw& draw,
                                  sdfCorePacketPtr_t aPacket, Frame* frame);
void xsdfModelTempPacketLinkOt(guest_ref<sdfModelRootNode_t> root, Frame* frame);
void xsdfModelLinkShapedDrawPacket(guest_ref<sdfModelRootNode_t> root,
                                               const NodeDraw& draw, u32 dataAddr, Frame* frame);
[[nodiscard]] Matrix4 xSetRotateMatrixZ(f32 aAngle);
void xdrawSprite3D(TexHandle aTexHandle, sdfSprite3DPtr_t aSprite, Frame* frame);
void xsdf3DPrimMakePoly(sdf3DPrimPolyParmPtr_t aPoly, Frame* frame);
void xfldDrawPoly3(const sdfVector3_t point[3], const u32 color[3], Frame* frame);
bool readShadowStream(u32 aPacket, u32 aEnd, const sdfGsState_t& aState,
                             Frame* frame);
void xsdfBillBoardMakeN(sdfBillBoardBatchPtr_t aBatch, u32 aNum, Frame* frame);
void xsdfModelSetNormalDrawPacket(const NodeDraw& draw, u32 chainAddr, Frame* frame);
[[nodiscard]] bool xcorePktBuildFaceList(u32 at, const corePktCb_t& cb, DrawMesh& mesh);
bool xcorePktSkinVertFirst(u32 at, const sdfCorePacket_t& packet,
                                    u32 nodesAddr, DrawMesh& mesh);


}  // namespace sdfhook
