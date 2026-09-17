#include <orbit/platform_services/PlatformConfig.hpp>

#include <toml++/toml.hpp>

#include <cstdint>
#include <fstream>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <unordered_set>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

namespace orbit::platform_services
{
namespace
{
[[nodiscard]] std::string RequiredString(
    const toml::table& table,
    const std::string_view key)
{
    const auto value =
        table[key].value<std::string>();

    if (!value.has_value())
    {
        throw std::runtime_error(
            "Platform configuration missing string field: " +
            std::string(key));
    }

    return *value;
}

[[nodiscard]] StatKind ParseStatKind(
    const std::string_view value)
{
    if (value == "integer")
    {
        return StatKind::Integer;
    }

    if (value == "float")
    {
        return StatKind::Float;
    }

    throw std::runtime_error(
        "Unknown platform stat kind: " +
        std::string(value));
}

[[nodiscard]] std::string_view StatKindName(
    const StatKind kind) noexcept
{
    return kind == StatKind::Integer
        ? "integer"
        : "float";
}

[[nodiscard]] TimelineClipPriority
ParseClipPriority(
    const std::string_view value)
{
    if (value == "none")
    {
        return TimelineClipPriority::None;
    }
    if (value == "standard")
    {
        return TimelineClipPriority::Standard;
    }
    if (value == "featured")
    {
        return TimelineClipPriority::Featured;
    }

    throw std::runtime_error(
        "Unknown timeline clip priority: " +
        std::string(value));
}

[[nodiscard]] std::string_view ClipPriorityName(
    const TimelineClipPriority priority) noexcept
{
    switch (priority)
    {
    case TimelineClipPriority::None:
        return "none";
    case TimelineClipPriority::Standard:
        return "standard";
    case TimelineClipPriority::Featured:
        return "featured";
    }

    return "none";
}

void ReplaceFile(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination)
{
#if defined(_WIN32)
    if (!MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING |
                MOVEFILE_WRITE_THROUGH))
    {
        throw std::system_error(
            static_cast<int>(GetLastError()),
            std::system_category(),
            "Failed to atomically replace platform configuration");
    }
#else
    std::filesystem::rename(
        temporary,
        destination);
#endif
}

void AddIssue(
    std::vector<PlatformConfigIssue>& issues,
    std::string code,
    std::string message)
{
    issues.push_back({
        .code = std::move(code),
        .message = std::move(message)
    });
}
} // namespace

PlatformConfiguration LoadPlatformConfiguration(
    const std::filesystem::path& path)
{
    const toml::table root =
        toml::parse_file(path.string());

    PlatformConfiguration configuration;

    const auto schema =
        root["schema_version"].value<i64>();

    if (!schema.has_value() ||
        *schema !=
            static_cast<i64>(
                kPlatformConfigSchemaVersion))
    {
        throw std::runtime_error(
            "Unsupported platform configuration schema version.");
    }

    configuration.schemaVersion =
        static_cast<u32>(*schema);

    if (const auto* steam =
            root["steam"].as_table();
        steam != nullptr)
    {
        configuration.steam.enabled =
            (*steam)["enabled"].
                value_or(false);

        const i64 appId =
            (*steam)["app_id"].
                value_or<i64>(0);

        if (appId < 0 ||
            appId >
                static_cast<i64>(
                    std::numeric_limits<u32>::max()))
        {
            throw std::runtime_error(
                "Steam App ID is outside the uint32 range.");
        }

        configuration.steam.appId =
            static_cast<u32>(appId);
    }

    if (const auto* achievements =
            root["achievement_mappings"].
                as_array();
        achievements != nullptr)
    {
        for (const auto& node : *achievements)
        {
            const auto* item =
                node.as_table();

            if (item == nullptr)
            {
                throw std::runtime_error(
                    "achievement_mappings entries must be tables.");
            }

            configuration.steam.achievements.
                push_back({
                    .id = RequiredString(
                        *item,
                        "id"),
                    .steamApiName =
                        RequiredString(
                            *item,
                            "steam_api_name")
                });
        }
    }

    if (const auto* stats =
            root["stat_mappings"].as_array();
        stats != nullptr)
    {
        for (const auto& node : *stats)
        {
            const auto* item =
                node.as_table();

            if (item == nullptr)
            {
                throw std::runtime_error(
                    "stat_mappings entries must be tables.");
            }

            configuration.steam.stats.
                push_back({
                    .id = RequiredString(
                        *item,
                        "id"),
                    .kind = ParseStatKind(
                        RequiredString(
                            *item,
                            "kind")),
                    .steamApiName =
                        RequiredString(
                            *item,
                            "steam_api_name")
                });
        }
    }

    if (const auto* events =
            root["timeline_mappings"].
                as_array();
        events != nullptr)
    {
        for (const auto& node : *events)
        {
            const auto* item =
                node.as_table();

            if (item == nullptr)
            {
                throw std::runtime_error(
                    "timeline_mappings entries must be tables.");
            }

            const i64 priority =
                (*item)["priority"].
                    value_or<i64>(0);

            if (priority < 0 ||
                priority >
                    static_cast<i64>(
                        std::numeric_limits<u32>::max()))
            {
                throw std::runtime_error(
                    "Timeline priority is outside the uint32 range.");
            }

            configuration.steam.
                timelineEvents.push_back({
                    .eventId =
                        RequiredString(
                            *item,
                            "event_id"),
                    .title =
                        (*item)["title"].
                            value_or<std::string>(
                                ""),
                    .description =
                        (*item)["description"].
                            value_or<std::string>(
                                ""),
                    .icon =
                        (*item)["icon"].
                            value_or<std::string>(
                                "steam_marker"),
                    .priority =
                        static_cast<u32>(
                            priority),
                    .clipPriority =
                        ParseClipPriority(
                            (*item)[
                                "clip_priority"].
                                value_or<std::string>(
                                    "none"))
                });
        }
    }

    if (const auto* rules =
            root["event_rules"].as_array();
        rules != nullptr)
    {
        for (const auto& node : *rules)
        {
            const auto* item =
                node.as_table();

            if (item == nullptr)
            {
                throw std::runtime_error(
                    "event_rules entries must be tables.");
            }

            GameEventRule rule{
                .eventId =
                    RequiredString(
                        *item,
                        "event_id"),
                .statDelta =
                    (*item)["stat_delta"].
                        value_or<f64>(1.0),
                .emitTimeline =
                    (*item)["emit_timeline"].
                        value_or(false)
            };

            if (const auto value =
                    (*item)["increment_stat"].
                        value<std::string>();
                value.has_value() &&
                !value->empty())
            {
                rule.incrementStat = *value;
            }

            if (const auto value =
                    (*item)["unlock_achievement"].
                        value<std::string>();
                value.has_value() &&
                !value->empty())
            {
                rule.unlockAchievement = *value;
            }

            if (const auto value =
                    (*item)["unlock_at"].
                        value<f64>();
                value.has_value())
            {
                rule.unlockAtStatValue =
                    *value;
            }

            configuration.eventRules.
                push_back(std::move(rule));
        }
    }

    return configuration;
}

