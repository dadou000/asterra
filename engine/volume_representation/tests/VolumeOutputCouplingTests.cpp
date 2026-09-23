#include <orbit/volume_representation/VolumeOutputCoupling.hpp>
#include <orbit/volume_representation/VolumeOutputRuntime.hpp>

#include <cstdlib>
#include <iostream>
#include <source_location>

namespace
{
void CheckOutput(
    const bool condition,
    const std::source_location location =
        std::source_location::current())
{
    if (!condition)
    {
        std::cerr
            << "Volume output coupling test failed at "
            << location.file_name()
            << ':'
            << location.line()
            << '\n';
        std::exit(1);
    }
}
} // namespace

void RunVolumeOutputCouplingTests()
{
    using namespace orbit;
    using namespace orbit::volume_representation;

    world_model::ResolvedVolumeDomain domain;
    domain.object = {.high = 0x38U, .low = 0x01U};
    domain.centerMeters = {10.0, 20.0, -5.0};
    domain.halfExtentsMeters = {4.0, 2.0, 3.0};
    domain.fieldMask =
        static_cast<u64>(world_model::VolumeField::Density) |
        static_cast<u64>(world_model::VolumeField::Emission);

    VolumeCacheData cache;
    cache.descriptor.volume = domain.object;
    cache.descriptor.resolutionX = 4U;
    cache.descriptor.resolutionY = 4U;
    cache.descriptor.resolutionZ = 4U;
    cache.descriptor.fieldMask = domain.fieldMask;
    cache.descriptor.centerMeters = domain.centerMeters;
    cache.descriptor.halfExtentsMeters = domain.halfExtentsMeters;
    cache.density.assign(64U, 1.0F);
    cache.emission.assign(64U, 0.5F);

    VolumeOutputCouplingService service;
    auto& settings = service.Settings(domain.object);
    settings.particlesEnabled = true;
    settings.surfaceDepositsEnabled = true;
    settings.fieldThreshold = 0.2F;
    settings.particleRatePerSecond = 100.0F;
    settings.particleBudgetPerStep = 100U;
    settings.surfaceDepositRatePerSecond = 40.0F;
    settings.surfaceDepositBudgetPerStep = 100U;
    settings.surfaceDepositRadiusMeters = 0.4F;

    const auto first = service.AdvanceBaked(domain, cache, 0.1);
    CheckOutput(first.particles.size() == 10U);
    CheckOutput(first.surfaceDeposits.size() == 4U);
    CheckOutput(first.diagnostics.requestedParticles == 10U);
    CheckOutput(first.diagnostics.requestedSurfaceDeposits == 4U);
    CheckOutput(first.diagnostics.thresholdRejected == 0U);

    for (const auto& particle : first.particles)
    {
        CheckOutput(particle.positionMeters.x >= 6.0);
        CheckOutput(particle.positionMeters.x <= 14.0);
        CheckOutput(particle.positionMeters.y >= 18.0);
        CheckOutput(particle.positionMeters.y <= 22.0);
        CheckOutput(particle.positionMeters.z >= -8.0);
        CheckOutput(particle.positionMeters.z <= -2.0);
        CheckOutput(particle.authority == 1.0F);
    }

    CheckOutput(first.surfaceDeposits.front().radiusMeters == 0.4F);
    CheckOutput(
        first.surfaceDeposits.front().maximumProjectionDistanceMeters ==
        8.0);

    // Concrete runtime queues must preserve source-volume identity. Consumers
    // need that provenance to select particle/material policy and the correct
    // physical body/frame instead of receiving anonymous requests.
    VolumeParticleRequestQueue particleQueue;
    VolumeSurfaceRequestQueue surfaceQueue;
    DispatchVolumeOutputs(
        first,
        &particleQueue,
        &surfaceQueue);
    CheckOutput(particleQueue.Pending().size() == first.particles.size());
    CheckOutput(surfaceQueue.Pending().size() == first.surfaceDeposits.size());
    CheckOutput(particleQueue.Pending().front().volume == domain.object);
    CheckOutput(surfaceQueue.Pending().front().volume == domain.object);
    CheckOutput(
        particleQueue.Pending().front().request.eventId ==
        first.particles.front().eventId);
    CheckOutput(
        surfaceQueue.Pending().front().request.eventId ==
        first.surfaceDeposits.front().eventId);

    const auto replayPosition =
        first.particles.front().positionMeters;
    const auto replayEvent =
        first.particles.front().eventId;

    service.Reset(domain.object);
    const auto replay = service.AdvanceBaked(domain, cache, 0.1);
    CheckOutput(replay.particles.size() == 10U);
    CheckOutput(replay.particles.front().eventId == replayEvent);
    CheckOutput(replay.particles.front().positionMeters == replayPosition);

    // Rate accumulation is independent of how a fixed simulated interval is
    // split across frames, provided the configured per-step budget is not hit.
    service.Reset(domain.object);
    const auto halfA = service.AdvanceBaked(domain, cache, 0.05);
    const auto halfACount = halfA.particles.size();
    const auto halfB = service.AdvanceBaked(domain, cache, 0.05);
    const auto splitCount =
        halfACount + halfB.particles.size();
    CheckOutput(splitCount == 10U);

    // Budgets intentionally drop excess work instead of accumulating an
    // unbounded catch-up burst on a later frame.
    service.Reset(domain.object);
    settings.particleRatePerSecond = 1000.0F;
    settings.particleBudgetPerStep = 5U;
    settings.surfaceDepositsEnabled = false;
    const auto budgeted =
        service.AdvanceBaked(domain, cache, 1.0);
    CheckOutput(budgeted.particles.size() == 5U);
    CheckOutput(budgeted.diagnostics.requestedParticles == 1000U);
    CheckOutput(budgeted.diagnostics.particleBudgetDropped == 995U);

    // A field below threshold performs bounded rejection work and emits no
    // accidental outputs.
    service.Reset(domain.object);
    settings.particleRatePerSecond = 10.0F;
    settings.particleBudgetPerStep = 10U;
    settings.fieldThreshold = 0.25F;
    VolumeCacheData emptyCache = cache;
    emptyCache.density.assign(64U, 0.0F);
    emptyCache.emission.assign(64U, 0.0F);
    const auto rejected =
        service.AdvanceBaked(domain, emptyCache, 1.0);
    CheckOutput(rejected.particles.empty());
    CheckOutput(rejected.diagnostics.thresholdRejected > 0U);
    CheckOutput(rejected.diagnostics.candidatesTested <= 80U);

    // The generic producer seam preserves transport velocity for a future
    // compact live-GPU output producer/readback path.
    service.Reset(domain.object);
    settings.fieldThreshold = 0.0F;
    settings.particleRatePerSecond = 1.0F;
    settings.particleBudgetPerStep = 1U;
    const auto generic = service.Advance(
        domain,
        [](const f64, const f64, const f64)
        {
            return VolumeOutputFieldSample{
                .density = 0.75F,
                .emission = 0.25F,
                .velocity = {1.0F, 2.0F, 3.0F}
            };
        },
        1.0);
    CheckOutput(generic.particles.size() == 1U);
    CheckOutput(generic.particles.front().velocity.x == 1.0F);
    CheckOutput(generic.particles.front().velocity.y == 2.0F);
    CheckOutput(generic.particles.front().velocity.z == 3.0F);
}
