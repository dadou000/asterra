#include <orbit/hot_reload/HotIterationService.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace orbit::hot_reload
{
namespace
{
struct HandlerRecord
{
    HotIterationHandlerId id{0U};
    ChangeKind kind{ChangeKind::Ignored};
    HotIterationHandler handler;
};

std::mutex gHandlerMutex;
std::vector<HandlerRecord> gHandlers;
HotIterationHandlerId gNextHandlerId{1U};
std::atomic<HotIterationService*> gActiveService{nullptr};
} // namespace

HotIterationHandlerId AddHotIterationHandler(
    const ChangeKind kind,
    HotIterationHandler handler)
{
    if (!handler)
    {
        return 0U;
    }

    std::scoped_lock lock(gHandlerMutex);
    const HotIterationHandlerId id =
        gNextHandlerId++;
    gHandlers.push_back({
        .id = id,
        .kind = kind,
        .handler = std::move(handler)
    });
    return id;
}

void RemoveHotIterationHandler(
    const HotIterationHandlerId id) noexcept
{
    if (id == 0U)
    {
        return;
    }

    std::scoped_lock lock(gHandlerMutex);
    std::erase_if(
        gHandlers,
        [id](const HandlerRecord& record)
        {
            return record.id == id;
        });
}

std::uint32_t PumpHotIterationEvents()
{
    auto events =
        DrainHotIterationEvents();

    std::uint32_t dispatched = 0U;

    for (const auto& event : events)
    {
        std::vector<HotIterationHandler> handlers;
        {
            std::scoped_lock lock(gHandlerMutex);
            for (const auto& record : gHandlers)
            {
                if (record.kind == event.kind &&
                    record.handler)
                {
                    handlers.push_back(
                        record.handler);
                }
            }
        }

        for (const auto& handler : handlers)
        {
            handler(event);
            ++dispatched;
        }
    }

    return dispatched;
}

void SetActiveHotIterationService(
    HotIterationService* const service) noexcept
{
    gActiveService.store(
        service,
        std::memory_order_release);
}

void AddHotIterationWatchRoot(
    const std::filesystem::path& root)
{
    if (auto* const service =
            gActiveService.load(
                std::memory_order_acquire);
        service != nullptr)
    {
        service->AddWatchRoot(root);
    }
}
} // namespace orbit::hot_reload
