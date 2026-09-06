#include "log.h"

#include <cstdio>
#include <utility>
#include <vector>

namespace sdfhook {
namespace {

constexpr LogLevel kDefaultLevel = LogLevel::Info;

struct Configured {
    LogLevel fallback = kDefaultLevel;
    std::vector<std::pair<std::string, LogLevel>> channels;
    u32 generation = 1;
};

Configured& configured() {
    static Configured levels;
    return levels;
}

LogLevel levelFromName(std::string_view name) {
    if (name == "trace") return LogLevel::Trace;
    if (name == "debug") return LogLevel::Debug;
    if (name == "info") return LogLevel::Info;
    if (name == "warn") return LogLevel::Warn;
    if (name == "error") return LogLevel::Error;
    if (name == "off") return LogLevel::Off;
    return kDefaultLevel;
}

const char* tagFor(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "T";
        case LogLevel::Debug: return "D";
        case LogLevel::Info: return "I";
        case LogLevel::Warn: return "W";
        case LogLevel::Error: return "E";
        case LogLevel::Off: return "-";
    }
    return "-";
}

}  // namespace

LogChannel::LogChannel(std::string name) : mName(std::move(name)) {}

LogLevel LogChannel::getLevel() const {
    const Configured& levels = configured();
    if (mGeneration == levels.generation) return mLevel;
    mLevel = levels.fallback;
    for (const auto& [channel, level] : levels.channels) {
        if (channel == mName) mLevel = level;
    }
    mGeneration = levels.generation;
    return mLevel;
}

void LogChannel::emit(LogLevel level, const std::string& message) const {
    const std::string line = std::format("{} {:<9} {}\n", tagFor(level), mName, message);
    std::fwrite(line.data(), 1, line.size(), stderr);
}

void LogChannel::dump(std::string_view row) const {
    std::fwrite(row.data(), 1, row.size(), stderr);
}

void configureLogging(std::string_view spec) {
    if (spec.empty()) return;
    configured().generation++;

    std::string_view text = spec;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const std::string_view entry = text.substr(0, comma);
        const auto equals = entry.find('=');
        if (equals == std::string_view::npos) {
            configured().fallback = levelFromName(entry);
        } else {
            configured().channels.emplace_back(entry.substr(0, equals),
                                               levelFromName(entry.substr(equals + 1)));
        }
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
}

}  // namespace sdfhook
