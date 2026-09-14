#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/world/Planet.hpp>

#include "../engine/terrain/src/ProceduralNoise.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace
{
using namespace orbit;

f64 ReferenceNoise(const math::Double3& p, const u64 seed)
{
    // Original eight-corner scalar evaluation, retained to verify that sharing
    // hashes did not change the established continental/climate noise field.
    const i64 x = static_cast<i64>(std::floor(p.x));
    const i64 y = static_cast<i64>(std::floor(p.y));
    const i64 z = static_cast<i64>(std::floor(p.z));
    const f64 tx = terrain::detail::Smooth(p.x - static_cast<f64>(x));
    const f64 ty = terrain::detail::Smooth(p.y - static_cast<f64>(y));
    const f64 tz = terrain::detail::Smooth(p.z - static_cast<f64>(z));
    const auto row = [&](const i64 dy, const i64 dz)
    {
        return terrain::detail::Lerp(terrain::detail::HashValue(x, y + dy, z + dz, seed),
            terrain::detail::HashValue(x + 1, y + dy, z + dz, seed), tx);
    };
    return terrain::detail::Lerp(terrain::detail::Lerp(row(0, 0), row(1, 0), ty),
        terrain::detail::Lerp(row(0, 1), row(1, 1), ty), tz);
}

bool NoiseRegression()
{
    math::Double3 sum{};
    math::Double3 squares{};
    f64 cross = 0.0;
    constexpr u32 count = 4096;
    for (u32 i = 0; i < count; ++i)
    {
        const math::Double3 p{static_cast<f64>(i) * 3.781 - 6000.0,
            static_cast<f64>(i) * -1.713, static_cast<f64>(i) * 9.891 + 0.25};
        const u64 seed = 0xABCDEF0198765432ULL + i;
        if (std::abs(ReferenceNoise(p, seed) - terrain::detail::ValueNoise3D(p, seed)) > 1.0e-14)
        {
            std::cerr << "Optimized scalar noise changed its reference field.\n";
            return false;
        }
        const auto v = terrain::detail::VectorNoise3D(p, seed);
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
            std::abs(v.x) > 1.0 || std::abs(v.y) > 1.0 || std::abs(v.z) > 1.0)
        {
            std::cerr << "Vector warp noise escaped its finite bounds.\n";
            return false;
        }
        sum = sum + v;
        squares = squares + math::Double3{v.x * v.x, v.y * v.y, v.z * v.z};
        cross += v.x * v.y;
    }
    if (std::abs(sum.x / count) > 0.05 || std::abs(sum.y / count) > 0.05 ||
        std::abs(sum.z / count) > 0.05 || squares.x / count < 0.03 ||
        squares.y / count < 0.03 || squares.z / count < 0.03 || std::abs(cross / count) > 0.025)
    {
        std::cerr << "Warp channels are degenerate or strongly correlated.\n";
        return false;
    }
    return true;
}

