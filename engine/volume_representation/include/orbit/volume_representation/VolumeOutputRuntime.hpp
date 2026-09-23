#pragma once

#include <orbit/time/SimulationTime.hpp>
#include <orbit/volume_representation/VolumeOutputCoupling.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <optional>
#include <vector>

namespace orbit::volume_representation
{
struct VolumeOutputRuntimeDiagnostics
{
    u32 discoveredVolumes{0U};
    u32 eligibleVolumes{0U};
    u32 advancedVolumes{0U};
    u32 volumesWithoutReadableAuthority{0U};
    u32 volumesWithStaleAuthority{0U};
    u32 dispatchedParticleRequests{0U};
    u32 dispatchedSurfaceRequests{0U};
    f64 deltaSeconds{0.0};
    bool firstTick{true};
    bool timeReversed{false};
};

class VolumeOutputRuntime
{
public:
    void SetParticleSink(
        VolumeParticleOutputSink* sink) noexcept;

    void SetSurfaceSink(
        VolumeSurfaceOutputSink* sink) noexcept;

    // Advances every authored Volume exactly once from simulation time rather
    // than render/UI cadence. At M38, a current M37 cache is the CPU-readable
    // authority. Stale caches are rejected rather than silently emitting from
    // authored state that no longer matches the world.
    [[nodiscard]] VolumeOutputRuntimeDiagnostics TickWorld(
        const scene::ObjectStore& objects,
        time::SimulationTime atTime);

    [[nodiscard]] const VolumeOutputRuntimeDiagnostics&
    Diagnostics() const noexcept;

    void Reset() noexcept;

private:
    [[nodiscard]] static std::vector<scene::ObjectRecord>
    DiscoverVolumes(const scene::ObjectStore& objects);

    VolumeParticleOutputSink* particleSink_{nullptr};
    VolumeSurfaceOutputSink* surfaceSink_{nullptr};
    std::optional<time::SimulationTime> lastTime_;
    VolumeOutputRuntimeDiagnostics diagnostics_{};
};

struct VolumeParticleQueuedRequest
{
    scene::ObjectId volume{};
    VolumeParticleSpawnRequest request{};
};

struct VolumeSurfaceQueuedRequest
{
    scene::ObjectId volume{};
    VolumeSurfaceDepositRequest request{};
};

class VolumeParticleRequestQueue final :
    public VolumeParticleOutputSink
{
public:
    void SubmitParticleSpawns(
        scene::ObjectId volume,
        std::span<const VolumeParticleSpawnRequest> requests) override;

    [[nodiscard]] std::span<const VolumeParticleQueuedRequest>
    Pending() const noexcept;

    // Transfers the complete pending batch to its consumer and atomically
    // leaves this queue empty. This is the preferred frame handoff: consumers
    // cannot accidentally observe requests twice and producers never need to
    // clear data before it has been consumed.
    [[nodiscard]] std::vector<VolumeParticleQueuedRequest>
    Drain() noexcept;

    void Clear() noexcept;

private:
    std::vector<VolumeParticleQueuedRequest> pending_;
};

class VolumeSurfaceRequestQueue final :
    public VolumeSurfaceOutputSink
{
public:
    void SubmitSurfaceDeposits(
        scene::ObjectId volume,
        std::span<const VolumeSurfaceDepositRequest> requests) override;

    [[nodiscard]] std::span<const VolumeSurfaceQueuedRequest>
    Pending() const noexcept;

    // Same ownership-transfer semantics as the particle queue. A physical
    // surface resolver can drain once, project each request in the appropriate
    // body/frame, and retain the resulting material/deposition work itself.
    [[nodiscard]] std::vector<VolumeSurfaceQueuedRequest>
    Drain() noexcept;

    void Clear() noexcept;

private:
    std::vector<VolumeSurfaceQueuedRequest> pending_;
};

[[nodiscard]] VolumeOutputRuntime& VolumeOutputRuntimeService() noexcept;
[[nodiscard]] VolumeParticleRequestQueue& VolumeParticleRequests() noexcept;
[[nodiscard]] VolumeSurfaceRequestQueue& VolumeSurfaceRequests() noexcept;
} // namespace orbit::volume_representation
