#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/lighting/RadianceClipmapResidency.hpp>
#include <orbit/math/Vector.hpp>

#include <span>
#include <unordered_map>
#include <vector>

namespace orbit::lighting
{
struct DynamicEmissiveSourceState
{
    u64 stableId{0U};
    u64 contentRevision{0U};

    math::Double3 centerInFrameMeters{};
    f64 sourceRadiusMeters{0.0};
    f64 influenceRangeMeters{0.0};
};

enum class EmissiveInvalidationReason : u8
{
    Added,
    Changed,
    Removed
};

struct EmissiveInvalidationEvent
{
    u64 stableId{0U};
    EmissiveInvalidationReason reason{
        EmissiveInvalidationReason::Changed};

    math::Double3 centerInFrameMeters{};
    f64 radiusMeters{0.0};
};

class EmissiveInvalidationTracker
{
public:
    [[nodiscard]] std::vector<EmissiveInvalidationEvent>
    Update(
        std::span<const DynamicEmissiveSourceState> sources);

    void Clear() noexcept;

    [[nodiscard]] std::size_t SourceCount() const noexcept;

private:
    struct State
    {
        DynamicEmissiveSourceState source{};
        bool seen{false};
    };

    std::unordered_map<u64, State> states_;
};

void ApplyEmissiveInvalidations(
    RadianceClipmapResidency& residency,
    std::span<const EmissiveInvalidationEvent> events,
    u64 sourceRevision,
    f32 priorityBoost = 4.0F);
} // namespace orbit::lighting
