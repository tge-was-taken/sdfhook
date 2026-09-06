#include "guest_ptr.h"

namespace sdfhook {
namespace detail {

thread_local const u8* g_ramBase = nullptr;
thread_local u32 g_ramSize = 0;

}  // namespace detail

void installGuestRam(const void* base, u32 size) {
    detail::g_ramBase = static_cast<const u8*>(base);
    detail::g_ramSize = size;
}

}  // namespace sdfhook
