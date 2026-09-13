#pragma once

#include <bitset>
#include <vector>

#include "libsdf.h"
#include "types.h"

namespace sdfhook {

/// A vertex as the GS latched it at its kick. X and Y are 12.4 with XYOFFSET still in them.
struct GsVertex {
    u16 x = 0;
    u16 y = 0;
    u32 z = 0;

    /// Red in the low byte.
    u32 rgba = 0;
    f32 s = 0.0f;
    f32 t = 0.0f;
    f32 q = 1.0f;

    /// 10.4 texels.
    u16 u = 0;
    u16 v = 0;
    u8 fog = 0;
};

/// The registers a primitive reads, as they stood at its kick, taken from its own context.
struct GsDrawState {
    /// PRIM's type with the attributes in force, PRIM's or PRMODE's as PRMODECONT picks.
    u32 prim = 0;
    u64 tex0 = 0;
    u64 tex1 = 0;
    u64 clamp = 0;
    u64 alpha = 0;
    u64 test = 0;
    u64 fba = 0;
    u64 frame = 0;
    u64 zbuf = 0;
    u64 xyoffset = 0;
    u64 scissor = 0;
    u64 texa = 0;
    u64 fogcol = 0;
    u64 colclamp = 0;
    u64 dthe = 0;
    u64 pabe = 0;
};

struct GsPrimitive {
    GsDrawState state;
    GsVertex vertex[3];

    /// 1 for a point, 2 for a line or sprite, 3 for a triangle.
    u32 count = 0;
};

/// An IMAGE transfer, which replaces whatever texture sat in that part of GS memory.
struct GsUpload {
    u64 bitbltbuf = 0;
    u64 trxpos = 0;
    u64 trxreg = 0;
    u64 trxdir = 0;
    u32 qwords = 0;
};

/// Channel 1 as VIF1 and the GIF receive it. VIF codes route DIRECT data to the GIF, whose tags
/// write the GS registers and kick vertices into primitives. VU1's input is only measured: what
/// VU1 draws comes from the model walk.
class GsDecoder {
public:
    GsDecoder();

    /// One DMA transfer: the tag's two VIF words, then its quadwords at `dataAddr`.
    void feed(const sdfDmaTag_t& tag, u32 dataAddr);

    /// What the last transfer did.
    const std::vector<GsPrimitive>& getPrimitives() const { return mPrimitives; }
    const std::vector<GsUpload>& getUploads() const { return mUploads; }
    const std::vector<GsUpload>& getReadbacks() const { return mReadbacks; }
    const std::bitset<128>& getWritten() const { return mWritten; }

    /// GIF data and nothing but register writes: no vertex, no image, no VU1 input.
    bool isStateOnly() const;

    u64 getRegister(u32 address) const { return mReg[address]; }

    /// ALPHA, TEST and the FRAME mask of one drawing context.
    sdfGsState_t getContext(u32 context) const;

private:
    void feedVif(const std::vector<u32>& words);
    u32 getUnpackWords(u32 code) const;
    void feedGif(const u8* qword);
    void readGifTag(const u8* qword);
    void writePacked(u32 descriptor, const u8* qword);
    void writeRegister(u32 address, u64 value);
    void kick(bool draws);
    GsDrawState captureState() const;

    u64 mReg[0x80] = {};
    GsVertex mVertex;

    /// The Q an ST write leaves for the next RGBAQ, which PACKED mode takes from there.
    f32 mQ = 1.0f;
    GsVertex mQueue[3];
    u32 mQueued = 0;

    /// VIF state carried from one transfer into the next.
    u32 mDirectQwords = 0;
    u32 mSkipWords = 0;
    u32 mCycleCl = 1;
    u32 mCycleWl = 1;

    /// GIF state, which the next DIRECT continues: the frame mask's A+D tag comes in a transfer
    /// of its own and its writes in the one after.
    sdfGifTag_t mTag{};
    u32 mLoopsLeft = 0;
    u32 mRegIndex = 0;
    u32 mImageQwords = 0;

    std::vector<GsPrimitive> mPrimitives;
    std::vector<GsUpload> mUploads;
    std::vector<GsUpload> mReadbacks;
    std::bitset<128> mWritten;
    bool mKicked = false;
    bool mFedVu = false;
    bool mHadGif = false;
};

/// Whether a packet is A+D state and nothing else, judged on its own.
bool isRegisterWrites(u32 packetAddr);

}  // namespace sdfhook
