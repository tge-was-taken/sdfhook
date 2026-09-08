#include "chain_census.h"

#include <cstdio>
#include <string>
#include <unordered_map>

#include "gif.h"
#include "guest_ptr.h"
#include "json.h"
#include "libsdf.h"
#include "packet_type.h"
#include "log.h"
#include "unsupported.h"

namespace sdfhook {

namespace {

LogChannel log("census");

/// The whole payload of an entry nothing typed, so its layout can be read rather than guessed.
std::string dumpWords(u32 aData, u32 aQwc) {
    std::string out;
    const u32 words = aQwc * 4 < 64 ? aQwc * 4 : 64;
    for (u32 i = 0; i < words; i++) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%08X ", u32Ptr_t{aData + i * 4}.valueOr());
        out += buffer;
    }
    return out;
}

// VIF1's command byte, the field that says what a quadword of a stream is for.
constexpr u32 vifCommand(u32 word) { return (word >> 24) & 0x7F; }

enum : u32 {
    vifNop = 0x00,
    vifStCycl = 0x01,
    vifOffset = 0x02,
    vifBase = 0x03,
    vifItop = 0x04,
    vifStMod = 0x05,
    vifMskPath3 = 0x06,
    vifMark = 0x07,
    vifFlushE = 0x10,
    vifFlush = 0x11,
    vifFlushA = 0x13,
    vifMsCal = 0x14,
    vifMsCalF = 0x15,
    vifMsCnt = 0x17,
    vifStMask = 0x20,
    vifStRow = 0x30,
    vifStCol = 0x31,
    vifMpg = 0x4A,
    vifDirect = 0x50,
    vifDirectHl = 0x51,
};

/// A GIF tag has reserved bits, a bounded register count and a payload that has to fit the DMA
/// tag's own quadword count. Without the last of those a block of floats passes as a tag.
bool looksLikeGifTag(const sdfGifTagWords_t& words, u32 aQwc) {
    if (sdfGifNloop(words) == 0) return false;
    // Bits 60..63 are NREG, and zero means sixteen, which SDF never writes.
    const u32 nreg = sdfGifNreg(words);
    if (nreg == 0) return false;
    // Bits 16..45 are reserved and the hardware requires them clear. PRIM is 47..57, so a test
    // over those rejects every tag that sets one.
    if (((words.tag >> 16) & 0x3FFFFFFF) != 0) return false;
    // PACKED writes one quadword per register per loop; the transfer has to hold them.
    if (sdfGifFlg(words) == 0 && u64(sdfGifNloop(words)) * nreg + 1 > aQwc) return false;
    return true;
}

/// Whether a stream's first word decodes as a VIF code. Only the commands SDF actually writes are
/// accepted, so a float that happens to sit in the command byte is refused.
bool looksLikeVif(u32 word) {
    const u32 command = vifCommand(word);
    if ((command & 0x60) == 0x60) return true;  // UNPACK
    switch (command) {
        case vifNop:
        case vifStCycl:
        case vifOffset:
        case vifBase:
        case vifItop:
        case vifStMod:
        case vifMskPath3:
        case vifMark:
        case vifFlushE:
        case vifFlush:
        case vifFlushA:
        case vifMsCal:
        case vifMsCalF:
        case vifMsCnt:
        case vifStMask:
        case vifStRow:
        case vifStCol:
        case vifMpg:
        case vifDirect:
        case vifDirectHl: return true;
        default: return false;
    }
}

/// How many 32-bit words of data follow a VIF code, so the next code in the stream can be found.
u32 vifDataWords(u32 aCode) {
    const u32 command = vifCommand(aCode);
    const u32 num = (aCode >> 16) & 0xFF;
    const u32 immediate = aCode & 0xFFFF;
    if ((command & 0x60) == 0x60) {
        const u32 vl = command & 3;
        const u32 vn = (command >> 2) & 3;
        const u32 count = num ? num : 256;
        // V4-5 packs a whole vector into 16 bits; every other format is elements times width.
        const u32 bits = (vl == 3) ? count * 16 : count * (vn + 1) * (32u >> vl);
        return (bits + 31) / 32;
    }
    switch (command) {
        case vifMpg: return (num ? num : 256) * 2;
        case vifDirect:
        case vifDirectHl: return (immediate ? immediate : 65536) * 4;
        case vifStMask: return 1;
        case vifStRow:
        case vifStCol: return 4;
        default: return 0;
    }
}

/// Every VIF code in one transfer. MSCAL is what actually runs a microprogram, so counting them
/// says whether the chain delimits its own draws or only a hook can.
void scanVif(u32 aData, u32 aQwc, u32* aCalls, u32* aUnpacks, u32* aMicrocode) {
    const u32 words = aQwc * 4;
    for (u32 at = 0; at < words;) {
        const u32 code = u32Ptr_t{aData + at * 4}.valueOr();
        const u32 command = vifCommand(code);
        if (!looksLikeVif(code)) return;
        if (command == vifMsCal || command == vifMsCalF || command == vifMsCnt) (*aCalls)++;
        if ((command & 0x60) == 0x60) (*aUnpacks)++;
        if (command == vifMpg) (*aMicrocode)++;
        at += 1 + vifDataWords(code);
    }
}

/// What a payload says it is on its own. A GIF list is typed by its tag and a VIF transfer by its
/// codes; anything else needs the builder to have named it.
PacketType classifySelf(u32 aData, u32 aQwc) {
    if (!aQwc) return PacketType::Empty;

    const sdfGifTagWords_t words = sdfGifTagWordsPtr_t{aData}.valueOr();
    if (looksLikeGifTag(words, aQwc)) {
        // NREG 1 naming A+D (0xE) is a run of register writes and draws nothing itself.
        const bool registersOnly = sdfGifNreg(words) == 1 && (words.regs & 0xF) == 0xE;
        return registersOnly ? PacketType::GifRegisters : PacketType::GifPrimitive;
    }

    const u32 word = u32Ptr_t{aData}.valueOr();
    if (looksLikeVif(word)) {
        return (vifCommand(word) & 0x60) == 0x60 ? PacketType::VifData : PacketType::VifSetup;
    }
    return PacketType::Unknown;
}

/// The entry's type: what it says it is, or failing that what its builder said. A builder is
/// handed the packet, its data, or an interior offset of either, so all four are probed.
PacketType classify(u32 aPacket, u32 aData, u32 aQwc, const char* aProducer,
                    const std::unordered_map<u32, PacketType>& aTypes, TypedBy* aHow) {
    // What the engine said it built is the only thing that identifies a packet outright.
    *aHow = TypedBy::Builder;
    for (const u32 probe : {aPacket, aData, aPacket + 16, aData + 16}) {
        const auto found = aTypes.find(probe);
        if (found != aTypes.end()) return found->second;
    }

    // The producer and the shape rules only apply where the payload is opaque. A producer links
    // several kinds of packet, so letting it name a VIF stream would type the stream as whatever
    // else that producer also builds.
    const PacketType encoding = classifySelf(aData, aQwc);
    if (encoding == PacketType::Unknown) {
        *aHow = TypedBy::Producer;
        const PacketType byProducer = packetTypeForProducer(aProducer);
        if (byProducer != PacketType::Unknown) return byProducer;

        *aHow = TypedBy::Shape;
        const PacketType byShape = packetTypeForShape(aData, aQwc);
        if (byShape != PacketType::Unknown) return byShape;
    } else if (encoding != PacketType::Empty) {
        // Described, not identified: the encoding is known and the meaning is not.
        *aHow = TypedBy::Payload;
        return encoding;
    } else {
        *aHow = TypedBy::Payload;
        return encoding;
    }

    *aHow = TypedBy::Nothing;
    return PacketType::Unknown;
}

}  // namespace

