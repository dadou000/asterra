#include <orbit/platform_services/PlatformServices.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace orbit::platform_services
{
std::string_view StandaloneProvider::Name()
    const noexcept
{
    return "standalone";
}

Capability StandaloneProvider::Capabilities()
    const noexcept
{
    return Capability::Achievements |
        Capability::Stats |
        Capability::Timeline;
}

std::optional<UserIdentity>
StandaloneProvider::User() const
{
    return std::nullopt;
}

bool StandaloneProvider::GetIntegerStat(
    const std::string_view id,
    i64& value)
{
    const auto found =
        integerStats_.find(std::string(id));

    if (found == integerStats_.end())
    {
        value = 0;
        return true;
    }

    value = found->second;
    return true;
}

bool StandaloneProvider::GetFloatStat(
    const std::string_view id,
    f64& value)
{
    const auto found =
        floatStats_.find(std::string(id));

    if (found == floatStats_.end())
    {
        value = 0.0;
        return true;
    }

    value = found->second;
    return true;
}

bool StandaloneProvider::SetIntegerStat(
    const std::string_view id,
    const i64 value)
{
    integerStats_.insert_or_assign(
        std::string(id),
        value);
    return true;
}

bool StandaloneProvider::SetFloatStat(
    const std::string_view id,
    const f64 value)
{
    floatStats_.insert_or_assign(
        std::string(id),
        value);
    return true;
}

bool StandaloneProvider::UnlockAchievement(
    const std::string_view id)
{
    achievements_.insert(std::string(id));
    return true;
}

bool StandaloneProvider::EmitTimelineEvent(
    const TimelineEvent& event)
{
    timelineEvents_.push_back(event);
    return true;
}

bool StandaloneProvider::Flush()
{
    return true;
}

bool StandaloneProvider::AchievementUnlocked(
    const std::string_view id) const
{
    return achievements_.contains(
        std::string(id));
}

const std::vector<TimelineEvent>&
StandaloneProvider::TimelineEvents()
    const noexcept
{
    return timelineEvents_;
}

PlatformServiceRegistry::PlatformServiceRegistry(
    std::unique_ptr<IPlatformProvider> provider)
    : provider_(std::move(provider))
{
    if (provider_ == nullptr)
    {
        throw std::invalid_argument(
            "Platform service registry requires a provider.");
    }
}

IPlatformProvider&
PlatformServiceRegistry::Provider() noexcept
{
    return *provider_;
}

const IPlatformProvider&
PlatformServiceRegistry::Provider() const noexcept
{
    return *provider_;
}

GameEventService::GameEventService(
    IPlatformProvider& provider,
    std::vector<StatDefinition> stats,
    std::vector<GameEventRule> rules)
    : provider_(provider),
      stats_(std::move(stats)),
      rules_(std::move(rules))
{
    for (const auto& stat : stats_)
    {
        if (stat.id.empty())
        {
            throw std::invalid_argument(
                "Platform stat IDs must not be empty.");
        }
    }

    for (const auto& rule : rules_)
    {
        if (rule.eventId.empty())
        {
            throw std::invalid_argument(
                "Game event rule IDs must not be empty.");
        }

        if (rule.incrementStat.has_value() &&
            FindStat(*rule.incrementStat) == nullptr)
        {
            throw std::invalid_argument(
                "Game event rule references an unknown stat: " +
                *rule.incrementStat);
        }

        if (rule.unlockAtStatValue.has_value() &&
            !rule.incrementStat.has_value())
        {
            throw std::invalid_argument(
                "Achievement threshold requires an incremented stat.");
        }
    }
}

const StatDefinition* GameEventService::FindStat(
    const std::string_view id) const noexcept
{
    const auto found = std::ranges::find(
        stats_,
        id,
        &StatDefinition::id);

    return found == stats_.end()
        ? nullptr
        : &*found;
}

GameEventResult GameEventService::Emit(
    const GameEvent& event)
{
    if (event.id.empty())
    {
        throw std::invalid_argument(
            "Game event IDs must not be empty.");
    }

    GameEventResult result;

    for (const auto& rule : rules_)
    {
        if (rule.eventId != event.id)
        {
            continue;
        }

        result.handled = true;
        std::optional<f64> resultingStat;

        if (rule.incrementStat.has_value())
        {
            const auto* definition =
                FindStat(*rule.incrementStat);

            if (definition == nullptr)
            {
                throw std::logic_error(
                    "Validated event rule lost its stat definition.");
            }

            const f64 delta =
                rule.statDelta * event.amount;

            bool succeeded = false;

            if (definition->kind ==
                StatKind::Integer)
            {
                i64 value = 0;
                succeeded =
                    provider_.GetIntegerStat(
                        definition->id,
                        value);

                const i64 increment =
                    static_cast<i64>(
                        std::llround(delta));
                value += increment;

                succeeded =
                    succeeded &&
                    provider_.SetIntegerStat(
                        definition->id,
                        value);
                resultingStat =
                    static_cast<f64>(value);
            }
            else
            {
                f64 value = 0.0;
                succeeded =
                    provider_.GetFloatStat(
                        definition->id,
                        value);
                value += delta;

                succeeded =
                    succeeded &&
                    provider_.SetFloatStat(
                        definition->id,
                        value);
                resultingStat = value;
            }

            result.providerSucceeded =
                result.providerSucceeded &&
                succeeded;
            result.changedStats.push_back(
                definition->id);
        }

        if (rule.unlockAchievement.has_value())
        {
            const bool thresholdReached =
                !rule.unlockAtStatValue.has_value() ||
                (resultingStat.has_value() &&
                 *resultingStat >=
                    *rule.unlockAtStatValue);

            if (thresholdReached)
            {
                const bool succeeded =
                    provider_.UnlockAchievement(
                        *rule.unlockAchievement);

                result.providerSucceeded =
                    result.providerSucceeded &&
                    succeeded;

                if (succeeded)
                {
                    result.unlockedAchievements.
                        push_back(
                            *rule.unlockAchievement);
                }
            }
        }

        if (rule.emitTimeline)
        {
            const bool succeeded =
                provider_.EmitTimelineEvent({
                    .id = event.id,
                    .title = event.title.empty()
                        ? event.id
                        : event.title,
                    .description =
                        event.description
                });

            result.providerSucceeded =
                result.providerSucceeded &&
                succeeded;
        }
    }

    return result;
}
} // namespace orbit::platform_services
