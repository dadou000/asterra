#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain_hydrology/HydrologyGrid.hpp>

#include <vector>

namespace orbit::terrain_erosion
{
struct SedimentTransportConfig
{
    f64 referenceDrainageAreaSquareMeters{
        100'000'000.0
    };

    f64 erosionScaleMeters{45.0};
    f64 maximumErosionMeters{40.0};

    f64 drainageAreaExponent{0.35};
    f64 slopeExponent{0.70};

    f64 depositionSlopeThreshold{0.002};
    f64 maximumLandDepositionFraction{0.35};
    f64 oceanDepositionFraction{0.85};
    f64 maximumDepositionMeters{35.0};
};

struct SedimentCell
{
    f32 erosionMeters{0.0F};
    f32 depositionMeters{0.0F};
    f32 netElevationDeltaMeters{0.0F};

    // Relative sediment load used by this shaping pass.
    f32 incomingSediment{0.0F};
    f32 outgoingSediment{0.0F};
};

struct SedimentTransportGrid
{
    u32 resolution{0};
    f64 spacingMeters{0.0};

    std::vector<SedimentCell> cells;

    f64 exportedSediment{0.0};

    [[nodiscard]] SedimentCell& At(
        u32 x,
        u32 y);

    [[nodiscard]] const SedimentCell& At(
        u32 x,
        u32 y) const;
};

[[nodiscard]] SedimentTransportGrid
BuildSedimentTransport(
    const terrain_hydrology::HydrologyGrid& hydrology,
    SedimentTransportConfig config = {});
} // namespace orbit::terrain_erosion
