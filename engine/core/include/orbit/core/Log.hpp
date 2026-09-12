#pragma once

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

void Write(Level level, std::string_view message);

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
