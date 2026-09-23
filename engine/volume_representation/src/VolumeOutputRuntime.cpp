#include <orbit/volume_representation/VolumeOutputRuntime.hpp>

#include <algorithm>
#include <chrono>
#include <utility>

namespace orbit::volume_representation
{
namespace
{
void DiscoverRecursive(
    const scene::ObjectStore& objects,
    const scene::ObjectRecord& record,
    std::vector<scene::ObjectRecord>& volumes)
{
    if (record.type == world_model::kVolumeType)
    {
        volumes.push_back(record);
    }

    for (const auto& child : objects.Children(record.id))
    {
        DiscoverRecursive(objects, child, volumes);
    }
}
} // namespace

void VolumeOutputRuntime::SetParticleSink(
    VolumeParticleOutputSink* const sink) noexcept
{
    particleSink_ = sink;
}

void VolumeOutputRuntime::SetSurfaceSink(
    VolumeSurfaceOutputSink* const sink) noexcept
{
    surfaceSink_ = sink;
}

VolumeOutputRuntimeDiagnostics VolumeOutputRuntime::TickWorld(
    const scene::ObjectStore& objects,
    const time::SimulationTime atTime)
{
    diagnostics_ = {};
    diagnostics_.firstTick = !lastTime_.has_value();

    f64 deltaSeconds = 0.0;

    if (lastTime_.has_value())
    {
        const auto elapsed =
            atTime - *lastTime_;

        if (elapsed.count() < 0)
        {
            diagnostics_.timeReversed = true;
            VolumeOutputs().RemoveMissing(objects);

            for (const auto& record : DiscoverVolumes(objects))
            {
                VolumeOutputs().Reset(record.id);
            }
        }
        else
        {
            deltaSeconds =
                std::chrono::duration<f64>(elapsed).count();
        }
    }

    lastTime_ = atTime;
    diagnostics_.deltaSeconds = deltaSeconds;

    VolumeOutputs().RemoveMissing(objects);
    VolumeCaches().RemoveMissing(objects);

    const auto volumes = DiscoverVolumes(objects);
    diagnostics_.discoveredVolumes =
        static_cast<u32>(volumes.size());

    for (const auto& record : volumes)
    {
        const auto domain =
            world_model::ResolveVolumeDomain(
                objects,
                record.id);

        if (!domain.has_value() || !domain->enabled)
        {
            continue;
        }

        auto& settings = VolumeOutputs().Settings(record.id);
        settings.particlesEnabled = domain->outputParticlesEnabled;
        settings.surfaceDepositsEnabled = domain->outputSurfaceDepositsEnabled;
        settings.fieldThreshold = domain->outputFieldThreshold;
        settings.particleRatePerSecond = domain->outputParticleRatePerSecond;
        settings.particleBudgetPerStep = domain->outputParticleBudgetPerStep;
        settings.particleLifetimeSeconds = domain->particleLifetimeSeconds;
        settings.particleLinearDragPerSecond = domain->particleLinearDragPerSecond;
        settings.particleRadiusMeters = domain->particleRadiusMeters;
        settings.particleEmissionScale = domain->particleEmissionScale;
        settings.particleGravityMode = domain->particleGravityMode;
        settings.particleGravityScale = domain->particleGravityScale;
        settings.particleCollisionMode = domain->particleCollisionMode;
        settings.particleRestitution = domain->particleRestitution;
        settings.surfaceDepositRatePerSecond = domain->outputSurfaceDepositRatePerSecond;
        settings.surfaceDepositBudgetPerStep = domain->outputSurfaceDepositBudgetPerStep;
        settings.surfaceDepositRadiusMeters = domain->outputSurfaceDepositRadiusMeters;
        settings.candidateMultiplier = domain->outputCandidateMultiplier;
        settings.surfaceEffect = static_cast<VolumeSurfaceEffect>(static_cast<u8>(domain->outputSurfaceEffect));
        settings.surfaceEffectHalfLifeSeconds = domain->outputSurfaceEffectHalfLifeSeconds;

        if (!settings.particlesEnabled &&
            !settings.surfaceDepositsEnabled)
        {
            continue;
        }

        ++diagnostics_.eligibleVolumes;

        const auto* cache =
            VolumeCaches().Find(record.id);

        if (cache == nullptr)
        {
            ++diagnostics_.volumesWithoutReadableAuthority;
            continue;
        }

        const auto inputs =
            world_model::ResolveVolumeInputs(
                objects,
                record.id);

        const VolumeCacheBakeSettings cacheSettings{
            .resolution = cache->descriptor.resolutionX,
            .fieldMask = cache->descriptor.fieldMask
        };

        if (!IsVolumeCacheCurrent(
                *cache,
                *domain,
                inputs,
                cacheSettings))
        {
            ++diagnostics_.volumesWithStaleAuthority;
            VolumeOutputs().Reset(record.id);
            continue;
        }

        const auto& batch =
            VolumeOutputs().AdvanceBaked(
                *domain,
                *cache,
                deltaSeconds);

        ++diagnostics_.advancedVolumes;
        diagnostics_.dispatchedParticleRequests +=
            static_cast<u32>(batch.particles.size());
        diagnostics_.dispatchedSurfaceRequests +=
            static_cast<u32>(batch.surfaceDeposits.size());

        DispatchVolumeOutputs(
            batch,
            particleSink_,
            surfaceSink_);
    }

    return diagnostics_;
}

const VolumeOutputRuntimeDiagnostics&
VolumeOutputRuntime::Diagnostics() const noexcept
{
    return diagnostics_;
}

void VolumeOutputRuntime::Reset() noexcept
{
    lastTime_.reset();
    diagnostics_ = {};
}

std::vector<scene::ObjectRecord>
VolumeOutputRuntime::DiscoverVolumes(
    const scene::ObjectStore& objects)
{
    std::vector<scene::ObjectRecord> volumes;

    for (const auto& root : objects.Roots())
    {
        DiscoverRecursive(objects, root, volumes);
    }

    std::sort(
        volumes.begin(),
        volumes.end(),
        [](const auto& a, const auto& b)
        {
            if (a.sortOrder != b.sortOrder)
            {
                return a.sortOrder < b.sortOrder;
            }

            if (a.id.high != b.id.high)
            {
                return a.id.high < b.id.high;
            }

            return a.id.low < b.id.low;
        });

    return volumes;
}

void VolumeParticleRequestQueue::SubmitParticleSpawns(
    const scene::ObjectId volume,
    const std::span<const VolumeParticleSpawnRequest> requests)
{
    pending_.reserve(
        pending_.size() + requests.size());

    for (const auto& request : requests)
    {
        pending_.push_back({
            .volume = volume,
            .request = request
        });
    }
}

std::span<const VolumeParticleQueuedRequest>
VolumeParticleRequestQueue::Pending() const noexcept
{
    return pending_;
}

std::vector<VolumeParticleQueuedRequest>
VolumeParticleRequestQueue::Drain() noexcept
{
    return std::exchange(
        pending_,
        std::vector<VolumeParticleQueuedRequest>{});
}

void VolumeParticleRequestQueue::Clear() noexcept
{
    pending_.clear();
}

void VolumeSurfaceRequestQueue::SubmitSurfaceDeposits(
    const scene::ObjectId volume,
    const std::span<const VolumeSurfaceDepositRequest> requests)
{
    pending_.reserve(
        pending_.size() + requests.size());

    for (const auto& request : requests)
    {
        pending_.push_back({
            .volume = volume,
            .request = request
        });
    }
}

std::span<const VolumeSurfaceQueuedRequest>
VolumeSurfaceRequestQueue::Pending() const noexcept
{
    return pending_;
}

std::vector<VolumeSurfaceQueuedRequest>
VolumeSurfaceRequestQueue::Drain() noexcept
{
    return std::exchange(
        pending_,
        std::vector<VolumeSurfaceQueuedRequest>{});
}

void VolumeSurfaceRequestQueue::Clear() noexcept
{
    pending_.clear();
}

VolumeOutputRuntime& VolumeOutputRuntimeService() noexcept
{
    static VolumeOutputRuntime runtime;
    return runtime;
}

VolumeParticleRequestQueue& VolumeParticleRequests() noexcept
{
    static VolumeParticleRequestQueue queue;
    return queue;
}

VolumeSurfaceRequestQueue& VolumeSurfaceRequests() noexcept
{
    static VolumeSurfaceRequestQueue queue;
    return queue;
}
} // namespace orbit::volume_representation
