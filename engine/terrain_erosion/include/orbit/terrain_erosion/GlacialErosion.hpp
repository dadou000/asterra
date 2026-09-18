#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain_erosion/SedimentExchange.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

#include <optional>
#include <span>
#include <vector>

namespace orbit::terrain_erosion
{
// Coarse climate/authoring forcing for one physical M08 cell.
struct GlacialClimateCell
{
    f32 meanAnnualTemperatureC{-5.0F};
    f32 snowfallMetersIceEquivalentPerYear{0.5F};

    // Persistent ice entering this process epoch. Cells outside the valid
    // process mask intentionally discard this initial ice.
    f32 initialIceThicknessMeters{0.0F};

    // Climate/authoring validity mask. Zero means no glacial process at all.
    f32 processMask{1.0F};

    // Source-side preservation coefficient. One prevents M15 terrain erosion
    // while still allowing ice to pass over the cell.
    f32 protection{0.0F};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct GlacialErosionConfig
{
    u32 iterations{80U};
    f64 timeStepYears{0.5};

    // Eligibility.
    f64 maximumGlacierTemperatureC{1.0};
    f64 temperatureTransitionC{3.0};
    f64 snowfallForFullEligibilityMetersPerYear{0.30};

    // Surface mass balance.
    f64 accumulationEfficiency{0.80};
    f64 meltStartTemperatureC{-0.5};
    f64 meltMetersIcePerYearPerDegreeC{0.60};
    f64 maximumAblationMetersPerYear{4.0};

    // Efficient shallow-ice-inspired local flow approximation. Flow speed is
    // normalized against reference thickness and ice-surface slope.
    f64 minimumIceThicknessForFlowMeters{1.0};
    f64 referenceIceThicknessMeters{50.0};
    f64 referenceSurfaceSlope{0.05};
    f64 referenceFlowSpeedMetersPerYear{40.0};
    f64 iceThicknessFlowExponent{2.0};
    f64 surfaceSlopeFlowExponent{1.0};
    f64 maximumFlowSpeedMetersPerYear{250.0};
    f64 maximumFlowFractionPerStep{0.35};

    // Basal/lateral erosion. Lateral erosion perpendicular to the local ice
    // flow is the explicit U-shaped widening term.
    f64 minimumIceThicknessForErosionMeters{5.0};
    f64 basalErosionMetersPerYearAtReference{0.012};
    f64 lateralErosionFraction{0.75};
    f64 referenceErosionSpeedMetersPerYear{40.0};
    f64 referenceErosionIceThicknessMeters{50.0};
    f64 erosionSpeedExponent{0.70};
    f64 erosionThicknessExponent{0.50};
    f64 maximumErosionDepthPerStepMeters{0.30};

    // Debris entrainment/transport and moraine deposition.
    f64 debrisTransportEfficiency{0.85};
    f64 moraineDepositionRatePerYear{0.75};
    f64 moraineThinIceThresholdMeters{8.0};
    f64 moraineStagnationSpeedMetersPerYear{6.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

// Evaluates the frozen climate/ice eligibility multiplier [0,1].
[[nodiscard]] f32 EvaluateGlacialEligibility(
    const GlacialClimateCell& climate,
    const GlacialErosionConfig& config = {}) noexcept;

struct GlacialCellState
{
    f32 eligibility{0.0F};
    f32 iceThicknessMeters{0.0F};
    f32 iceSurfaceMeters{0.0F};

    i32 flowDx{0};
    i32 flowDy{0};

    f32 flowSpeedMetersPerYear{0.0F};
    f64 outgoingIceVolumeCubicMeters{0.0};

    f64 cumulativeBasalErosionMeters{0.0};
    f64 cumulativeLateralErosionMeters{0.0};
    f64 cumulativeErodedKg{0.0};

    f64 cumulativeTransportedSedimentKg{0.0};
    f64 cumulativeMoraineDepositedKg{0.0};
};

struct GlacialIceMassBalance
{
    f64 initialIceVolumeCubicMeters{0.0};
    f64 accumulatedIceVolumeCubicMeters{0.0};
    f64 ablatedIceVolumeCubicMeters{0.0};
    f64 finalIceVolumeCubicMeters{0.0};

    f64 balanceErrorCubicMeters{0.0};
    f64 balanceRelativeError{0.0};
};

struct GlacialMaterialBalance
{
    f64 initialLooseMassKg{0.0};
    f64 finalLooseMassKg{0.0};
    f64 newlyExcavatedBedrockKg{0.0};
    f64 finalMobileSedimentKg{0.0};
    f64 depositedFromMobileKg{0.0};

    f64 balanceErrorKg{0.0};
    f64 balanceRelativeError{0.0};
};

struct GlacialErosionResult
{
    terrain_material_column::MaterialColumnPage material;
    std::vector<GlacialCellState> cells;

    // M14 is the only persistent mobile-sediment authority.
    std::optional<SedimentExchangePage> sedimentExchange;

    GlacialIceMassBalance iceBalance{};
    GlacialMaterialBalance materialBalance{};

    [[nodiscard]] const GlacialCellState& At(
        u32 x,
        u32 y) const;
};

// Deterministic local glacier/terrain evolution. No process is permitted in a
// cell whose evaluated climate eligibility is zero.
[[nodiscard]] GlacialErosionResult SimulateGlacialErosion(
    terrain_material_column::MaterialColumnPage material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    std::span<const GlacialClimateCell> climate,
    const GlacialErosionConfig& config = {});
} // namespace orbit::terrain_erosion
