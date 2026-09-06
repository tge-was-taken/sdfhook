#include "unsupported.h"
#include "log.h"

#include <map>
#include <mutex>
#include <set>
#include <string>

namespace sdfhook {
namespace {

LogChannel log("unsupported");

std::mutex g_mutex;
std::map<std::string, std::map<u64, u64>> g_seen;
u64 g_total = 0;

}  // namespace

void reportUnsupported(const char* what, u64 value) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_total++;
    u64& count = g_seen[what][value];
    if (count++) return;
    log.warn("{} 0x{:X}", what, value);
}

u64 getUnsupportedCount() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_total;
}

void printUnsupported() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& [what, values] : g_seen) {
        for (const auto& [value, count] : values) {
            log.warn("{} 0x{:X} x{}", what, value, count);
        }
    }
}

}  // namespace sdfhook
