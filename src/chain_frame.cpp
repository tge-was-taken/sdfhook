#include "chain_frame.h"

#include <algorithm>
#include <cstring>

#include "guest_ptr.h"
#include "libsdf.h"
#include "render/sprite.h"

namespace sdfhook {

namespace {

/// The GS registers a PACKED transfer names, of the ones a 2D draw is made of.
enum : u32 {
    gifRegPrim = 0x00,
    gifRegRgbaq = 0x01,
    gifRegUv = 0x03,
    gifRegXyzf2 = 0x04,
    gifRegXyz2 = 0x05,
    gifRegAd = 0x0E,
};

/// One PRIM's worth of vertices, closed when the primitive or the texture changes. A GS draw is a
/// run of vertices under one state, which is what a sprite has to be built from.
class PrimRun {
public:
    PrimRun(HookContext& h, Frame* frame, ChainFrameCounts* counts)
        : mHooks(h), mFrame(frame), mCounts(counts) {}

    ~PrimRun() { close(); }

    void setPrim(u32 aPrim) {
        if (mHavePrim && aPrim == mPrim) return;
        close();
        mPrim = aPrim;
        mHavePrim = true;
    }

    void setColor(const Rgba& aColor) { mColor = aColor; }
    void setUv(f32 aU, f32 aV) { mU = aU; mV = aV; }

    void addVertex(f32 aX, f32 aY) {
        if (!mHavePrim) return;
        if (mCount == std::size(mCorner)) close();
        mCorner[mCount++] = SpriteCorner{aX, aY, mU, mV, mColor};
        // A sprite is two opposite corners and a strip or fan is four, so either closes here.
        if (mCount == (sdfPrimType(mPrim) == sdfPrimSprite ? 2u : 4u)) close();
    }

private:
    void close() {
        if (!mCount) return;
        const u32 want = sdfPrimType(mPrim) == sdfPrimSprite ? 2u : 4u;
        if (mCount != want) {
            mCounts->verticesDropped += mCount;
            mCount = 0;
            return;
        }
        emit();
        mCount = 0;
    }

    void emit();

    HookContext& mHooks;
    Frame* mFrame;
    ChainFrameCounts* mCounts;
    SpriteCorner mCorner[4]{};
    u32 mCount = 0;
    u32 mPrim = 0;
    bool mHavePrim = false;
    Rgba mColor{0x80, 0x80, 0x80, 0x80};
    f32 mU = 0.0f;
    f32 mV = 0.0f;
};

void PrimRun::emit() {
    DrawSprite sprite{};
    if (sdfPrimType(mPrim) == sdfPrimSprite) {
        // Two opposite corners, flat shaded, so the other two are filled from them.
        sprite.corner[0] = mCorner[0];
        sprite.corner[1] = SpriteCorner{mCorner[1].x, mCorner[0].y, mCorner[1].u, mCorner[0].v,
                                        mCorner[1].color};
        sprite.corner[2] = mCorner[1];
        sprite.corner[3] = SpriteCorner{mCorner[0].x, mCorner[1].y, mCorner[0].u, mCorner[1].v,
                                        mCorner[1].color};
    } else {
        // A strip or fan arrives in the GS's own winding, which the third and fourth swap into.
        sprite.corner[0] = mCorner[0];
        sprite.corner[1] = mCorner[1];
        sprite.corner[2] = mCorner[3];
        sprite.corner[3] = mCorner[2];
    }
    for (const SpriteCorner& corner : sprite.corner) {
        mCounts->minX = std::min(mCounts->minX, corner.x);
        mCounts->maxX = std::max(mCounts->maxX, corner.x);
        mCounts->minY = std::min(mCounts->minY, corner.y);
        mCounts->maxY = std::max(mCounts->maxY, corner.y);
    }
    sprite.producer = "chainWalk";
    sprite.alpha = sdfPrimAbe(mPrim) ? mHooks.state.alpha : GsAlpha::Opaque;
    mFrame->addSprite(sprite);
    mCounts->sprites++;
}

/// PACKED spreads one register over a quadword, each field in its own 32-bit word.
void feedPacked(PrimRun& run, u32 aReg, const u8* aQword) {
    u32 word[4] = {};
    std::memcpy(word, aQword, sizeof(word));
    switch (aReg) {
        case gifRegPrim: run.setPrim(word[0] & 0x7FF); break;
        case gifRegRgbaq:
            run.setColor(Rgba{u8(word[0]), u8(word[1]), u8(word[2]), u8(word[3])});
            break;
        case gifRegUv:
            run.setUv(f32(word[0] & 0x3FFF) / 16.0f, f32(word[1] & 0x3FFF) / 16.0f);
            break;
        case gifRegXyz2:
        case gifRegXyzf2:
            run.addVertex(f32(word[0]) / 16.0f - kOffsetX, f32(word[1]) / 16.0f - kOffsetY);
            break;
        default: break;
    }
}

}  // namespace

ChainFrameCounts buildFrameFromChain(u32 aTagHead, HookContext& h, Frame* frame) {
    ChainFrameCounts counts;
    if (!frame) return counts;

    PrimRun run(h, frame, &counts);
    u32 stack[16] = {};
    u32 depth = 0;
    u32 at = aTagHead & 0x0FFFFFFFu;

    constexpr u32 kMaxChainTags = 65536;
    for (u32 steps = 0; at && steps < kMaxChainTags; steps++) {
        const u32* tag = u32Ptr_t{at}.slice(2);
        if (!tag) break;
        const u32 qwc = tag[0] & 0xFFFF;
        const u32 id = (tag[0] >> 28) & 7;
        const u32 addr = tag[1] & 0x0FFFFFFFu;
        const bool byRef = id == sdfDmaTagRef || id == sdfDmaTagRefs || id == sdfDmaTagRefe;
        const u32 data = byRef ? addr : at + sdfQwordBytes;
        counts.entries++;

        if (qwc) {
            const sdfGifTagWords_t words = sdfGifTagWordsPtr_t{data}.valueOr();
            const u32 nreg = sdfGifNreg(words);
            const u32 nloop = sdfGifNloop(words);
            const bool packed = sdfGifFlg(words) == 0 && nreg && nloop
                                && ((words.tag >> 16) & 0x3FFFFFFF) == 0
                                && u64(nloop) * nreg + 1 <= qwc;
            if (packed && nreg == 1 && (words.regs & 0xF) == gifRegAd) {
                counts.registerWrites++;
            } else if (packed) {
                if (sdfGifPre(words)) run.setPrim(sdfGifPrim(words));
                for (u32 loop = 0; loop < nloop; loop++) {
                    for (u32 reg = 0; reg < nreg; reg++) {
                        const u8* qword =
                            u8Ptr_t{data + sdfQwordBytes * (1 + loop * nreg + reg)}.slice(16);
                        if (!qword) break;
                        feedPacked(run, u32((words.regs >> (4 * reg)) & 0xF), qword);
                    }
                }
            } else {
                counts.opaque++;
            }
        }

        switch (id) {
            case sdfDmaTagRef:
            case sdfDmaTagRefs: at += sdfQwordBytes; break;
            case sdfDmaTagCnt: at += sdfQwordBytes * (1 + qwc); break;
            case sdfDmaTagNext: at = addr; break;
            case sdfDmaTagCall:
                if (depth == std::size(stack)) { at = 0; break; }
                stack[depth++] = at + sdfQwordBytes * (1 + qwc);
                at = addr;
                break;
            case sdfDmaTagRet: at = depth ? stack[--depth] : 0; break;
            default: at = 0; break;
        }
    }
    return counts;
}

}  // namespace sdfhook
