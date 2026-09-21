#include <orbit/lighting/RepresentationLightingContinuity.hpp>

#include <algorithm>

namespace orbit::lighting
{
LightingContinuityResult EvaluateLightingContinuity(
    const std::span<const RepresentationLightingAuthority> authorities,
    const f64 activeWeightThreshold) noexcept
{
    LightingContinuityResult result;

    const f64 threshold =
        std::max(
            activeWeightThreshold,
            0.0);

    const RepresentationLightingAuthority* reference =
        nullptr;

    for (const auto& authority : authorities)
    {
        if (authority.weight <= threshold)
        {
            continue;
        }

        ++result.activeRepresentations;

        if (reference == nullptr)
        {
            reference = &authority;
            continue;
        }

        result.directLightingCoherent =
            result.directLightingCoherent &&
            authority.directLightingFingerprint ==
                reference->directLightingFingerprint;

        result.emissionAuthorityCoherent =
            result.emissionAuthorityCoherent &&
            authority.emissionAuthorityFingerprint ==
                reference->emissionAuthorityFingerprint;

        result.radianceIdentityCoherent =
            result.radianceIdentityCoherent &&
            authority.radianceFrame ==
                reference->radianceFrame &&
            authority.radianceBody ==
                reference->radianceBody;
    }

    return result;
}

RadianceRefreshReason EvaluateRadianceRefresh(
    const u64 previousLightingFingerprint,
    const u64 currentLightingFingerprint,
    const frames::FrameId previousFrame,
    const frames::FrameId currentFrame,
    const universe::BodyId previousBody,
    const universe::BodyId currentBody) noexcept
{
    if (previousFrame &&
        currentFrame &&
        previousFrame != currentFrame)
    {
        return RadianceRefreshReason::FrameChanged;
    }

    if (previousBody &&
        currentBody &&
        previousBody != currentBody)
    {
        return RadianceRefreshReason::BodyChanged;
    }

    if (previousLightingFingerprint != 0U &&
        currentLightingFingerprint != 0U &&
        previousLightingFingerprint !=
            currentLightingFingerprint)
    {
        return
            RadianceRefreshReason::
                PhysicalLightingChanged;
    }

    return RadianceRefreshReason::None;
}
} // namespace orbit::lighting
