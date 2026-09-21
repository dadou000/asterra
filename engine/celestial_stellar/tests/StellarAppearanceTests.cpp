#include <orbit/celestial_stellar/StellarAppearance.hpp>

#include <cmath>

int main()
{
    using namespace orbit::celestial_stellar;

    const auto cool =
        BlackbodyColorLinear(3000.0);
    const auto solar =
        BlackbodyColorLinear(5772.0);
    const auto hot =
        BlackbodyColorLinear(12000.0);

    if (!(cool.x > cool.z))
        return 1;

    if (!(hot.z > hot.x))
        return 2;

    if (!(solar.x > 0.6) ||
        !(solar.y > 0.6))
        return 3;

    if (!(LinearLimbDarkening(1.0,0.6) >
          LinearLimbDarkening(0.0,0.6)))
        return 4;

    StellarAppearanceParameters p;
    p.activitySeed=42;

    const double g0 =
        GranulationModulation(
            {1.0,0.0,0.0},p);
    const double g1 =
        GranulationModulation(
            {0.0,1.0,0.0},p);

    if (std::abs(g0-g1)<1e-6)
        return 5;

    if (!(ChromosphereProfile(
              1.01,p)>0.0) ||
        ChromosphereProfile(
              1.20,p)!=0.0)
        return 6;

    if (!(CoronaProfile(
              1.10,p)>0.0) ||
        CoronaProfile(
              4.0,p)!=0.0)
        return 7;

    if (StellarAppearanceFingerprint(p)==0U)
        return 8;

    return 0;
}
