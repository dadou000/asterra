#pragma once

#include <orbit/studio_session/VolumeParticleOutputState.hpp>
#include <orbit/volume_render/VolumeParticleGpuBinding.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace orbit::studio_ui
{
[[nodiscard]] inline std::vector<volume_render::VolumeParticleGpuSpawn>
BuildVolumeParticleRenderBatch(
    const std::span<const studio_session::VolumeParticleRuntimeEvent> events,
    const math::Double3 presentationOriginMeters,
    const u32 maximumCount =
        volume_render::VolumeParticleGpuBinding::MaximumSpawnCount)
{
    struct Candidate
    {
        const studio_session::VolumeParticleRuntimeEvent* event{nullptr};
        f32 authority{0.0F};
    };

    std::vector<Candidate> candidates;
    candidates.reserve(events.size());
    for (const auto& event : events)
    {
        const auto& request = event.request;
        const f32 authority = std::isfinite(request.authority)
            ? std::max(request.authority, 0.0F)
            : 0.0F;
        candidates.push_back({&event, authority});
    }

    if (candidates.size() > maximumCount)
    {
        std::partial_sort(
            candidates.begin(),
            candidates.begin() + maximumCount,
            candidates.end(),
            [](const Candidate& a, const Candidate& b)
            {
                if (a.authority != b.authority)
                {
                    return a.authority > b.authority;
                }
                return a.event->request.eventId < b.event->request.eventId;
            });
        candidates.resize(maximumCount);
    }

    std::vector<volume_render::VolumeParticleGpuSpawn> result;
    result.reserve(candidates.size());
    for (const auto& candidate : candidates)
    {
        const auto& request = candidate.event->request;
        const auto relative = math::Double3{
            request.positionMeters.x - presentationOriginMeters.x,
            request.positionMeters.y - presentationOriginMeters.y,
            request.positionMeters.z - presentationOriginMeters.z
        };

        result.push_back({
            .positionMeters = {
                static_cast<f32>(relative.x),
                static_cast<f32>(relative.y),
                static_cast<f32>(relative.z)},
            .authority = candidate.authority,
            .velocityMetersPerSecond = request.velocity,
            .density = std::isfinite(request.density)
                ? std::max(request.density, 0.0F)
                : 0.0F,
            .emission = std::isfinite(request.emission)
                ? std::max(request.emission, 0.0F)
                : 0.0F
        });
    }

    return result;
}
} // namespace orbit::studio_ui
