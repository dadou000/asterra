#pragma once

#include <orbit/studio_session/VolumeSurfaceEffectState.hpp>
#include <orbit/terrain_render/SurfaceEffectGpuBinding.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace orbit::studio_ui
{
struct VolumeSurfaceEffectRenderBatch
{
    universe::BodyId body{};
    std::vector<terrain_render::SurfaceEffectGpuStamp> stamps;
    u32 sourceStampCount{0U};
    u32 droppedForRenderBudget{0U};
};

[[nodiscard]] inline terrain_render::SurfaceEffectKind
ToRenderEffect(
    const volume_representation::VolumeSurfaceEffect effect) noexcept
{
    using Source = volume_representation::VolumeSurfaceEffect;
    using Target = terrain_render::SurfaceEffectKind;

    switch (effect)
    {
    case Source::Wetness:
        return Target::Wetness;
    case Source::Soot:
        return Target::Soot;
    case Source::Ash:
        return Target::Ash;
    case Source::Sediment:
        return Target::Sediment;
    case Source::Heat:
        return Target::Heat;
    }

    return Target::Wetness;
}

[[nodiscard]] inline math::Float3
NormalizedRenderDirection(const math::Double3 point) noexcept
{
    const f64 lengthSquared =
        point.x * point.x +
        point.y * point.y +
        point.z * point.z;

    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-20)
    {
        return {0.0F, 1.0F, 0.0F};
    }

    const f64 inverse = 1.0 / std::sqrt(lengthSquared);
    return {
        static_cast<f32>(point.x * inverse),
        static_cast<f32>(point.y * inverse),
        static_cast<f32>(point.z * inverse)
    };
}

[[nodiscard]] inline f64
SurfacePointRadiusMeters(
    const math::Double3 point,
    const f64 fallbackRadiusMeters) noexcept
{
    const f64 lengthSquared =
        point.x * point.x +
        point.y * point.y +
        point.z * point.z;

    if (std::isfinite(lengthSquared) && lengthSquared > 1.0e-20)
    {
        return std::sqrt(lengthSquared);
    }

    if (std::isfinite(fallbackRadiusMeters))
    {
        return std::max(fallbackRadiusMeters, 0.001);
    }

    return 0.001;
}

// Builds a bounded GPU-facing snapshot for one body. Directions and angular
// radii avoid losing small footprints to float precision at planetary scale.
// Each stamp derives its angular radius from its own body-local surface radius,
// which remains correct across floating-origin shifts and better follows
// non-spherical reference surfaces than one global radius approximation.
[[nodiscard]] inline VolumeSurfaceEffectRenderBatch
BuildVolumeSurfaceEffectRenderBatch(
    const universe::BodyId body,
    const f64 referenceRadiusMeters,
    const std::span<const studio_session::VolumeSurfaceEffectStamp> source,
    const u32 maximumGpuStamps =
        terrain_render::SurfaceEffectGpuBinding::MaximumStampCount)
{
    VolumeSurfaceEffectRenderBatch result{
        .body = body
    };

    std::vector<const studio_session::VolumeSurfaceEffectStamp*> candidates;
    candidates.reserve(source.size());

    for (const auto& stamp : source)
    {
        if (stamp.body == body && stamp.amount > 0.0F)
        {
            candidates.push_back(&stamp);
        }
    }

    result.sourceStampCount = static_cast<u32>(candidates.size());

    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [](const auto* left, const auto* right)
        {
            return left->amount > right->amount;
        });

    const std::size_t count =
        std::min<std::size_t>(
            candidates.size(),
            maximumGpuStamps);
    result.stamps.reserve(count);

    for (std::size_t index = 0U; index < count; ++index)
    {
        const auto& stamp = *candidates[index];
        const f64 localRadius =
            SurfacePointRadiusMeters(
                stamp.bodyLocalSurfacePointMeters,
                referenceRadiusMeters);

        result.stamps.push_back({
            .bodyFixedDirection =
                NormalizedRenderDirection(
                    stamp.bodyLocalSurfacePointMeters),
            .angularRadiusRadians =
                static_cast<f32>(
                    std::max(
                        static_cast<f64>(stamp.radiusMeters) /
                            localRadius,
                        1.0e-9)),
            .amount = stamp.amount,
            .effect = ToRenderEffect(stamp.effect)
        });
    }

    result.droppedForRenderBudget =
        static_cast<u32>(candidates.size() - count);
    return result;
}

// Convenience path for presentation code that already has resolved body-local
// surface points. The per-stamp radius above is authoritative, so no separate
// celestial radius lookup is required.
[[nodiscard]] inline VolumeSurfaceEffectRenderBatch
BuildVolumeSurfaceEffectRenderBatch(
    const universe::BodyId body,
    const std::span<const studio_session::VolumeSurfaceEffectStamp> source,
    const u32 maximumGpuStamps =
        terrain_render::SurfaceEffectGpuBinding::MaximumStampCount)
{
    return BuildVolumeSurfaceEffectRenderBatch(
        body,
        1.0,
        source,
        maximumGpuStamps);
}
} // namespace orbit::studio_ui
