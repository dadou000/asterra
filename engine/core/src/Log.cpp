#include <orbit/core/Log.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <mutex>
#include <utility>
#include <vector>

namespace orbit::log
{
namespace
{
struct SinkRecord
{
    SinkId id{0};
    Sink sink;
};

std::mutex g_logMutex;
std::vector<SinkRecord> g_sinks;
SinkId g_nextSinkId{1};

constexpr std::string_view ToString(
    const Level level)
{
    switch (level)
    {
    case Level::Trace:
        return "TRACE";
    case Level::Info:
        return "INFO";
    case Level::Warning:
        return "WARN";
    case Level::Error:
        return "ERROR";
    }

    return "UNKNOWN";
}
} // namespace

void Write(
    const Level level,
    const std::string_view message)
{
    const auto now =
        std::chrono::system_clock::now();

    std::vector<Sink> sinks;

    {
        std::scoped_lock lock(
            g_logMutex);

        std::clog <<
            std::format(
                "[{:%H:%M:%S}] [{}] {}\n",
                now,
                ToString(level),
                message);

        sinks.reserve(
            g_sinks.size());

        for (const SinkRecord& record :
             g_sinks)
        {
            sinks.push_back(
                record.sink);
        }
    }

    // Run observers outside the logger mutex. Editor sinks may update
    // their own synchronized model and are free to log secondary
    // diagnostics without deadlocking the core logger.
    for (const Sink& sink : sinks)
    {
        if (sink)
        {
            sink(level, message);
        }
    }
}

SinkId AddSink(Sink sink)
{
    if (!sink)
    {
        return 0;
    }

    std::scoped_lock lock(
        g_logMutex);

    const SinkId id =
        g_nextSinkId++;

    g_sinks.push_back({
        .id = id,
        .sink = std::move(sink)
    });

    return id;
}

void RemoveSink(
    const SinkId id) noexcept
{
    if (id == 0)
    {
        return;
    }

    std::scoped_lock lock(
        g_logMutex);

    std::erase_if(
        g_sinks,
        [id](const SinkRecord& record)
        {
            return record.id == id;
        });
}
} // namespace orbit::log
