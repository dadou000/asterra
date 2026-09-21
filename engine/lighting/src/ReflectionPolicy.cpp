#include <orbit/lighting/ReflectionPolicy.hpp>

#include <algorithm>
#include <cmath>

namespace orbit::lighting
{
ReflectionPlan PlanReflection(
    const ReflectionRequest& request,
    const bool hardwareRayQueryAvailable,
    const bool softwareExactAvailable,
    const ReflectionPolicyConfig& config) noexcept
{
    const f32 roughness =
        std::clamp(
            std::isfinite(request.roughness)
                ? request.roughness
                : 1.0F,
            0.0F,
            1.0F);

    const f32 confidence =
        std::clamp(
            std::isfinite(
                request.screenConfidence)
                ? request.screenConfidence
                : 0.0F,
            0.0F,
            1.0F);

    const f32 importance =
        std::max(
            std::isfinite(request.importance)
                ? request.importance
                : 0.0F,
            0.0F);

    ReflectionPlan result;

    if (request.screenHit &&
        confidence >=
            config.minimumScreenConfidence)
    {
        result.useScreenHit = true;

        // Rough/glossy reflections do not require exact continuation after a
        // valid visible-scene hit. Mirror-like pixels may still request exact
        // only when screen data fails.
        return result;
    }

    if (roughness <=
            config.preciseMaximumRoughness &&
        importance >=
            config.minimumExactImportance)
    {
        if (hardwareRayQueryAvailable)
        {
            result.requestExactVisibility =
                true;
            result.fallback =
                ReflectionFallback::
                    HardwareRayQuery;
            return result;
        }

        if (softwareExactAvailable)
        {
            result.requestExactVisibility =
                true;
            result.fallback =
                ReflectionFallback::
                    SoftwareExact;
            return result;
        }

        // RT-off/no exact provider remains visually stable: the same mirror
        // request falls back to the broad cache rather than disappearing.
        result.useRadianceCache = true;
        result.fallback =
            ReflectionFallback::
                RadianceCache;
        return result;
    }

    if (roughness <=
        config.screenTraceMaximumRoughness)
    {
        // Glossy unresolved screen rays use broad cache representation. They
        // intentionally do not consume expensive exact visibility.
        result.useRadianceCache = true;
        result.fallback =
            ReflectionFallback::
                RadianceCache;
        return result;
    }

    // Rough surfaces are cache-only by design.
    result.useRadianceCache = true;
    result.fallback =
        ReflectionFallback::
            RadianceCache;
    return result;
}
} // namespace orbit::lighting
