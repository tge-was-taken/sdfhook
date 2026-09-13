#pragma once

#include <bit>
#include <cmath>

#include "guest_ptr.h"
#include "libsdf.h"
#include "model.h"
#include "render/frame.h"
#include "types.h"
#include "unsupported.h"

// Shared by model.cpp and hooks_impl_model.cpp: the mesh conversion helpers and the VIF reader
// both halves need. Inline rather than duplicated, so one definition answers for both.
namespace sdfhook::modelread {

// The palette is indexed by a byte in the vertex, and no measured mesh comes near this.
constexpr u16 kMaxBones = 64;

// unpackTexture scales screenScale.z by this at 0x00CC; the shader has no such term and every
// savestate measures it zero.
inline void reportRootDepthBias(f32 bias) {
    if (bias != 0.0f) reportUnsupported("root depth bias", std::bit_cast<u32>(bias));
}

/// The one influence a body without weights has, so every mesh reaches the shader alike.
inline void setRigidInfluence(MeshVertex& vertex, const sdfVector3_t& position,
                       const sdfVector3_t* normal) {
    static constexpr sdfVector3_t kNoNormal{};
    const sdfVector3_t& n = normal ? *normal : kNoNormal;
    MeshInfluence& only = vertex.influence[0];
    only.x = position.x;
    only.y = position.y;
    only.z = position.z;
    only.nx = n.x;
    only.ny = n.y;
    only.nz = n.z;
    only.weight = 1.0f;
    only.bone = 0.0f;
}

constexpr f32 kZero3[3] = {};

// A packet's payload advances quadword by quadword, whatever a stream's own element size is.
inline u32 align16(u32 address) { return (address + 15) & ~15u; }

/// The engine keeps the left operand in VU0's VF28-31 and passes only the right, which is what
/// "Rev" names.
/// @fn MultMatrixRev
/// @note SLUS-20911 0x002B66B8
inline Matrix4 MultMatrixRev(const Matrix4& aLeft, const Matrix4& aMatrix) {
    Matrix4 out{};
    for (int i = 0; i < 4; i++) {
        for (int c = 0; c < 4; c++) {
            out.m[i][c] = aLeft.m[i][0] * aMatrix.m[0][c] + aLeft.m[i][1] * aMatrix.m[1][c] +
                          aLeft.m[i][2] * aMatrix.m[2][c] + aLeft.m[i][3] * aMatrix.m[3][c];
        }
    }
    return out;
}

// The six bits VCLIP sets. sdfModelCheckBounds ANDs them across the corners, so only agreement
// matters, not which plane each names.
inline u32 getClipCode(const f32 v[4]) {
    // VCLIP judges against |w|, so a corner behind the eye is not outside every plane at once.
    const f32 w = std::fabs(v[3]);
    u32 code = 0;
    for (int axis = 0; axis < 3; axis++) {
        if (v[axis] > w) code |= 1u << (2 * axis);
        if (v[axis] < -w) code |= 1u << (2 * axis + 1);
    }
    return code;
}

inline void transformPoint(const Matrix4& m, f32 x, f32 y, f32 z, f32 out[4]) {
    for (int c = 0; c < 4; c++) {
        out[c] = x * m.m[0][c] + y * m.m[1][c] + z * m.m[2][c] + m.m[3][c];
    }
}

// A list's item array is allocated apart from its header, so an item costs two loads.
template <typename List>
auto readListItem(guest_ptr<List> aList, u32 aIndex) {
    const auto items = aList ? aList->items : decltype(List::items){};
    using Item = std::remove_pointer_t<decltype(items.operator->())>;
    return items.addr ? guest_ptr<Item>{items.addr + u32(sizeof(Item)) * aIndex}.readOr() : Item{};
}

/// A VIF code, as the mesh body carries it: an immediate, a count, then the command.
struct VifCode {
    u32 immediate = 0;
    u32 count = 0;
    u32 command = 0;
    u32 next = 0;
};

inline bool readVifCode(u32 at, VifCode& out) {
    const u32Ptr_t at32{at};
    if (!at32) return false;
    const u32 word = *at32;
    out.immediate = word & 0xFFFF;
    out.count = (word >> 16) & 0xFF;
    out.command = (word >> 24) & 0xFF;
    out.next = at + 4;
    return true;
}

inline bool isUnpack(const VifCode& code) { return (code.command & sdfVifUnpack) == sdfVifUnpack; }

/// Bytes one unpacked element takes, from the command's own component count and element size.
inline u32 getUnpackWidth(const VifCode& code) {
    static const u32 kElementBytes[4] = {4, 2, 1, 0};
    return kElementBytes[code.command & 3] * (((code.command >> 2) & 3) + 1);
}

/// The UNPACK naming one vertex array, checked against the caller's width and count so a misread
/// offset is refused rather than decoded into plausible nonsense.
inline bool readArray(u32 at, u32 width, u32 count, u32& data, u32& next) {
    VifCode code;
    if (!readVifCode(at, code) || !isUnpack(code)) return false;
    if (getUnpackWidth(code) != width || code.count != count) return false;
    data = code.next;
    next = data + width * count;
    return true;
}

/// Vertices in the batch, and for a weighted mesh the nodes beyond the first blending into them.
inline bool readBatchHeader(u32 at, u16& first, u16& second, u32& next) {
    VifCode code;
    if (!readVifCode(at, code) || !isUnpack(code) || code.count != 1) return false;
    const u16* pair = u16Ptr_t{code.next}.slice(2);
    if (!pair) return false;
    first = pair[0];
    second = pair[1];
    next = code.next + 4;
    return true;
}

/// Past the microprogram kick and the flush that close a batch, to the next quadword.
inline u32 skipBatchEnd(u32 at) {
    for (int code = 0; code < 4; code++) {
        VifCode tag;
        if (!readVifCode(at, tag)) return 0;
        at = tag.next;
        if (tag.command == sdfVifFlushEnd) return align16(at);
    }
    return 0;
}

}  // namespace sdfhook::modelread
