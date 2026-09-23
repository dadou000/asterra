#pragma once

#include <orbit/studio_session/VolumeSurfaceOutputResolver.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

namespace orbit::studio_session
{
struct VolumeSurfaceEffectStamp
{
    scene::ObjectId sourceVolume{};
    u64 sourceEventId{0U};
    universe::BodyId body{};
    frames::FrameId frame{};
    volume_representation::VolumeSurfaceEffect effect{
        volume_representation::VolumeSurfaceEffect::Wetness};
    universe::SurfaceCoordinate coordinate{};
    math::Double3 bodyLocalSurfacePointMeters{};
    f32 radiusMeters{0.25F};
    f32 amount{0.0F};
    f32 halfLifeSeconds{30.0F};
};

struct VolumeSurfaceEffectDiagnostics
{
    u32 submitted{0U};
    u32 added{0U};
    u32 merged{0U};
    u32 decayed{0U};
    u32 expired{0U};
    u32 removedWithBody{0U};
    u32 evictedForBudget{0U};
    u32 active{0U};
};

// Runtime-only material influence produced by volumetric output. It never
// writes semantic terrain authority. Render/material/physics consumers sample
// this state and are free to map Wetness/Soot/Ash/Sediment/Heat into their own
// representation. State is body-local so floating-origin and celestial motion
// do not invalidate deposited effects.
class VolumeSurfaceEffectState
{
public:
    static constexpr u32 kMaximumStampsPerBody = 4096U;
    static constexpr f32 kExpirationAmount = 1.0e-4F;

    void Advance(
        const universe::BodyRegistry& bodies,
        const std::span<const ResolvedVolumeSurfaceDeposit> deposits,
        const f64 deltaSeconds)
    {
        diagnostics_ = {};
        diagnostics_.submitted = static_cast<u32>(deposits.size());

        RemoveMissingBodies(bodies);
        Decay(deltaSeconds);

        for (const auto& deposit : deposits)
        {
            AddOrMerge(deposit);
        }

        EnforceBodyBudgets();
        diagnostics_.active = static_cast<u32>(stamps_.size());
    }

    [[nodiscard]] std::span<const VolumeSurfaceEffectStamp>
    Stamps() const noexcept
    {
        return stamps_;
    }

    [[nodiscard]] const VolumeSurfaceEffectDiagnostics&
    Diagnostics() const noexcept
    {
        return diagnostics_;
    }

    // Samples accumulated influence at one body-local point. Influence uses a
    // compact radial kernel and sums all matching stamps; values are not
    // clamped because some consumers need accumulated thermal/deposition load.
    [[nodiscard]] f32 InfluenceAt(
        const universe::BodyId body,
        const math::Double3& bodyLocalPointMeters,
        const volume_representation::VolumeSurfaceEffect effect) const noexcept
    {
        f64 influence = 0.0;

        for (const auto& stamp : stamps_)
        {
            if (stamp.body != body || stamp.effect != effect)
            {
                continue;
            }

            const f64 radius =
                std::max(static_cast<f64>(stamp.radiusMeters), 1.0e-6);
            const f64 distance =
                std::sqrt(DistanceSquared(
                    stamp.bodyLocalSurfacePointMeters,
                    bodyLocalPointMeters));

            if (distance >= radius)
            {
                continue;
            }

            const f64 normalized = 1.0 - distance / radius;
            influence +=
                static_cast<f64>(stamp.amount) *
                normalized * normalized;
        }

        if (!std::isfinite(influence))
        {
            return 0.0F;
        }

        return static_cast<f32>(
            std::min(
                influence,
                static_cast<f64>(std::numeric_limits<f32>::max())));
    }

    void Clear() noexcept
    {
        stamps_.clear();
        diagnostics_ = {};
    }

private:
    [[nodiscard]] static f64 DistanceSquared(
        const math::Double3& a,
        const math::Double3& b) noexcept
    {
        const f64 dx = a.x - b.x;
        const f64 dy = a.y - b.y;
        const f64 dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }

    void RemoveMissingBodies(
        const universe::BodyRegistry& bodies)
    {
        const auto oldSize = stamps_.size();
        std::erase_if(
            stamps_,
            [&bodies](const VolumeSurfaceEffectStamp& stamp)
            {
                return bodies.FindBody(stamp.body) == nullptr;
            });
        diagnostics_.removedWithBody =
            static_cast<u32>(oldSize - stamps_.size());
    }

