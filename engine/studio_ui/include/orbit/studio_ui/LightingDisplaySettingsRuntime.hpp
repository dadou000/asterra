#pragma once

#include <orbit/studio_ui/LightingDisplaySettings.hpp>

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

namespace orbit::studio_ui
{
using StudioDisplayDefaultsConsumer =
    std::function<void(const StudioDisplayDefaults&)>;

namespace detail
{
[[nodiscard]] inline std::vector<
    std::pair<const void*, StudioDisplayDefaultsConsumer>>&
DisplayDefaultsConsumers() noexcept
{
    static std::vector<
        std::pair<const void*, StudioDisplayDefaultsConsumer>>
        consumers;
    return consumers;
}
} // namespace detail

inline void RegisterStudioDisplayDefaultsConsumer(
    const void* owner,
    StudioDisplayDefaultsConsumer consumer)
{
    auto& consumers = detail::DisplayDefaultsConsumers();
    const auto found = std::find_if(
        consumers.begin(),
        consumers.end(),
        [owner](const auto& entry)
        {
            return entry.first == owner;
        });

    if (found == consumers.end())
    {
        consumers.emplace_back(owner, std::move(consumer));
    }
    else
    {
        found->second = std::move(consumer);
    }

    const auto published = StudioDisplayDefaultsSnapshot();
    if (published.revision != 0U)
    {
        consumers.back().second(published.settings);
    }
}

inline void PublishStudioDisplayDefaultsRuntime(
    const StudioDisplayDefaults& settings)
{
    PublishStudioDisplayDefaults(settings);
    for (auto& [owner, consumer] :
         detail::DisplayDefaultsConsumers())
    {
        static_cast<void>(owner);
        if (consumer)
        {
            consumer(settings);
        }
    }
}
} // namespace orbit::studio_ui
