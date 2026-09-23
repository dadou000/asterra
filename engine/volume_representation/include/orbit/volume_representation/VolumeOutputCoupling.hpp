#pragma once

#include <orbit/math/Vector.hpp>
#include <orbit/scene/ObjectStore.hpp>
#include <orbit/volume_representation/VolumeCache.hpp>
#include <orbit/world_model/VolumeSchemas.hpp>

#include <functional>
#include <map>
#include <span>
#include <vector>

namespace orbit::volume_representation
{
// Semantic effect carried by a resolved M38 surface deposit. The volume layer
// owns only this transport-neutral classification; material/render systems
// decide how each channel changes BRDF, albedo, thermal state, particles, etc.
enum class VolumeSurfaceEffect : u8
{
    Wetness = 0U,
    Soot = 1U,
    Ash = 2U,
    Sediment = 3U,
    Heat = 4U
};

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
    f32 particleLifetimeSeconds{2.0F};
    f32 particleLinearDragPerSecond{0.0F};
    f32 particleRadiusMeters{0.08F};
    f32 particleEmissionScale{1.0F};
    world_model::VolumeParticleGravityMode particleGravityMode{world_model::VolumeParticleGravityMode::None};
    f32 particleGravityScale{1.0F};
    world_model::VolumeParticleCollisionMode particleCollisionMode{world_model::VolumeParticleCollisionMode::None};
    f32 particleRestitution{0.25F};

    f32 surfaceDepositRatePerSecond{24.0F};
    u32 surfaceDepositBudgetPerStep{128U};
    f32 surfaceDepositRadiusMeters{0.25F};

    // Rejection sampling remains explicitly bounded even when only a tiny
    // fraction of a volume is above the selected output threshold.
    u32 candidateMultiplier{8U};

    VolumeSurfaceEffect surfaceEffect{VolumeSurfaceEffect::Wetness};
    // Runtime influence decay. <= 0 means persistent until explicitly cleared.
    f32 surfaceEffectHalfLifeSeconds{30.0F};
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
    f32 lifetimeSeconds{2.0F};
    f32 linearDragPerSecond{0.0F};
    f32 radiusMeters{0.08F};
    f32 emissionScale{1.0F};
    math::Float3 baseColor{1.0F, 1.0F, 1.0F};
    math::Float3 emissionColor{1.0F, 0.32F, 0.06F};
    world_model::VolumeParticleGravityMode gravityMode{world_model::VolumeParticleGravityMode::None};
    f32 gravityScale{1.0F};
    world_model::VolumeParticleCollisionMode collisionMode{world_model::VolumeParticleCollisionMode::None};
    f32 restitution{0.25F};
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

    VolumeSurfaceEffect effect{VolumeSurfaceEffect::Wetness};
    f32 halfLifeSeconds{30.0F};
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

// Downstream systems implement only the channel they own. The volume layer
// never includes particle renderer, terrain, collision or material-system
// headers, which keeps the dependency direction one-way.
class VolumeParticleOutputSink
{
public:
    virtual ~VolumeParticleOutputSink() = default;
    virtual void SubmitParticleSpawns(
        scene::ObjectId volume,
        std::span<const VolumeParticleSpawnRequest> requests) = 0;
};

class VolumeSurfaceOutputSink
{
public:
    virtual ~VolumeSurfaceOutputSink() = default;
    virtual void SubmitSurfaceDeposits(
        scene::ObjectId volume,
        std::span<const VolumeSurfaceDepositRequest> requests) = 0;
};

void DispatchVolumeOutputs(
    const VolumeOutputBatch& batch,
    VolumeParticleOutputSink* particleSink,
    VolumeSurfaceOutputSink* surfaceSink);

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
