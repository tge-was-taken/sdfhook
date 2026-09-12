#pragma once
#include <cstring>
#include <functional>
#include <type_traits>

#include "types.h"

namespace sdfhook {

/// The emulator's main RAM, for the thread that installs it. Per thread on purpose: the render
/// thread never installs it, so a guest_ptr that reaches it reads as unmapped rather than as a race.
void installGuestRam(const void* base, u32 size);

namespace detail {

extern thread_local const u8* g_ramBase;
extern thread_local u32 g_ramSize;

/// Null unless the whole span is inside RAM on this thread. Address zero is never mapped, so a null
/// guest pointer cannot be mistaken for the first byte of RAM.
inline void* mapGuest(u32 addr, u32 size) {
    const u32 offset = addr & 0x1FFFFFFu;
    if (!g_ramBase || !addr || u64(offset) + size > g_ramSize) return nullptr;
    return const_cast<u8*>(g_ramBase) + offset;
}

}  // namespace detail

/// A pointer as the guest stores it: four bytes whatever the host is, and dereferenceable. The host
/// pointer is worked out per access rather than kept, since the guest keeps writing through it.
template <typename T>
struct guest_ptr {
    u32 addr = 0;

    T* operator->() const { return static_cast<T*>(detail::mapGuest(addr, countBytes(1))); }

    /// Only where T is an object: guest_ptr<void> is an address the engine passes through.
    template <typename U = T>
    U& operator*() const {
        return *operator->();
    }

    /// A copy of its own, for the callers that keep one past the hook that read it.
    template <typename U = T>
    U readOr(U fallback = {}) const {
        const U* p = operator->();
        return p ? *p : fallback;
    }

    /// A run of `count` elements, or null unless all of it is mapped.
    T* slice(u32 count) const { return static_cast<T*>(detail::mapGuest(addr, countBytes(count))); }

    /// Whether it can be read, not merely whether it is non-zero.
    explicit operator bool() const { return operator->() != nullptr; }

    bool operator==(const guest_ptr& other) const { return addr == other.addr; }
    bool operator!=(const guest_ptr& other) const { return addr != other.addr; }

private:
    static constexpr u32 countBytes(u32 count) {
        if constexpr (std::is_void_v<T>) return count;
        else return u32(sizeof(T)) * count;
    }
};

static_assert(sizeof(guest_ptr<int>) == sizeof(u32));

// Only for data the engine really does keep untyped: raw quadword spans, packed pixels, a stack
// argument. Anything with a shape gets a struct and its own pointer alias instead.
using u8Ptr_t = guest_ptr<u8>;
using u16Ptr_t = guest_ptr<u16>;
using u32Ptr_t = guest_ptr<u32>;
using u64Ptr_t = guest_ptr<u64>;
using f32Ptr_t = guest_ptr<f32>;

/// A guest object the caller has already found mapped, so the callee neither re-checks nor copies.
/// It keeps the address, which is what the engine's own lists are keyed by.
template <typename T>
class guest_ref {
public:
    explicit guest_ref(guest_ptr<T> pointer) : mPointer(pointer) {}

    T* operator->() const { return mPointer.operator->(); }
    T& operator*() const { return *mPointer; }
    operator const T&() const { return *mPointer; }


private:
    guest_ptr<T> mPointer;
};

}  // namespace sdfhook

template <typename T>
struct std::hash<sdfhook::guest_ptr<T>> {
    sdfhook::usize operator()(sdfhook::guest_ptr<T> pointer) const {
        return std::hash<sdfhook::u32>()(pointer.addr);
    }
};
