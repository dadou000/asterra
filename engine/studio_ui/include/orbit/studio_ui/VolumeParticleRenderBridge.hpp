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
                    return a.authority > b.authority;
                return a.event->request.eventId < b.event->request.eventId;
            });
        candidates.resize(maximumCount);
    }

    std::vector<volume_render::VolumeParticleGpuSpawn> result;
    result.reserve(candidates.size());
    for (const auto& candidate : candidates)
    {
        const auto& event = *candidate.event;
        const auto& request = event.request;
        const auto relative = math::Double3{
            request.positionMeters.x - presentationOriginMeters.x,
            request.positionMeters.y - presentationOriginMeters.y,
            request.positionMeters.z - presentationOriginMeters.z
        };
        const math::Double3 bodyCenterRelative{
            -presentationOriginMeters.x,
            -presentationOriginMeters.y,
            -presentationOriginMeters.z
        };

        const u32 behaviorFlags =
            (static_cast<u32>(request.gravityMode) & 0x3U) |
            ((static_cast<u32>(request.collisionMode) & 0x3U) << 2U);

        result.push_back({
            .positionMeters = {
                static_cast<f32>(relative.x),
                static_cast<f32>(relative.y),
                static_cast<f32>(relative.z)},
            .authority = candidate.authority,
            .velocityMetersPerSecond = request.velocity,
            .density = std::isfinite(request.density) ? std::max(request.density, 0.0F) : 0.0F,
            .emission = std::isfinite(request.emission) ? std::max(request.emission, 0.0F) : 0.0F,
            .lifetimeSeconds = std::isfinite(request.lifetimeSeconds) ? std::max(request.lifetimeSeconds, 0.001F) : 2.0F,
            .linearDragPerSecond = std::isfinite(request.linearDragPerSecond) ? std::max(request.linearDragPerSecond, 0.0F) : 0.0F,
            .radiusMeters = std::isfinite(request.radiusMeters) ? std::max(request.radiusMeters, 0.001F) : 0.08F,
            .emissionScale = std::isfinite(request.emissionScale) ? std::max(request.emissionScale, 0.0F) : 1.0F,
            .gravityScale = std::isfinite(request.gravityScale) ? std::max(request.gravityScale, 0.0F) : 1.0F,
            .restitution = std::isfinite(request.restitution) ? std::clamp(request.restitution, 0.0F, 1.0F) : 0.25F,
            .behaviorFlags = behaviorFlags,
            .baseColor = request.baseColor,
            .emissionColor = request.emissionColor,
            .bodyCenterMeters = {
                static_cast<f32>(bodyCenterRelative.x),
                static_cast<f32>(bodyCenterRelative.y),
                static_cast<f32>(bodyCenterRelative.z)},
            .gravitationalParameterM3PerS2 =
                std::isfinite(event.physics.gravitationalParameterM3PerS2)
                    ? static_cast<f32>(std::max(event.physics.gravitationalParameterM3PerS2, 0.0))
                    : 0.0F,
            .surfaceRadiiMeters = {
                static_cast<f32>(std::max(event.physics.surfaceRadiiMeters.x, 0.0)),
                static_cast<f32>(std::max(event.physics.surfaceRadiiMeters.y, 0.0)),
                static_cast<f32>(std::max(event.physics.surfaceRadiiMeters.z, 0.0))},
            .gravitySofteningMeters =
                std::isfinite(event.physics.gravitySofteningMeters)
                    ? static_cast<f32>(std::max(event.physics.gravitySofteningMeters, 0.0))
                    : 0.0F,
            .physicalSurfaceEnabled = event.physics.hasPhysicalSurface ? 1.0F : 0.0F
        });
    }

    return result;
}
} // namespace orbit::studio_ui