bool MountainRegression()
{
    const world::PlanetDefinition planet{.radiusMeters = 6'000'000.0};
    terrain::AnalyticTerrainDesc desc{.seed = 0xA57E22AULL};
    const terrain::AnalyticTerrainSource source(planet, desc);
    const terrain::AnalyticTerrainSource repeated(planet, desc);
    auto otherDesc = desc;
    otherDesc.mountains.warpAmplitudeMeters += 1.0;
    const terrain::AnalyticTerrainSource other(planet, otherDesc);
    if (source.Revision() != repeated.Revision() || source.Revision() == other.Revision())
    {
        std::cerr << "Terrain recipe revisions are not stable/distinct.\n";
        return false;
    }

    // Deterministic approximately equal-area spherical survey, including oceans.
    f64 highest = -1.0e30;
    f64 lowest = 1.0e30;
    math::Double3 peak{};
    constexpr u32 count = 65'536;
    constexpr f64 goldenAngle = std::numbers::pi * (3.0 - 2.2360679774997896964);
    for (u32 i = 0; i < count; ++i)
    {
        const f64 y = 1.0 - 2.0 * (static_cast<f64>(i) + 0.5) / count;
        const f64 radius = std::sqrt(1.0 - y * y);
        const math::Double3 direction{radius * std::cos(goldenAngle * i), y, radius * std::sin(goldenAngle * i)};
        for (const f64 footprint : {20.0, 200.0, 2'000.0, 20'000.0, 200'000.0})
        {
            const auto sample = source.Sample({direction, footprint});
            if (!std::isfinite(sample.elevationMeters) || sample.elevationMeters > 8000.0 ||
                !std::isfinite(sample.coarseElevationMeters) || sample.coarseElevationMeters > 8000.0 ||
                !std::isfinite(sample.climate.temperatureC))
            {
                std::cerr << "Terrain escaped its elevation bound.\n";
                return false;
            }
            if (footprint == 20.0)
            {
                lowest = std::min(lowest, sample.elevationMeters);
                if (sample.elevationMeters > highest) { highest = sample.elevationMeters; peak = direction; }
            }
        }
    }
    if (highest < 7800.0 || lowest >= -500.0)
    {
        std::cerr << "Terrain must have near-8km mountains and preserve ocean basins.\n";
        return false;
    }
    if (source.Sample({peak, 20.0}).biomes.alpine < 0.99F ||
        source.Sample({peak, 20.0}).climate.temperatureC >= 0.0F)
    {
        std::cerr << "High summits must have cold alpine climate.\n";
        return false;
    }

    // Exercise every band fade boundary, including the no-octave fast path.
    const f64 warpStretch = 1.0 + 11.25 * desc.mountains.warpAmplitudeMeters / desc.mountains.warpWavelengthMeters;
    f64 wavelength = desc.mountains.wavelengthMeters;
    for (u32 octave = 0; octave < desc.mountains.octaves; ++octave)
    {
        for (const f64 edge : {2.0, 4.0})
        {
            const f64 cutoff = wavelength / (edge * warpStretch * 2.0);
            const f64 a = source.Sample({peak, cutoff * (1.0 - 1.0e-7)}).elevationMeters;
            const f64 b = source.Sample({peak, cutoff * (1.0 + 1.0e-7)}).elevationMeters;
            if (std::abs(a - b) > 0.01)
            {
                std::cerr << "Mountain octave filtering has a discontinuous boundary.\n";
                return false;
            }
        }
        wavelength /= 2.03;
    }

    const auto frame = world::MakeSurfaceFrame(peak);
    f64 localMin = highest;
    for (const math::Double2 offset : {math::Double2{10'000.0, 0}, {-10'000.0, 0}, {0, 10'000.0}, {0, -10'000.0}})
    {
        localMin = std::min(localMin, source.Sample({world::DirectionAtSurfaceOffset(planet, frame, offset), 20.0}).elevationMeters);
    }
    if (highest - localMin < 500.0)
    {
        std::cerr << "The summit region is a plateau rather than mountain relief.\n";
        return false;
    }

    // Actual shared-source worker calls must equal serial queries exactly.
    const auto task = [&]()
    {
        f64 sum = 0.0;
        for (u32 i = 0; i < 1024; ++i)
        {
            sum += source.Sample({world::DirectionAtSurfaceOffset(planet, frame,
                {static_cast<f64>(i) * 10.0, static_cast<f64>(i) * -7.0}), 20.0}).elevationMeters;
        }
        return sum;
    };
    auto a = std::async(std::launch::async, task);
    auto b = std::async(std::launch::async, task);
    const f64 serial = task();
    if (a.get() != serial || b.get() != serial)
    {
        std::cerr << "Concurrent terrain sampling is not deterministic.\n";
        return false;
    }

    auto raisedSea = desc;
    raisedSea.global.seaLevelMeters = 500.0;
    const terrain::AnalyticTerrainSource shifted(planet, raisedSea);
    if (shifted.Sample({peak, 20.0}).elevationMeters > 8500.0)
    {
        std::cerr << "Elevation limit must be relative to sea level.\n";
        return false;
    }
    desc.mountains.octaves = 17;
    try
    {
        const terrain::AnalyticTerrainSource invalid(planet, desc);
        std::cerr << "Unbounded octave recipe was accepted.\n";
        return false;
    }
    catch (const std::invalid_argument&) {}
    return true;
}
} // namespace

int main()
{
    return NoiseRegression() && MountainRegression() ? 0 : 1;
}
