#pragma once

#include <orbit/terrain_biome/BiomeService.hpp>
#include <orbit/terrain_material_column/SurfaceResolver.hpp>

#include <span>
#include <vector>

namespace orbit::surface_model
{
enum class RenderedSurfaceMaterialKind : u8
{
    Bedrock,
    Regolith,
    Soil,
    Sand,
    Debris,
    Snow,
    Moss,
    Litter,
    Dust
};

struct SurfaceMaterialFeatureMasks
{
    f32 slopeDegrees{0.0F};
    f32 curvature{0.0F};

    f32 snowCoverage{0.0F};
    f32 mossPotential{0.0F};
    f32 litterAvailability{0.0F};
    f32 dustAvailability{0.0F};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct SurfaceMaterialContribution
{
    RenderedSurfaceMaterialKind kind{
        RenderedSurfaceMaterialKind::Bedrock};

    f32 weight{0.0F};

    terrain_geology::RockTypeId rock{};

    [[nodiscard]] bool IsValid() const noexcept;
};

struct ResolvedSurfaceMaterialBlend
{
    std::vector<SurfaceMaterialContribution> contributions;

    [[nodiscard]] f32 Weight(
        RenderedSurfaceMaterialKind kind) const noexcept;

    [[nodiscard]] f32 TotalWeight() const noexcept;
    [[nodiscard]] bool IsValid() const noexcept;
};

[[nodiscard]] ResolvedSurfaceMaterialBlend ResolveSurfaceMaterialBlend(
    const terrain_material_column::ExposedSurfaceState& physicalSurface,
    const terrain_biome::BiomeService& biomeService,
    std::span<const terrain_biome::ResolvedBiomeWeight> biomeWeights,
    const SurfaceMaterialFeatureMasks& features);
} // namespace orbit::surface_model
