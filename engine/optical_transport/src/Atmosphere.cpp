#include "orbit/optical_transport/Atmosphere.hpp"

#include <algorithm>
#include <cmath>

namespace orbit::optical_transport
{

AerosolProperties AerosolFor(AerosolKind kind)
{
    switch (kind)
    {
    case AerosolKind::MarineSalt: return {1.2, 0.2e-6, 4.0e-5};
    case AerosolKind::Sulfate: return {0.6, 0.08e-6, 6.0e-5};
    case AerosolKind::Organic: return {0.1, 0.1e-6, 3.0e-5};
    case AerosolKind::DustSmoke: return {0.03, 0.3e-6, 5.0e-5};
    }
    return {0.1, 0.1e-6, 3.0e-5};
}

double SaturationVapourPressure(double temperatureK)
{
    const double c = temperatureK - 273.15;
    return 610.94 * std::exp(17.625 * c / (c + 243.04));
}

double RelativeHumidity(double mixingRatio, double pressurePa, double temperatureK)
{
    const double vapourPressure = pressurePa * mixingRatio / (0.622 + mixingRatio);
    return vapourPressure / SaturationVapourPressure(temperatureK);
}

double HygroscopicGrowthFactor(double kappa, double relativeHumidity)
{
    const double rh = std::clamp(relativeHumidity, 0.0, kMaxHumidityForGrowth);
    // kappa-Kohler (curvature neglected): aw = (D^3 - D0^3) / (D^3 - D0^3 (1 - kappa)) = rh.
    // Solve for the volume ratio V = (D/D0)^3: V = 1 + kappa * rh / (1 - rh).
    const double volumeRatio = 1.0 + kappa * rh / (1.0 - rh);
    return std::cbrt(volumeRatio);
}

double HumidExtinctionScale(AerosolKind kind, double relativeHumidity)
{
    const AerosolProperties props = AerosolFor(kind);
    const double growth = HygroscopicGrowthFactor(props.kappa, relativeHumidity);
    // Cross-section grows with radius^2; the particle's refractive index dilutes toward water (1.33).
    const double volumeFraction = 1.0 / (growth * growth * growth);
    const double dilution = 0.5 + 0.5 * volumeFraction;
    return growth * growth * dilution;
}

bool IsFog(double relativeHumidity)
{
    return relativeHumidity >= 1.0;
}

double AirRefractivity(double wavelengthMicrometers, double temperatureK, double pressurePa, double vapourPressurePa)
{
    const double sigma2 = 1.0 / (wavelengthMicrometers * wavelengthMicrometers);
    // Edlen (1966) dispersion for standard air (15 C, 101325 Pa, dry), (n - 1) * 1e8.
    const double standard = 8342.13 + 2406030.0 / (130.0 - sigma2) + 15997.0 / (38.9 - sigma2);
    const double tC = temperatureK - 273.15;
    const double density = (pressurePa / 101325.0) * (1.0 + 0.00366 * 15.0) / (1.0 + 0.00366 * tC);
    // Humidity correction (Edlen): subtract f * (5.722 - 0.0457 sigma^2) * 1e-8 with f in Pa -> mmHg-like scale.
    const double vapourTorr = vapourPressurePa / 133.322;
    const double humidity = vapourTorr * (5.722 - 0.0457 * sigma2) * 1.0e-8 / (1.0 + 0.00366 * tC);
    return standard * 1.0e-8 * density - humidity;
}

double RefractiveGradientVertical(double refractivity, double temperatureK, double lapseKPerM, double gravity)
{
    // n - 1 ~ rho; for hydrostatic ideal gas d(ln rho)/dz = -(g/(R T)) - (1/T) dT/dz, with dT/dz = -lapse.
    const double specificGas = 287.05;
    const double dlnRho = -gravity / (specificGas * temperatureK) + lapseKPerM / temperatureK;
    return refractivity * dlnRho;
}

Vec3 BendRay(Vec3 d, Vec3 g, double n, double stepLength)
{
    const double dot = d.x * g.x + d.y * g.y + d.z * g.z;
    const double inv = stepLength / n;
    Vec3 out{d.x + (g.x - d.x * dot) * inv, d.y + (g.y - d.y * dot) * inv, d.z + (g.z - d.z * dot) * inv};
    const double len = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z);
    return {out.x / len, out.y / len, out.z / len};
}

double Transmission(double extinction, double length)
{
    return std::exp(-extinction * length);
}

} // namespace orbit::optical_transport
