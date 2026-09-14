#include <orbit/math/Vector.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain_cache/TerrainPageBuilder.hpp>
#include <orbit/world/Planet.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

namespace
{
using namespace orbit;
using Clock = std::chrono::steady_clock;

std::vector<math::Double3> Directions(const u32 count)
{
    std::vector<math::Double3> result;
    result.reserve(count);
    constexpr f64 goldenAngle = std::numbers::pi * (3.0 - 2.2360679774997896964);
    for (u32 i = 0; i < count; ++i)
    {
        const f64 y = 1.0 - 2.0 * (static_cast<f64>(i) + 0.5) / count;
        const f64 radius = std::sqrt(1.0 - y * y);
        const f64 angle = goldenAngle * i;
        result.push_back({radius * std::cos(angle), y, radius * std::sin(angle)});
    }
    return result;
}
} // namespace

int main(int argc, char** argv)
{
    using namespace orbit;
    const world::PlanetDefinition planet{.radiusMeters = 6'000'000.0};
    const terrain::AnalyticTerrainSource source(planet, {
        .seed = 0xA57E22AULL
    });
    const auto directions = Directions(65'536);
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "workload,footprint_m,samples,median_ms,ns_per_sample,checksum\n";
    for (const f64 footprint : {20.0, 200.0, 2'000.0, 20'000.0, 200'000.0})
    {
        std::array<f64, 7> times{};
        f64 checksum = 0.0;
        for (std::size_t repeat = 0; repeat < times.size() + 1U; ++repeat)
        {
            f64 sum = 0.0;
            const auto start = Clock::now();
            for (const auto& direction : directions)
            {
                const auto sample = source.Sample({direction, footprint});
                sum += sample.elevationMeters + sample.climate.temperatureC + sample.biomes.alpine;
            }
            const f64 milliseconds = std::chrono::duration<f64, std::milli>(Clock::now() - start).count();
            if (repeat != 0)
            {
                times[repeat - 1U] = milliseconds;
            }
            checksum = sum;
        }
        std::sort(times.begin(), times.end());
        std::cout << "planet," << footprint << ',' << directions.size() << ',' << times[3]
                  << ',' << times[3] * 1.0e6 / static_cast<f64>(directions.size()) << ',' << checksum << '\n';
    }

    // Complete cold page construction, including cube projection and packing.
    const std::array<math::Double3, 2> pageDirections{{
        {0.65, 0.35, 0.68}, // Original sandbox location (now an ocean/coastal tile).
        {0.365111510558, 0.187057495117, -0.911977564625} // Mountain range.
    }};
    for (std::size_t region = 0; region < pageDirections.size(); ++region)
    for (const u32 level : {5U, 9U, 13U})
    {
        std::array<f64, 7> times{};
        f64 checksum = 0.0;
        const auto tile = world::TileForDirection(pageDirections[region], static_cast<u8>(level));
        for (std::size_t repeat = 0; repeat < times.size() + 1U; ++repeat)
        {
            const auto start = Clock::now();
            const auto page = terrain_cache::BuildTerrainPage(planet, source, {
                .tile = tile, .resolution = 65, .sourceRevision = source.Revision()
            });
            const f64 milliseconds = std::chrono::duration<f64, std::milli>(Clock::now() - start).count();
            if (repeat != 0) times[repeat - 1U] = milliseconds;
            checksum = 0.0;
            for (const auto& sample : page.samples) checksum += sample.elevationMeters;
        }
        std::sort(times.begin(), times.end());
        std::cout << (region == 0 ? "page_L" : "mountain_page_L") << level << ',' << world::ApproximateTileWidthMeters(planet, tile) / 64.0
                  << ",4225," << times[3] << ',' << times[3] * 1.0e6 / 4225.0 << ',' << checksum << '\n';
    }

    std::vector<f64> heights;
    math::Double3 peak{};
    f64 highest = -1.0e30;
    for (const auto& direction : directions)
    {
        const f64 height = source.Sample({direction, 20.0}).elevationMeters;
        heights.push_back(height);
        if (height > highest) { highest = height; peak = direction; }
    }
    std::sort(heights.begin(), heights.end());
    std::cerr << std::setprecision(12) << "height_min=" << heights.front() << " p50=" << heights[heights.size()/2]
              << " p95=" << heights[heights.size()*95/100] << " p99=" << heights[heights.size()*99/100]
              << " max=" << highest << " peak_direction=" << peak.x << ',' << peak.y << ',' << peak.z << '\n';

    // Optional local heightfield around the highest surveyed point, for QA.
    if (argc > 1)
    {
        std::ofstream file(argv[1]);
        file << "x_m,y_m,height_m\n";
        const auto frame = world::MakeSurfaceFrame(peak);
        constexpr u32 resolution = 257;
        for (u32 y = 0; y < resolution; ++y)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                const math::Double2 offset{(static_cast<f64>(x) - 128.0) * 250.0,
                    (static_cast<f64>(y) - 128.0) * 250.0};
                const auto direction = world::DirectionAtSurfaceOffset(planet, frame, offset);
                file << offset.x << ',' << offset.y << ',' << source.Sample({direction, 20.0}).elevationMeters << '\n';
            }
        }
    }
}
