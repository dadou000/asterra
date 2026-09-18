#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

#include <span>
#include <vector>

namespace orbit::terrain_erosion
{
// Coarse wind/process forcing sampled onto one physical M08 page. Later climate
// and biome systems can generate this field; M13 only consumes it.
struct AeolianCellForcing
{
    f32 windEastMetersPerSecond{0.0F};
    f32 windNorthMetersPerSecond{0.0F};

    // Generic surface/vegetation/protection resistance [0,1]. M13 intentionally
    // does not own vegetation assets before the biome milestones exist.
    f32 surfaceResistance{0.0F};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct AeolianErosionConfig
{
    u32 iterations{96};
    f64 timeStepSeconds{0.20};

    // Local carrying capacity in kg/m2.
    f64 capacityCoefficient{0.030};
    f64 windSpeedExponent{2.0};

    // Upwind obstacle ray length and the exponential sensitivity to positive
    // obstruction slope. Exposure below 1 produces lee deposition.
    u32 shadowRayCells{4};
    f64 shadowStrength{8.0};
    f64 windwardExposureGain{0.50};
    f64 minimumExposure{0.05};
    f64 maximumExposure{1.75};

    f64 pickupRatePerSecond{1.25};
    f64 depositionRatePerSecond{1.50};

    // Fraction of freshly lifted sand that remains as near-surface reptation
    // instead of entering the airborne saltation lane.
    f64 reptationFraction{0.20};

    // Fraction of airborne mass advected one local D8 wind cell per second at
    // referenceSaltationWindMetersPerSecond.
    f64 saltationRatePerSecond{2.0};
    f64 referenceSaltationWindMetersPerSecond{10.0};

    f64 maximumSandPickupDepthPerStepMeters{0.05};
    f64 maximumSoilPickupDepthPerStepMeters{0.02};
    f64 maximumDepositionDepthPerStepMeters{0.08};

    // Moisture suppresses pickup/capacity without altering wind direction.
    f64 moistureSuppressionExponent{2.0};

    // Bedrock abrasion is intentionally orders of magnitude slower than loose
    // material transport. The M02 aeolian coefficient further scales it.
    f64 bedrockAbrasionMetersPerSecondAtReferenceWind{2.0e-6};
    f64 maximumBedrockAbrasionDepthPerStepMeters{2.0e-4};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct AeolianCellState
{
    f32 exposure{1.0F};
    f32 capacityKgPerSquareMeter{0.0F};

    f64 airborneSandKg{0.0};
    f64 airborneFinesKg{0.0};

    f64 cumulativeSandPickedKg{0.0};
    f64 cumulativeSoilPickedKg{0.0};
    f64 cumulativeReptatedKg{0.0};
    f64 cumulativeDepositedKg{0.0};
    f64 cumulativeBedrockAbradedKg{0.0};
};

struct AeolianMassBalance
{
    f64 initialLooseMassKg{0.0};
    f64 finalLooseMassKg{0.0};
    f64 abradedBedrockMassKg{0.0};
    f64 finalAirborneMassKg{0.0};

    // M13 physical pages are closed to mobile sediment. M14 owns explicit
    // cross-page transport flux.
    f64 boundaryLossKg{0.0};

    f64 materialBalanceErrorKg{0.0};
    f64 materialBalanceRelativeError{0.0};
};

struct AeolianErosionResult
{
    terrain_material_column::MaterialColumnPage material;
    std::vector<AeolianCellState> cells;
    AeolianMassBalance massBalance{};

    [[nodiscard]] const AeolianCellState& At(
        u32 x,
        u32 y) const;
};

// The forcing field must contain N*N samples. Wind is in the page-local M01
// east/north tangent frame. Fixed inputs are deterministic.
[[nodiscard]] AeolianErosionResult SimulateAeolianErosion(
    terrain_material_column::MaterialColumnPage material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    std::span<const AeolianCellForcing> forcing,
    const AeolianErosionConfig& config = {});
} // namespace orbit::terrain_erosion
