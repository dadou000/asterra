#include <orbit/lighting/RepresentationLightingContinuity.hpp>
#include <orbit/lighting/RadianceClipmap.hpp>

#include <array>

int main()
{
    using namespace orbit;
    using namespace orbit::lighting;

    const frames::FrameId frame{
        .high = 1U,
        .low = 2U};
    const universe::BodyId body{
        .high = 3U,
        .low = 4U};

    const std::array coherent{
        RepresentationLightingAuthority{
            .representation =
                celestial_representation::Representation::
                    ProductionSurface,
            .weight = 0.55,
            .directLightingFingerprint = 100U,
            .emissionAuthorityFingerprint = 200U,
            .radianceFrame = frame,
            .radianceBody = body
        },
        RepresentationLightingAuthority{
            .representation =
                celestial_representation::Representation::
                    MacroDisplacedGlobe,
            .weight = 0.45,
            .directLightingFingerprint = 100U,
            .emissionAuthorityFingerprint = 200U,
            .radianceFrame = frame,
            .radianceBody = body
        }
    };

    const auto coherentResult =
        EvaluateLightingContinuity(coherent);

    if (!coherentResult.Passed() ||
        coherentResult.activeRepresentations != 2U)
    {
        return 1;
    }

    auto badEmission = coherent;
    badEmission[1].emissionAuthorityFingerprint = 201U;

    const auto badEmissionResult =
        EvaluateLightingContinuity(badEmission);

    if (badEmissionResult.Passed() ||
        !badEmissionResult.directLightingCoherent ||
        badEmissionResult.emissionAuthorityCoherent ||
        !badEmissionResult.radianceIdentityCoherent)
    {
        return 2;
    }

    // Representation switching alone is not a cache-refresh reason.
    if (EvaluateRadianceRefresh(
            500U,
            500U,
            frame,
            frame,
            body,
            body) !=
        RadianceRefreshReason::None)
    {
        return 3;
    }

    if (EvaluateRadianceRefresh(
            500U,
            501U,
            frame,
            frame,
            body,
            body) !=
        RadianceRefreshReason::PhysicalLightingChanged)
    {
        return 4;
    }

    const frames::FrameId otherFrame{
        .high = 9U,
        .low = 10U};

    if (EvaluateRadianceRefresh(
            500U,
            500U,
            frame,
            otherFrame,
            body,
            body) !=
        RadianceRefreshReason::FrameChanged)
    {
        return 5;
    }

    const universe::BodyId otherBody{
        .high = 11U,
        .low = 12U};

    if (EvaluateRadianceRefresh(
            500U,
            500U,
            frame,
            frame,
            body,
            otherBody) !=
        RadianceRefreshReason::BodyChanged)
    {
        return 6;
    }

    LightingView view;
    view.frame = frame;
    view.body = body;

    const RadianceClipmapConfig config{
        .baseCellSizeMeters = 4.0,
        .levelScale = 4.0,
        .levelCount = 3U,
        .cellsPerAxis = 8U
    };

    const math::Double3 point{
        100.0,
        200.0,
        -300.0
    };

    const auto surfaceKey =
        RadianceCellForPoint(
            point,
            config,
            1U,
            view);

    // A representation handoff and an ordinary GPU-origin rebase must not
    // change logical GI identity.
    view.gpuOriginInFrameMeters =
        {50'000.0, -20'000.0, 3'000.0};
    ++view.gpuOriginRevision;

    const auto macroKey =
        RadianceCellForPoint(
            point,
            config,
            1U,
            view);

    if (surfaceKey != macroKey)
    {
        return 7;
    }

    return 0;
}
