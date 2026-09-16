#include <orbit/editor_model/OutputLog.hpp>

#include <algorithm>
#include <utility>

namespace orbit::editor_model
{
OutputLog::OutputLog(
    const std::size_t maximumEntries)
    : maximumEntries_(
          std::max<std::size_t>(
              maximumEntries,
              1U))
{
    sink_ =
        log::AddSink(
            [this](
                const log::Level level,
                const std::string_view message)
            {
                std::scoped_lock lock(
                    mutex_);

                entries_.push_back({
                    .level = level,
                    .message =
                        std::string(message)
                });

                if (entries_.size() >
                    maximumEntries_)
                {
                    const std::size_t eraseCount =
                        entries_.size() -
                        maximumEntries_;

                    entries_.erase(
                        entries_.begin(),
                        entries_.begin() +
                            static_cast<
                                std::ptrdiff_t>(
                                    eraseCount));
                }
            });
}

OutputLog::~OutputLog()
{
    log::RemoveSink(sink_);
}

std::vector<OutputEntry>
OutputLog::Snapshot() const
{
    std::scoped_lock lock(
        mutex_);
    return entries_;
}

void OutputLog::Clear()
{
    std::scoped_lock lock(
        mutex_);
    entries_.clear();
}
} // namespace orbit::editor_model
