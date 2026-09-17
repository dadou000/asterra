#include <orbit/platform_services/PlatformConfig.hpp>
#include <orbit/platform_services/PlatformServices.hpp>

#include <cassert>
#include <filesystem>

int main()
{
    using namespace orbit::platform_services;

    StandaloneProvider provider;

    GameEventService events(
        provider,
        {
            {
                .id = "vehicles.repaired",
                .kind = StatKind::Integer
            }
        },
        {
            {
                .eventId = "vehicle.repaired",
                .incrementStat =
                    "vehicles.repaired",
                .statDelta = 1.0,
                .unlockAchievement =
                    "garage.first_repair",
                .unlockAtStatValue = 1.0,
                .emitTimeline = true
            }
        });

    const auto result =
        events.Emit({
            .id = "vehicle.repaired",
            .title = "Vehicle repaired",
            .description =
                "Completed a repair."
        });

    assert(result.handled);
    assert(result.providerSucceeded);

    orbit::i64 repaired = 0;
    assert(provider.GetIntegerStat(
        "vehicles.repaired",
        repaired));
    assert(repaired == 1);
    assert(provider.AchievementUnlocked(
        "garage.first_repair"));
    assert(provider.TimelineEvents().size() == 1);

    PlatformConfiguration configuration;
    configuration.steam.enabled = true;
    configuration.steam.appId = 480;
    configuration.steam.achievements.push_back({
        .id = "garage.first_repair",
        .steamApiName = "ACH_FIRST_REPAIR"
    });
    configuration.steam.stats.push_back({
        .id = "vehicles.repaired",
        .kind = StatKind::Integer,
        .steamApiName = "STAT_VEHICLES_REPAIRED"
    });
    configuration.steam.timelineEvents.push_back({
        .eventId = "vehicle.repaired",
        .title = "Vehicle repaired",
        .description = "Completed a repair.",
        .icon = "steam_achievement",
        .priority = 10,
        .clipPriority =
            TimelineClipPriority::Standard
    });
    configuration.eventRules.push_back({
        .eventId = "vehicle.repaired",
        .incrementStat = "vehicles.repaired",
        .statDelta = 1.0,
        .unlockAchievement =
            "garage.first_repair",
        .unlockAtStatValue = 1.0,
        .emitTimeline = true
    });

    assert(ValidatePlatformConfiguration(
        configuration,
        true).empty());

    const auto root =
        std::filesystem::temp_directory_path() /
        "orbit_platform_services_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    const auto path =
        root /
        "PlatformServices.toml";

    SavePlatformConfigurationAtomic(
        path,
        configuration);

    const auto loaded =
        LoadPlatformConfiguration(path);

    assert(loaded.steam.enabled);
    assert(loaded.steam.appId == 480);
    assert(loaded.steam.stats.size() == 1);
    assert(loaded.eventRules.size() == 1);
    assert(ValidatePlatformConfiguration(
        loaded,
        true).empty());

    std::filesystem::remove_all(root);
    return 0;
}
