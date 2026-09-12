#include <orbit/terrain_erosion/HydrologyRefinement.hpp>
#include <orbit/terrain_hydrology/HydrologyGrid.hpp>

#include <cmath>
#include <iostream>

namespace
{
[[nodiscard]] bool Inside(
    const orbit::i32 value) noexcept
{
    return
        value >= 0 &&
        value < 5;
}
} // namespace

int main()
{
    orbit::terrain_hydrology::HydrologyGrid
        hydrology{};

    hydrology.config = {
        .resolution = 5,
        .halfExtentMeters = 2'000.0,
        .footprintMeters = 1'000.0,
        .useCoarseElevation = true,
        .conditionDepressions = true,
        .minimumDrainageDropMeters =
            0.05
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
        ConditionDepressions(
            hydrology);

    orbit::terrain_hydrology::
        RouteHydrology(
            hydrology);

    const auto refined =
        orbit::terrain_erosion::
            RefineHydrologyWithSediment(
                hydrology,
                {
                    .iterations = 2,
                    .elevationDeltaScale =
                        0.5,
                    .sediment = {
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
                    }
                });

    if (refined.hydrology.cells.size() !=
            hydrology.cells.size() ||
        refined.lastSediment.cells.size() !=
            hydrology.cells.size())
    {
        std::cerr
            << "Hydrology refinement returned mismatched grid sizes.\n";
        return 1;
    }

    if (!(refined.hydrology.At(
              0,
              2).
              elevationMeters <
          hydrology.At(
              0,
              2).
              elevationMeters))
    {
        std::cerr
            << "Hydrology refinement did not erode the upstream terrain.\n";
        return 1;
    }

    if (!(refined.hydrology.At(
              4,
              2).
              elevationMeters >
          hydrology.At(
              4,
              2).
              elevationMeters))
    {
        std::cerr
            << "Hydrology refinement did not deposit sediment at the outlet.\n";
        return 1;
    }

    for (orbit::u32 y = 0;
         y < 5;
         ++y)
    {
        for (orbit::u32 x = 0;
             x < 5;
             ++x)
        {
            const auto& cell =
                refined.hydrology.At(
                    x,
                    y);

            if (cell.flowDx == 0 &&
                cell.flowDy == 0)
            {
                continue;
            }

            const orbit::i32 nx =
                static_cast<orbit::i32>(
                    x) +
                cell.flowDx;

            const orbit::i32 ny =
                static_cast<orbit::i32>(
                    y) +
                cell.flowDy;

            if (!Inside(nx) ||
                !Inside(ny))
            {
                std::cerr
                    << "Hydrology refinement produced an out-of-bounds flow edge.\n";
                return 1;
            }

            const auto& downstream =
                refined.hydrology.At(
                    static_cast<orbit::u32>(
                        nx),
                    static_cast<orbit::u32>(
                        ny));

            if (!(cell.
                      drainageElevationMeters >
                  downstream.
                      drainageElevationMeters))
            {
                std::cerr
                    << "Hydrology refinement left a non-descending flow edge.\n";
                return 1;
            }
        }
    }

    return 0;
}