/// One row per packet handed to a sdfDrawTagLink* entry point, which is the set that has to be
/// identified exhaustively: a packet whose type is unknown cannot be processed without running the
/// hardware that consumes it.
void censusLinked(const std::vector<LinkedPacket>& aLinked, SymbolCache& aSymbols, u32 aFrame,
                  const std::unordered_map<u32, PacketType>& aTypes) {
    static const char* const kLinkKind[] = {"packet", "ref", "call"};
    for (const LinkedPacket& packet : aLinked) {
        DmaTag tag{};
        readDmaTag(packet.addr, tag);
        const bool byRef = tag.id == sdfDmaTagRef || tag.id == sdfDmaTagRefs
                           || tag.id == sdfDmaTagRefe;
        const u32 data = byRef ? tag.addr : packet.addr + sdfQwordBytes;

        const char* producer = aSymbols.getFunction(packet.caller);
        TypedBy how = TypedBy::Nothing;
        const PacketType type = classify(packet.addr, data, tag.qwc, producer, aTypes, &how);
        u32 calls = 0, unpacks = 0, microcode = 0;
        scanVif(data, tag.qwc, &calls, &unpacks, &microcode);

        log.dump(JsonObject()
                     .addString("dump", "linked")
                     .addUint("frame", aFrame)
                     .addHexDigits("at", packet.addr, 8)
                     .addString("link", kLinkKind[u8(packet.kind)])
                     .addUint("dmaId", tag.id)
                     .addUint("qwc", tag.qwc)
                     .addString("type", packetTypeName(type))
                     .addString("how", typedByName(how))
                     .addString("by", producer)
                     .addUint("mscal", calls)
                     .take());
    }
}

