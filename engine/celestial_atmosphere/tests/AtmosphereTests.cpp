#include <orbit/celestial_atmosphere/Atmosphere.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>

int main()
{
    using namespace orbit::celestial_atmosphere;

    AtmosphereLutConfig config{
        .transmittanceWidth = 24U,
        .transmittanceHeight = 8U,
        .multiScatteringWidth = 12U,
        .multiScatteringHeight = 6U,
        .skyViewWidth = 24U,
        .skyViewHeight = 12U,
        .opticalDepthSteps = 24U,
        .multiDirectionSamples = 16U,
        .skyViewSteps = 16U
    };

    AtmosphereParameters earth;

    // The GPU atmosphere sampling schedule must vary continuously as the
    // closest approach moves through the ray interval. The original
    // 2.5%-threshold mapping jumped by ~0.1 of an orbital ray length,
    // creating concentric dawn/terminator rings in neighboring pixels.
    const auto marchBoundary = [](const double u, const double c)
    {
        const double left = std::sqrt(c);
        const double right = std::sqrt(1.0 - c);
        const double pivot = left / std::max(left + right, 1.0e-6);
        if (u <= pivot)
        {
            const double q = std::clamp(
                (pivot - u) / std::max(pivot, 1.0e-6), 0.0, 1.0);
            return c * (1.0 - q * q * q);
        }
        const double q = std::clamp(
            (u - pivot) / std::max(1.0 - pivot, 1.0e-6), 0.0, 1.0);
        return c + (1.0 - c) * q * q * q;
    };
    for (const double c : {0.0, 0.001, 0.025, 0.25, 0.5,
                           0.75, 0.975, 0.999, 1.0})
    {
        double previous = marchBoundary(0.0, c);
        if (std::abs(previous) > 1.0e-12)
            return 9;
        for (int k = 1; k <= 40; ++k)
        {
            const double next = marchBoundary(double(k) / 40.0, c);
            if (!(next >= previous && next <= 1.0 + 1.0e-12))
                return 10;
            previous = next;
        }
        if (std::abs(previous - 1.0) > 1.0e-12)
            return 11;
    }
    // The values immediately either side of the old arbitrary cutoff
    // must now differ only by the movement of the physical ray.
    for (const double c : {0.025, 0.975})
    {
        for (int k = 0; k <= 40; ++k)
        {
            const double u = double(k) / 40.0;
            const double a = marchBoundary(u, c - 1.0e-6);
            const double b = marchBoundary(u, c + 1.0e-6);
            if (std::abs(b - a) > 1.0e-3)
                return 12;
        }
    }

    const auto seaLevel =
        SampleAtmosphere(
            earth,
            earth.bottomRadiusMeters);

    const auto high =
        SampleAtmosphere(
            earth,
            earth.bottomRadiusMeters +
                50000.0);

    if (!(seaLevel.rayleighDensity >
            high.rayleighDensity) ||
        !(seaLevel.mieDensity >
            high.mieDensity))
    {
        return 1;
    }

    const auto a =
        BuildStaticLuts(
            earth,
            config);
    const auto b =
        BuildStaticLuts(
            earth,
            config);

    if (a.fingerprint !=
            b.fingerprint ||
        a.transmittance.texels.size() !=
            24U * 8U ||
        a.multiScattering.texels.size() !=
            12U * 6U)
    {
        return 2;
    }

    bool hasTransmission = false;
    bool hasExtinction = false;

    for (const auto value :
         a.transmittance.texels)
    {
        hasTransmission |=
            value.x > 0.5F;
        hasExtinction |=
            value.x < 0.5F;
    }

    if (!hasTransmission ||
        !hasExtinction)
    {
        return 3;
    }

    // Sunlight at the ground reddens toward the horizon: more extinction in
    // blue than red, and less light overall than at the zenith.
    const auto zenithSun =
        SunTransmittanceAt(earth, a, earth.bottomRadiusMeters, 1.0);
    const auto lowSun =
        SunTransmittanceAt(earth, a, earth.bottomRadiusMeters, 0.08);

    if (!(zenithSun.x > lowSun.x) ||
        !(lowSun.x > lowSun.z) ||
        !(zenithSun.z > lowSun.z) ||
        !(zenithSun.x <= 1.0))
    {
        return 5;
    }

    const auto sky =
        BuildSkyView(
            earth,
            a,
            {
                .observerRadiusMeters =
                    earth.bottomRadiusMeters +
                    2.0,
                .sunDirectionBody = {
                    0.0, 0.0, 1.0},
                .incidentIrradianceWattsPerSquareMeter = {
                    1361.0, 1361.0, 1361.0}
            },
            config);

    if (sky.skyView.texels.size() !=
            24U * 12U ||
        sky.fingerprint == 0U)
    {
        return 4;
    }

    bool hasSkyRadiance = false;

    for (const auto value :
         sky.skyView.texels)
    {
        if (value.x > 0.0F ||
            value.y > 0.0F ||
            value.z > 0.0F)
        {
            hasSkyRadiance = true;
            break;
        }
    }

    if (!hasSkyRadiance)
    {
        return 5;
    }

    const auto orbitalSky =
        BuildSkyView(
            earth,
            a,
            {
                .observerRadiusMeters =
                    earth.topRadiusMeters +
                    2.0e6,
                .sunDirectionBody = {
                    0.0, 0.0, 1.0},
                .incidentIrradianceWattsPerSquareMeter = {
                    1361.0, 1361.0, 1361.0}
            },
            config);

    bool hasOrbitalLimb = false;

    for (const auto value :
         orbitalSky.skyView.texels)
    {
        if (value.x > 0.0F ||
            value.y > 0.0F ||
            value.z > 0.0F)
        {
            hasOrbitalLimb = true;
            break;
        }
    }

    if (!hasOrbitalLimb ||
        orbitalSky.fingerprint ==
            sky.fingerprint ||
        a.fingerprint !=
            b.fingerprint)
    {
        return 6;
    }

    const auto darkerSkyFingerprint =
        AtmosphereSkyFingerprint(
            earth,
            a.fingerprint,
            {
                .observerRadiusMeters =
                    earth.bottomRadiusMeters +
                    2.0,
                .sunDirectionBody = {
                    0.0, 0.0, 1.0},
                .incidentIrradianceWattsPerSquareMeter = {
                    680.5, 680.5, 680.5}
            },
            config);

    if (darkerSkyFingerprint ==
        sky.fingerprint)
    {
        return 7;
    }

    const auto revised =
        AtmosphereFingerprint(
            AtmosphereParameters{
                .bottomRadiusMeters =
                    earth.bottomRadiusMeters,
                .topRadiusMeters =
                    earth.topRadiusMeters +
                    1000.0
            },
            config);

    if (revised == a.fingerprint)
    {
        return 8;
    }

    return 0;
}
