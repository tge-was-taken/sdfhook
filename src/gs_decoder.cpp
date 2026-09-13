#include "gs_decoder.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include "guest_ptr.h"
#include "unsupported.h"

namespace sdfhook {

namespace {

enum VifCommand : u32 {
    kVifNop = 0x00,
    kVifStcycl = 0x01,
    kVifOffset = 0x02,
    kVifBase = 0x03,
    kVifItop = 0x04,
    kVifStmod = 0x05,
    kVifMskpath3 = 0x06,
    kVifMark = 0x07,
    kVifFlushe = 0x10,
    kVifFlush = 0x11,
    kVifFlusha = 0x13,
    kVifMscal = 0x14,
    kVifMscalf = 0x15,
    kVifMscnt = 0x17,
    kVifStmask = 0x20,
    kVifStrow = 0x30,
    kVifStcol = 0x31,
    kVifMpg = 0x4A,
    kVifDirect = 0x50,
    kVifDirecthl = 0x51,
};

/// UNPACK is the 0x60..0x7F block.
constexpr bool isUnpack(u32 command) { return (command & 0x60) == 0x60; }

enum GifMode : u32 { kGifPacked = 0, kGifReglist = 1, kGifImage = 2, kGifImageToo = 3 };

enum TrxDirection : u32 { kTrxHostToLocal = 0, kTrxLocalToHost = 1, kTrxLocalToLocal = 2 };

u32 readWord(const u8* qword, u32 index) {
    u32 word = 0;
    std::memcpy(&word, qword + 4 * index, sizeof(word));
    return word;
}

/// A REGLIST or PACKED descriptor's register, for the ones that name one outright.
bool getDescriptorAddress(u32 descriptor, u32* address) {
    switch (descriptor) {
        case sdfGifRegPrim: *address = sdfGsAddrPrim; return true;
        case sdfGifRegRgbaq: *address = sdfGsAddrRgbaq; return true;
        case sdfGifRegSt: *address = sdfGsAddrSt; return true;
        case sdfGifRegUv: *address = sdfGsAddrUv; return true;
        case sdfGifRegXyzf2: *address = sdfGsAddrXyzf2; return true;
        case sdfGifRegXyz2: *address = sdfGsAddrXyz2; return true;
        case sdfGifRegTex01: *address = sdfGsAddrTex01; return true;
        case sdfGifRegTex02: *address = sdfGsAddrTex02; return true;
        case sdfGifRegClamp1: *address = sdfGsAddrClamp1; return true;
        case sdfGifRegClamp2: *address = sdfGsAddrClamp2; return true;
        case sdfGifRegFog: *address = sdfGsAddrFog; return true;
        case sdfGifRegXyzf3: *address = sdfGsAddrXyzf3; return true;
        case sdfGifRegXyz3: *address = sdfGsAddrXyz3; return true;
        case sdfGifRegReserved:
        case sdfGifRegAD:
        case sdfGifRegNop: return false;
    }
    return false;
}

}  // namespace

GsDecoder::GsDecoder() {
    // What the engine's tables set, until the chain says otherwise.
    mReg[sdfGsAddrAlpha1] = mReg[sdfGsAddrAlpha2] = 0x44;
    mReg[sdfGsAddrTest1] = mReg[sdfGsAddrTest2] = 0x717FB;
    // The GS resets PRMODECONT to take attributes from PRIM.
    mReg[sdfGsAddrPrmodecont] = 1;
}

bool GsDecoder::isStateOnly() const {
    return mHadGif && !mKicked && mUploads.empty() && !mFedVu;
}

sdfGsState_t GsDecoder::getContext(u32 context) const {
    return {mReg[sdfGsAddrAlpha1 + context], mReg[sdfGsAddrTest1 + context],
            sdfGetFrameMask(mReg[sdfGsAddrFrame1 + context])};
}

void GsDecoder::feed(const sdfDmaTag_t& tag, u32 dataAddr) {
    mPrimitives.clear();
    mUploads.clear();
    mReadbacks.clear();
    mWritten.reset();
    mKicked = false;
    mFedVu = false;
    mHadGif = false;

    std::vector<u32> words;
    words.reserve(2 + usize(tag.qwc) * 4);
    words.push_back(tag.dat0);
    words.push_back(tag.dat1);
    if (tag.qwc) {
        const u32* data = u32Ptr_t{dataAddr}.slice(u32(tag.qwc) * 4);
        if (!data) {
            reportUnsupported("DMA transfer outside guest memory", tag.qwc);
            return;
        }
        words.insert(words.end(), data, data + usize(tag.qwc) * 4);
    }
    feedVif(words);
}

/// The tag's two words sit in the upper half of its quadword, so a DIRECT there starts its data
/// on the next quadword boundary.
void GsDecoder::feedVif(const std::vector<u32>& words) {
    const auto getPosition = [](usize index) { return u32(index + 2); };
    usize i = 0;
    // A command still taking data is fed on by the transfer's quadwords, not its tag.
    if (mDirectQwords || mSkipWords) {
        if (words[0] || words[1]) reportUnsupported("VIF code in a tag mid-command", words[1]);
        i = 2;
    }
    while (i < words.size()) {
        if (mDirectQwords) {
            if (getPosition(i) % 4) {
                reportUnsupported("DIRECT data off a quadword boundary", getPosition(i) % 4);
                mDirectQwords = 0;
                return;
            }
            const u32 available = u32((words.size() - i) / 4);
            const u32 take = std::min(available, mDirectQwords);
            for (u32 q = 0; q < take; q++) {
                feedGif(reinterpret_cast<const u8*>(&words[i + 4 * q]));
            }
            i += 4 * usize(take);
            mDirectQwords -= take;
            if (!take) return;
            continue;
        }
        if (mSkipWords) {
            const u32 take = u32(std::min<usize>(mSkipWords, words.size() - i));
            i += take;
            mSkipWords -= take;
            continue;
        }

        const u32 code = words[i++];
        const u32 command = (code >> 24) & 0x7F;
        const u32 immediate = code & 0xFFFF;
        const u32 num = (code >> 16) & 0xFF;
        switch (command) {
            case kVifNop:
            case kVifOffset:
            case kVifBase:
            case kVifItop:
            case kVifStmod:
            case kVifMskpath3:
            case kVifMark:
            case kVifFlushe:
            case kVifFlush:
            case kVifFlusha: break;
            case kVifStcycl:
                mCycleCl = immediate & 0xFF;
                mCycleWl = immediate >> 8;
                break;
            case kVifMscal:
            case kVifMscalf:
            case kVifMscnt: mFedVu = true; break;
            case kVifStmask: mSkipWords = 1; break;
            case kVifStrow:
            case kVifStcol: mSkipWords = 4; break;
            case kVifMpg:
                mFedVu = true;
                if (getPosition(i) % 2) i++;
                mSkipWords = (num ? num : 256) * 2;
                break;
            case kVifDirect:
            case kVifDirecthl:
                mDirectQwords = immediate ? immediate : 65536;
                i += (4 - getPosition(i) % 4) % 4;
                break;
            default:
                if (isUnpack(command)) {
                    mFedVu = true;
                    mSkipWords = getUnpackWords(code);
                    break;
                }
                reportUnsupported("VIF command", command);
                return;
        }
    }
}

/// The words an UNPACK reads from the stream. A write cycle longer than the read cycle fills
/// the gap from the row registers, so fewer vectors come from the stream.
u32 GsDecoder::getUnpackWords(u32 code) const {
    const u32 command = (code >> 24) & 0x7F;
    const u32 vn = (command >> 2) & 3;
    const u32 vl = command & 3;
    const u32 num = ((code >> 16) & 0xFF) ? (code >> 16) & 0xFF : 256;
    const u32 bits = (vn == 3 && vl == 3) ? 16 : (32u >> vl) * (vn + 1);
    const u32 cl = mCycleCl ? mCycleCl : 256;
    const u32 wl = mCycleWl ? mCycleWl : 256;
    const u32 vectors = wl > cl ? cl * (num / wl) + std::min(num % wl, cl) : num;
    return (vectors * bits + 31) / 32;
}

void GsDecoder::feedGif(const u8* qword) {
    mHadGif = true;
    if (mImageQwords) {
        mImageQwords--;
        return;
    }
    if (!mLoopsLeft) {
        readGifTag(qword);
        return;
    }
    const u32 nreg = sdfGetGifNreg(mTag) ? sdfGetGifNreg(mTag) : 16;
    const auto advance = [&] {
        if (++mRegIndex < nreg) return;
        mRegIndex = 0;
        mLoopsLeft--;
    };
    if (sdfGetGifFlg(mTag) == kGifPacked) {
        writePacked(sdfGetGifRegAt(mTag.regs, mRegIndex), qword);
        advance();
        return;
    }
    // REGLIST: two raw registers a quadword, the last one padding when the count is odd.
    for (u32 half = 0; half < 2 && mLoopsLeft; half++) {
        u64 value = 0;
        std::memcpy(&value, qword + 8 * half, sizeof(value));
        const u32 descriptor = sdfGetGifRegAt(mTag.regs, mRegIndex);
        u32 address = 0;
        if (getDescriptorAddress(descriptor, &address)) {
            writeRegister(address, value);
        } else if (descriptor != sdfGifRegNop) {
            reportUnsupported("REGLIST descriptor", descriptor);
        }
        advance();
    }
}

void GsDecoder::readGifTag(const u8* qword) {
    std::memcpy(&mTag, qword, sizeof(mTag));
    mRegIndex = 0;
    const u32 nloop = sdfGetGifNloop(mTag);
    const u32 mode = sdfGetGifFlg(mTag);
    if (mode == kGifImage || mode == kGifImageToo) {
        mImageQwords = nloop;
        mLoopsLeft = 0;
        if (nloop) {
            mUploads.push_back({mReg[sdfGsAddrBitbltbuf], mReg[sdfGsAddrTrxpos],
                                mReg[sdfGsAddrTrxreg], mReg[sdfGsAddrTrxdir], nloop});
        }
        return;
    }
    // PRE is honoured in PACKED mode only.
    if (mode == kGifPacked && sdfGetGifPre(mTag)) writeRegister(sdfGsAddrPrim, sdfGetGifPrim(mTag));
    mLoopsLeft = nloop;
}

void GsDecoder::writePacked(u32 descriptor, const u8* qword) {
    const u32 w0 = readWord(qword, 0);
    const u32 w1 = readWord(qword, 1);
    const u32 w2 = readWord(qword, 2);
    const u32 w3 = readWord(qword, 3);
    const bool adc = (w3 >> 15) & 1;
    switch (descriptor) {
        case sdfGifRegPrim: writeRegister(sdfGsAddrPrim, w0 & 0x7FF); break;
        case sdfGifRegRgbaq: {
            const u64 rgba = (w0 & 0xFF) | ((w1 & 0xFF) << 8) | ((w2 & 0xFF) << 16) | ((w3 & 0xFF) << 24);
            writeRegister(sdfGsAddrRgbaq, rgba | (u64(std::bit_cast<u32>(mQ)) << 32));
            break;
        }
        case sdfGifRegSt:
            mQ = std::bit_cast<f32>(w2);
            writeRegister(sdfGsAddrSt, w0 | (u64(w1) << 32));
            break;
        case sdfGifRegUv: writeRegister(sdfGsAddrUv, (w0 & 0x3FFF) | (u64(w1 & 0x3FFF) << 16)); break;
        case sdfGifRegXyzf2:
        case sdfGifRegXyzf3: {
            const u64 value = (w0 & 0xFFFF) | (u64(w1 & 0xFFFF) << 16) |
                              (u64((w2 >> 4) & 0xFFFFFF) << 32) | (u64((w3 >> 4) & 0xFF) << 56);
            const bool draws = descriptor == sdfGifRegXyzf2 && !adc;
            writeRegister(draws ? sdfGsAddrXyzf2 : sdfGsAddrXyzf3, value);
            break;
        }
        case sdfGifRegXyz2:
        case sdfGifRegXyz3: {
            const u64 value = (w0 & 0xFFFF) | (u64(w1 & 0xFFFF) << 16) | (u64(w2) << 32);
            const bool draws = descriptor == sdfGifRegXyz2 && !adc;
            writeRegister(draws ? sdfGsAddrXyz2 : sdfGsAddrXyz3, value);
            break;
        }
        case sdfGifRegTex01:
        case sdfGifRegTex02:
        case sdfGifRegClamp1:
        case sdfGifRegClamp2: {
            u32 address = 0;
            getDescriptorAddress(descriptor, &address);
            writeRegister(address, w0 | (u64(w1) << 32));
            break;
        }
        case sdfGifRegFog: writeRegister(sdfGsAddrFog, u64((w3 >> 4) & 0xFF) << 56); break;
        case sdfGifRegAD: writeRegister(w2 & 0x7F, w0 | (u64(w1) << 32)); break;
        case sdfGifRegNop: break;
        case sdfGifRegReserved: reportUnsupported("PACKED descriptor", descriptor); break;
    }
}

void GsDecoder::writeRegister(u32 address, u64 value) {
    switch (address) {
        case sdfGsAddrPrim:
            mQueued = 0;
            break;
        case sdfGsAddrRgbaq:
            mVertex.rgba = u32(value);
            mVertex.q = std::bit_cast<f32>(u32(value >> 32));
            break;
        case sdfGsAddrSt:
            mVertex.s = std::bit_cast<f32>(u32(value));
            mVertex.t = std::bit_cast<f32>(u32(value >> 32));
            break;
        case sdfGsAddrUv:
            mVertex.u = u16(value & 0x3FFF);
            mVertex.v = u16((value >> 16) & 0x3FFF);
            break;
        case sdfGsAddrFog: mVertex.fog = u8(value >> 56); break;
        case sdfGsAddrXyzf2:
        case sdfGsAddrXyzf3:
            mVertex.x = u16(value);
            mVertex.y = u16(value >> 16);
            mVertex.z = u32((value >> 32) & 0xFFFFFF);
            mVertex.fog = u8(value >> 56);
            mReg[address] = value;
            mWritten.set(address);
            kick(address == sdfGsAddrXyzf2);
            return;
        case sdfGsAddrXyz2:
        case sdfGsAddrXyz3:
            mVertex.x = u16(value);
            mVertex.y = u16(value >> 16);
            mVertex.z = u32(value >> 32);
            mReg[address] = value;
            mWritten.set(address);
            kick(address == sdfGsAddrXyz2);
            return;
        case sdfGsAddrTex21:
        case sdfGsAddrTex22: {
            // TEX2 rewrites TEX0's format and CLUT fields and leaves the rest.
            constexpr u64 kFields = (u64(0x3F) << 20) | ~((u64(1) << 37) - 1);
            u64& tex0 = mReg[sdfGsAddrTex01 + (address - sdfGsAddrTex21)];
            tex0 = (tex0 & ~kFields) | (value & kFields);
            break;
        }
        case sdfGsAddrTrxdir:
            if ((value & 3) == kTrxLocalToLocal) reportUnsupported("GS local-to-local copy", 0);
            if ((value & 3) == kTrxLocalToHost) {
                mReadbacks.push_back({mReg[sdfGsAddrBitbltbuf], mReg[sdfGsAddrTrxpos],
                                      mReg[sdfGsAddrTrxreg], value, 0});
            }
            break;
        case sdfGsAddrHwreg: reportUnsupported("GS image data through HWREG", 0); break;
        case sdfGsAddrTex01:
        case sdfGsAddrTex02:
        case sdfGsAddrClamp1:
        case sdfGsAddrClamp2:
        case sdfGsAddrTex11:
        case sdfGsAddrTex12:
        case sdfGsAddrXyoffset1:
        case sdfGsAddrXyoffset2:
        case sdfGsAddrPrmodecont:
        case sdfGsAddrPrmode:
        case sdfGsAddrTexclut:
        case sdfGsAddrScanmsk:
        case sdfGsAddrMiptbp11:
        case sdfGsAddrMiptbp12:
        case sdfGsAddrMiptbp21:
        case sdfGsAddrMiptbp22:
        case sdfGsAddrTexa:
        case sdfGsAddrFogcol:
        case sdfGsAddrTexflush:
        case sdfGsAddrScissor1:
        case sdfGsAddrScissor2:
        case sdfGsAddrAlpha1:
        case sdfGsAddrAlpha2:
        case sdfGsAddrDimx:
        case sdfGsAddrDthe:
        case sdfGsAddrColclamp:
        case sdfGsAddrTest1:
        case sdfGsAddrTest2:
        case sdfGsAddrPabe:
        case sdfGsAddrFba1:
        case sdfGsAddrFba2:
        case sdfGsAddrFrame1:
        case sdfGsAddrFrame2:
        case sdfGsAddrZbuf1:
        case sdfGsAddrZbuf2:
        case sdfGsAddrBitbltbuf:
        case sdfGsAddrTrxpos:
        case sdfGsAddrTrxreg:
        case sdfGsAddrSignal:
        case sdfGsAddrFinish:
        case sdfGsAddrLabel: break;
        default:
            reportUnsupported("GS register address", address);
            return;
    }
    mReg[address] = value;
    mWritten.set(address);
}

/// The vertex joins the queue; an XYZ2 completes a primitive, an XYZ3 only advances the queue.
void GsDecoder::kick(bool draws) {
    mKicked = true;
    const auto emit = [&](const GsVertex& a, const GsVertex* b, const GsVertex* c) {
        if (!draws) return;
        GsPrimitive primitive;
        primitive.state = captureState();
        primitive.vertex[0] = a;
        primitive.count = 1;
        if (b) primitive.vertex[primitive.count++] = *b;
        if (c) primitive.vertex[primitive.count++] = *c;
        mPrimitives.push_back(primitive);
    };
    const GsVertex& v = mVertex;
    switch (sdfGetPrimType(u32(mReg[sdfGsAddrPrim]))) {
        case sdfPrimPoint: emit(v, nullptr, nullptr); break;
        case sdfPrimLine:
        case sdfPrimSprite:
            if (mQueued == 0) {
                mQueue[mQueued++] = v;
                break;
            }
            emit(mQueue[0], &v, nullptr);
            mQueued = 0;
            break;
        case sdfPrimLineStrip:
            if (mQueued) emit(mQueue[0], &v, nullptr);
            mQueue[0] = v;
            mQueued = 1;
            break;
        case sdfPrimTriangle:
            if (mQueued < 2) {
                mQueue[mQueued++] = v;
                break;
            }
            emit(mQueue[0], &mQueue[1], &v);
            mQueued = 0;
            break;
        case sdfPrimTriStrip:
            if (mQueued < 2) {
                mQueue[mQueued++] = v;
                break;
            }
            emit(mQueue[0], &mQueue[1], &v);
            mQueue[0] = mQueue[1];
            mQueue[1] = v;
            break;
        case sdfPrimTriFan:
            if (mQueued < 2) {
                mQueue[mQueued++] = v;
                break;
            }
            emit(mQueue[0], &mQueue[1], &v);
            mQueue[1] = v;
            break;
        default: reportUnsupported("PRIM type", u32(mReg[sdfGsAddrPrim] & 7)); break;
    }
}

GsDrawState GsDecoder::captureState() const {
    const u64 attributes = (mReg[sdfGsAddrPrmodecont] & 1) ? mReg[sdfGsAddrPrim] : mReg[sdfGsAddrPrmode];
    const u32 context = u32((attributes >> 9) & 1);
    GsDrawState state;
    state.prim = u32((mReg[sdfGsAddrPrim] & 7) | (attributes & 0x7F8));
    state.tex0 = mReg[sdfGsAddrTex01 + context];
    state.tex1 = mReg[sdfGsAddrTex11 + context];
    state.clamp = mReg[sdfGsAddrClamp1 + context];
    state.alpha = mReg[sdfGsAddrAlpha1 + context];
    state.test = mReg[sdfGsAddrTest1 + context];
    state.fba = mReg[sdfGsAddrFba1 + context];
    state.frame = mReg[sdfGsAddrFrame1 + context];
    state.zbuf = mReg[sdfGsAddrZbuf1 + context];
    state.xyoffset = mReg[sdfGsAddrXyoffset1 + context];
    state.scissor = mReg[sdfGsAddrScissor1 + context];
    state.texa = mReg[sdfGsAddrTexa];
    state.fogcol = mReg[sdfGsAddrFogcol];
    state.colclamp = mReg[sdfGsAddrColclamp];
    state.dthe = mReg[sdfGsAddrDthe];
    state.pabe = mReg[sdfGsAddrPabe];
    return state;
}

bool isRegisterWrites(u32 packetAddr) {
    const sdfDmaTagPtr_t tag{packetAddr};
    if (!tag) return false;
    GsDecoder decoder;
    decoder.feed(*tag, sdfDmaIsRef(*tag) ? tag->addr & 0x0FFFFFFFu : packetAddr + sdfQwordBytes);
    return decoder.isStateOnly();
}

}  // namespace sdfhook
