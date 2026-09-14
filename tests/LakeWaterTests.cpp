#include <orbit/terrain_water/LakeWater.hpp>
#include <orbit/world/Planet.hpp>

#include <cmath>
#include <iostream>

int main()
{
    orbit::terrain_hydrology::HydrologyGrid hydrology{};

    hydrology.config = {
        .resolution = 5,
        .halfExtentMeters = 2'000.0
    };

    hydrology.spacingMeters = 1'000.0;

    hydrology.surfaceFrame =
        orbit::world::MakeSurfaceFrame({
            1.0,
            0.0,
            0.0
        });

    hydrology.cells.resize(25);

    for (auto& cell : hydrology.cells)
    {
        cell.elevationMeters = 120.0F;
        cell.drainageElevationMeters = 120.0F;
        cell.depressionFillMeters = 0.0F;
        cell.oceanWeight = 0.0F;
    }

    auto setCell =
        [&hydrology](
            const orbit::u32 x,
            const orbit::u32 y,
            const orbit::f32 elevation,
            const orbit::f32 drainage)
        {
            auto& cell =
                hydrology.At(
                    x,
                    y);

            cell.elevationMeters =
                elevation;

            cell.drainageElevationMeters =
                drainage;

            cell.depressionFillMeters =
                drainage -
                elevation;
        };

    setCell(
        2,
        2,
        90.0F,
        100.0F);

    setCell(
        2,
        3,
        92.0F,
        100.5F);

    setCell(
        3,
        2,
        95.0F,
        101.0F);

    // Same connected basin, but outside the logical region core.
    setCell(
        4,
        2,
        94.0F,
        101.5F);

    const auto lakes =
        orbit::terrain_water::
            BuildLakeWaterField(
                hydrology,
                1'500.0,
                {
                    .minimumWaterDepthMeters =
                        1.0,
                    .minimumCellsPerBasin =
                        2
                });

    if (lakes.basins.size() != 1 ||
        lakes.cells.size() != 3)
    {
        std::cerr
            << "Lake extraction did not group the basin or clip overlap cells correctly.\n";
        return 1;
    }

    const auto& basin =
        lakes.basins.front();

    if (std::abs(
            static_cast<orbit::f64>(
                basin.
                    surfaceElevationMeters) -
            100.0) >
            1.0e-6 ||
        std::abs(
            static_cast<orbit::f64>(
                basin.
                    maximumDepthMeters) -
            10.0) >
            1.0e-6)
    {
        std::cerr
            << "Lake extraction produced the wrong basin surface or depth.\n";
        return 1;
    }

    if (std::abs(
            basin.areaSquareMeters -
            3'000'000.0) >
        1.0e-6)
    {
        std::cerr
            << "Lake extraction produced the wrong owned basin area.\n";
        return 1;
    }

    for (const auto& cell : lakes.cells)
    {
        if (cell.basinIndex != 0 ||
            cell.surfaceElevationMeters !=
                basin.
                    surfaceElevationMeters ||
            cell.depthMeters < 1.0F ||
            std::abs(
                cell.offsetMeters.x) >
                1'500.0 ||
            std::abs(
                cell.offsetMeters.y) >
                1'500.0)
        {
            std::cerr
                << "Lake extraction produced invalid owned cell data.\n";
            return 1;
        }
    }

    // A shallow fringe remains part of a qualifying deep basin. Water extends
    // past the old half-cell quad and stops at the interpolated bank crossing.
    setCell(1, 2, 99.8F, 100.0F);
    const auto shore = orbit::terrain_water::BuildLakeWaterField(hydrology, 1'500.0);
    if (shore.cells.size() != 4 || shore.basins.size() != 1)
    {
        std::cerr << "Shallow shoreline cells were discarded.\n";
        return 1;
    }
    for (int step = 0; step <= 100; ++step)
    {
        const double x = -static_cast<double>(step) * 10.0;
        const auto sample = orbit::terrain_water::SampleLakeWater(shore, {x, 0.0});
        if (sample.depthMeters <= 0.0 || sample.influence != 1.0 ||
            std::abs(sample.bedElevationMeters + sample.depthMeters - 100.0) > 1.0e-4)
        {
            std::cerr << "Lake surface did not stay level through the shallow shore.\n";
            return 1;
        }
    }
    const auto dry = orbit::terrain_water::SampleLakeWater(shore, {-1'100.0, 0.0});
    const auto outside = orbit::terrain_water::SampleLakeWater(shore, {-3'000.0, 0.0});
    if (dry.depthMeters != 0.0 || outside.influence != 0.0)
    {
        std::cerr << "Lake leaked through the bank or outside its support.\n";
        return 1;
    }
    // Support survives core clipping; neighbouring regions can sample overlap.
    const auto overlap = orbit::terrain_water::SampleLakeWater(shore, {2'000.0, 0.0});
    if (overlap.depthMeters <= 0.0)
    {
        std::cerr << "Lake overlap was discarded with the owned-cell mesh.\n";
        return 1;
    }
    return 0;
}