void SavePlatformConfigurationAtomic(
    const std::filesystem::path& path,
    const PlatformConfiguration& configuration)
{
    std::filesystem::create_directories(
        path.parent_path());

    toml::table root;
    root.insert(
        "schema_version",
        static_cast<i64>(
            configuration.schemaVersion));

    toml::table steam;
    steam.insert(
        "enabled",
        configuration.steam.enabled);
    steam.insert(
        "app_id",
        static_cast<i64>(
            configuration.steam.appId));
    root.insert(
        "steam",
        std::move(steam));

    toml::array achievements;
    for (const auto& mapping :
         configuration.steam.achievements)
    {
        toml::table item;
        item.insert("id", mapping.id);
        item.insert(
            "steam_api_name",
            mapping.steamApiName);
        achievements.push_back(
            std::move(item));
    }
    root.insert(
        "achievement_mappings",
        std::move(achievements));

    toml::array stats;
    for (const auto& mapping :
         configuration.steam.stats)
    {
        toml::table item;
        item.insert("id", mapping.id);
        item.insert(
            "kind",
            StatKindName(mapping.kind));
        item.insert(
            "steam_api_name",
            mapping.steamApiName);
        stats.push_back(
            std::move(item));
    }
    root.insert(
        "stat_mappings",
        std::move(stats));

    toml::array timeline;
    for (const auto& mapping :
         configuration.steam.timelineEvents)
    {
        toml::table item;
        item.insert(
            "event_id",
            mapping.eventId);
        item.insert(
            "title",
            mapping.title);
        item.insert(
            "description",
            mapping.description);
        item.insert(
            "icon",
            mapping.icon);
        item.insert(
            "priority",
            static_cast<i64>(
                mapping.priority));
        item.insert(
            "clip_priority",
            ClipPriorityName(
                mapping.clipPriority));
        timeline.push_back(
            std::move(item));
    }
    root.insert(
        "timeline_mappings",
        std::move(timeline));

    toml::array rules;
    for (const auto& rule :
         configuration.eventRules)
    {
        toml::table item;
        item.insert(
            "event_id",
            rule.eventId);
        if (rule.incrementStat.has_value())
        {
            item.insert(
                "increment_stat",
                *rule.incrementStat);
        }
        item.insert(
            "stat_delta",
            rule.statDelta);
        if (rule.unlockAchievement.has_value())
        {
            item.insert(
                "unlock_achievement",
                *rule.unlockAchievement);
        }
        if (rule.unlockAtStatValue.has_value())
        {
            item.insert(
                "unlock_at",
                *rule.unlockAtStatValue);
        }
        item.insert(
            "emit_timeline",
            rule.emitTimeline);
        rules.push_back(
            std::move(item));
    }
    root.insert(
        "event_rules",
        std::move(rules));

    std::filesystem::path temporary = path;
    temporary += ".tmp";

    {
        std::ofstream output(
            temporary,
            std::ios::binary |
                std::ios::trunc);

        if (!output)
        {
            throw std::runtime_error(
                "Unable to create platform configuration.");
        }

        output << root;
        output.flush();

        if (!output)
        {
            throw std::runtime_error(
                "Unable to write platform configuration.");
        }
    }

    ReplaceFile(
        temporary,
        path);
}

