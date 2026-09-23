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
    // than render/UI cadence. At M38, attached M37 caches are the readable
    // authority. Live GPU producers can be attached behind the same coupling
    // seam without changing downstream consumers.
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

class VolumeParticleRequestQueue final :
    public VolumeParticleOutputSink
{
public:
    void SubmitParticleSpawns(
        scene::ObjectId volume,
        std::span<const VolumeParticleSpawnRequest> requests) override;

    [[nodiscard]] std::span<const VolumeParticleSpawnRequest>
    Pending() const noexcept;

    void Clear() noexcept;

private:
    std::vector<VolumeParticleSpawnRequest> pending_;
};

class VolumeSurfaceRequestQueue final :
    public VolumeSurfaceOutputSink
{
public:
    void SubmitSurfaceDeposits(
        scene::ObjectId volume,
        std::span<const VolumeSurfaceDepositRequest> requests) override;

    [[nodiscard]] std::span<const VolumeSurfaceDepositRequest>
    Pending() const noexcept;

    void Clear() noexcept;

private:
    std::vector<VolumeSurfaceDepositRequest> pending_;
};

[[nodiscard]] VolumeOutputRuntime& VolumeOutputRuntimeService() noexcept;
[[nodiscard]] VolumeParticleRequestQueue& VolumeParticleRequests() noexcept;
[[nodiscard]] VolumeSurfaceRequestQueue& VolumeSurfaceRequests() noexcept;
} // namespace orbit::volume_representation
