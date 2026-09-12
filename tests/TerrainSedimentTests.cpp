#include <orbit/terrain_erosion/SedimentTransport.hpp>
#include <orbit/terrain_hydrology/HydrologyGrid.hpp>

#include <cmath>
#include <iostream>

int main()
{
    orbit::terrain_hydrology::HydrologyGrid
        hydrology{};

    hydrology.config = {
        .resolution = 5,
        .halfExtentMeters = 2'000.0,
        .footprintMeters = 1'000.0,
        .useCoarseElevation = true,
        .conditionDepressions = false,
        .minimumDrainageDropMeters = 0.0
    };

    hydrology.spacingMeters =
        1'000.0;

    hydrology.cells.resize(25);

    for (orbit::u32 y = 0;
         y < 5;
         ++y)
    {
        for (orbit::u32 x = 0;
             x < 5;
             ++x)
        {
            auto& cell =
                hydrology.At(
                    x,
                    y);

            const orbit::f32 elevation =
                static_cast<orbit::f32>(
                    100.0 -
                    static_cast<orbit::f64>(
                        x) *
                        10.0);

            cell.elevationMeters =
                elevation;

            cell.drainageElevationMeters =
                elevation;

            cell.runoffWeight =
                1.0F;

            cell.oceanWeight =
                x == 4
                    ? 1.0F
                    : 0.0F;
        }
    }

    orbit::terrain_hydrology::
        RouteHydrology(
            hydrology);

    const auto sediment =
        orbit::terrain_erosion::
            BuildSedimentTransport(
                hydrology,
                {
                    .referenceDrainageAreaSquareMeters =
                        1'000'000.0,
                    .erosionScaleMeters =
                        50.0,
                    .maximumErosionMeters =
                        20.0,
                    .drainageAreaExponent =
                        0.35,
                    .slopeExponent =
                        0.70,
                    .depositionSlopeThreshold =
                        0.002,
                    .maximumLandDepositionFraction =
                        0.25,
                    .oceanDepositionFraction =
                        0.85,
                    .maximumDepositionMeters =
                        100.0
                });

    const auto& upstream =
        sediment.At(
            0,
            2);

    const auto& downstreamLand =
        sediment.At(
            3,
            2);

    const auto& ocean =
        sediment.At(
            4,
            2);

    if (upstream.erosionMeters <=
            0.0F ||
        upstream.
            netElevationDeltaMeters >=
            0.0F)
    {
        std::cerr
            << "Sediment transport did not erode the sloped upstream terrain.\n";
        return 1;
    }

    if (!(downstreamLand.
              erosionMeters >
          upstream.
              erosionMeters))
    {
        std::cerr
            << "Sediment erosion did not grow with contributing drainage area.\n";
        return 1;
    }

    if (ocean.erosionMeters !=
            0.0F ||
        ocean.depositionMeters <=
            0.0F ||
        ocean.netElevationDeltaMeters <=
            0.0F)
    {
        std::cerr
            << "Sediment transport did not deposit at the ocean outlet.\n";
        return 1;
    }

    orbit::f64 totalErosion = 0.0;
    orbit::f64 totalDeposition = 0.0;

    for (const auto& cell :
         sediment.cells)
    {
        totalErosion +=
            cell.erosionMeters;

        totalDeposition +=
            cell.depositionMeters;
    }

    const orbit::f64 balance =
        totalErosion -
        totalDeposition -
        sediment.
            exportedSediment;

    if (std::abs(balance) >
        1.0e-3)
    {
        std::cerr
            << "Sediment transport did not conserve its relative sediment load.\n";
        return 1;
    }

    return 0;
}
