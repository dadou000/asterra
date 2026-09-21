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

    return 0;
}
