#pragma once

#include <functional>

#include "types.h"

namespace sdfhook {

// Four bytes whatever the host is, and not dereferenceable: reading one means reading emulator
// memory, which can fail. Guest::read takes it.
template <typename T>
struct guest_ptr {
    u32 addr = 0;

    explicit operator bool() const { return addr != 0; }
    bool operator==(const guest_ptr& other) const { return addr == other.addr; }
    bool operator!=(const guest_ptr& other) const { return addr != other.addr; }
};

static_assert(sizeof(guest_ptr<int>) == sizeof(u32));

}  // namespace sdfhook

template <typename T>
struct std::hash<sdfhook::guest_ptr<T>> {
    sdfhook::usize operator()(sdfhook::guest_ptr<T> pointer) const {
        return std::hash<sdfhook::u32>()(pointer.addr);
    }
};
