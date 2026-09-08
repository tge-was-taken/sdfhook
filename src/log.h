#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "types.h"

namespace sdfhook {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Off };

/// One subsystem's output, named so SDF_LOG can set its level on its own. Declare one per
/// translation unit; a hand-written "[model]" tag is invisible to the level filter.
class LogChannel {
public:
    explicit LogChannel(std::string name);

    // A level nothing reaches for yet, not unfinished work.
    template <typename... Args>
    // cppcheck-suppress unusedFunction
    void trace(std::format_string<Args...> fmt, Args&&... args) const {
        write(LogLevel::Trace, fmt, std::forward<Args>(args)...);
    }

    // A level nothing reaches for yet, not unfinished work.
    template <typename... Args>
    // cppcheck-suppress unusedFunction
    void debug(std::format_string<Args...> fmt, Args&&... args) const {
        write(LogLevel::Debug, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) const {
        write(LogLevel::Info, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) const {
        write(LogLevel::Warn, fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) const {
        write(LogLevel::Error, fmt, std::forward<Args>(args)...);
    }

    /// A finished JSONL row, written without a level prefix so the stream stays machine readable.
    void dump(std::string_view row) const;

    bool isEnabled(LogLevel level) const { return level >= getLevel(); }

private:
    template <typename... Args>
    void write(LogLevel level, std::format_string<Args...> fmt, Args&&... args) const {
        if (!isEnabled(level)) return;
        emit(level, std::format(fmt, std::forward<Args>(args)...));
    }

    void emit(LogLevel level, const std::string& message) const;

    /// Resolved on use rather than at construction: every channel is a file-scope static, so it is
    /// built before main can read SDF_LOG, and a level taken in the constructor is always the
    /// default. That silently discarded every trace and debug call in the codebase.
    LogLevel getLevel() const;

    std::string mName;
    mutable LogLevel mLevel = LogLevel::Info;
    mutable u32 mGeneration = 0;
};

/// One level for everything ("trace"), or per channel ("model=trace,render=warn"). Takes effect on
/// channels already constructed, which is all of them.
void configureLogging(std::string_view spec);

}  // namespace sdfhook
