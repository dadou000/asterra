#include <orbit/terrain_stream/TerrainMorphRefresh.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::terrain_stream
{
namespace
{
[[nodiscard]] bool Moved(
    const terrain_view::ClipmapLevelMotion& motion) noexcept
{
    return motion.fullRefresh ||
        motion.cellShiftX != 0 || motion.cellShiftY != 0;
}

// Union dirty cells before creating jobs: intersecting edge/transition strips
// must not generate duplicate samples. Coalesce into physical rectangles to
// keep job dispatch and GPU copy counts bounded by the bands, not by grid rows.
[[nodiscard]] std::vector<PhysicalRegion> BuildRegions(
    std::vector<u8>& dirty,
    const u32 resolution)
{
    std::vector<PhysicalRegion> regions;
    for (u32 y = 0; y < resolution; ++y)
    {
        for (u32 x = 0; x < resolution; ++x)
        {
            if (dirty[static_cast<std::size_t>(y) * resolution + x] == 0)
            {
                continue;
            }

            u32 endX = x + 1U;
            while (endX < resolution &&
                dirty[static_cast<std::size_t>(y) * resolution + endX] != 0)
            {
                ++endX;
            }

            u32 endY = y;
            while (endY < resolution)
            {
                auto begin = dirty.begin() +
                    static_cast<std::size_t>(endY) * resolution + x;
                const auto end = begin + (endX - x);
                if (std::find(begin, end, u8{0}) != end)
                {
                    break;
                }
                std::fill(begin, end, u8{0});
                ++endY;
            }
            regions.push_back({x, y, endX - x, endY - y});
            x = endX - 1U;
        }
    }
    return regions;
}
} // namespace

void RefreshTerrainMorphRegions(
    const terrain_view::ClipmapLayout& layout,
    const terrain_view::ClipmapMotionUpdate& motion,
    ResidencyUpdate& residency)
{
    if (layout.levels.size() != motion.levels.size() ||
        layout.levels.size() != residency.levels.size())
    {
        throw std::invalid_argument(
            "Orbit terrain morph refresh requires matching clipmap levels.");
    }

    // The outermost level has no morph payload to invalidate.
    for (std::size_t index = 0; index + 1U < layout.levels.size(); ++index)
    {
        auto& update = residency.levels[index];
        const auto& movement = motion.levels[index];
        // Morph targets now snap to the globally anchored parent lattice,
        // not to the parent's moving window. A parent recenter therefore does
        // not change any retained fine sample. Only motion of this level can
        // change a retained sample's local morph weight/band membership.
        if (update.fullRefresh ||
            !Moved(movement))
        {
            continue;
        }

        const auto& level = layout.levels[index];
        const u32 resolution = level.gridResolution;
        const f64 halfCells = static_cast<f64>(resolution - 1U) * 0.5;
        std::vector<u8> dirty(static_cast<std::size_t>(resolution) * resolution, 0);
        for (const auto& region : update.refreshRegions)
        {
            for (u32 y = region.y; y < region.y + region.height; ++y)
            {
                auto begin = dirty.begin() +
                    static_cast<std::size_t>(y) * resolution + region.x;
                std::fill_n(begin, region.width, u8{1});
            }
        }

        const auto inMorphBand = [&](const f64 x, const f64 y)
        {
            return std::max(std::abs(x - halfCells), std::abs(y - halfCells)) *
                level.sampleSpacingMeters > level.morphStartHalfExtentMeters;
        };

        for (u32 y = 0; y < resolution; ++y)
        {
            for (u32 x = 0; x < resolution; ++x)
            {
                // A retained physical sample moves from old logical (x+dx,
                // y+dy) to new logical (x,y). Its old blended height/biomes
                // must be restored even when it has left the current band.
                const f64 oldX = static_cast<f64>(x) +
                    static_cast<f64>(movement.cellShiftX);
                const f64 oldY = static_cast<f64>(y) +
                    static_cast<f64>(movement.cellShiftY);
                const bool wasMorphed =
                    oldX >= 0.0 && oldX < resolution &&
                    oldY >= 0.0 && oldY < resolution &&
                    inMorphBand(oldX, oldY);

                if (inMorphBand(x, y) || wasMorphed)
                {
                    const u32 physicalX = (x + update.originX) % resolution;
                    const u32 physicalY = (y + update.originY) % resolution;
                    dirty[static_cast<std::size_t>(physicalY) * resolution + physicalX] = 1;
                }
            }
        }
        update.refreshRegions = BuildRegions(dirty, resolution);
    }
}
} // namespace orbit::terrain_stream
