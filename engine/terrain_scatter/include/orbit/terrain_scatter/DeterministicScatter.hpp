#pragma once

#include <orbit/core/StrongId.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/terrain_material_column/SurfaceResolver.hpp>
#include <orbit/world/Planet.hpp>

#include <span>
#include <vector>

namespace orbit::terrain_scatter
{
struct DerivedScatterInstanceIdTag;
using DerivedScatterInstanceId =
    core::StrongId<DerivedScatterInstanceIdTag>;

struct ScatterPageIdentity
{
    world::PlanetId planet{};
    world::PlanetTileId tile{};

    // Revisions that affect the placement inputs/rules. They participate in
    // the page key so changing source data intentionally produces a new
    // derived population, while streaming the same page does not.
    u64 sourceRevision{0};
    u64 scatterRevision{0};

    u64 generationSeed{0};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct ScatterCellInput
{
    // Resolved M20 weight of the biome that owns the rule being evaluated.
    f32 biomeWeight{0.0F};

    terrain_material_column::ExposedSurfaceKind exposedMaterial{
        terrain_material_column::ExposedSurfaceKind::Bedrock};

    f32 slopeDegrees{0.0F};
    f32 soilDepthMeters{0.0F};
    f32 moisture{0.0F};

    // 0 = no exclusion, 1 = fully excluded.
    f32 exclusionMask{0.0F};

    // Persistent authored multiplier supplied by biome/authoring masks.
    f32 authoredDensity{1.0F};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct ScatterPageRequest
{
    ScatterPageIdentity identity{};

    u32 gridResolution{0};

    // Planting grid cell width in meters. The production request should use
    // the rule's minimum spacing. Larger cells remain valid and simply reduce
    // the maximum candidate density.
    f32 cellSizeMeters{1.0F};

    terrain_biome::BiomeScatterLayerRule rule{};

    // Owning biome's M19/M20 global scatter multiplier.
    f32 biomeDensityMultiplier{1.0F};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct DerivedScatterInstance
{
    DerivedScatterInstanceId id{};

    terrain_biome::BiomeScatterKind kind{
        terrain_biome::BiomeScatterKind::Grass};

    // Tangent-page local east/north offset. Height/up orientation is sampled
    // by the consumer from the canonical terrain surface.
    math::Double2 localOffsetMeters{};

    f32 yawRadians{0.0F};
    f32 uniformScale{1.0F};

    u32 cellX{0};
    u32 cellY{0};

    [[nodiscard]] bool IsValid() const noexcept;
};

// Stable 32-bit page key shared with the M22 GPU shader.
[[nodiscard]] u32 ScatterPageHash(
    const ScatterPageIdentity& identity) noexcept;

[[nodiscard]] u32 ScatterRuleHash(
    const terrain_biome::BiomeScatterLayerRule& rule) noexcept;

// CPU/reference implementation of the same planting-grid algorithm used by
// the M22 GPU pass. One input sample corresponds to one planting-grid cell.
[[nodiscard]] std::vector<DerivedScatterInstance>
GenerateDeterministicScatter(
    const ScatterPageRequest& request,
    std::span<const ScatterCellInput> cells);
} // namespace orbit::terrain_scatter
