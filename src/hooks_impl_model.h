#pragma once

#include "model.h"

namespace sdfhook {

/// What each hooked engine function does, over the parser's state. ModelParser decodes the mesh
/// formats these hand it; this is the other half, so a hook handler stays pure transport.
/// The lighting VU1 takes from a parallel light packet.
MeshLighting getLighting(const sdf3DLightParallelPacket_t& light);

/// The fog term a fog packet carries.
MeshFog getFog(const sdf3DFogPacket_t& fog);

class ModelHooks {
public:
    using NodeDraw = ModelParser::NodeDraw;

    explicit ModelHooks(ModelParser& parser) : mParser(parser) { parser.setHooks(this); }

    void sdfModelLinkOt(sdfModelRootNodePtr_t aRoot, Frame* frame);

    /// So the ramp's coordinate is read from the guest rather than fitted.
    /// @fn corePktSetFaceDivParm
    /// @note SLUS-20911 0x002B6A30
    void setFaceDivTerms(f32 soften, f32 bias);

    /// Reads the ramp corePktMakeFaceDivParm left on the caller's stack, keyed by the material
    /// packet the cb names.
    /// @fn corePktApplyFaceDivColor
    /// @note SLUS-20911 0x002B72D8
    void readFaceDivParm(corePktCbPtr_t aCb, sdfVector4Ptr_t aParm);

    /// Offsets are a rotated pair of view-space axes per corner, so a quad carries size and spin.
    /// @fn sdfBillBoardMakeN
    /// @note SLUS-20911 0x002BB0F0
    void sdfBillBoardMakeN(sdfBillBoardBatchPtr_t aBatch, u32 aNum, Frame* frame);

    // 0x2BAAF4 ors in IIP, FGE and ABE; 0x16A15C only IIP and ABE.
    static constexpr u16 kPrimPolyAttr = 0x68;
    static constexpr u16 kEffectPolyAttr = 0x48;
    void makePoly(sdf3DPrimPolyParmPtr_t aPoly, u16 baseAttr, Frame* frame);

    /// sdfBillBoardMakeN is handed the batch alone, and the table index lives on the node above.
    void readBillboardTables();

    /// Three world positions and three colors, no texture and no UVs.
    /// @fn fldDrawPoly3
    /// @note SLUS-20911 0x0012A5E0
    void fldDrawPoly3(const sdfVector3_t point[3], const u32 color[3], Frame* frame);

    /// A world-space VIF stream whose header sits past a color unpack, not at the target.
    /// @fn fldSdwDraw
    /// @note SLUS-20911 0x001193E8
    bool fldSdwDraw(u32 aPacket, u32 aEnd, const sdfGsState_t& aState, Frame* frame);

    /// @fn sdf3DPrimMakePoly
    /// @note SLUS-20911 0x002BAAB8
    void sdf3DPrimMakePoly(sdf3DPrimPolyParmPtr_t aPoly, Frame* frame) {
        makePoly(aPoly, kPrimPolyAttr, frame);
    }

    /// The game's copy for effects, which draws without fog.
    /// @fn effPkt3DPrimMakePoly
    /// @note SLUS-20911 0x0016A120
    void effPkt3DPrimMakePoly(sdf3DPrimPolyParmPtr_t aPoly, Frame* frame) {
        makePoly(aPoly, kEffectPolyAttr, frame);
    }

    void drawSprite3D(TexHandle aTexHandle, Sprite3DPtr_t aSprite, Frame* frame);

    /// @fn sdfMatMakePacket
    /// @note SLUS-20911 0x002B3558
    void readMaterialPacket(const sdfMat_t& material, DrawMesh& mesh,
                            Frame* frame);

    /// @fn corePktEmitMultiTexPass
    /// @note SLUS-20911 0x002B8C30
    void readMultiTexturePacket(const sdfMat_t& material, sdfMatPacketPtr_t packet, DrawMesh& mesh,
                                Frame* frame);

    void readLighting(sdf3DLightParallelPacketPtr_t aLight);

    void readFog(sdf3DFogPacketPtr_t aFog);

    /// @fn sdfModelCheckBounds
    /// @note SLUS-20911 0x002B1838
    bool sdfModelCheckBounds(sdfModelBoundsPtr_t aBounds, const Matrix4& clip) const;

    void walkChain(guest_ref<sdfModelRootNode_t> root, const NodeDraw& draw,
                   u32 chainAddr, Frame* frame);

    /// @fn sdfCorePacketGen
    /// @note SLUS-20911 0x002B9178
    void sdfCorePacketGen(guest_ref<sdfModelRootNode_t> root, const NodeDraw& draw,
                         sdfCorePacketPtr_t aPacket, Frame* frame);

    /// @fn corePktSkinVertFirst
    /// @note SLUS-20911 0x002B6BC0
    bool readWeightedBatches(u32 at, const sdfCorePacket_t& packet,
                             u32 nodesAddr, DrawMesh& mesh);

    void sdfModelTempPacketLinkOt(guest_ref<sdfModelRootNode_t> root, Frame* frame);

    /// Type 2: up to two blend targets, their streams chained behind the mesh's. Also unseen.
    /// @fn sdfModelLinkShapedDrawPacket
    /// @note SLUS-20911 0x002B0CB0
    void sdfModelLinkShapedDrawPacket(guest_ref<sdfModelRootNode_t> root, const NodeDraw& draw,
                                      u32 dataAddr, Frame* frame);

    /// @fn sdfModelSetNormalDrawPacket
    /// @note SLUS-20911 0x002B0AC0
    void sdfModelSetNormalDrawPacket(const NodeDraw& draw, u32 chainAddr, Frame* frame);
    static bool isBoneRef(u32 vifCode, u32 qwc);

    /// @fn corePktBuildFaceList
    /// @note SLUS-20911 0x002B7A80
    bool corePktBuildFaceList(u32 at, const sdfCorePacket_t& packet, DrawMesh& mesh);

private:
    ModelParser& mParser;
};

}  // namespace sdfhook
