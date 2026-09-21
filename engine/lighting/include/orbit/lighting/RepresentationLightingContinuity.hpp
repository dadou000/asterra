#pragma once

#include <orbit/celestial_representation/RepresentationResolver.hpp>
#include <orbit/core/Types.hpp>
#include <orbit/frames/FrameGraph.hpp>
#include <orbit/universe/BodyRegistry.hpp>

#include <span>

namespace orbit::lighting
{
struct RepresentationLightingAuthority
{
    celestial_representation::Representation representation{
        celestial_representation::Representation::ProductionSurface};

    f64 weight{0.0};

    u64 directLightingFingerprint{0U};
    u64 emissionAuthorityFingerprint{0U};

    frames::FrameId radianceFrame{};
    universe::BodyId radianceBody{};
};

struct LightingContinuityResult
{
    u32 activeRepresentations{0U};

    bool directLightingCoherent{true};
    bool emissionAuthorityCoherent{true};
    bool radianceIdentityCoherent{true};

    [[nodiscard]] constexpr bool Passed() const noexcept
    {
        return
            directLightingCoherent &&
            emissionAuthorityCoherent &&
            radianceIdentityCoherent;
    }
};

[[nodiscard]] LightingContinuityResult
EvaluateLightingContinuity(
    std::span<const RepresentationLightingAuthority> authorities,
    f64 activeWeightThreshold = 1.0e-4) noexcept;

enum class RadianceRefreshReason : u8
{
    None,
    PhysicalLightingChanged,
    FrameChanged,
    BodyChanged
};

[[nodiscard]] RadianceRefreshReason
EvaluateRadianceRefresh(
    u64 previousLightingFingerprint,
    u64 currentLightingFingerprint,
    frames::FrameId previousFrame,
    frames::FrameId currentFrame,
    universe::BodyId previousBody,
    universe::BodyId currentBody) noexcept;
} // namespace orbit::lighting
