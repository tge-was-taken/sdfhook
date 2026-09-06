#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace sdfhook {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Off };

/// One subsystem's output, named so SDF_LOG can set its level on its own.
///
/// Declare one per translation unit (`LogChannel log("model");`) and never tag a message by hand:
/// a literal "[model]" in a format string is invisible to the level filter and drifts from the file
/// it is in.
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

    bool isEnabled(LogLevel level) const { return level >= mLevel; }

private:
    template <typename... Args>
    void write(LogLevel level, std::format_string<Args...> fmt, Args&&... args) const {
        if (!isEnabled(level)) return;
        emit(level, std::format(fmt, std::forward<Args>(args)...));
    }

    void emit(LogLevel level, const std::string& message) const;

    std::string mName;
    LogLevel mLevel = LogLevel::Info;
};

/// One level for everything ("trace"), or per channel ("model=trace,render=warn"). Call before any
/// channel is constructed: a channel takes its level at construction.
void configureLogging(std::string_view spec);

}  // namespace sdfhook
