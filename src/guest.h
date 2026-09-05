#pragma once

#include <cstring>

#include "pcsx2hook/pcsx2hook.h"
#include "guest_ptr.h"
#include "types.h"

namespace sdfhook {

// A guest pointer read mid-frame is often null or stale, so every read reports rather than faults.
class Guest {
public:
    explicit Guest(pcsx2hook_ctx* runtime) : mRuntime(runtime) {}

    template <typename T>
    bool read(u32 address, T& out) const {
        const void* p = pcsx2hook_map_guest(mRuntime, address, sizeof(T));
        if (!p) return false;
        std::memcpy(&out, p, sizeof(T));
        return true;
    }

    template <typename T>
    T readOr(u32 address, T fallback = {}) const {
        T value{};
        return read(address, value) ? value : fallback;
    }

    u32 readU32(u32 address) const { return readOr<u32>(address); }

    template <typename T>
    bool read(guest_ptr<T> pointer, T& out) const {
        return read(pointer.addr, out);
    }

    template <typename T>
    T readOr(guest_ptr<T> pointer, T fallback = {}) const {
        T value{};
        return read(pointer, value) ? value : fallback;
    }

    // Null unless the whole span is mapped, so a bulk copy needs one check rather than one per
    // element. The guest keeps writing through it, so copy out before returning from the hook.
    const void* map(u32 address, u32 size) const {
        return pcsx2hook_map_guest(mRuntime, address, size);
    }

    bool isMapped(u32 address, u32 size = 4) const { return map(address, size) != nullptr; }

private:
    pcsx2hook_ctx* mRuntime;
};

}  // namespace sdfhook
