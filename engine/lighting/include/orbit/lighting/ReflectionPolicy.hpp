#pragma once

#include <orbit/core/Types.hpp>

namespace orbit::lighting
{
enum class ReflectionFallback : u8
{
    None,
    RadianceCache,
    SoftwareExact,
    HardwareRayQuery
};

struct ReflectionPolicyConfig
{
    // Mirror-like surfaces request precise off-screen continuation when the
    // screen trace cannot answer confidently.
    f32 preciseMaximumRoughness{0.08F};

    // Glossy surfaces still use screen traces but degrade to broad cache
    // representation instead of spending exact rays.
    f32 screenTraceMaximumRoughness{0.45F};

    f32 minimumScreenConfidence{0.70F};
    f32 minimumExactImportance{0.20F};
};

struct ReflectionRequest
{
    f32 roughness{0.5F};
    f32 screenConfidence{0.0F};
    f32 importance{1.0F};
    bool screenHit{false};
};

struct ReflectionPlan
{
    bool useScreenHit{false};
    bool requestExactVisibility{false};
    bool useRadianceCache{false};
    ReflectionFallback fallback{
        ReflectionFallback::None};
};

[[nodiscard]] ReflectionPlan PlanReflection(
    const ReflectionRequest& request,
    bool hardwareRayQueryAvailable,
    bool softwareExactAvailable,
    const ReflectionPolicyConfig& config = {}) noexcept;
} // namespace orbit::lighting
