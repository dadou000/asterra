#include <orbit/celestial_atmosphere/Atmosphere.hpp>

#include <cmath>

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
        return 6;
    }

    return 0;
}
