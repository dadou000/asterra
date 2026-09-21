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

    return 0;
}
