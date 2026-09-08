#pragma once

#include <cstddef>
#include <cstdint>

namespace sdfhook {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using f32 = float;
using usize = std::size_t;

// A packed GS color. Alpha is in the PS2's own range, where 0x80 is opaque.
struct Rgba {
    u8 r, g, b, a;

    /// 0xRRGGBBAA byte-reversed for the GS, which is what xfrSwapColor does.
    static Rgba fromColorWord(u32 value) {
        return Rgba{u8(value >> 24), u8(value >> 16), u8(value >> 8), u8(value)};
    }

    /// Already in GS order, red in the low byte.
    static Rgba fromPacked(u32 value) {
        return Rgba{u8(value & 0xFF), u8((value >> 8) & 0xFF), u8((value >> 16) & 0xFF),
                    u8((value >> 24) & 0xFF)};
    }
};

}  // namespace sdfhook
