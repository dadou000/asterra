#pragma once

#include <orbit/studio_session/VolumeSurfaceEffectState.hpp>
#include <orbit/terrain_render/SurfaceEffects.hpp>

#include <algorithm>
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

// Builds a bounded GPU-facing snapshot for one body. Strongest effects are
// retained first when the runtime state exceeds the render upload budget.
// Runtime authority is untouched; this is a presentation snapshot only.
[[nodiscard]] inline VolumeSurfaceEffectRenderBatch
BuildVolumeSurfaceEffectRenderBatch(
    const universe::BodyId body,
    const std::span<const studio_session::VolumeSurfaceEffectStamp> source,
    const u32 maximumGpuStamps = 512U)
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
        result.stamps.push_back({
            .bodyLocalPointMeters = {
                static_cast<f32>(stamp.bodyLocalSurfacePointMeters.x),
                static_cast<f32>(stamp.bodyLocalSurfacePointMeters.y),
                static_cast<f32>(stamp.bodyLocalSurfacePointMeters.z)
            },
            .radiusMeters = stamp.radiusMeters,
            .amount = stamp.amount,
            .effect = ToRenderEffect(stamp.effect)
        });
    }

    result.droppedForRenderBudget =
        static_cast<u32>(candidates.size() - count);
    return result;
}
} // namespace orbit::studio_ui
