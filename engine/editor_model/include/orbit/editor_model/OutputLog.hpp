#pragma once

#include <orbit/core/Log.hpp>

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace orbit::editor_model
{
struct OutputEntry
{
    log::Level level{log::Level::Info};
    std::string message;
};

class OutputLog
{
public:
    explicit OutputLog(
        std::size_t maximumEntries = 2'000);
    ~OutputLog();

    OutputLog(const OutputLog&) = delete;
    OutputLog& operator=(const OutputLog&) = delete;

    [[nodiscard]] std::vector<OutputEntry>
    Snapshot() const;

    void Clear();

private:
    mutable std::mutex mutex_;
    std::vector<OutputEntry> entries_;
    std::size_t maximumEntries_{2'000};
    log::SinkId sink_{0};
};
} // namespace orbit::editor_model