void censusChain(u32 aTag, const std::vector<LinkedPacket>& aLinked, SymbolCache& aSymbols,
                 u32 aFrame, const std::unordered_map<u32, PacketType>& aTypes) {
    censusLinked(aLinked, aSymbols, aFrame, aTypes);
    // Who linked each packet, so an entry the walk finds can still be named by the producer that
    // built it. A chain-walk renderer would not have this; the census uses it to say what it loses.
    std::unordered_map<u32, u32> byAddr;
    for (const LinkedPacket& packet : aLinked) byAddr.emplace(packet.addr, packet.caller);

    u32 stack[16] = {};
    u32 depth = 0;
    u32 at = u32Ptr_t{aTag + 4}.valueOr() & 0x0FFFFFFFu;

    constexpr u32 kMaxChainTags = 65536;
    u32 steps = 0;
    for (; at && steps < kMaxChainTags; steps++) {
        const u32* tag = u32Ptr_t{at}.slice(2);
        if (!tag) break;
        const u32 qwc = tag[0] & 0xFFFF;
        const u32 id = (tag[0] >> 28) & 7;
        const u32 addr = tag[1] & 0x0FFFFFFFu;
        // A tag that names its data carries it elsewhere; a CNT tag's data follows the tag.
        const bool byRef = id == sdfDmaTagRef || id == sdfDmaTagRefs || id == sdfDmaTagRefe;
        const u32 data = byRef ? addr : at + sdfQwordBytes;

        const auto owner = byAddr.find(at);
        const char* producer = owner == byAddr.end() ? "-" : aSymbols.getFunction(owner->second);
        TypedBy how = TypedBy::Nothing;
        const PacketType type = classify(at, data, qwc, producer, aTypes, &how);
        u32 calls = 0, unpacks = 0, microcode = 0;
        scanVif(data, qwc, &calls, &unpacks, &microcode);

        const std::string words =
            type == PacketType::Unknown ? dumpWords(data, qwc) : std::string();
        log.dump(JsonObject()
                     .addString("dump", "census")
                     .addUint("frame", aFrame)
                     .addUint("step", steps)
                     .addHexDigits("at", at, 8)
                     .addUint("dmaId", id)
                     .addUint("qwc", qwc)
                     .addString("type", packetTypeName(type))
                     .addString("how", typedByName(how))
                     .addString("by", producer)
                     .addHexDigits("w0", u32Ptr_t{data}.valueOr(), 8)
                     .addHexDigits("w1", u32Ptr_t{data + 4}.valueOr(), 8)
                     .addHexDigits("w4", u32Ptr_t{data + 16}.valueOr(), 8)
                     .addUint("mscal", calls)
                     .addUint("unpacks", unpacks)
                     .addUint("mpg", microcode)
                     .addString("words", words.c_str())
                     .take());

        switch (id) {
            case sdfDmaTagRef:
            case sdfDmaTagRefs: at += sdfQwordBytes; break;
            case sdfDmaTagCnt: at += sdfQwordBytes * (1 + qwc); break;
            case sdfDmaTagNext: at = addr; break;
            case sdfDmaTagCall:
                if (depth == std::size(stack)) return;
                stack[depth++] = at + sdfQwordBytes * (1 + qwc);
                at = addr;
                break;
            case sdfDmaTagRet: at = depth ? stack[--depth] : 0; break;
            default: at = 0; break;  // REFE and END both stop the chain
        }
    }
    if (steps == kMaxChainTags) reportUnsupported("census chain tags", steps);
}

}  // namespace sdfhook
