#include <orbit/terrain_erosion/HydrologyRefinement.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace orbit::terrain_erosion
{
HydrologyRefinementResult
RefineHydrologyWithSediment(
    terrain_hydrology::HydrologyGrid hydrology,
    const HydrologyRefinementConfig config)
{
    std::vector<f32>
        cumulativeElevationDeltaMeters(
            hydrology.cells.size(),
            0.0F);

    if (config.iterations == 0)
    {
        return {
            .hydrology =
                std::move(hydrology),
            .lastSediment = {},
            .cumulativeElevationDeltaMeters =
                std::move(
                    cumulativeElevationDeltaMeters)
        };
    }

    if (!std::isfinite(
            config.elevationDeltaScale) ||
        config.elevationDeltaScale <
            0.0)
    {
        throw std::invalid_argument(
            "Orbit hydrology refinement requires a finite non-negative elevation delta scale.");
    }

    SedimentTransportGrid lastSediment{};

    for (u32 iteration = 0;
         iteration < config.iterations;
         ++iteration)
    {
        lastSediment =
            BuildSedimentTransport(
                hydrology,
                config.sediment);

        if (lastSediment.cells.size() !=
            hydrology.cells.size())
        {
            throw std::runtime_error(
                "Orbit hydrology refinement sediment grid size mismatch.");
        }

        for (std::size_t index = 0;
             index <
                hydrology.cells.size();
             ++index)
        {
            terrain_hydrology::HydrologyCell&
                hydroCell =
                    hydrology.cells[index];

            const SedimentCell& sedimentCell =
                lastSediment.cells[index];

            const f64 delta =
                static_cast<f64>(
                    sedimentCell.
                        netElevationDeltaMeters) *
                config.
                    elevationDeltaScale;

            cumulativeElevationDeltaMeters[
                index] +=
                    static_cast<f32>(
                        delta);

            const f64 refinedElevation =
                static_cast<f64>(
                    hydroCell.
                        elevationMeters) +
                delta;

            hydroCell.elevationMeters =
                static_cast<f32>(
                    refinedElevation);

            hydroCell.
                drainageElevationMeters =
                    hydroCell.
                        elevationMeters;

            hydroCell.depressionFillMeters =
                0.0F;
        }

        if (hydrology.config.
                conditionDepressions)
        {
            terrain_hydrology::
                ConditionDepressions(
                    hydrology);
        }

        terrain_hydrology::
            RouteHydrology(
                hydrology);
    }

    return {
        .hydrology =
            std::move(hydrology),
        .lastSediment =
            std::move(lastSediment),
        .cumulativeElevationDeltaMeters =
            std::move(
                cumulativeElevationDeltaMeters)
    };
}
} // namespace orbit::terrain_erosion
