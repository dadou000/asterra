#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_erosion/SedimentExchange.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

#include <array>
#include <span>
#include <vector>

namespace orbit::terrain_water
{
enum class CoastalBoundaryMode : u8
{
    ClosedWall,
    OpenOcean,
    Neighbor
};

struct CoastalBoundaryCell
{
    CoastalBoundaryMode mode{CoastalBoundaryMode::ClosedWall};

    // Bathymetry and free-surface state just outside the physical page.
    f32 bedElevationMeters{0.0F};
    f32 waterSurfaceElevationMeters{0.0F};

    // Local page tangent components: +x east, +y south.
    f32 velocityEastMetersPerSecond{0.0F};
    f32 velocitySouthMetersPerSecond{0.0F};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct CoastalBoundaryState
{
    std::vector<CoastalBoundaryCell> north;
    std::vector<CoastalBoundaryCell> east;
    std::vector<CoastalBoundaryCell> south;
    std::vector<CoastalBoundaryCell> west;

    u64 revision{0};

    [[nodiscard]] bool IsComplete(u32 resolution) const noexcept;
};

struct CoastalWaveForcing
{
    bool enabled{true};

    f64 amplitudeMeters{0.35};
    f64 periodSeconds{8.0};
    f64 phaseRadians{0.0};

    // Unit propagation direction in local tangent coordinates. It does not
    // have to be axis-aligned with a page boundary.
    math::Double2 direction{1.0, 0.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct CoastalShallowWaterConfig
{
    f64 seaLevelMeters{0.0};

    f64 gravityMetersPerSecondSquared{9.81};
    f64 cflNumber{0.42};
    f64 maximumTimeStepSeconds{0.20};

    f64 wetThresholdMeters{0.02};
    f64 dryThresholdMeters{0.005};

    f64 manningRoughness{0.025};
    f64 maximumVelocityMetersPerSecond{25.0};

    CoastalWaveForcing wave{};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct CoastalCellState
{
    f64 waterDepthMeters{0.0};
    f64 waterSurfaceElevationMeters{0.0};

    // Depth-integrated momentum q = h*u [m2/s].
    f64 momentumEastSquareMetersPerSecond{0.0};
    f64 momentumSouthSquareMetersPerSecond{0.0};

    math::Double2 velocityMetersPerSecond{};

    bool wet{false};
    bool shoreline{false};

    // Diagnostics used by sediment coupling. This is not terrain authority.
    f64 specificWaveCurrentEnergySquareMetersPerSecondSquared{0.0};
};

struct CoastalWaterBalance
{
    f64 initialVolumeCubicMeters{0.0};

    // Positive is net water entering through open/neighbor boundaries.
    f64 cumulativeBoundaryVolumeCubicMeters{0.0};

    f64 finalVolumeCubicMeters{0.0};

    f64 balanceErrorCubicMeters{0.0};
    f64 balanceRelativeError{0.0};
};

struct CoastalWaterPage
{
    u32 resolution{0};
    f64 spacingMeters{0.0};

    f64 elapsedSeconds{0.0};
    f64 lastTimeStepSeconds{0.0};

    std::vector<CoastalCellState> cells;
    CoastalWaterBalance balance{};

    [[nodiscard]] CoastalCellState& At(u32 x, u32 y);
    [[nodiscard]] const CoastalCellState& At(u32 x, u32 y) const;
};

// Initial water is the still-water ocean intersection with the current M08
// physical surface. Terrain above sea level starts dry.
[[nodiscard]] CoastalWaterPage InitializeCoastalShallowWater(
    const terrain_material_column::MaterialColumnPage& material,
    const CoastalShallowWaterConfig& config = {});

// Advances a positivity-clamped finite-volume shallow-water state. Empty
// boundaries mean closed walls. A supplied boundary must contain all four
// complete edge arrays so page seams/open-ocean forcing are explicit.
void AdvanceCoastalShallowWater(
    CoastalWaterPage& water,
    const terrain_material_column::MaterialColumnPage& material,
    const CoastalBoundaryState& boundary,
    const CoastalShallowWaterConfig& config,
    u32 stepCount);

struct CoastalCellForcing
{
    // Authored preservation coefficient: 1 = preserve terrain.
    f32 protection{0.0F};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct CoastalSedimentConfig
{
    // Only shallow coastal water participates in surf-zone morphodynamics.
    f64 activeDepthMeters{12.0};

    // Erosion rate at reference specific wave/current energy.
    f64 referenceEnergySquareMetersPerSecondSquared{2.0};
    f64 erosionMetersPerSecondAtReference{0.003};
    f64 maximumErosionDepthPerStepMeters{0.08};

    // Fraction of freshly mobilized material assigned to bedload.
    f64 sandBedloadFraction{0.85};
    f64 finesBedloadFraction{0.10};
    f64 coarseDebrisBedloadFraction{1.0};

    // Conservative one-cell advection fraction is additionally bounded here.
    f64 transportRate{1.0};
    f64 maximumTransportFractionPerStep{0.75};

    // Settling/deposition in low-energy or shoreline cells.
    f64 depositionVelocityThresholdMetersPerSecond{0.55};
    f64 depositionRatePerSecond{0.70};
    f64 shorelineDepositionMultiplier{1.75};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct CoastalProcessConfig
{
    bool enabled{true};
    u32 hydrodynamicSteps{160U};

    CoastalShallowWaterConfig water{};
    CoastalSedimentConfig sediment{};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct CoastalProcessDiagnostics
{
    f64 erodedMassKg{0.0};
    f64 depositedMassKg{0.0};

    f64 transportedWaterborneKg{0.0};
    f64 transportedBedloadKg{0.0};

    f64 initialMaterialLooseKg{0.0};
    f64 finalMaterialLooseKg{0.0};
    f64 newlyExcavatedBedrockKg{0.0};
    f64 finalMobileSedimentKg{0.0};

    f64 materialBalanceErrorKg{0.0};
    f64 materialBalanceRelativeError{0.0};

    u32 shorelineCellCount{0};
};

struct CoastalProcessResult
{
    terrain_material_column::MaterialColumnPage material;
    terrain_erosion::SedimentExchangePage sedimentExchange;
    CoastalWaterPage water;

    CoastalProcessDiagnostics diagnostics{};
};

// Coupled M17 terrain-process epoch. Water evolves every hydrodynamic step;
// surf-zone erosion/deposition and M14 transport use the same current shoreline
// and velocity field. If config.enabled is false, M08 and M14 are returned
// bit-for-bit unchanged and no coastal process is executed.
[[nodiscard]] CoastalProcessResult SimulateCoastalProcess(
    terrain_material_column::MaterialColumnPage material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    terrain_erosion::SedimentExchangePage sedimentExchange,
    const CoastalBoundaryState& boundary = {},
    std::span<const CoastalCellForcing> forcing = {},
    const CoastalProcessConfig& config = {});
} // namespace orbit::terrain_water
