#pragma once

#include <orbit/core/Types.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace orbit::platform_services
{
enum class Capability : u32
{
    None = 0,
    User = 1U << 0U,
    Achievements = 1U << 1U,
    Stats = 1U << 2U,
    Timeline = 1U << 3U,
    RichPresence = 1U << 4U
};

[[nodiscard]] constexpr Capability operator|(
    const Capability left,
    const Capability right) noexcept
{
    return static_cast<Capability>(
        static_cast<u32>(left) |
        static_cast<u32>(right));
}

[[nodiscard]] constexpr bool HasCapability(
    const Capability capabilities,
    const Capability capability) noexcept
{
    return (
        static_cast<u32>(capabilities) &
        static_cast<u32>(capability)) != 0U;
}

enum class StatKind : u8
{
    Integer,
    Float
};

enum class TimelineClipPriority : u8
{
    None,
    Standard,
    Featured
};

struct UserIdentity
{
    std::string id;
    std::string displayName;
};

struct TimelineEvent
{
    std::string id;
    std::string title;
    std::string description;
    std::string icon{"steam_marker"};
    u32 priority{0};
    TimelineClipPriority clipPriority{
        TimelineClipPriority::None};
};

class IPlatformProvider
{
public:
    virtual ~IPlatformProvider() = default;

    [[nodiscard]] virtual std::string_view Name()
        const noexcept = 0;
    [[nodiscard]] virtual Capability Capabilities()
        const noexcept = 0;
    [[nodiscard]] virtual std::optional<UserIdentity> User()
        const = 0;

    [[nodiscard]] virtual bool GetIntegerStat(
        std::string_view id,
        i64& value) = 0;
    [[nodiscard]] virtual bool GetFloatStat(
        std::string_view id,
        f64& value) = 0;
    [[nodiscard]] virtual bool SetIntegerStat(
        std::string_view id,
        i64 value) = 0;
    [[nodiscard]] virtual bool SetFloatStat(
        std::string_view id,
        f64 value) = 0;
    [[nodiscard]] virtual bool UnlockAchievement(
        std::string_view id) = 0;
    [[nodiscard]] virtual bool EmitTimelineEvent(
        const TimelineEvent& event) = 0;
    [[nodiscard]] virtual bool Flush() = 0;
};

class StandaloneProvider final : public IPlatformProvider
{
public:
    [[nodiscard]] std::string_view Name()
        const noexcept override;
    [[nodiscard]] Capability Capabilities()
        const noexcept override;
    [[nodiscard]] std::optional<UserIdentity> User()
        const override;

    [[nodiscard]] bool GetIntegerStat(
        std::string_view id,
        i64& value) override;
    [[nodiscard]] bool GetFloatStat(
        std::string_view id,
        f64& value) override;
    [[nodiscard]] bool SetIntegerStat(
        std::string_view id,
        i64 value) override;
    [[nodiscard]] bool SetFloatStat(
        std::string_view id,
        f64 value) override;
    [[nodiscard]] bool UnlockAchievement(
        std::string_view id) override;
    [[nodiscard]] bool EmitTimelineEvent(
        const TimelineEvent& event) override;
    [[nodiscard]] bool Flush() override;

    [[nodiscard]] bool AchievementUnlocked(
        std::string_view id) const;
    [[nodiscard]] const std::vector<TimelineEvent>&
        TimelineEvents() const noexcept;

private:
    std::unordered_map<std::string, i64>
        integerStats_;
    std::unordered_map<std::string, f64>
        floatStats_;
    std::unordered_set<std::string>
        achievements_;
    std::vector<TimelineEvent>
        timelineEvents_;
};

class PlatformServiceRegistry
{
public:
    explicit PlatformServiceRegistry(
        std::unique_ptr<IPlatformProvider> provider);

    [[nodiscard]] IPlatformProvider& Provider()
        noexcept;
    [[nodiscard]] const IPlatformProvider& Provider()
        const noexcept;

private:
    std::unique_ptr<IPlatformProvider> provider_;
};

struct StatDefinition
{
    std::string id;
    StatKind kind{StatKind::Integer};
};

struct GameEventRule
{
    std::string eventId;
    std::optional<std::string> incrementStat;
    f64 statDelta{1.0};
    std::optional<std::string> unlockAchievement;
    std::optional<f64> unlockAtStatValue;
    bool emitTimeline{false};
};

struct GameEvent
{
    std::string id;
    f64 amount{1.0};
    std::string title;
    std::string description;
};

struct GameEventResult
{
    bool handled{false};
    bool providerSucceeded{true};
    std::vector<std::string> changedStats;
    std::vector<std::string> unlockedAchievements;
};

class GameEventService
{
public:
    GameEventService(
        IPlatformProvider& provider,
        std::vector<StatDefinition> stats,
        std::vector<GameEventRule> rules);

    [[nodiscard]] GameEventResult Emit(
        const GameEvent& event);

private:
    [[nodiscard]] const StatDefinition* FindStat(
        std::string_view id) const noexcept;

    IPlatformProvider& provider_;
    std::vector<StatDefinition> stats_;
    std::vector<GameEventRule> rules_;
};
} // namespace orbit::platform_services
