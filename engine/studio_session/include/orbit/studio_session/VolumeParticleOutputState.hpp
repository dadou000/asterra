#pragma once

#include <orbit/volume_representation/VolumeOutputRuntime.hpp>

#include <span>
#include <utility>
#include <vector>

namespace orbit::studio_session
{
struct VolumeParticleRuntimeEvent
{
    scene::ObjectId sourceVolume{};
    volume_representation::VolumeParticleSpawnRequest request{};
};

struct VolumeParticleOutputDiagnostics
{
    u32 submitted{0U};
    u32 retained{0U};
    u64 generation{0U};
};

// Exactly-once frame handoff for M38 particle output. This is intentionally a
// transport/render packet rather than a particle simulator: authored particle
// lifetime, forces, collision and visual policy do not yet exist in the M38
// contract, so this layer must not invent them. A later GPU particle backend
// can consume this packet directly while preserving source-volume provenance.
class VolumeParticleOutputState
{
public:
    void Consume(
        std::vector<volume_representation::VolumeParticleQueuedRequest> requests)
    {
        diagnostics_.submitted = static_cast<u32>(requests.size());

        events_.clear();
        events_.reserve(requests.size());

        for (auto& queued : requests)
        {
            events_.push_back({
                .sourceVolume = queued.volume,
                .request = std::move(queued.request)
            });
        }

        diagnostics_.retained = static_cast<u32>(events_.size());
        ++diagnostics_.generation;
    }

    [[nodiscard]] std::span<const VolumeParticleRuntimeEvent>
    Events() const noexcept
    {
        return events_;
    }

    [[nodiscard]] const VolumeParticleOutputDiagnostics&
    Diagnostics() const noexcept
    {
        return diagnostics_;
    }

    void Clear() noexcept
    {
        events_.clear();
        diagnostics_ = {};
    }

private:
    std::vector<VolumeParticleRuntimeEvent> events_;
    VolumeParticleOutputDiagnostics diagnostics_{};
};

[[nodiscard]] inline VolumeParticleOutputState&
VolumeParticleOutputs() noexcept
{
    static VolumeParticleOutputState state;
    return state;
}
} // namespace orbit::studio_session
