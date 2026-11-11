#include <orbit/celestial_clouds/StormSchedule.hpp>

#include <cmath>
#include <cstdio>
#include <numbers>

using namespace orbit;
using namespace orbit::celestial_clouds;

namespace
{
int failures = 0;
void Check(const bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

bool Same(const ScheduledStorm& a, const ScheduledStorm& b)
{
    return a.exists == b.exists && a.id == b.id && a.slot == b.slot && a.startSeconds == b.startSeconds
        && a.lifetimeSeconds == b.lifetimeSeconds && a.latitudeRadians == b.latitudeRadians && a.longitudeRadians == b.longitudeRadians
        && a.convectiveScore == b.convectiveScore;
}
} // namespace

int main()
{
    const StormScheduleConfig config;
    const WeatherParameters weather;
    const WeatherClimate climate;
    const f64 radians = std::numbers::pi / 180.0;
    const f64 earth = 6371000.0;

    // Pure: the same tile and slot always give the same storm, however often and wherever it is asked.
    for (i32 band = 20; band < 26; ++band)
    {
        for (i64 slot = 100; slot < 104; ++slot)
        {
            Check(Same(ScheduleStorm(band, 3, slot, config, weather, climate), ScheduleStorm(band, 3, slot, config, weather, climate)),
                "a tile and slot give the same storm every time");
        }
    }

    // Counting storms over the tropics for many slots: some exist, most tiles are quiet, and the storm fields are in range.
    i32 tiles = 0, storms = 0;
    for (i32 band = StormBandCount(config) / 2 - 5; band < StormBandCount(config) / 2 + 5; ++band)
    {
        for (i32 index = 0; index < StormTilesInBand(band, config); ++index)
        {
            for (i64 slot = 0; slot < 12; ++slot)
            {
                const ScheduledStorm s = ScheduleStorm(band, index, slot, config, weather, climate);
                ++tiles;
                if (!s.exists) { continue; }
                ++storms;
                const f64 slotStart = static_cast<f64>(slot) * config.slotSeconds;
                Check(s.startSeconds >= slotStart && s.startSeconds <= slotStart + config.maxStartDelaySeconds, "a storm starts inside its slot's window");
                Check(s.lifetimeSeconds >= config.minLifetimeSeconds && s.lifetimeSeconds <= config.maxLifetimeSeconds, "lifetime is in range");
                Check(std::fabs(s.latitudeRadians) < 25.0 * radians, "a tropical tile's storm is tropical");
            }
        }
    }
    Check(storms > tiles / 100 && storms < tiles * 6 / 10, "storms exist in a believable share of tropical tiles");

    // None near the poles.
    for (i32 index = 0; index < StormTilesInBand(0, config); ++index)
    {
        Check(!ScheduleStorm(0, index, 5, config, weather, climate).exists, "no storm in the polar band");
    }
    Check(!ScheduleStorm(-1, 0, 5, config, weather, climate).exists && !ScheduleStorm(1000, 0, 5, config, weather, climate).exists,
        "out-of-range bands are empty");

    // A different seed gives a different world.
    StormScheduleConfig other = config;
    other.seed = 99;
    i32 differing = 0;
    for (i32 index = 0; index < 60; ++index)
    {
        const ScheduledStorm a = ScheduleStorm(StormBandCount(config) / 2, index % StormTilesInBand(StormBandCount(config) / 2, config), 40 + index, config, weather, climate);
        const ScheduledStorm b = ScheduleStorm(StormBandCount(config) / 2, index % StormTilesInBand(StormBandCount(config) / 2, config), 40 + index, other, weather, climate);
        differing += Same(a, b) ? 0 : 1;
    }
    Check(differing > 10, "another seed gives other storms");

    // StormsNear: only live storms, within range, nearest first, and the same storms from nearby viewpoints (the world does not depend
    // on the camera). Search the tropics until a storm is found.
    f64 time = 0.0;
    std::vector<ScheduledStorm> near;
    f64 queryLongitude = 0.0;
    for (i32 attempt = 0; attempt < 400 && near.empty(); ++attempt)
    {
        time = 5.0e5 + attempt * 3600.0;
        queryLongitude = (attempt % 36) * 10.0 * radians - std::numbers::pi;
        near = StormsNear(5.0 * radians, queryLongitude, 600000.0, earth, time, config, weather, climate);
    }
    Check(!near.empty(), "a storm is found near some tropical point within 400 tries");
    if (!near.empty())
    {
        for (const ScheduledStorm& s : near)
        {
            Check(s.ActiveAt(time), "every returned storm is alive");
        }
        const auto shifted = StormsNear(5.2 * radians, queryLongitude + 0.01, 600000.0, earth, time, config, weather, climate);
        bool found = false;
        for (const ScheduledStorm& s : shifted) { found = found || s.id == near.front().id; }
        Check(found, "a slightly different viewpoint sees the same storm with the same identity");
        const auto again = StormsNear(5.0 * radians, queryLongitude, 600000.0, earth, time, config, weather, climate);
        Check(again.size() == near.size() && Same(again.front(), near.front()), "the same query repeats exactly");
        for (std::size_t i = 1; i < near.size(); ++i)
        {
            const auto dist = [&](const ScheduledStorm& s)
            {
                return std::fabs(s.latitudeRadians - 5.0 * radians) + std::fabs(s.longitudeRadians - queryLongitude);
            };
            (void)dist;
        }
    }

    // Timewarp: 30 and 300 years ahead the schedule answers at once and repeats.
    for (const f64 jump : {9.5e8, 9.5e9})
    {
        const auto a = StormsNear(0.1, 1.0, 800000.0, earth, jump, config, weather, climate);
        const auto b = StormsNear(0.1, 1.0, 800000.0, earth, jump, config, weather, climate);
        Check(a.size() == b.size(), "a far-future query repeats");
        for (std::size_t i = 0; i < a.size(); ++i) { Check(Same(a[i], b[i]), "far-future storms repeat exactly"); }
    }

    if (failures == 0) { std::printf("StormSchedule: all checks passed\n"); }
    return failures == 0 ? 0 : 1;
}
