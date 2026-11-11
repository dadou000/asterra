#include <orbit/terrain_water/LakeWater.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_hydrology/DrainagePage.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>
#include <orbit/world/Planet.hpp>

#include <cmath>
#include <iostream>
#include <vector>

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

    // Production extraction consumes the physical M09 page and final M08
    // material column, preserving derived lake and spill metadata.
    constexpr orbit::u32 pageResolution = 7U;
    orbit::terrain_material_column::MaterialColumnPage material(
        pageResolution, 10.0);
    for (orbit::u32 y = 0U; y < pageResolution; ++y)
        for (orbit::u32 x = 0U; x < pageResolution; ++x)
        {
            const bool inBasin = x >= 2U && x <= 4U && y >= 2U && y <= 4U;
            const orbit::f32 bedrock = inBasin
                ? (x == 3U && y == 3U ? 90.0F : 100.0F)
                : 120.0F;
            material.SetCell(x, y, {
                .bedrockHeightMeters = bedrock,
                .referenceBedrockHeightMeters = bedrock,
                .bedrockMaterial = orbit::terrain_geology::reference_rock::Basalt});
        }
    orbit::terrain::PhysicalTerrainPageKey pageKey{};
    pageKey.resolution = pageResolution;
    std::vector<orbit::terrain_hydrology::DrainageCellInput> drainageInputs(
        static_cast<std::size_t>(pageResolution) * pageResolution);
    orbit::terrain_hydrology::DrainagePageHalo halo{};
    const orbit::terrain_hydrology::DrainageBoundaryCell boundary{
        .surfaceHeightMeters = 120.0F,
        .conditionedHeightMeters = 120.0F};
    halo.north.assign(pageResolution, boundary);
    halo.east.assign(pageResolution, boundary);
    halo.south.assign(pageResolution, boundary);
    halo.west.assign(pageResolution, boundary);
    halo.corners.fill(boundary);
    const auto drainage = orbit::terrain_hydrology::BuildDrainagePage(
        material, pageKey, drainageInputs, halo);
    const auto physicalLakes = orbit::terrain_water::BuildLakeWaterField(
        drainage, material, {.minimumWaterDepthMeters = 1.0, .minimumCellsPerBasin = 2U});
    if (physicalLakes.basins.size() != 1U)
    {
        std::cerr << "M09 physical-page extraction did not identify the closed basin.\n";
        return 1;
    }
    const auto& physicalBasin = physicalLakes.basins.front();
    if (physicalBasin.id == 0U || physicalBasin.cellCount < 2U ||
        physicalBasin.maximumDepthMeters < 20.0F ||
        physicalBasin.spillCellIndex >= pageResolution * pageResolution ||
        physicalBasin.outletCellIndex >= pageResolution * pageResolution)
    {
        std::cerr << "M09 physical lake metadata omitted depth or its routed spill.\n";
        return 1;
    }
    orbit::terrain_erosion::RiverNetwork rivers{};
    rivers.sourcePage = drainage.SourcePage();
    rivers.resolution = pageResolution;
    rivers.spacingMeters = drainage.SpacingMeters();
    rivers.nodes.push_back({
        .id = {.high = 1U, .low = 2U},
        .sourceX = physicalBasin.outletCellIndex % pageResolution,
        .sourceY = physicalBasin.outletCellIndex / pageResolution});
    const auto connectedLakes = orbit::terrain_water::BuildLakeWaterField(
        drainage, material, rivers);
    if (connectedLakes.basins.front().downstreamRiverNode != rivers.nodes.front().id ||
        connectedLakes.basins.front().downstreamRiverCellCount != 0U)
    {
        std::cerr << "Lake spill was not associated with its immediate downstream river node.\n";
        return 1;
    }

    return 0;
}
