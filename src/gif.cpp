#include "gif.h"

#include <algorithm>
#include <vector>

#include <cstring>

#include "unsupported.h"

namespace sdfhook {
namespace {



// Collects vertices into runs, one per PRIM and texture. A run closes when either changes, since
// together they are what a single draw call can carry.
class GeometryWalk {
public:
    explicit GeometryWalk(GifCounts* counts) : mCounts(counts) {}
    ~GeometryWalk() { close(); }

    void setPrim(u32 prim) {
        if (mHavePrim && prim == mPrim) return;
        close();
        mPrim = prim;
        mHavePrim = true;
    }

    // A texture bound mid-run starts a new run, since a draw call can only carry one.
    void setTex0(u64 value) {
        if (value == mTex0) return;
        close();
        mTex0 = value;
        const u32 tbp = sdfDecodeTex0(value).tbp;
        if (std::find(mTbps.begin(), mTbps.end(), tbp) == mTbps.end()) mTbps.push_back(tbp);
    }

    const std::vector<u32>& getTbps() const { return mTbps; }
    void clearTbps() { mTbps.clear(); }

    void rgbaq(const sdfGifRgbaq_t&) {}

private:
    std::vector<u32> mTbps;

public:
    void uv(const sdfGifUv_t&) {}

    void position(const sdfGifXyz2_t& xyz) {
        if (!mHavePrim) return;
        (void)xyz;
        mCounts->verts[sdfGetPrimType(mPrim)]++;
        mCount++;
    }

private:
    void close() {
        if (!mCount) return;
        mCounts->prims[sdfGetPrimType(mPrim)]++;
        if (sdfGetPrimTme(mPrim) && mTex0) {
            mCounts->texDraws[sdfDecodeTex0(mTex0).psm & 0x3F]++;
        } else {
            mCounts->untexturedDraws++;
        }
        mCount = 0;
    }

