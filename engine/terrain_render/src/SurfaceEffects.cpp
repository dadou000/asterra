#include <orbit/terrain_render/SurfaceEffects.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::terrain_render
{
namespace
{
[[nodiscard]] f32 Saturate(const f32 value) noexcept
{
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] math::Float3 NormalizeSafe(
    const math::Float3 value) noexcept
{
    const f32 lengthSquared =
        value.x * value.x +
        value.y * value.y +
        value.z * value.z;

    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-20F)
    {
        return {0.0F, 1.0F, 0.0F};
    }

    const f32 inverse = 1.0F / std::sqrt(lengthSquared);
    return {
        value.x * inverse,
        value.y * inverse,
        value.z * inverse
    };
}

[[nodiscard]] f32 AngularDistance(
    const math::Float3 a,
    const math::Float3 b) noexcept
{
    const auto na = NormalizeSafe(a);
    const auto nb = NormalizeSafe(b);
    const f32 cosine =
        std::clamp(
            na.x * nb.x +
            na.y * nb.y +
            na.z * nb.z,
            -1.0F,
            1.0F);
    return std::acos(cosine);
}

[[nodiscard]] math::Float3 Lerp(
    const math::Float3 a,
    const math::Float3 b,
    const f32 t) noexcept
{
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    };
}
} // namespace

SurfaceEffectInfluence EvaluateSurfaceEffects(
    const math::Float3 bodyFixedDirection,
    const std::span<const SurfaceEffectGpuStamp> stamps) noexcept
{
    SurfaceEffectInfluence result{};

    for (const auto& stamp : stamps)
    {
        const f32 radius =
            std::isfinite(stamp.angularRadiusRadians)
                ? std::max(stamp.angularRadiusRadians, 1.0e-9F)
                : 1.0e-9F;
        const f32 amount =
            std::isfinite(stamp.amount)
                ? std::max(stamp.amount, 0.0F)
                : 0.0F;
        const f32 distance =
            AngularDistance(
                bodyFixedDirection,
                stamp.bodyFixedDirection);

        if (distance >= radius)
        {
            continue;
        }

        const f32 normalized = 1.0F - distance / radius;
        const f32 influence = amount * normalized * normalized;

        switch (stamp.effect)
        {
        case SurfaceEffectKind::Wetness:
            result.wetness += influence;
            break;
        case SurfaceEffectKind::Soot:
            result.soot += influence;
            break;
        case SurfaceEffectKind::Ash:
            result.ash += influence;
            break;
        case SurfaceEffectKind::Sediment:
            result.sediment += influence;
            break;
        case SurfaceEffectKind::Heat:
            result.heat += influence;
            break;
        }
    }

    result.wetness = Saturate(result.wetness);
    result.soot = Saturate(result.soot);
    result.ash = Saturate(result.ash);
    result.sediment = Saturate(result.sediment);
    result.heat = std::max(result.heat, 0.0F);
    return result;
}

SurfacePbrState ApplySurfaceEffects(
    const SurfacePbrState& base,
    const SurfaceEffectInfluence& influence) noexcept
{
    SurfacePbrState result = base;

    const f32 wetness = Saturate(influence.wetness);
    const f32 soot = Saturate(influence.soot);
    const f32 ash = Saturate(influence.ash);
    const f32 sediment = Saturate(influence.sediment);
    const f32 heat = std::max(influence.heat, 0.0F);

    result.baseColor =
        Lerp(
            result.baseColor,
            {
                result.baseColor.x * 0.52F,
                result.baseColor.y * 0.52F,
                result.baseColor.z * 0.52F
            },
            wetness);
    result.roughness =
        result.roughness +
        (0.12F - result.roughness) * wetness;

    result.baseColor =
        Lerp(result.baseColor, {0.018F, 0.016F, 0.014F}, soot);
    result.roughness =
        result.roughness +
        (0.94F - result.roughness) * soot;
    result.metallic *= 1.0F - soot;

    result.baseColor =
        Lerp(result.baseColor, {0.43F, 0.42F, 0.40F}, ash);
    result.roughness =
        result.roughness +
        (0.98F - result.roughness) * ash;
    result.metallic *= 1.0F - ash;

    result.baseColor =
        Lerp(result.baseColor, {0.36F, 0.23F, 0.11F}, sediment);
    result.roughness =
        result.roughness +
        (0.90F - result.roughness) * sediment;

    if (heat > 0.0F)
    {
        const f32 hot = std::min(heat, 8.0F);
        result.emission.x += 3.5F * hot;
        result.emission.y += 0.72F * hot * std::min(hot, 2.0F);
        result.emission.z += 0.08F * hot * std::max(hot - 0.35F, 0.0F);
        result.roughness =
            std::clamp(result.roughness + 0.08F * Saturate(heat), 0.02F, 1.0F);
    }

    result.roughness = std::clamp(result.roughness, 0.02F, 1.0F);
    result.metallic = Saturate(result.metallic);
    return result;
}
} // namespace orbit::terrain_render
