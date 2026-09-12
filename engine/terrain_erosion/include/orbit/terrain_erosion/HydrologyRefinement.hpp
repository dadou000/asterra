#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain_erosion/SedimentTransport.hpp>
#include <orbit/terrain_hydrology/HydrologyGrid.hpp>

#include <vector>

namespace orbit::terrain_erosion
{
struct HydrologyRefinementConfig
{
    u32 iterations{1};

    // Multiplier applied to the sediment pass elevation delta each iteration.
    f64 elevationDeltaScale{1.0};

    SedimentTransportConfig sediment{};
};

struct HydrologyRefinementResult
{
    terrain_hydrology::HydrologyGrid hydrology{};
    SedimentTransportGrid lastSediment{};

    // Sum of the applied erosion/deposition elevation deltas.
    std::vector<f32> cumulativeElevationDeltaMeters;
};

[[nodiscard]] HydrologyRefinementResult
RefineHydrologyWithSediment(
    terrain_hydrology::HydrologyGrid hydrology,
    HydrologyRefinementConfig config = {});
} // namespace orbit::terrain_erosion
