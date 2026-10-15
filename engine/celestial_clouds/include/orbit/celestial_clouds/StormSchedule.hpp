#pragma once

#include <orbit/celestial_clouds/WeatherModel.hpp>
#include <orbit/core/Types.hpp>

#include <vector>

// Phase 6 of the storm domain plan (docs: /world/weather-lab/global-system-phases): where and when simulated storms exist, as a pure
// function of the absolute simulation time. Nothing here depends on the camera or on what was simulated before: the planet is cut into
// fixed lat/lon tiles of about 4 degrees, and for every (tile, six-hour slot) a hash decides whether a storm exists, where inside the tile
// it starts, when and how long it lives; the odds come from the analytic weather map's convective score at the tile centre. Two launches
// at the same time therefore agree on every storm, and a time jump needs no stepping through the skipped time to know what exists.
namespace orbit::celestial_clouds
{
struct StormScheduleConfig
{
    u64 seed{1};
    f64 slotSeconds{21600.0};
    f64 tileDegrees{4.0};
    f64 minLifetimeSeconds{9000.0};      // 2.5 h
    f64 maxLifetimeSeconds{16200.0};     // 4.5 h
    f64 maxStartDelaySeconds{10800.0};   // a storm starts within the first 3 h of its slot
    f64 maxLatitudeDegrees{60.0};        // no storms beyond this latitude
    f64 probabilityScale{0.6};           // chance of a storm in a tile at a convective score of 1
};

struct ScheduledStorm
{
    bool exists{false};
    u64 id{0};                  // stable identity of (tile, slot), for caches
    i64 slot{0};
    i32 band{0};
    i32 index{0};
    f64 startSeconds{0.0};
    f64 lifetimeSeconds{0.0};
    f64 latitudeRadians{0.0};   // where the storm starts, +Y is the pole
    f64 longitudeRadians{0.0};  // longitude grows toward -Z (the viewer's body-fixed convention)
    f64 convectiveScore{0.0};

    [[nodiscard]] bool ActiveAt(const f64 seconds) const noexcept
    {
        return exists && seconds >= startSeconds && seconds < startSeconds + lifetimeSeconds;
    }
};

// The storm of one tile and slot (`exists` false when the hash rules it out). `weather` carries the layer's noise scales and seed and the
// season (subsolar latitude, quantised to 2 degrees inside so the answer does not change as the season drifts within a storm's life).
[[nodiscard]] ScheduledStorm ScheduleStorm(
    i32 band,
    i32 index,
    i64 slot,
    const StormScheduleConfig& config,
    const WeatherParameters& weather,
    const WeatherClimate& climate);

// Storms alive at `seconds` whose starting point is within `radiusMeters` of the given position on a planet of `planetRadiusMeters`,
// nearest first.
[[nodiscard]] std::vector<ScheduledStorm> StormsNear(
    f64 latitudeRadians,
    f64 longitudeRadians,
    f64 radiusMeters,
    f64 planetRadiusMeters,
    f64 seconds,
    const StormScheduleConfig& config,
    const WeatherParameters& weather,
    const WeatherClimate& climate);

// Number of tiles in a band and the tile layout, exposed for tests.
[[nodiscard]] i32 StormBandCount(const StormScheduleConfig& config) noexcept;
[[nodiscard]] i32 StormTilesInBand(i32 band, const StormScheduleConfig& config) noexcept;
} // namespace orbit::celestial_clouds
