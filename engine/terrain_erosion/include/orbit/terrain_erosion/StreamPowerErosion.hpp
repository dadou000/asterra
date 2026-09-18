#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain/TerrainContracts.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_hydrology/DrainagePage.hpp>
#include <orbit/terrain_macro_geology/MacroGeologyField.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>
#include <orbit/world/Planet.hpp>

#include <span>
#include <vector>

namespace orbit::terrain_erosion
{
enum class StreamPowerDrainageMeasure : u8
{
    DrainageArea,
    Discharge
};

struct StreamPowerErosionConfig
{
    u32 iterations{32};

    // M05 exposes geological uplift as a forcing amplitude in meters rather
    // than a simulation speed. This explicit coupling converts one meter of
    // M05 forcing into bedrock displacement per equilibrium iteration.
    f64 upliftCouplingPerIteration{0.001};

    // M04 height intent is treated as an equilibrium offset from the initial
    // physical surface. This relaxation moves toward that target without
    // re-applying the authored offset as a fresh stamp every iteration.
    f64 authoredHeightRelaxation{0.05};

    // E = K * measure^m * slope^n * erodibility * erosionAllowance.
    f64 incisionCoefficientMetersPerIteration{0.25};
    f64 drainageExponent{0.5};
    f64 slopeExponent{1.0};

    StreamPowerDrainageMeasure drainageMeasure{
        StreamPowerDrainageMeasure::DrainageArea};

    f64 referenceDrainageAreaSquareMeters{1'000'000.0};
    f64 referenceDischargeCubicMetersPerSecond{1.0};

    f64 looseMaterialErodibility{1.0};
    f64 minimumBedSlope{1.0e-5};
    f64 maximumIncisionMetersPerIteration{25.0};

    terrain_hydrology::DrainageRoutingConfig drainage{};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct StreamPowerCellForcing
{
    // M05 forcing amplitude. Positive is uplift, negative is subsidence.
    f64 upliftForcingMeters{0.0};

    // M04 authored height result sampled with zero height baseline. M10 treats
    // this as a persistent equilibrium offset from the page's initial surface.
    f64 authoredElevationOffsetMeters{0.0};

    // M04 protection coefficient [0,1]. Protection scales erosion only;
    // tectonic uplift/subsidence remains geological forcing.
    f64 protection{0.0};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct StreamPowerCellResult
{
    f32 initialSurfaceHeightMeters{0.0F};
    f32 finalSurfaceHeightMeters{0.0F};

    f32 cumulativeBedrockDisplacementMeters{0.0F};
    f32 cumulativeIncisionMeters{0.0F};

    f32 lastStreamPowerMetersPerIteration{0.0F};
    f32 lastSlope{0.0F};
    f32 lastErodibility{0.0F};

    f64 finalDrainageAreaSquareMeters{0.0};
    f64 finalDischargeCubicMetersPerSecond{0.0};
};

struct StreamPowerErosionResult
{
    terrain::PhysicalTerrainPageKey sourcePage{};
    u64 revision{0};
    u32 resolution{0};
    f64 spacingMeters{0.0};

    std::vector<StreamPowerCellResult> cells;

    [[nodiscard]] const StreamPowerCellResult& At(
        u32 x,
        u32 y) const;
};

struct StreamPowerBakeSummary
{
    f64 removedMassKg{0.0};
    f64 totalIncisionMeters{0.0};
    f64 totalBedrockDisplacementMeters{0.0};
};

// Samples M05/M04 forcing at the canonical M01 positions of one physical page.
// The page resolution is interpreted as including both tile edges, matching
// the seam-sharing physical-page convention used by M09.
[[nodiscard]] std::vector<StreamPowerCellForcing>
BuildStreamPowerForcing(
    const terrain_material_column::MaterialColumnPage& materialColumn,
    const terrain::PhysicalTerrainPageKey& sourcePage,
    const world::PlanetDefinition& planet,
    const terrain_macro_geology::MacroGeologyField& macroGeology);

// Stable derived-state identity. Camera/cache residency is absent by design.
[[nodiscard]] u64 StreamPowerRevisionFingerprint(
    const terrain::PhysicalTerrainPageKey& sourcePage,
    u64 geologyRevision,
    u64 drainageHaloRevision,
    std::span<const StreamPowerCellForcing> forcing,
    const StreamPowerErosionConfig& config) noexcept;

// Iterates large-scale stream-power equilibrium. Every iteration re-runs M09
// drainage on the evolving physical material-column copy; no droplet/local
// hydraulic simulation is involved.
[[nodiscard]] StreamPowerErosionResult SolveStreamPowerErosion(
    const terrain_material_column::MaterialColumnPage& materialColumn,
    const terrain::PhysicalTerrainPageKey& sourcePage,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    std::span<const terrain_hydrology::DrainageCellInput> drainageInputs,
    const terrain_hydrology::DrainagePageHalo& drainageHalo,
    std::span<const StreamPowerCellForcing> forcing,
    const StreamPowerErosionConfig& config = {});

// Bakes the solved geological displacement first, then top-down M08 incision.
// This preserves loose-before-bedrock erosion and keeps tectonic displacement
// out of the excavation mass ledger.
[[nodiscard]] StreamPowerBakeSummary ApplyStreamPowerErosionResult(
    terrain_material_column::MaterialColumnPage& materialColumn,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    const StreamPowerErosionResult& result);
} // namespace orbit::terrain_erosion
