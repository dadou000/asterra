#include <orbit/core/Log.hpp>

#include <chrono>
#include <format>
#include <iostream>
#include <mutex>

namespace orbit::log
{
namespace
{
std::mutex g_logMutex;

constexpr std::string_view ToString(const Level level)
{
    switch (level)
    {
    case Level::Trace:   return "TRACE";
    case Level::Info:    return "INFO";
    case Level::Warning: return "WARN";
    case Level::Error:   return "ERROR";
    }

    return "UNKNOWN";
}
} // namespace

void Write(const Level level, const std::string_view message)
{
    const auto now = std::chrono::system_clock::now();

    std::scoped_lock lock(g_logMutex);
    std::clog << std::format("[{:%H:%M:%S}] [{}] {}\n", now, ToString(level), message);
}
} // namespace orbit::log
