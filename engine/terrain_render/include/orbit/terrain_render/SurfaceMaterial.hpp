#pragma once

#include <orbit/surface_model/SurfaceMaterialResolver.hpp>

#include <array>

namespace orbit::terrain_render
{
struct SurfaceMaterialRenderInput
{
    static constexpr std::size_t MaterialCount = 9U;

    std::array<f32, MaterialCount> weights{};
    terrain_geology::RockTypeId exposedBedrock{};

    [[nodiscard]] f32 TotalWeight() const noexcept;
};

[[nodiscard]] SurfaceMaterialRenderInput MakeSurfaceMaterialRenderInput(
    const surface_model::ResolvedSurfaceMaterialBlend& blend);
} // namespace orbit::terrain_render
