#include <orbit/celestial_clouds/StormSchedule.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace orbit::celestial_clouds
{
namespace
{
constexpr f64 kPi = std::numbers::pi;

u64 SplitMix(u64 x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
}

u64 Hash(const u64 seed, const i32 band, const i32 index, const i64 slot, const u64 salt)
{
    u64 h = SplitMix(seed ^ 0xA5A5A5A5A5A5A5A5ULL);
    h = SplitMix(h ^ static_cast<u64>(static_cast<i64>(band) + 1000));
    h = SplitMix(h ^ static_cast<u64>(static_cast<i64>(index) + 100000));
    h = SplitMix(h ^ static_cast<u64>(slot));
    return SplitMix(h ^ salt);
}

f64 Unit(const u64 h) { return static_cast<f64>(h >> 11U) * (1.0 / 9007199254740992.0); }

f64 SmoothStep(const f64 a, const f64 b, const f64 x)
{
    const f64 t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

f64 BandCentreLatitude(const i32 band, const StormScheduleConfig& config)
{
    return (-90.0 + (static_cast<f64>(band) + 0.5) * config.tileDegrees) * kPi / 180.0;
}

math::Double3 Direction(const f64 latitude, const f64 longitude)
{
    // The viewer's body-fixed convention: +Y is the pole and longitude grows toward -Z.
    return {std::cos(latitude) * std::cos(longitude), std::sin(latitude), -std::cos(latitude) * std::sin(longitude)};
}
} // namespace

i32 StormBandCount(const StormScheduleConfig& config) noexcept
{
    return std::max(1, static_cast<i32>(std::lround(180.0 / config.tileDegrees)));
}

i32 StormTilesInBand(const i32 band, const StormScheduleConfig& config) noexcept
{
    return std::max(1, static_cast<i32>(std::lround(360.0 * std::cos(BandCentreLatitude(band, config)) / config.tileDegrees)));
}

ScheduledStorm ScheduleStorm(
    const i32 band,
    const i32 index,
    const i64 slot,
    const StormScheduleConfig& config,
    const WeatherParameters& weather,
    const WeatherClimate& climate)
{
    ScheduledStorm storm;
    storm.band = band;
    storm.index = index;
    storm.slot = slot;
    storm.id = Hash(config.seed, band, index, slot, 0x1D);
    const i32 bands = StormBandCount(config);
    if (band < 0 || band >= bands)
    {
        return storm;
    }
    const f64 centreLatitude = BandCentreLatitude(band, config);
    if (std::fabs(centreLatitude) > config.maxLatitudeDegrees * kPi / 180.0)
    {
        return storm;
    }
    const i32 tiles = StormTilesInBand(band, config);
    if (index < 0 || index >= tiles)
    {
        return storm;
    }
    const f64 tileLongitude = 2.0 * kPi / static_cast<f64>(tiles);
    const f64 centreLongitude = -kPi + (static_cast<f64>(index) + 0.5) * tileLongitude;

    // The weather map at the middle of the slot's start window decides how convective this tile is.
    WeatherParameters params = weather;
    const f64 step = 2.0 * kPi / 180.0;
    params.subsolarLatitudeRadians = std::round(weather.subsolarLatitudeRadians / step) * step;
    const f64 slotStart = static_cast<f64>(slot) * config.slotSeconds;
    const WeatherState state = EvaluateWeather(
        Direction(centreLatitude, centreLongitude), slotStart + 0.5 * config.maxStartDelaySeconds, params, climate);
    const f64 score = std::clamp(state.coverage * SmoothStep(0.55, 0.9, state.cloudType) + 0.25 * state.precipitation, 0.0, 1.0);
    storm.convectiveScore = score;

    if (Unit(Hash(config.seed, band, index, slot, 0x2B)) >= score * config.probabilityScale)
    {
        return storm;
    }
    storm.exists = true;
    storm.startSeconds = slotStart + Unit(Hash(config.seed, band, index, slot, 0x3C)) * config.maxStartDelaySeconds;
    storm.lifetimeSeconds = config.minLifetimeSeconds
        + Unit(Hash(config.seed, band, index, slot, 0x4D)) * (config.maxLifetimeSeconds - config.minLifetimeSeconds);
    const f64 jitterLatitude = (Unit(Hash(config.seed, band, index, slot, 0x5E)) - 0.5) * 0.8 * config.tileDegrees * kPi / 180.0;
    const f64 jitterLongitude = (Unit(Hash(config.seed, band, index, slot, 0x6F)) - 0.5) * 0.8 * tileLongitude;
    storm.latitudeRadians = centreLatitude + jitterLatitude;
    storm.longitudeRadians = centreLongitude + jitterLongitude;
    return storm;
}

std::vector<ScheduledStorm> StormsNear(
    const f64 latitudeRadians,
    const f64 longitudeRadians,
    const f64 radiusMeters,
    const f64 planetRadiusMeters,
    const f64 seconds,
    const StormScheduleConfig& config,
    const WeatherParameters& weather,
    const WeatherClimate& climate)
{
    std::vector<ScheduledStorm> found;
    const f64 angular = radiusMeters / std::max(planetRadiusMeters, 1.0);
    const f64 tileRadians = config.tileDegrees * kPi / 180.0;
    const f64 lowLatitude = latitudeRadians - angular - tileRadians;
    const f64 highLatitude = latitudeRadians + angular + tileRadians;
    const i32 bands = StormBandCount(config);
    const i64 currentSlot = static_cast<i64>(std::floor(seconds / config.slotSeconds));
    // A storm lives at most max start delay + max lifetime, which is less than two slots.
    const i64 firstSlot = currentSlot - static_cast<i64>(std::ceil((config.maxStartDelaySeconds + config.maxLifetimeSeconds) / config.slotSeconds));
    const math::Double3 here = Direction(latitudeRadians, longitudeRadians);
    for (i32 band = 0; band < bands; ++band)
    {
        const f64 centre = BandCentreLatitude(band, config);
        if (centre < lowLatitude || centre > highLatitude)
        {
            continue;
        }
        const i32 tiles = StormTilesInBand(band, config);
        for (i32 index = 0; index < tiles; ++index)
        {
            for (i64 slot = firstSlot; slot <= currentSlot; ++slot)
            {
                ScheduledStorm storm = ScheduleStorm(band, index, slot, config, weather, climate);
                if (!storm.ActiveAt(seconds))
                {
                    continue;
                }
                const math::Double3 there = Direction(storm.latitudeRadians, storm.longitudeRadians);
                const f64 dot = std::clamp(here.x * there.x + here.y * there.y + here.z * there.z, -1.0, 1.0);
                if (std::acos(dot) * planetRadiusMeters <= radiusMeters)
                {
                    found.push_back(storm);
                }
            }
        }
    }
    std::sort(found.begin(), found.end(), [&](const ScheduledStorm& a, const ScheduledStorm& b)
    {
        const auto distance = [&](const ScheduledStorm& s)
        {
            const math::Double3 there = Direction(s.latitudeRadians, s.longitudeRadians);
            return std::acos(std::clamp(here.x * there.x + here.y * there.y + here.z * there.z, -1.0, 1.0));
        };
        const f64 da = distance(a), db = distance(b);
        return da != db ? da < db : a.id < b.id;
    });
    return found;
}
} // namespace orbit::celestial_clouds
