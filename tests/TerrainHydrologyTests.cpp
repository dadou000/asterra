#include <orbit/terrain_hydrology/HydrologyGrid.hpp>
#include <orbit/terrain_hydrology/RiverGraph.hpp>

#include <cmath>
#include <iostream>

namespace
{
class RecordingTerrainSource final :
    public orbit::terrain::TerrainSource
{
public:
    explicit RecordingTerrainSource(
        const orbit::world::PlanetId expectedPlanet)
        : expectedPlanet_(expectedPlanet)
    {
    }

    [[nodiscard]] orbit::terrain::TerrainSample Sample(
        const orbit::terrain::TerrainQuery& query)
        const noexcept override
    {
        ++calls_;

        const auto position =
            query.SurfacePosition();

        canonical_ =
            canonical_ &&
            position.planet == expectedPlanet_ &&
            std::abs(
                orbit::math::Length(
                    position.unitDirection) -
                1.0) < 1.0e-12 &&
            query.Footprint().IsValid();

        return {
            .elevationMeters = 100.0,
            .coarseElevationMeters = 100.0,
            .climate = {
                .temperatureC = 15.0F,
                .humidity = 0.5F,
                .precipitation = 0.5F,
                .continentality = 0.5F
            }
        };
    }

    [[nodiscard]] bool Canonical() const noexcept
    {
        return canonical_;
    }

    [[nodiscard]] orbit::u32 Calls() const noexcept
    {
        return calls_;
    }

private:
    orbit::world::PlanetId expectedPlanet_{};
    mutable bool canonical_{true};
    mutable orbit::u32 calls_{0};
};
} // namespace

