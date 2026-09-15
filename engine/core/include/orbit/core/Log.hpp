#pragma once

#include <orbit/core/Types.hpp>

#include <functional>
#include <string_view>

namespace orbit::log
{
enum class Level
{
    Trace,
    Info,
    Warning,
    Error
};

using SinkId = u64;
using Sink =
    std::function<void(
        Level,
        std::string_view)>;

void Write(Level level, std::string_view message);

[[nodiscard]] SinkId AddSink(Sink sink);
void RemoveSink(SinkId id) noexcept;

inline void Info(std::string_view message)
{
    Write(Level::Info, message);
}

inline void Warning(std::string_view message)
{
    Write(Level::Warning, message);
}

inline void Error(std::string_view message)
{
    Write(Level::Error, message);
}
} // namespace orbit::log
