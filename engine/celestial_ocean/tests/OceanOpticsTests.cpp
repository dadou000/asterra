#include <orbit/celestial_ocean/OceanOptics.hpp>

#include <cmath>

int main()
{
    using namespace orbit::celestial_ocean;

    const OceanOpticalParameters p{};

    const double f0 =
        DielectricNormalReflectance(
            1.0,
            p.refractiveIndex);

    if (!(f0 > 0.019) ||
        !(f0 < 0.022))
        return 1;

    if (!(SchlickFresnel(0.05, f0) >
          SchlickFresnel(1.0, f0)))
        return 2;

    const double aligned =
        GgxSpecularBrdf(
            1.0, 1.0, 1.0, 1.0,
            p.orbitalRoughness,
            f0);

    const double off =
        GgxSpecularBrdf(
            0.5, 0.5, 0.5, 0.7,
            p.orbitalRoughness,
            f0);

    if (!(aligned > off) ||
        !(aligned > 0.0))
        return 3;

    const auto t1 =
        WaterColumnTransmittance(
            p.absorptionPerMeter,
            1.0);
    const auto t50 =
        WaterColumnTransmittance(
            p.absorptionPerMeter,
            50.0);

    if (!(t50.x < t1.x) ||
        !(t50.y < t1.y) ||
        !(t50.z < t1.z))
        return 4;

    const auto shallow =
        DeepWaterColor(p, 1.0);
    const auto deep =
        DeepWaterColor(p, 100.0);

    if (!(deep.z > shallow.z))
        return 5;

    if (OceanOpticalFingerprint(p) == 0U)
        return 6;

    orbit::celestial_appearance::
        PlanetaryAppearanceProduct appearance;
    appearance.faceResolution = 1U;
    appearance.fingerprint = 99U;
    appearance.texels.resize(1U);
    appearance.texels[0].albedoLinear = {
        0.10F, 0.12F, 0.14F};
    appearance.texels[0].roughness = 0.8F;
    appearance.texels[0].oceanMask = 1.0F;
    appearance.texels[0].waterDepthMeters = 100.0F;
    appearance.texels[0].directLightTransmittance = 0.25F;

    const auto beforeColor =
        appearance.texels[0].albedoLinear;
    const auto beforeFingerprint =
        appearance.fingerprint;

    ApplyOrbitalOceanAppearance(
        appearance,
        p);

    if (appearance.fingerprint ==
            beforeFingerprint ||
        appearance.texels[0].
            albedoLinear.z <=
            beforeColor.z ||
        std::abs(
            appearance.texels[0].
                roughness -
            static_cast<float>(
                p.orbitalRoughness)) >
            1.0e-5F ||
        appearance.texels[0].
            directLightTransmittance !=
            0.25F ||
        appearance.texels[0].
            oceanMask !=
            1.0F)
        return 7;

    return 0;
}
