#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>
#include <orbit/terrain_erosion/SedimentExchange.hpp>

#include <optional>
#include <span>
#include <vector>

namespace orbit::terrain_erosion
{
// M11 CPU/reference implementation of the same cell state used by the GPU
// virtual-pipe solver. Flux is water volume rate through the four cell faces.
struct HydraulicCellState
{
    f64 waterDepthMeters{0.0};

    f64 fluxWestCubicMetersPerSecond{0.0};
    f64 fluxEastCubicMetersPerSecond{0.0};
    f64 fluxNorthCubicMetersPerSecond{0.0};
    f64 fluxSouthCubicMetersPerSecond{0.0};

    math::Double2 velocityMetersPerSecond{};

    // M14 typed shared waterborne state. The scalar is retained as a
    // compatibility/debug total and is synchronized from this value.
    SedimentMass suspendedSediment{};
    f64 suspendedSedimentKg{0.0};

    f64 cumulativeErodedKg{0.0};
    f64 cumulativeDepositedKg{0.0};
    f64 cumulativeErodedDepthMeters{0.0};
    f64 cumulativeDepositedDepthMeters{0.0};
};

struct HydraulicErosionConfig
{
    u32 iterations{64};
    f64 timeStepSeconds{0.25};

    // Uniform rain is added to the optional per-cell rainfall rate supplied to
    // SimulateHydraulicErosion().
    f64 rainfallMetersPerSecond{0.0002};

    // Virtual-pipe shallow-water parameters after Mei/Decaudin/Hu.
    f64 gravityMetersPerSecondSquared{9.81};
    f64 pipeCrossSectionSquareMeters{1.0};

    // Target suspended concentration:
    // kg/m3 = capacityCoefficient * speed * slope.
    f64 sedimentCapacityCoefficient{450.0};
    f64 maximumSedimentConcentrationKgPerCubicMeter{1'600.0};

    // Fractions of capacity deficit/excess exchanged with the physical column
    // per second.
    f64 erosionRatePerSecond{0.35};
    f64 depositionRatePerSecond{0.55};

    f64 maximumErosionDepthPerStepMeters{0.25};
    f64 maximumDepositionDepthPerStepMeters{0.25};

    // Loose material mobility relative to bedrock. Bedrock mobility is derived
    // from M02 hydraulic erodibility, hardness and cohesion.
    f64 regolithMobility{0.55};
    f64 soilMobility{0.90};
    f64 sandMobility{1.00};
    f64 debrisMobility{0.35};

    // Infiltration is modulated by M02 permeability and remaining M08 moisture
    // capacity. Evaporation is a first-order fractional loss rate.
    f64 infiltrationMetersPerSecond{0.00005};
    f64 moistureCapacityDepthMeters{0.20};
    f64 evaporationRatePerSecond{0.015};

    // Hydraulically transported sediment is deposited into the shared mobile
    // sand lane. M14 later generalizes this into typed shared sediment.
    terrain_material_column::LooseMaterialKind depositionMaterial{
        terrain_material_column::LooseMaterialKind::Sand};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct HydraulicMassBalance
{
    f64 totalErodedKg{0.0};
    f64 totalDepositedKg{0.0};
    f64 finalSuspendedKg{0.0};

    // Closed-page M11 currently has no boundary mass loss. The field is kept
    // explicit because M14 will exchange transported sediment across pages.
    f64 sedimentBoundaryLossKg{0.0};

    f64 materialBalanceErrorKg{0.0};
    f64 materialBalanceRelativeError{0.0};

    // Independent check from the physical M08 column:
    // initial loose + excavated bedrock == final loose + suspended.
    f64 physicalColumnBalanceErrorKg{0.0};
    f64 physicalColumnBalanceRelativeError{0.0};
};

struct HydraulicErosionResult
{
    terrain_material_column::MaterialColumnPage material;
    std::vector<HydraulicCellState> cells;

    // M14 inter-process authority for mobile sediment. M11 hydrodynamic
    // buffers remain solver scratch; this typed page is the handoff state.
    std::optional<SedimentExchangePage> sedimentExchange;

    HydraulicMassBalance massBalance{};

    [[nodiscard]] const HydraulicCellState& At(
        u32 x,
        u32 y) const;
    [[nodiscard]] HydraulicCellState& At(
        u32 x,
        u32 y);
};

// rainfallRateMetersPerSecond is an optional N*N additive source field.
// Empty means uniform rainfall only. Physical page boundaries are closed in
// this M11 solver; page-to-page mobile sediment/water exchange is deliberately
// deferred to M14's unified transport boundary contract.
[[nodiscard]] HydraulicErosionResult SimulateHydraulicErosion(
    terrain_material_column::MaterialColumnPage material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    std::span<const f32> rainfallRateMetersPerSecond = {},
    const HydraulicErosionConfig& config = {});
} // namespace orbit::terrain_erosion
