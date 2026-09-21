#include <orbit/lighting/EmissiveInvalidation.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace orbit::lighting
{
namespace
{
[[nodiscard]] bool SamePosition(
    const math::Double3& a,
    const math::Double3& b) noexcept
{
    constexpr f64 kEpsilonSquared =
        1.0e-12;

    return
        math::LengthSquared(a - b) <=
        kEpsilonSquared;
}

[[nodiscard]] f64 EffectiveRadius(
    const DynamicEmissiveSourceState& source) noexcept
{
    return
        std::max(
            std::max(
                source.sourceRadiusMeters,
                0.0),
            std::max(
                source.influenceRangeMeters,
                0.0));
}

[[nodiscard]] EmissiveInvalidationEvent EventForUnion(
    const DynamicEmissiveSourceState& previous,
    const DynamicEmissiveSourceState& current,
    const EmissiveInvalidationReason reason)
{
    const f64 previousRadius =
        EffectiveRadius(previous);
    const f64 currentRadius =
        EffectiveRadius(current);

    const math::Double3 delta =
        current.centerInFrameMeters -
        previous.centerInFrameMeters;

    const f64 distance =
        math::Length(delta);

    if (!std::isfinite(distance))
    {
        throw std::invalid_argument(
            "Dynamic emissive source position must be finite.");
    }

    if (distance <= 1.0e-12)
    {
        return {
            .stableId = current.stableId,
            .reason = reason,
            .centerInFrameMeters =
                current.centerInFrameMeters,
            .radiusMeters =
                std::max(
                    previousRadius,
                    currentRadius)
        };
    }

    // Minimal conservative sphere containing both old and new influence
    // spheres. This handles moving headlights/screens without global flush.
    const f64 radius =
        (distance +
         previousRadius +
         currentRadius) *
        0.5;

    const f64 fromPrevious =
        (radius -
         previousRadius) /
        distance;

    return {
        .stableId = current.stableId,
        .reason = reason,
        .centerInFrameMeters =
            previous.centerInFrameMeters +
            delta * fromPrevious,
        .radiusMeters = radius
    };
}
} // namespace

std::vector<EmissiveInvalidationEvent>
EmissiveInvalidationTracker::Update(
    const std::span<const DynamicEmissiveSourceState> sources)
{
    for (auto& [id, state] : states_)
    {
        static_cast<void>(id);
        state.seen = false;
    }

    std::vector<EmissiveInvalidationEvent>
        events;

    events.reserve(
        sources.size());

    for (const auto& source :
         sources)
    {
        if (source.stableId == 0U ||
            !std::isfinite(
                source.centerInFrameMeters.x) ||
            !std::isfinite(
                source.centerInFrameMeters.y) ||
            !std::isfinite(
                source.centerInFrameMeters.z) ||
            !std::isfinite(
                source.sourceRadiusMeters) ||
            !std::isfinite(
                source.influenceRangeMeters) ||
            source.sourceRadiusMeters < 0.0 ||
            source.influenceRangeMeters < 0.0)
        {
            throw std::invalid_argument(
                "Dynamic emissive source state is invalid.");
        }

        const auto found =
            states_.find(
                source.stableId);

        if (found == states_.end())
        {
            states_.emplace(
                source.stableId,
                State{
                    .source = source,
                    .seen = true
                });

            events.push_back({
                .stableId =
                    source.stableId,
                .reason =
                    EmissiveInvalidationReason::
                        Added,
                .centerInFrameMeters =
                    source.centerInFrameMeters,
                .radiusMeters =
                    EffectiveRadius(source)
            });

            continue;
        }

        auto& state =
            found->second;

        state.seen = true;

        const bool changed =
            state.source.contentRevision !=
                source.contentRevision ||
            !SamePosition(
                state.source.
                    centerInFrameMeters,
                source.centerInFrameMeters) ||
            state.source.sourceRadiusMeters !=
                source.sourceRadiusMeters ||
            state.source.influenceRangeMeters !=
                source.influenceRangeMeters;

        if (changed)
        {
            events.push_back(
                EventForUnion(
                    state.source,
                    source,
                    EmissiveInvalidationReason::
                        Changed));

            state.source =
                source;
        }
    }

    for (auto it = states_.begin();
         it != states_.end();)
    {
        if (it->second.seen)
        {
            ++it;
            continue;
        }

        const auto removed =
            it->second.source;

        events.push_back({
            .stableId =
                removed.stableId,
            .reason =
                EmissiveInvalidationReason::
                    Removed,
            .centerInFrameMeters =
                removed.centerInFrameMeters,
            .radiusMeters =
                EffectiveRadius(removed)
        });

        it =
            states_.erase(it);
    }

    std::stable_sort(
        events.begin(),
        events.end(),
        [](const auto& a,
           const auto& b)
        {
            return
                a.stableId <
                b.stableId;
        });

    return events;
}

void EmissiveInvalidationTracker::Clear() noexcept
{
    states_.clear();
}

std::size_t
EmissiveInvalidationTracker::SourceCount() const noexcept
{
    return states_.size();
}

void ApplyEmissiveInvalidations(
    RadianceClipmapResidency& residency,
    const std::span<
        const EmissiveInvalidationEvent> events,
    const u64 sourceRevision,
    const f32 priorityBoost)
{
    for (const auto& event :
         events)
    {
        residency.InvalidateSphere(
            event.centerInFrameMeters,
            event.radiusMeters,
            sourceRevision,
            priorityBoost);
    }
}
} // namespace orbit::lighting