int main()
{
    const orbit::world::PlanetId planetId{
        .high = 0x485944524f4c4f47ULL,
        .low = 0x594d303100000001ULL
    };

    const orbit::world::PlanetDefinition planet{
        .radiusMeters = 6'000'000.0,
        .id = planetId
    };

    const orbit::math::Double3 regionDirection =
        orbit::math::Normalize(
            orbit::math::Double3{
                1.0,
                2.0,
                3.0
            });

    const orbit::world::SurfaceFrame regionFrame =
        orbit::world::MakeSurfaceFrame(
            regionDirection);

    RecordingTerrainSource recordingSource{
        planetId
    };

    const auto sampledGrid =
        orbit::terrain_hydrology::BuildHydrologyGrid(
            planet,
            recordingSource,
            regionFrame,
            {
                .resolution = 5,
                .halfExtentMeters = 200.0,
                .footprintMeters = 20.0,
                .useCoarseElevation = true,
                .conditionDepressions = false
            });

    if (sampledGrid.origin.planet != planetId ||
        orbit::math::Dot(
            sampledGrid.origin.unitDirection,
            regionDirection) <
            1.0 - 1.0e-12 ||
        orbit::math::Dot(
            sampledGrid.surfaceFrame.up,
            regionDirection) <
            1.0 - 1.0e-12 ||
        !recordingSource.Canonical() ||
        recordingSource.Calls() != 25U)
    {
        std::cerr
            << "Hydrology did not preserve and propagate the canonical M01 surface position.\n";
        return 1;
    }

    orbit::terrain_hydrology::HydrologyGrid grid{};

    grid.config = {
        .resolution = 5,
        .halfExtentMeters = 2.0,
        .footprintMeters = 1.0,
        .useCoarseElevation = true
    };

    grid.spacingMeters = 1.0;
    grid.cells.resize(25);

    for (orbit::u32 y = 0;
         y < 5;
         ++y)
    {
        for (orbit::u32 x = 0;
             x < 5;
             ++x)
        {
            auto& cell =
                grid.At(
                    x,
                    y);

            cell.elevationMeters =
                static_cast<orbit::f32>(
                    std::abs(
                        static_cast<orbit::i32>(x) -
                        2) +
                    std::abs(
                        static_cast<orbit::i32>(y) -
                        2));

            cell.drainageElevationMeters =
                cell.elevationMeters;
            cell.runoffWeight = 1.0F;
            cell.oceanWeight = 0.0F;
        }
    }

    orbit::terrain_hydrology::
        RouteHydrology(grid);

    const auto& center =
        grid.At(
            2,
            2);

    if (center.flowDx != 0 ||
        center.flowDy != 0)
    {
        std::cerr
            << "Hydrology basin center is not a sink.\n";
        return 1;
    }

    if (std::abs(
            center.flowAccumulation -
            25.0F) >
        1.0e-5F)
    {
        std::cerr
            << "Hydrology flow accumulation did not converge into the basin center.\n";
        return 1;
    }

    if (grid.At(0, 0).flowDx != 1 ||
        grid.At(0, 0).flowDy != 1)
    {
        std::cerr
            << "Hydrology D8 routing did not choose the steepest diagonal descent.\n";
        return 1;
    }

    const orbit::f64 centerArea =
        grid.DrainageAreaSquareMeters(
            2,
            2);

    if (std::abs(
            centerArea -
            25.0) >
        1.0e-9)
    {
        std::cerr
            << "Hydrology drainage-area conversion is wrong.\n";
        return 1;
    }

    const auto rivers =
        orbit::terrain_hydrology::
            ExtractRiverCells(
                grid,
                20.0);

    if (rivers.size() != 1 ||
        rivers[0] !=
            2U +
            2U * 5U)
    {
        std::cerr
            << "Hydrology river extraction did not isolate the basin outlet.\n";
        return 1;
    }

    const auto graph =
        orbit::terrain_hydrology::
            BuildRiverGraph(
                grid,
                3.0);

    if (graph.nodes.empty() ||
        graph.segments.empty())
    {
        std::cerr
            << "Hydrology river graph is unexpectedly empty.\n";
        return 1;
    }

    bool reachesCenter = false;

    for (const auto& segment :
         graph.segments)
    {
        if (segment.upstreamNode >=
                graph.nodes.size() ||
            segment.downstreamNode >=
                graph.nodes.size())
        {
            std::cerr
                << "River graph contains an invalid node reference.\n";
            return 1;
        }

        const auto& upstream =
            graph.nodes[
                segment.upstreamNode];

        const auto& downstream =
            graph.nodes[
                segment.downstreamNode];

        if (downstream.elevationMeters >
            upstream.elevationMeters)
        {
            std::cerr
                << "River graph contains an uphill segment.\n";
            return 1;
        }

        if (downstream.sourceCellIndex ==
            2U + 2U * 5U)
        {
            reachesCenter = true;
        }
    }

    if (!reachesCenter)
    {
        std::cerr
            << "River graph does not connect drainage into the basin outlet.\n";
        return 1;
    }

    orbit::terrain_hydrology::HydrologyGrid
        depressionGrid{};

    depressionGrid.config = {
        .resolution = 5,
        .halfExtentMeters = 2.0,
        .footprintMeters = 1.0,
        .useCoarseElevation = true,
        .conditionDepressions = true,
        .minimumDrainageDropMeters = 0.1
    };

    depressionGrid.spacingMeters = 1.0;
    depressionGrid.cells.resize(25);

    for (orbit::u32 y = 0;
         y < 5;
         ++y)
    {
        for (orbit::u32 x = 0;
             x < 5;
             ++x)
        {
            auto& cell =
                depressionGrid.At(
                    x,
                    y);

            const bool boundary =
                x == 0 ||
                y == 0 ||
                x == 4 ||
                y == 4;

            cell.elevationMeters =
                boundary
                    ? 10.0F
                    : 0.0F;

            cell.drainageElevationMeters =
                cell.elevationMeters;

            cell.runoffWeight = 1.0F;
        }
    }

    depressionGrid.At(
        2,
        0).
        elevationMeters = 2.0F;

    depressionGrid.At(
        2,
        0).
        drainageElevationMeters = 2.0F;

    orbit::terrain_hydrology::
        ConditionDepressions(
            depressionGrid);

    orbit::terrain_hydrology::
        RouteHydrology(
            depressionGrid);

    const auto& conditionedCenter =
        depressionGrid.At(
            2,
            2);

    if (conditionedCenter.elevationMeters !=
            0.0F ||
        conditionedCenter.
            depressionFillMeters <=
            0.0F ||
        conditionedCenter.
            drainageElevationMeters <=
            conditionedCenter.
                elevationMeters)
    {
        std::cerr
            << "Priority-flood conditioning did not preserve raw terrain while filling the drainage surface.\n";
        return 1;
    }

    orbit::u32 traceX = 2;
    orbit::u32 traceY = 2;
    bool reachedBoundary = false;

    for (orbit::u32 step = 0;
         step < 25;
         ++step)
    {
        if (traceX == 0 ||
            traceY == 0 ||
            traceX == 4 ||
            traceY == 4)
        {
            reachedBoundary = true;
            break;
        }

        const auto& cell =
            depressionGrid.At(
                traceX,
                traceY);

        if (cell.flowDx == 0 &&
            cell.flowDy == 0)
        {
            break;
        }

        traceX =
            static_cast<orbit::u32>(
                static_cast<orbit::i32>(
                    traceX) +
                cell.flowDx);

        traceY =
            static_cast<orbit::u32>(
                static_cast<orbit::i32>(
                    traceY) +
                cell.flowDy);
    }

    if (!reachedBoundary)
    {
        std::cerr
            << "Priority-flood drainage did not create an outlet path from the enclosed basin.\n";
        return 1;
    }

    return 0;
}
