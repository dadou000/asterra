#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/terrain_geology/GeologicalMaterial.hpp>
#include <orbit/terrain_material_column/MaterialColumnPage.hpp>

#include <span>
#include <vector>

namespace orbit::terrain_erosion
{
struct ThermalErosionConfig
{
    u32 maximumIterations{96};

    f64 sandReposeDegrees{33.0};
    f64 debrisReposeDegrees{40.0};
    f64 regolithReposeDegrees{37.0};
    f64 soilReposeDegrees{46.0};

    // Exposed bedrock is not a loose granular material. It only fails above a
    // separate rock-slope threshold, further modulated by M02 fracture
    // tendency, hardness and cohesion.
    f64 minimumBedrockFailureDegrees{58.0};
    f64 bedrockFailureAngleRangeDegrees{24.0};
    f64 bedrockFractureRate{0.20};

    // Fraction of the geometric excess removed per local relaxation step.
    f64 relaxation{0.50};
    f64 maximumTransferDepthPerIterationMeters{0.50};
    f64 convergenceDepthMeters{1.0e-5};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct ThermalCellState
{
    f32 maximumSlopeDegrees{0.0F};
    f32 activeReposeDegrees{0.0F};

    f64 movedOutKg{0.0};
    f64 receivedKg{0.0};
    f64 producedDebrisKg{0.0};

    bool unstable{false};
};

struct ThermalMassBalance
{
    f64 initialLooseMassKg{0.0};
    f64 finalLooseMassKg{0.0};

    // Bedrock converted into talus during this M12 solve.
    f64 fracturedBedrockMassKg{0.0};

    f64 materialBalanceErrorKg{0.0};
    f64 materialBalanceRelativeError{0.0};
};

struct ThermalErosionResult
{
    terrain_material_column::MaterialColumnPage material;
    std::vector<ThermalCellState> cells;

    u32 iterationsExecuted{0};
    bool converged{false};

    ThermalMassBalance massBalance{};

    [[nodiscard]] const ThermalCellState& At(
        u32 x,
        u32 y) const;
};

// protection is optional N*N [0,1]. A value of 1 preserves the cell from
// source-side thermal failure; it may still receive material from neighbors.
[[nodiscard]] ThermalErosionResult SimulateThermalErosion(
    terrain_material_column::MaterialColumnPage material,
    const terrain_geology::GeologicalMaterialLibrary& geology,
    std::span<const f32> protection = {},
    const ThermalErosionConfig& config = {});
} // namespace orbit::terrain_erosion
