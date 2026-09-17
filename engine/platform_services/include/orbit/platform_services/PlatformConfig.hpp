#pragma once

#include <orbit/platform_services/PlatformServices.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace orbit::platform_services
{
inline constexpr u32
    kPlatformConfigSchemaVersion = 1;

struct AchievementMapping
{
    std::string id;
    std::string steamApiName;
};

struct StatMapping
{
    std::string id;
    StatKind kind{StatKind::Integer};
    std::string steamApiName;
};

struct TimelineMapping
{
    std::string eventId;
    std::string title;
    std::string description;
    std::string icon{"steam_marker"};
    u32 priority{0};
    TimelineClipPriority clipPriority{
        TimelineClipPriority::None};
};

struct SteamConfiguration
{
    bool enabled{false};
    u32 appId{0};
    std::vector<AchievementMapping> achievements;
    std::vector<StatMapping> stats;
    std::vector<TimelineMapping> timelineEvents;
};

struct PlatformConfiguration
{
    u32 schemaVersion{
        kPlatformConfigSchemaVersion};
    SteamConfiguration steam;
    std::vector<GameEventRule> eventRules;
};

struct PlatformConfigIssue
{
    std::string code;
    std::string message;
};

[[nodiscard]] PlatformConfiguration
LoadPlatformConfiguration(
    const std::filesystem::path& path);

void SavePlatformConfigurationAtomic(
    const std::filesystem::path& path,
    const PlatformConfiguration& configuration);

[[nodiscard]] std::vector<PlatformConfigIssue>
ValidatePlatformConfiguration(
    const PlatformConfiguration& configuration,
    bool requireSteam);
} // namespace orbit::platform_services
