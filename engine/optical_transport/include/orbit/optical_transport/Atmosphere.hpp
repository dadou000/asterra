#pragma once

// Composition and refraction queries shared by every consumer of the atmosphere. SI units throughout.

namespace orbit::optical_transport
{

enum class AerosolKind
{
    MarineSalt,
    Sulfate,
    Organic,
    DustSmoke,
};

struct AerosolProperties
{
    double kappa;          // kappa-Kohler hygroscopicity
    double dryRadius;      // m, representative dry radius
    double dryExtinction;  // 1/m per unit mixing at RH = 0 (reference scale)
};

AerosolProperties AerosolFor(AerosolKind kind);

// Saturation vapour pressure over water (Pa), Magnus form, T in kelvin.
double SaturationVapourPressure(double temperatureK);
// Relative humidity in [0, inf) from absolute water-vapour mixing ratio (kg/kg), pressure (Pa) and temperature.
double RelativeHumidity(double mixingRatio, double pressurePa, double temperatureK);

// Wet/dry radius ratio from kappa-Kohler at relative humidity rh (0..1). Diverges toward activation, so rh is
// clamped to kMaxHumidityForGrowth; at and above that the haze regime hands over to fog (condensed droplets).
inline constexpr double kMaxHumidityForGrowth = 0.995;
double HygroscopicGrowthFactor(double kappa, double relativeHumidity);

// Extinction scale of the aerosol relative to dry: scales with wet cross-section (growth^2) with a
// refractive-index dilution toward water.
double HumidExtinctionScale(AerosolKind kind, double relativeHumidity);

// True when the air holds condensed droplets (fog/cloud) rather than haze.
bool IsFog(double relativeHumidity);

// Refractivity (n - 1) of moist air for visible wavelength (um), T (K), pressure (Pa), vapour pressure (Pa).
// Edlen-type dispersion with pressure/temperature/humidity corrections (accurate to about 1e-8 for visible light).
double AirRefractivity(double wavelengthMicrometers, double temperatureK, double pressurePa, double vapourPressurePa);

// Vertical gradient of n for an atmosphere with the given local lapse rate (K/m); used to bend rays.
double RefractiveGradientVertical(double refractivity, double temperatureK, double lapseKPerM, double gravity = 9.80665);

// Ray bend: d' = d + ds * (grad n - d (d . grad n)) / n, renormalised. Vectors are 3-element arrays.
struct Vec3
{
    double x = 0.0, y = 0.0, z = 0.0;
};
Vec3 BendRay(Vec3 direction, Vec3 gradN, double n, double stepLength);

// Beer-Lambert transmission through a homogeneous slab.
double Transmission(double extinction, double length);

} // namespace orbit::optical_transport
