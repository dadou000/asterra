#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/math/Vector.hpp>

#include <array>
#include <span>

namespace orbit::terrain_render
{
enum class SurfaceEffectKind : u32
{
    Wetness = 0U,
    Soot = 1U,
    Ash = 2U,
    Sediment = 3U,
    Heat = 4U
};

struct SurfaceEffectGpuStamp
{
    math::Float3 bodyLocalPointMeters{};
    f32 radiusMeters{0.25F};
    f32 amount{0.0F};
    SurfaceEffectKind effect{SurfaceEffectKind::Wetness};
    f32 reserved0{0.0F};
    f32 reserved1{0.0F};
};

static_assert(sizeof(SurfaceEffectGpuStamp) == 32U);

struct SurfaceEffectInfluence
{
    f32 wetness{0.0F};
    f32 soot{0.0F};
    f32 ash{0.0F};
    f32 sediment{0.0F};
    f32 heat{0.0F};
};

struct SurfacePbrState
{
    math::Float3 baseColor{1.0F, 1.0F, 1.0F};
    f32 roughness{0.82F};
    f32 metallic{0.0F};
    math::Float3 emission{};
};

[[nodiscard]] SurfaceEffectInfluence EvaluateSurfaceEffects(
    math::Float3 bodyLocalPointMeters,
    std::span<const SurfaceEffectGpuStamp> stamps) noexcept;

[[nodiscard]] SurfacePbrState ApplySurfaceEffects(
    const SurfacePbrState& base,
    const SurfaceEffectInfluence& influence) noexcept;
} // namespace orbit::terrain_render