std::vector<PlatformConfigIssue>
ValidatePlatformConfiguration(
    const PlatformConfiguration& configuration,
    const bool requireSteam)
{
    std::vector<PlatformConfigIssue> issues;

    if (configuration.schemaVersion !=
        kPlatformConfigSchemaVersion)
    {
        AddIssue(
            issues,
            "platform.schema",
            "Unsupported platform configuration schema version.");
    }

    if (requireSteam &&
        !configuration.steam.enabled)
    {
        AddIssue(
            issues,
            "platform.steam.disabled",
            "Steam build profile requires Steam backend enablement.");
    }

    if ((requireSteam ||
         configuration.steam.enabled) &&
        configuration.steam.appId == 0)
    {
        AddIssue(
            issues,
            "platform.steam.app_id",
            "Steam backend requires a non-zero App ID.");
    }

    std::unordered_set<std::string>
        achievementIds;
    std::unordered_set<std::string>
        achievementNames;

    for (const auto& mapping :
         configuration.steam.achievements)
    {
        if (mapping.id.empty() ||
            mapping.steamApiName.empty())
        {
            AddIssue(
                issues,
                "platform.steam.achievement.empty",
                "Achievement mappings require both Orbit ID and Steam API name.");
        }

        if (!achievementIds.insert(
                mapping.id).second)
        {
            AddIssue(
                issues,
                "platform.steam.achievement.duplicate_id",
                "Duplicate Orbit achievement ID: " +
                    mapping.id);
        }

        if (!achievementNames.insert(
                mapping.steamApiName).second)
        {
            AddIssue(
                issues,
                "platform.steam.achievement.duplicate_name",
                "Duplicate Steam achievement API name: " +
                    mapping.steamApiName);
        }
    }

    std::unordered_set<std::string>
        statIds;
    std::unordered_set<std::string>
        statNames;

    for (const auto& mapping :
         configuration.steam.stats)
    {
        if (mapping.id.empty() ||
            mapping.steamApiName.empty())
        {
            AddIssue(
                issues,
                "platform.steam.stat.empty",
                "Stat mappings require both Orbit ID and Steam API name.");
        }

        if (!statIds.insert(
                mapping.id).second)
        {
            AddIssue(
                issues,
                "platform.steam.stat.duplicate_id",
                "Duplicate Orbit stat ID: " +
                    mapping.id);
        }

        if (!statNames.insert(
                mapping.steamApiName).second)
        {
            AddIssue(
                issues,
                "platform.steam.stat.duplicate_name",
                "Duplicate Steam stat API name: " +
                    mapping.steamApiName);
        }
    }

    std::unordered_set<std::string>
        eventIds;

    for (const auto& mapping :
         configuration.steam.timelineEvents)
    {
        if (mapping.eventId.empty())
        {
            AddIssue(
                issues,
                "platform.steam.timeline.empty",
                "Timeline mapping requires an event ID.");
        }

        if (!eventIds.insert(
                mapping.eventId).second)
        {
            AddIssue(
                issues,
                "platform.steam.timeline.duplicate",
                "Duplicate timeline event mapping: " +
                    mapping.eventId);
        }

        if (mapping.icon.empty())
        {
            AddIssue(
                issues,
                "platform.steam.timeline.icon",
                "Timeline mapping requires an icon name.");
        }
    }

    for (const auto& rule :
         configuration.eventRules)
    {
        if (rule.eventId.empty())
        {
            AddIssue(
                issues,
                "platform.event_rule.id",
                "Event rules require a semantic event ID.");
        }

        if (rule.incrementStat.has_value() &&
            !statIds.contains(
                *rule.incrementStat))
        {
            AddIssue(
                issues,
                "platform.event_rule.stat",
                "Event rule references unmapped stat: " +
                    *rule.incrementStat);
        }

        if (rule.unlockAchievement.has_value() &&
            !achievementIds.contains(
                *rule.unlockAchievement))
        {
            AddIssue(
                issues,
                "platform.event_rule.achievement",
                "Event rule references unmapped achievement: " +
                    *rule.unlockAchievement);
        }

        if (rule.emitTimeline &&
            !eventIds.contains(rule.eventId))
        {
            AddIssue(
                issues,
                "platform.event_rule.timeline",
                "Timeline-enabled event has no timeline mapping: " +
                    rule.eventId);
        }

        if (rule.unlockAtStatValue.has_value() &&
            !rule.incrementStat.has_value())
        {
            AddIssue(
                issues,
                "platform.event_rule.threshold",
                "Achievement threshold requires increment_stat.");
        }
    }

    return issues;
}
} // namespace orbit::platform_services