    void Decay(const f64 deltaSeconds)
    {
        const f64 dt =
            std::isfinite(deltaSeconds)
                ? std::max(deltaSeconds, 0.0)
                : 0.0;

        if (dt > 0.0)
        {
            for (auto& stamp : stamps_)
            {
                if (stamp.halfLifeSeconds <= 0.0F ||
                    !std::isfinite(stamp.halfLifeSeconds))
                {
                    continue;
                }

                stamp.amount = static_cast<f32>(
                    static_cast<f64>(stamp.amount) *
                    std::exp2(
                        -dt /
                        static_cast<f64>(stamp.halfLifeSeconds)));
                ++diagnostics_.decayed;
            }
        }

        const auto oldSize = stamps_.size();
        std::erase_if(
            stamps_,
            [](const VolumeSurfaceEffectStamp& stamp)
            {
                return
                    !std::isfinite(stamp.amount) ||
                    stamp.amount < kExpirationAmount;
            });
        diagnostics_.expired =
            static_cast<u32>(oldSize - stamps_.size());
    }

    void AddOrMerge(const ResolvedVolumeSurfaceDeposit& deposit)
    {
        const auto& request = deposit.request;
        const f32 amount =
            std::isfinite(request.amount)
                ? std::max(request.amount, 0.0F)
                : 0.0F;

        if (amount < kExpirationAmount)
        {
            return;
        }

        const f32 radius =
            std::isfinite(request.radiusMeters)
                ? std::max(request.radiusMeters, 0.001F)
                : 0.25F;
        const f32 halfLife =
            std::isfinite(request.halfLifeSeconds)
                ? std::max(request.halfLifeSeconds, 0.0F)
                : 30.0F;

        // Merge only overlapping stamps from the same semantic channel/body.
        // This bounds dense spray/deposition without losing aggregate load.
        for (auto iterator = stamps_.rbegin();
             iterator != stamps_.rend();
             ++iterator)
        {
            if (iterator->body != deposit.body ||
                iterator->effect != request.effect)
            {
                continue;
            }

            const f64 mergeRadius =
                std::max(
                    static_cast<f64>(iterator->radiusMeters),
                    static_cast<f64>(radius));

            if (DistanceSquared(
                    iterator->bodyLocalSurfacePointMeters,
                    deposit.bodyLocalSurfacePointMeters) >
                mergeRadius * mergeRadius)
            {
                continue;
            }

            iterator->amount += amount;
            iterator->radiusMeters =
                std::max(iterator->radiusMeters, radius);
            // Zero means persistent. Once an overlapping runtime influence is
            // persistent, merging a decaying contribution must not make the
            // accumulated state transient again.
            iterator->halfLifeSeconds =
                iterator->halfLifeSeconds <= 0.0F || halfLife <= 0.0F
                    ? 0.0F
                    : std::max(iterator->halfLifeSeconds, halfLife);
            iterator->sourceVolume = deposit.volume;
            iterator->sourceEventId = request.eventId;
            ++diagnostics_.merged;
            return;
        }

        stamps_.push_back({
            .sourceVolume = deposit.volume,
            .sourceEventId = request.eventId,
            .body = deposit.body,
            .frame = deposit.frame,
            .effect = request.effect,
            .coordinate = deposit.coordinate,
            .bodyLocalSurfacePointMeters =
                deposit.bodyLocalSurfacePointMeters,
            .radiusMeters = radius,
            .amount = amount,
            .halfLifeSeconds = halfLife
        });
        ++diagnostics_.added;
    }

    void EnforceBodyBudgets()
    {
        // Body-local budget prevents a pathological high-rate effect from
        // growing CPU memory without bound. Evict the weakest influence first.
        std::vector<universe::BodyId> bodies;
        bodies.reserve(stamps_.size());
        for (const auto& stamp : stamps_)
        {
            if (std::find(bodies.begin(), bodies.end(), stamp.body) ==
                bodies.end())
            {
                bodies.push_back(stamp.body);
            }
        }

        for (const auto body : bodies)
        {
            for (;;)
            {
                u32 count = 0U;
                auto weakest = stamps_.end();

                for (auto iterator = stamps_.begin();
                     iterator != stamps_.end();
                     ++iterator)
                {
                    if (iterator->body != body)
                    {
                        continue;
                    }

                    ++count;
                    if (weakest == stamps_.end() ||
                        iterator->amount < weakest->amount)
                    {
                        weakest = iterator;
                    }
                }

                if (count <= kMaximumStampsPerBody ||
                    weakest == stamps_.end())
                {
                    break;
                }

                stamps_.erase(weakest);
                ++diagnostics_.evictedForBudget;
            }
        }
    }

    std::vector<VolumeSurfaceEffectStamp> stamps_;
    VolumeSurfaceEffectDiagnostics diagnostics_{};
};

[[nodiscard]] inline VolumeSurfaceEffectState&
VolumeSurfaceEffects() noexcept
{
    static VolumeSurfaceEffectState state;
    return state;
}
} // namespace orbit::studio_session
