#include <orbit/celestial_compact_objects/CompactObject.hpp>

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

    return 0;
}
