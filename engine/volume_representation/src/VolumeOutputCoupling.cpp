#include <orbit/volume_representation/VolumeOutputCoupling.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace orbit::volume_representation
{
namespace
{
struct RateBudget
{
    u32 requested{0U};
    u32 admitted{0U};
    u32 dropped{0U};
};

[[nodiscard]] RateBudget ResolveRateBudget(
    const f32 ratePerSecond,
    const u32 budget,
    const f64 deltaSeconds,
    f64& carry) noexcept
{
    const f64 rate =
        std::isfinite(ratePerSecond)
            ? std::max(static_cast<f64>(ratePerSecond), 0.0)
            : 0.0;
    const f64 dt =
        std::isfinite(deltaSeconds)
            ? std::max(deltaSeconds, 0.0)
            : 0.0;

    const f64 due = carry + rate * dt;
    const f64 whole = std::floor(due);
    carry = due - whole;

    const f64 cappedWhole =
        std::min(
            whole,
            static_cast<f64>(std::numeric_limits<u32>::max()));
    const u32 requested =
        static_cast<u32>(cappedWhole);
    const u32 admitted =
        std::min(requested, budget);

    return {
        .requested = requested,
        .admitted = admitted,
        .dropped = requested - admitted
    };
}

[[nodiscard]] f64 RadicalInverse(
    u64 index,
    const u32 base) noexcept
{
    f64 result = 0.0;
    f64 factor = 1.0 / static_cast<f64>(base);

    while (index != 0U)
    {
        result +=
            static_cast<f64>(index % base) * factor;
        index /= base;
        factor /= static_cast<f64>(base);
    }

    return result;
}

[[nodiscard]] math::Double3 PositionInDomain(
    const world_model::ResolvedVolumeDomain& domain,
    const f64 u,
    const f64 v,
    const f64 w) noexcept
{
    const f64 hx = std::abs(domain.halfExtentsMeters.x);
    const f64 hy = std::abs(domain.halfExtentsMeters.y);
    const f64 hz = std::abs(domain.halfExtentsMeters.z);

    return {
        domain.centerMeters.x + (u * 2.0 - 1.0) * hx,
        domain.centerMeters.y + (v * 2.0 - 1.0) * hy,
        domain.centerMeters.z + (w * 2.0 - 1.0) * hz
    };
}

[[nodiscard]] f32 FiniteNonNegative(
    const f32 value) noexcept
{
    return std::isfinite(value)
        ? std::max(value, 0.0F)
        : 0.0F;
}

[[nodiscard]] math::Float3 FiniteVelocity(
    const math::Float3 value) noexcept
{
    return {
        std::isfinite(value.x) ? value.x : 0.0F,
        std::isfinite(value.y) ? value.y : 0.0F,
        std::isfinite(value.z) ? value.z : 0.0F
    };
}

struct SanitizedSample
{
    f32 density{0.0F};
    f32 emission{0.0F};
    math::Float3 velocity{};
    f32 authority{0.0F};
};

[[nodiscard]] SanitizedSample Sanitize(
    const VolumeOutputFieldSample sample) noexcept
{
    const f32 density =
        FiniteNonNegative(sample.density);
    const f32 emission =
        FiniteNonNegative(sample.emission);

    return {
        .density = density,
        .emission = emission,
        .velocity = FiniteVelocity(sample.velocity),
        .authority = std::max(density, emission)
    };
}

[[nodiscard]] u32 CandidateLimit(
    const u32 target,
    const u32 multiplier) noexcept
{
    if (target == 0U)
    {
        return 0U;
    }

    const u64 boundedMultiplier =
        std::clamp<u64>(multiplier, 1U, 64U);
    const u64 requested =
        std::max<u64>(
            target,
            static_cast<u64>(target) * boundedMultiplier);

    return static_cast<u32>(
        std::min<u64>(
            requested,
            std::numeric_limits<u32>::max()));
}
} // namespace

VolumeOutputSettings& VolumeOutputCouplingService::Settings(
    const scene::ObjectId volume)
{
    return entries_[volume].settings;
}

const VolumeOutputBatch& VolumeOutputCouplingService::Advance(
    const world_model::ResolvedVolumeDomain& domain,
    const VolumeOutputSampler& sampler,
    const f64 deltaSeconds)
{
    auto& entry = entries_[domain.object];
    auto& batch = entry.latest;

    batch = {};
    batch.volume = domain.object;
    batch.diagnostics.deltaSeconds =
        std::isfinite(deltaSeconds)
            ? std::max(deltaSeconds, 0.0)
            : 0.0;

    if (!domain.enabled || !sampler)
    {
        return batch;
    }

    const auto particleBudget =
        ResolveRateBudget(
            entry.settings.particlesEnabled
                ? entry.settings.particleRatePerSecond
                : 0.0F,
            entry.settings.particleBudgetPerStep,
            batch.diagnostics.deltaSeconds,
            entry.particleCarry);
    const auto surfaceBudget =
        ResolveRateBudget(
            entry.settings.surfaceDepositsEnabled
                ? entry.settings.surfaceDepositRatePerSecond
                : 0.0F,
            entry.settings.surfaceDepositBudgetPerStep,
            batch.diagnostics.deltaSeconds,
            entry.surfaceCarry);

    batch.diagnostics.requestedParticles =
        particleBudget.requested;
    batch.diagnostics.particleBudgetDropped =
        particleBudget.dropped;
    batch.diagnostics.requestedSurfaceDeposits =
        surfaceBudget.requested;
    batch.diagnostics.surfaceBudgetDropped =
        surfaceBudget.dropped;

    const f32 threshold =
        FiniteNonNegative(entry.settings.fieldThreshold);
    const u32 multiplier =
        std::clamp(
            entry.settings.candidateMultiplier,
            1U,
            64U);

    const f32 particleLifetime = std::isfinite(entry.settings.particleLifetimeSeconds) ? std::max(entry.settings.particleLifetimeSeconds, 0.001F) : 2.0F;
    const f32 particleDrag = std::isfinite(entry.settings.particleLinearDragPerSecond) ? std::max(entry.settings.particleLinearDragPerSecond, 0.0F) : 0.0F;
    const f32 particleRadius = std::isfinite(entry.settings.particleRadiusMeters) ? std::max(entry.settings.particleRadiusMeters, 0.001F) : 0.08F;
    const f32 particleEmissionScale = std::isfinite(entry.settings.particleEmissionScale) ? std::max(entry.settings.particleEmissionScale, 0.0F) : 1.0F;
    const f32 particleGravityScale = std::isfinite(entry.settings.particleGravityScale) ? std::max(entry.settings.particleGravityScale, 0.0F) : 1.0F;
    const f32 particleRestitution = std::isfinite(entry.settings.particleRestitution) ? std::clamp(entry.settings.particleRestitution, 0.0F, 1.0F) : 0.25F;
    const f32 particleWaterDensityRatio = std::isfinite(entry.settings.particleWaterDensityRatio) ? std::clamp(entry.settings.particleWaterDensityRatio, 0.01F, 100.0F) : 1.0F;
    const f32 particleWaterDrag = std::isfinite(entry.settings.particleWaterDragPerSecond) ? std::clamp(entry.settings.particleWaterDragPerSecond, 0.0F, 1000.0F) : 0.0F;
    const f32 particleWaterBuoyancy = std::isfinite(entry.settings.particleWaterBuoyancyScale) ? std::clamp(entry.settings.particleWaterBuoyancyScale, 0.0F, 16.0F) : 1.0F;
    const f32 particleWaterSplashScale = std::isfinite(entry.settings.particleWaterSplashScale) ? std::clamp(entry.settings.particleWaterSplashScale, 0.0F, 64.0F) : 1.0F;

    batch.particles.reserve(particleBudget.admitted);

    const u32 particleCandidateLimit =
        CandidateLimit(
            particleBudget.admitted,
            multiplier);

    for (u32 attempt = 0U;
         attempt < particleCandidateLimit &&
         batch.particles.size() < particleBudget.admitted;
         ++attempt)
    {
        const u64 sequence =
            ++entry.particleSequence;
        const f64 u = RadicalInverse(sequence, 2U);
        const f64 v = RadicalInverse(sequence, 3U);
        const f64 w = RadicalInverse(sequence, 5U);

        const auto sample = Sanitize(sampler(u, v, w));
        ++batch.diagnostics.candidatesTested;

        if (sample.authority < threshold)
        {
            ++batch.diagnostics.thresholdRejected;
            continue;
        }

        batch.particles.push_back({
            .eventId = sequence,
            .positionMeters =
                PositionInDomain(domain, u, v, w),
            .velocity = sample.velocity,
            .authority = sample.authority,
            .density = sample.density,
            .emission = sample.emission,
            .lifetimeSeconds = particleLifetime,
            .linearDragPerSecond = particleDrag,
            .radiusMeters = particleRadius,
            .emissionScale = particleEmissionScale,
            .baseColor = domain.scatteringColor,
            .emissionColor = domain.emissionColor,
            .gravityMode = entry.settings.particleGravityMode,
            .gravityScale = particleGravityScale,
            .collisionMode = entry.settings.particleCollisionMode,
            .restitution = particleRestitution,
            .waterDensityRatio = particleWaterDensityRatio,
            .waterDragPerSecond = particleWaterDrag,
            .waterBuoyancyScale = particleWaterBuoyancy,
            .killOnWaterImmersion = entry.settings.particleKillOnWaterImmersion,
            .splashOnWaterEntry = entry.settings.particleSplashOnWaterEntry,
            .waterSplashScale = particleWaterSplashScale
        });
    }

    batch.surfaceDeposits.reserve(
        surfaceBudget.admitted);

    const u32 surfaceCandidateLimit =
        CandidateLimit(
            surfaceBudget.admitted,
            multiplier);
    const f64 projectionDistance =
        2.0 *
        std::max({
            std::abs(domain.halfExtentsMeters.x),
            std::abs(domain.halfExtentsMeters.y),
            std::abs(domain.halfExtentsMeters.z),
            0.01
        });
    const f32 depositRadius =
        std::isfinite(entry.settings.surfaceDepositRadiusMeters)
            ? std::max(
                  entry.settings.surfaceDepositRadiusMeters,
                  0.001F)
            : 0.25F;
    const f32 effectHalfLife =
        std::isfinite(entry.settings.surfaceEffectHalfLifeSeconds)
            ? std::max(entry.settings.surfaceEffectHalfLifeSeconds, 0.0F)
            : 30.0F;

    for (u32 attempt = 0U;
         attempt < surfaceCandidateLimit &&
         batch.surfaceDeposits.size() < surfaceBudget.admitted;
         ++attempt)
    {
        const u64 sequence =
            ++entry.surfaceSequence;
        const f64 u = RadicalInverse(sequence, 5U);
        const f64 v = RadicalInverse(sequence, 7U);
        const f64 w = RadicalInverse(sequence, 11U);

        const auto sample = Sanitize(sampler(u, v, w));
        ++batch.diagnostics.candidatesTested;

        if (sample.authority < threshold)
        {
            ++batch.diagnostics.thresholdRejected;
            continue;
        }

        batch.surfaceDeposits.push_back({
            .eventId =
                0x8000000000000000ULL |
                (sequence & 0x7fffffffffffffffULL),
            .samplePositionMeters =
                PositionInDomain(domain, u, v, w),
            .transportVelocity = sample.velocity,
            .maximumProjectionDistanceMeters =
                projectionDistance,
            .radiusMeters = depositRadius,
            .amount = sample.authority,
            .density = sample.density,
            .emission = sample.emission,
            .effect = entry.settings.surfaceEffect,
            .halfLifeSeconds = effectHalfLife
        });
    }

    batch.diagnostics.emittedParticles =
        static_cast<u32>(batch.particles.size());
    batch.diagnostics.emittedSurfaceDeposits =
        static_cast<u32>(batch.surfaceDeposits.size());

    return batch;
}

const VolumeOutputBatch& VolumeOutputCouplingService::AdvanceBaked(
    const world_model::ResolvedVolumeDomain& domain,
    const VolumeCacheData& cache,
    const f64 deltaSeconds)
{
    return Advance(
        domain,
        [&cache](const f64 u, const f64 v, const f64 w)
        {
            return VolumeOutputFieldSample{
                .density =
                    SampleVolumeCacheDensity(cache, u, v, w),
                .emission =
                    SampleVolumeCacheEmission(cache, u, v, w),
                .velocity = {}
            };
        },
        deltaSeconds);
}

const VolumeOutputBatch* VolumeOutputCouplingService::Latest(
    const scene::ObjectId volume) const noexcept
{
    const auto found = entries_.find(volume);
    return found == entries_.end()
        ? nullptr
        : &found->second.latest;
}

void VolumeOutputCouplingService::Reset(
    const scene::ObjectId volume) noexcept
{
    const auto found = entries_.find(volume);
    if (found == entries_.end())
    {
        return;
    }

    found->second.particleCarry = 0.0;
    found->second.surfaceCarry = 0.0;
    found->second.particleSequence = 0U;
    found->second.surfaceSequence = 0U;
    found->second.latest = {};
    found->second.latest.volume = volume;
}

void VolumeOutputCouplingService::RemoveMissing(
    const scene::ObjectStore& objects)
{
    for (auto iterator = entries_.begin();
         iterator != entries_.end();)
    {
        if (!objects.Find(iterator->first).has_value())
        {
            iterator = entries_.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
}

VolumeOutputCouplingService& VolumeOutputs() noexcept
{
    static VolumeOutputCouplingService service;
    return service;
}
} // namespace orbit::volume_representation
