#include <orbit/celestial_representation/RepresentationResolver.hpp>
#include <orbit/celestial_representation/RepresentationTracker.hpp>

#include <cmath>

int main()
{
    using namespace orbit::celestial_representation;

    ResolveInput input{
        .bodyRadiusMeters = 6.4e6,
        .maximumProductionDetailMeters = 20'000.0,
        .maximumMacroDisplacementMeters = 12'000.0,
        .cameraDistanceToCenterMeters = 6.5e6,
        .verticalFieldOfViewRadians = 1.0,
        .viewportHeightPixels = 1080.0,
        .features = {
            .productionSurfaceAvailable = true,
            .macroDisplacementAvailable = true,
            .complexFarAppearance = false,
            .radiativeEmitter = false
        }
    };

    const auto close = Resolve(input);

    if (close.representation !=
            Representation::ProductionSurface ||
        close.productionDetailErrorPixels <= 2.0)
    {
        return 1;
    }

    input.maximumProductionDetailMeters = 1.0;
    input.cameraDistanceToCenterMeters = 2.0e7;

    const auto macro = Resolve(input);

    if (macro.representation !=
        Representation::MacroDisplacedGlobe)
    {
        return 2;
    }

    input.maximumMacroDisplacementMeters = 1.0;
    input.cameraDistanceToCenterMeters = 5.0e7;

    const auto smooth = Resolve(input);

    if (smooth.representation !=
        Representation::SmoothGlobe)
    {
        return 3;
    }

    input.cameraDistanceToCenterMeters = 8.0e8;

    const auto disc = Resolve(input);

    if (disc.representation !=
        Representation::AnalyticDiscImpostor)
    {
        return 4;
    }

    input.features.complexFarAppearance = true;

    const auto cached = Resolve(input);

    if (cached.representation !=
        Representation::CachedDiscImpostor)
    {
        return 5;
    }

    input.cameraDistanceToCenterMeters = 3.0e10;

    const auto point = Resolve(input);

    if (point.representation !=
        Representation::PointProxy)
    {
        return 6;
    }

    input.features.radiativeEmitter = true;

    const auto stellar = Resolve(input);

    if (stellar.representation !=
        Representation::StellarPointProxy)
    {
        return 7;
    }

    ResolveInput hysteresis = input;
    hysteresis.features.radiativeEmitter = false;
    hysteresis.cameraDistanceToCenterMeters =
        1.304e10;
    hysteresis.previous =
        Representation::AnalyticDiscImpostor;

    const auto held =
        Resolve(hysteresis);

    if (held.representation !=
            Representation::AnalyticDiscImpostor ||
        !held.hysteresisHeld)
    {
        return 8;
    }

    if (held.blendToLower < 0.0 ||
        held.blendToLower > 1.0)
    {
        return 9;
    }

    ResolveInput qualityCase = input;
    qualityCase.features.radiativeEmitter = false;
    qualityCase.cameraDistanceToCenterMeters =
        1.6e10;
    qualityCase.policy.qualityScale = 1.0;

    const auto normalQuality =
        Resolve(qualityCase);

    qualityCase.policy.qualityScale = 2.0;

    const auto highQuality =
        Resolve(qualityCase);

    if (normalQuality.representation !=
            Representation::PointProxy ||
        highQuality.representation ==
            Representation::PointProxy)
    {
        return 10;
    }

    RepresentationTracker tracker;
    const RepresentationSubjectId subject{
        .high = 7,
        .low = 9
    };

    ResolveInput tracked = hysteresis;
    tracked.previous.reset();

    const auto trackedFirst =
        tracker.ResolveFor(
            subject,
            tracked);

    if (!tracker.Previous(subject).has_value() ||
        tracker.Previous(subject) !=
            trackedFirst.representation)
    {
        return 11;
    }

    tracker.Reset(subject);

    if (tracker.Previous(subject).has_value())
    {
        return 12;
    }

    ResolveInput transitionInput{
        .bodyRadiusMeters = 1.0e6,
        .maximumProductionDetailMeters = 2'000.0,
        .maximumMacroDisplacementMeters = 10'000.0,
        .cameraDistanceToCenterMeters = 1.0e6,
        .verticalFieldOfViewRadians = 1.0,
        .viewportHeightPixels = 1'000.0,
        .features = {
            .productionSurfaceAvailable = true,
            .macroDisplacementAvailable = true
        },
        .policy = {
            .productionSurfaceErrorPixels = 2.0,
            .macroDisplacementErrorPixels = 0.45,
            .smoothGlobeMinimumRadiusPixels = 10.0,
            .discImpostorMinimumRadiusPixels = 0.55,
            .qualityScale = 1.0,
            .hysteresisFraction = 0.15
        }
    };

    Decision transitionDecision =
        Resolve(transitionInput);

    transitionDecision.productionDetailErrorPixels =
        2.3;

    const auto surfaceOnly =
        ResolveSurfaceGlobeTransition(
            transitionInput,
            transitionDecision);

    if (surfaceOnly.productionSurfaceWeight != 1.0 ||
        surfaceOnly.macroGlobeWeight != 0.0 ||
        surfaceOnly.overlapping)
    {
        return 13;
    }

    transitionDecision.productionDetailErrorPixels =
        2.0;

    const auto midpoint =
        ResolveSurfaceGlobeTransition(
            transitionInput,
            transitionDecision);

    if (std::abs(
            midpoint.productionSurfaceWeight -
            0.5) >
            1.0e-12 ||
        std::abs(
            midpoint.macroGlobeWeight -
            0.5) >
            1.0e-12 ||
        !midpoint.overlapping)
    {
        return 14;
    }

    transitionDecision.productionDetailErrorPixels =
        1.7;

    const auto globeOnly =
        ResolveSurfaceGlobeTransition(
            transitionInput,
            transitionDecision);

    if (globeOnly.productionSurfaceWeight != 0.0 ||
        globeOnly.macroGlobeWeight != 1.0 ||
        globeOnly.overlapping)
    {
        return 15;
    }

    if (std::abs(
            midpoint.productionSurfaceWeight +
            midpoint.macroGlobeWeight -
            1.0) >
        1.0e-12)
    {
        return 16;
    }

    ResolveInput missingMacro = transitionInput;
    missingMacro.features.macroDisplacementAvailable =
        false;

    const auto noMacroDecision =
        Resolve(missingMacro);
    const auto noMacro =
        ResolveSurfaceGlobeTransition(
            missingMacro,
            noMacroDecision);

    if (noMacro.productionSurfaceWeight != 1.0 ||
        noMacro.macroGlobeWeight != 0.0)
    {
        return 17;
    }

    ResolveInput missingSurface = transitionInput;
    missingSurface.features.productionSurfaceAvailable =
        false;

    const auto noSurfaceDecision =
        Resolve(missingSurface);
    const auto noSurface =
        ResolveSurfaceGlobeTransition(
            missingSurface,
            noSurfaceDecision);

    if (noSurface.productionSurfaceWeight != 0.0 ||
        noSurface.macroGlobeWeight != 1.0)
    {
        return 18;
    }

    ResolveInput farBlendInput = transitionInput;
    farBlendInput.maximumProductionDetailMeters = 1.0;
    farBlendInput.maximumMacroDisplacementMeters = 450.0;
    farBlendInput.cameraDistanceToCenterMeters = 1.0e6;

    Decision farDecision =
        Resolve(farBlendInput);

    farDecision.macroDisplacementErrorPixels =
        farBlendInput.policy.macroDisplacementErrorPixels;

    const auto macroSmoothBlend =
        ResolveRepresentationBlend(
            farBlendInput,
            farDecision);

    if (macroSmoothBlend.richer !=
            Representation::MacroDisplacedGlobe ||
        macroSmoothBlend.lower !=
            Representation::SmoothGlobe ||
        std::abs(
            macroSmoothBlend.richerWeight -
            0.5) > 1.0e-12 ||
        std::abs(
            macroSmoothBlend.lowerWeight -
            0.5) > 1.0e-12)
    {
        return 19;
    }

    farDecision.macroDisplacementErrorPixels = 0.0;
    farDecision.projectedRadiusPixels =
        farBlendInput.policy.smoothGlobeMinimumRadiusPixels;

    const auto smoothDiscBlend =
        ResolveRepresentationBlend(
            farBlendInput,
            farDecision);

    if (smoothDiscBlend.richer !=
            Representation::SmoothGlobe ||
        smoothDiscBlend.lower !=
            Representation::AnalyticDiscImpostor ||
        !smoothDiscBlend.overlapping)
    {
        return 20;
    }

    farDecision.projectedRadiusPixels =
        farBlendInput.policy.discImpostorMinimumRadiusPixels;

    const auto discPointBlend =
        ResolveRepresentationBlend(
            farBlendInput,
            farDecision);

    if (discPointBlend.richer !=
            Representation::AnalyticDiscImpostor ||
        discPointBlend.lower !=
            Representation::PointProxy ||
        !discPointBlend.overlapping)
    {
        return 21;
    }

    if (std::abs(
            discPointBlend.richerWeight +
            discPointBlend.lowerWeight -
            1.0) > 1.0e-12)
    {
        return 22;
    }

    return 0;
}
