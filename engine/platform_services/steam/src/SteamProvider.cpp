#include <orbit/platform_services/steam/SteamProvider.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace orbit::platform_services::steam
{
SteamProvider::SteamProvider(
    SteamConfiguration configuration,
    std::unique_ptr<ISteamClientBridge> bridge)
    : configuration_(std::move(configuration)),
      bridge_(std::move(bridge))
{
    if (bridge_ == nullptr)
    {
        throw std::invalid_argument(
            "Steam provider requires a Steam client bridge.");
    }

    if (!configuration_.enabled ||
        configuration_.appId == 0)
    {
        throw std::invalid_argument(
            "Steam provider requires an enabled configuration and App ID.");
    }

    if (!bridge_->Initialize(
            configuration_.appId))
    {
        throw std::runtime_error(
            "Steamworks initialization failed.");
    }
}

std::string_view SteamProvider::Name()
    const noexcept
{
    return "steam";
}

Capability SteamProvider::Capabilities()
    const noexcept
{
    return Capability::User |
        Capability::Achievements |
        Capability::Stats |
        Capability::Timeline;
}

std::optional<UserIdentity>
SteamProvider::User() const
{
    return bridge_->User();
}

const AchievementMapping*
SteamProvider::FindAchievement(
    const std::string_view id) const noexcept
{
    const auto found =
        std::ranges::find(
            configuration_.achievements,
            id,
            &AchievementMapping::id);

    return found ==
            configuration_.achievements.end()
        ? nullptr
        : &*found;
}

const StatMapping* SteamProvider::FindStat(
    const std::string_view id) const noexcept
{
    const auto found =
        std::ranges::find(
            configuration_.stats,
            id,
            &StatMapping::id);

    return found ==
            configuration_.stats.end()
        ? nullptr
        : &*found;
}

const TimelineMapping*
SteamProvider::FindTimeline(
    const std::string_view eventId) const noexcept
{
    const auto found =
        std::ranges::find(
            configuration_.timelineEvents,
            eventId,
            &TimelineMapping::eventId);

    return found ==
            configuration_.timelineEvents.end()
        ? nullptr
        : &*found;
}

bool SteamProvider::GetIntegerStat(
    const std::string_view id,
    i64& value)
{
    const auto* mapping = FindStat(id);

    return mapping != nullptr &&
        mapping->kind == StatKind::Integer &&
        bridge_->GetIntegerStat(
            mapping->steamApiName,
            value);
}

bool SteamProvider::GetFloatStat(
    const std::string_view id,
    f64& value)
{
    const auto* mapping = FindStat(id);

    return mapping != nullptr &&
        mapping->kind == StatKind::Float &&
        bridge_->GetFloatStat(
            mapping->steamApiName,
            value);
}

bool SteamProvider::SetIntegerStat(
    const std::string_view id,
    const i64 value)
{
    const auto* mapping = FindStat(id);

    if (mapping == nullptr ||
        mapping->kind != StatKind::Integer ||
        value <
            static_cast<i64>(
                std::numeric_limits<i32>::min()) ||
        value >
            static_cast<i64>(
                std::numeric_limits<i32>::max()))
    {
        return false;
    }

    return bridge_->SetIntegerStat(
        mapping->steamApiName,
        value);
}

bool SteamProvider::SetFloatStat(
    const std::string_view id,
    const f64 value)
{
    const auto* mapping = FindStat(id);

    return mapping != nullptr &&
        mapping->kind == StatKind::Float &&
        bridge_->SetFloatStat(
            mapping->steamApiName,
            value);
}

bool SteamProvider::UnlockAchievement(
    const std::string_view id)
{
    const auto* mapping =
        FindAchievement(id);

    return mapping != nullptr &&
        bridge_->UnlockAchievement(
            mapping->steamApiName);
}

bool SteamProvider::EmitTimelineEvent(
    const TimelineEvent& event)
{
    const auto* mapping =
        FindTimeline(event.id);

    if (mapping == nullptr)
    {
        return false;
    }

    const std::string_view title =
        event.title.empty()
            ? std::string_view(
                mapping->title)
            : std::string_view(
                event.title);
    const std::string_view description =
        event.description.empty()
            ? std::string_view(
                mapping->description)
            : std::string_view(
                event.description);

    return bridge_->AddTimelineEvent(
        title,
        description,
        mapping->icon,
        mapping->priority,
        mapping->clipPriority);
}

bool SteamProvider::Flush()
{
    return bridge_->StoreStats();
}
} // namespace orbit::platform_services::steam
