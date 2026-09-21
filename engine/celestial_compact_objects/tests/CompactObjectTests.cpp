#include <orbit/celestial_compact_objects/CompactObject.hpp>
#include <orbit/celestial_representation/RepresentationResolver.hpp>

#include <cmath>

int main()
{
    using namespace orbit;
    using namespace orbit::celestial_compact_objects;

    CompactObjectParameters compact{
        .gravitationalParameterM3PerS2 =
            1.3271645321e20
    };

    const auto scales =
        ResolveScales(compact);

    if (!(scales.schwarzschildRadiusMeters >
          2000.0) ||
        !(scales.photonSphereRadiusMeters >
          scales.schwarzschildRadiusMeters) ||
        !(scales.iscoRadiusMeters >
          scales.photonSphereRadiusMeters) ||
        !(scales.criticalImpactParameterMeters >
          scales.photonSphereRadiusMeters))
    {
        return 1;
    }

    const f64 expectedCritical =
        3.0 *
        std::sqrt(3.0) *
        scales.gravitationalRadiusMeters;

    if (std::abs(
            scales.
                criticalImpactParameterMeters -
            expectedCritical) >
        expectedCritical * 1.0e-12)
    {
        return 2;
    }

    const auto presentation =
        BuildCompactObjectPresentation(
            compact);

    if (presentation.fingerprint == 0U ||
        presentation.shadowRadiusMeters <=
            scales.schwarzschildRadiusMeters)
    {
        return 3;
    }

    const f64 weak =
        WeakFieldDeflectionRadians(
            compact,
            scales.gravitationalRadiusMeters *
                1000.0);

    if (!(weak > 0.0 &&
          weak < 0.01))
    {
        return 4;
    }

    AccretionFlowParameters flow{};

    const auto accretion =
        BuildAccretionFlowProduct(
            flow,
            compact,
            64U);

    if (accretion.fingerprint == 0U ||
        accretion.radialProfile.size() !=
            64U ||
        !(accretion.outerRadiusMeters >
          accretion.innerRadiusMeters))
    {
        return 5;
    }

    auto changed = compact;
    changed.shadowScale = 1.2;

    if (CompactObjectFingerprint(changed) ==
        presentation.fingerprint)
    {
        return 6;
    }

    const f64 opticalRadius =
        accretion.outerRadiusMeters;

    const auto resolveAtPixels =
        [&](const f64 projectedRadiusPixels)
        {
            const f64 fov = 1.0;
            const f64 height = 1000.0;
            const f64 angular =
                projectedRadiusPixels *
                fov /
                height;
            const f64 distance =
                opticalRadius /
                std::sin(angular);

            return celestial_representation::
                Resolve({
                    .bodyRadiusMeters =
                        opticalRadius,
                    .maximumProductionDetailMeters =
                        0.0,
                    .maximumMacroDisplacementMeters =
                        0.0,
                    .cameraDistanceToCenterMeters =
                        distance,
                    .verticalFieldOfViewRadians =
                        fov,
                    .viewportHeightPixels =
                        height,
                    .features = {
                        .productionSurfaceAvailable =
                            false,
                        .macroDisplacementAvailable =
                            false,
                        .complexFarAppearance =
                            false,
                        .radiativeEmitter =
                            false
                    }
                });
        };

    const auto resolved =
        resolveAtPixels(20.0);
    const auto disc =
        resolveAtPixels(2.0);
    const auto point =
        resolveAtPixels(0.2);

    if (resolved.representation !=
            celestial_representation::
                Representation::SmoothGlobe ||
        disc.representation !=
            celestial_representation::
                Representation::
                    AnalyticDiscImpostor ||
        point.representation !=
            celestial_representation::
                Representation::PointProxy)
    {
        return 7;
    }

    const auto boundary =
        resolveAtPixels(0.55);

    const auto boundaryBlend =
        celestial_representation::
            ResolveRepresentationBlend(
                {
                    .bodyRadiusMeters =
                        opticalRadius,
                    .maximumProductionDetailMeters =
                        0.0,
                    .maximumMacroDisplacementMeters =
                        0.0,
                    .cameraDistanceToCenterMeters =
                        opticalRadius /
                        std::sin(0.55 / 1000.0),
                    .verticalFieldOfViewRadians =
                        1.0,
                    .viewportHeightPixels =
                        1000.0,
                    .features = {
                        .productionSurfaceAvailable =
                            false,
                        .macroDisplacementAvailable =
                            false,
                        .complexFarAppearance =
                            false,
                        .radiativeEmitter =
                            false
                    }
                },
                boundary);

    if (!boundaryBlend.overlapping ||
        boundaryBlend.lower !=
            celestial_representation::
                Representation::PointProxy ||
        !(boundaryBlend.lowerWeight > 0.0 &&
          boundaryBlend.lowerWeight < 1.0))
    {
        return 8;
    }

    return 0;
}
