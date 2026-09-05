#include "unsupported.h"

#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace sdfhook {
namespace {

std::mutex g_mutex;
std::map<std::string, std::map<u64, u64>> g_seen;
u64 g_total = 0;

}  // namespace

void reportUnsupported(const char* what, u64 value) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_total++;
    u64& count = g_seen[what][value];
    if (count++) return;
    std::fprintf(stderr, "[unsupported] %s 0x%llX\n", what, (unsigned long long)value);
}

u64 getUnsupportedCount() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_total;
}

void printUnsupported() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& [what, values] : g_seen) {
        for (const auto& [value, count] : values) {
            std::fprintf(stderr, "[unsupported] %s 0x%llX x%llu\n", what.c_str(),
                         (unsigned long long)value, (unsigned long long)count);
        }
    }
}

}  // namespace sdfhook
