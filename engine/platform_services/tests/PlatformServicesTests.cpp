#include <orbit/platform_services/PlatformConfig.hpp>
#include <orbit/platform_services/PlatformServices.hpp>

#include <cstdlib>
#include <filesystem>

namespace
{
void Check(const bool condition)
{
    if (!condition)
    {
        std::abort();
    }
}
} // namespace

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

    Check(result.handled);
    Check(result.providerSucceeded);

    orbit::i64 repaired = 0;
    Check(provider.GetIntegerStat(
        "vehicles.repaired",
        repaired));
    Check(repaired == 1);
    Check(provider.AchievementUnlocked(
        "garage.first_repair"));
    Check(provider.TimelineEvents().size() == 1);

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

    Check(ValidatePlatformConfiguration(
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

    Check(loaded.steam.enabled);
    Check(loaded.steam.appId == 480);
    Check(loaded.steam.stats.size() == 1);
    Check(loaded.eventRules.size() == 1);
    Check(ValidatePlatformConfiguration(
        loaded,
        true).empty());

    std::filesystem::remove_all(root);
    return 0;
}