    GifCounts* mCounts;
    u32 mPrim = 0;
    bool mHavePrim = false;
    u32 mCount = 0;
    u64 mTex0 = 0;
};

// PACKED spreads one register over a whole quadword, each field in its own 32-bit word.
void feedPacked(GeometryWalk& walk, u32 reg, const u8* qword) {
    switch (reg) {
        case sdfGifRegRgbaq: {
            sdfGifRgbaq_t value{};
            std::memcpy(&value, qword, sizeof(value));
            walk.rgbaq(value);
            break;
        }
        case sdfGifRegUv: {
            sdfGifUv_t value{};
            std::memcpy(&value, qword, sizeof(value));
            walk.uv(value);
            break;
        }
        case sdfGifRegXyz2:
        case sdfGifRegXyzf2: {
            sdfGifXyz2_t value{};
            std::memcpy(&value, qword, sizeof(value));
            walk.position(value);
            break;
        }
        default:
            break;
    }
}

// GS register addresses for PRIM through XYZ2 match the GIF descriptor ids, so one decoder serves
// REGLIST and A+D both.
void feedRegister(GeometryWalk& walk, u32 reg, u64 word) {
    switch (reg) {
        case sdfGifRegPrim:
            walk.setPrim(u32(word & 0x7FF));
            break;
        case sdfGifRegRgbaq:
            walk.rgbaq(sdfGifRgbaq_t{u32(word & 0xFF), u32((word >> 8) & 0xFF),
                                     u32((word >> 16) & 0xFF), u32((word >> 24) & 0xFF)});
            break;
        case sdfGifRegUv:
            walk.uv(sdfGifUv_t{u32(word & 0x3FFF), u32((word >> 16) & 0x3FFF), {0, 0}});
            break;
        case sdfGifRegXyzf2:
            walk.position(sdfGifXyz2_t{u32(word & 0xFFFF), u32((word >> 16) & 0xFFFF),
                                       u32((word >> 32) & 0xFFFFFF), 0});
            break;
        case sdfGifRegXyz2:
            walk.position(sdfGifXyz2_t{u32(word & 0xFFFF), u32((word >> 16) & 0xFFFF),
                                       u32(word >> 32), 0});
            break;
        default:
            break;
    }
}

// An A+D quadword is a value and the register address to put it in. Geometry arrives this way as
// readily as state does: a full-screen quad sets PRIM, RGBAQ and two XYZ2 through it.
void feedAd(GeometryWalk& walk, const u8* qword) {
    u64 value = 0;
    u64 addr = 0;
    std::memcpy(&value, qword, sizeof(value));
    std::memcpy(&addr, qword + 8, sizeof(addr));
    const u32 reg = u32(addr & 0xFF);
    if (reg <= sdfGifRegXyz2) {
        feedRegister(walk, reg, value);
    } else if (reg == sdfGsRegTex0_1 || reg == sdfGsRegTex0_2) {
        walk.setTex0(value);
    }
}

// Quadwords a tag's own data occupies, which is how the walk finds the next tag.
constexpr u32 kNotAGifList = 0xFFFFFFFF;

u32 countPayloadQwords(u32 flg, u32 nloop, u32 nreg) {
    switch (flg) {
        case 0:
            return nloop * nreg;
        case 1:
            return (nloop * nreg + 1) / 2;
        case 2:
            return nloop;  // IMAGE, one quadword per loop
        default:
            return kNotAGifList;  // FLG 3 is reserved: these quadwords are not a GIF list
    }
}

}  // namespace

class GifParser::State {
public:
    explicit State(GifCounts* counts) : walk(counts), counts(counts) {}
    GeometryWalk walk;
    GifCounts* counts;
};

GifParser::GifParser(GifCounts* counts) : mState(std::make_unique<State>(counts)) {}

GifParser::~GifParser() = default;

const std::vector<u32>& GifParser::getParsedTbps() const { return mState->walk.getTbps(); }

void GifParser::parse(u32 packetAddr) {
    GeometryWalk& walk = mState->walk;
    walk.clearTbps();
    GifCounts& counts = *mState->counts;
    const sdfDmaTagPtr_t tagPtr{packetAddr};
    if (!tagPtr || !tagPtr->qwc) return;
    const sdfDmaTag_t tag = *tagPtr;

    const u32 dataAddr = sdfDmaIsRef(tag) ? tag.addr : packetAddr + sdfQwordBytes;
    const u8* data = u8Ptr_t{dataAddr}.slice(tag.qwc * sdfQwordBytes);
    if (!data) return;

    u32 left = tag.qwc;
    while (left) {
        sdfGifTag_t gif{};
        std::memcpy(&gif, data, sizeof(gif));
        data += sdfQwordBytes;
        left--;

        const u32 nloop = sdfGetGifNloop(gif);
        const u32 nreg = sdfGetGifNreg(gif) ? sdfGetGifNreg(gif) : 16;  // the field encodes 16 as zero
        const u32 flg = sdfGetGifFlg(gif);
        const u32 payload = countPayloadQwords(flg, nloop, nreg);
        if (!nloop || payload == kNotAGifList || payload > left) {
            counts.unparsed++;
            return;
        }
        if (sdfGetGifPre(gif)) walk.setPrim(sdfGetGifPrim(gif));

        if (flg == 0) {
            for (u32 i = 0; i < payload; i++) {
                const u32 reg = sdfGetGifRegAt(gif.regs, i % nreg);
                const u8* qword = data + i * sdfQwordBytes;
                if (reg == sdfGifRegAD) {
                    feedAd(walk, qword);
                } else {
                    feedPacked(walk, reg, qword);
                }
            }
        } else if (flg == 1) {
            for (u32 i = 0; i < nloop * nreg; i++) {
                u64 word = 0;
                std::memcpy(&word, data + i * 8, sizeof(word));
                feedRegister(walk, sdfGetGifRegAt(gif.regs, i % nreg), word);
            }
        }

        data += payload * sdfQwordBytes;
        left -= payload;
    }
}

bool GsRegisterReplay::write(u32 at, u32 count) {
    const sdfGrTagStdReg_t* writes = sdfGrTagStdRegPtr_t{at}.slice(count);
    if (!writes) return false;
    bool kicked = false;
    for (u32 i = 0; i < count; i++) {
        switch (u32(writes[i].reg & 0xFF)) {
            case sdfGsAddrXyzf2:
            case sdfGsAddrXyz2:
            case sdfGsAddrXyzf3:
            case sdfGsAddrXyz3: kicked = true; break;
            case sdfGsAddrAlpha1: mState.context[0].alpha = writes[i].data; break;
            case sdfGsAddrAlpha2: mState.context[1].alpha = writes[i].data; break;
            case sdfGsAddrTest1: mState.context[0].test = writes[i].data; break;
            case sdfGsAddrTest2: mState.context[1].test = writes[i].data; break;
            case sdfGsAddrFrame1: mState.context[0].frameMask = sdfGetFrameMask(writes[i].data); break;
            case sdfGsAddrFrame2: mState.context[1].frameMask = sdfGetFrameMask(writes[i].data); break;
            default: mUntracked.set(writes[i].reg & 0x7F); break;
        }
    }
    return kicked;
}

bool GsRegisterReplay::apply(u32 packetAddr) {
    const u32 pending = mPending;
    mPending = 0;
    mUntracked.reset();
    const sdfDmaTagPtr_t tag{packetAddr};
    if (!tag || !tag->qwc) return false;

    // A REF tag names its data elsewhere; anything else carries it straight after the tag.
    const u32 data = sdfDmaIsRef(*tag) ? tag->addr : packetAddr + sdfQwordBytes;
    const u32 qwc = tag->qwc;
    u32 at = std::min(pending, qwc);
    bool kicked = write(data, at);
    while (at < qwc) {
        // Anything but an A+D tag is geometry or VIF, and nothing after it is replayed.
        const sdfGifTagPtr_t gif{data + sdfQwordBytes * at};
        if (!gif || sdfGetGifFlg(*gif) != 0 || sdfGetGifNreg(*gif) != 1 ||
            sdfGetGifRegAt(gif->regs, 0) != sdfGifRegAD) {
            return false;
        }
        const u32 count = sdfGetGifNloop(*gif);
        const u32 here = std::min(count, qwc - at - 1);
        kicked |= write(data + sdfQwordBytes * (at + 1), here);
        mPending = count - here;
        at += 1 + here;
    }
    return !kicked;
}

bool isRegisterWrites(u32 packetAddr) {
    GsRegisterReplay replay;
    return replay.apply(packetAddr);
}

}  // namespace sdfhook
