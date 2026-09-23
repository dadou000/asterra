#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/volume_representation/VolumeCache.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <functional>
#include <map>
#include <vector>

namespace orbit::volume_representation
{
// M38 deliberately publishes transport-neutral requests. Particle systems,
// terrain, meshes and material/wetness systems consume these requests without
// making the volume solver depend on any one presentation or surface backend.
struct VolumeOutputSettings
{
    bool particlesEnabled{false};
    bool surfaceDepositsEnabled{false};

    f32 fieldThreshold{0.15F};

    f32 particleRatePerSecond{64.0F};
    u32 particleBudgetPerStep{256U};

    f32 surfaceDepositRatePerSecond{24.0F};
    u32 surfaceDepositBudgetPerStep{128U};
    f32 surfaceDepositRadiusMeters{0.25F};

    // Rejection sampling remains explicitly bounded even when only a tiny
    // fraction of a volume is above the selected output threshold.
    u32 candidateMultiplier{8U};
};

struct VolumeOutputFieldSample
{
    f32 density{0.0F};
    f32 emission{0.0F};
    math::Float3 velocity{};
};

struct VolumeParticleSpawnRequest
{
    u64 eventId{0U};
    math::Double3 positionMeters{};
    math::Float3 velocity{};
    f32 authority{0.0F};
    f32 density{0.0F};
    f32 emission{0.0F};
};

struct VolumeSurfaceDepositRequest
{
    u64 eventId{0U};

    // This is the field sample location, not a pre-resolved terrain point.
    // The consumer projects/raycasts to the appropriate physical surface in
    // its own body/frame context. This avoids assuming a global -Y gravity.
    math::Double3 samplePositionMeters{};
    math::Float3 transportVelocity{};
    f64 maximumProjectionDistanceMeters{0.0};

    f32 radiusMeters{0.25F};
    f32 amount{0.0F};
    f32 density{0.0F};
    f32 emission{0.0F};
};

struct VolumeOutputDiagnostics
{
    u32 requestedParticles{0U};
    u32 emittedParticles{0U};
    u32 requestedSurfaceDeposits{0U};
    u32 emittedSurfaceDeposits{0U};
    u32 candidatesTested{0U};
    u32 thresholdRejected{0U};
    u32 particleBudgetDropped{0U};
    u32 surfaceBudgetDropped{0U};
    f64 deltaSeconds{0.0};
};

struct VolumeOutputBatch
{
    scene::ObjectId volume{};
    std::vector<VolumeParticleSpawnRequest> particles;
    std::vector<VolumeSurfaceDepositRequest> surfaceDeposits;
    VolumeOutputDiagnostics diagnostics{};
};

using VolumeOutputSampler =
    std::function<VolumeOutputFieldSample(f64 u, f64 v, f64 w)>;

class VolumeOutputCouplingService
{
public:
    [[nodiscard]] VolumeOutputSettings& Settings(
        scene::ObjectId volume);

    // Advance from any field authority. Live GPU solvers can feed this seam
    // through a compact producer/readback path later without changing particle
    // or surface consumers.
    [[nodiscard]] const VolumeOutputBatch& Advance(
        const world_model::ResolvedVolumeDomain& domain,
        const VolumeOutputSampler& sampler,
        f64 deltaSeconds);

    // M37 is the first CPU-readable authoritative field source, so M38 ships a
    // direct cache adapter rather than pretending GPU-only live fields are CPU
    // readable.
    [[nodiscard]] const VolumeOutputBatch& AdvanceBaked(
        const world_model::ResolvedVolumeDomain& domain,
        const VolumeCacheData& cache,
        f64 deltaSeconds);

    [[nodiscard]] const VolumeOutputBatch* Latest(
        scene::ObjectId volume) const noexcept;

    void Reset(scene::ObjectId volume) noexcept;
    void RemoveMissing(const scene::ObjectStore& objects);

private:
    struct Entry
    {
        VolumeOutputSettings settings{};
        f64 particleCarry{0.0};
        f64 surfaceCarry{0.0};
        u64 particleSequence{0U};
        u64 surfaceSequence{0U};
        VolumeOutputBatch latest{};
    };

    std::map<scene::ObjectId, Entry> entries_;
};

[[nodiscard]] VolumeOutputCouplingService& VolumeOutputs() noexcept;
} // namespace orbit::volume_representation
