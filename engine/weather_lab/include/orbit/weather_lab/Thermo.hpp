#pragma once

#include <cstdint>
#include <vector>

// Moist thermodynamics and the Weisman-Klemp supercell environment used by the
// weather lab. Constants follow CM1 (constants.F) so SC-01 comparisons start
// from the same atmosphere.

namespace orbit::weather_lab
{
namespace thermo
{
inline constexpr float kGravity = 9.81F;
inline constexpr float kCp = 1005.7F;
inline constexpr float kRd = 287.04F;
inline constexpr float kRv = 461.5F;
inline constexpr float kLv = 2.5e6F;
inline constexpr float kP00 = 100000.0F;
inline constexpr float kRepsilon = kRv / kRd;
} // namespace thermo

// Saturation mixing ratio over liquid water (Bolton 1980), temperature in K,
// pressure in Pa.
[[nodiscard]] float SaturationMixingRatio(float pressure, float temperature);

struct SupercellSoundingParams
{
    // Weisman-Klemp analytic thermodynamic profile (CM1 isnd = 5).
    float tropopauseHeight = 12000.0F;
    float tropopauseTheta = 343.0F;
    float tropopauseTemperature = 213.0F;
    float surfaceTheta = 300.0F;
    float surfacePressure = 100000.0F;
    float boundaryLayerMixingRatio = 0.014F;
    // Quarter-circle hodograph (CM1 iwnd = 2).
    float windDepthLow = 2000.0F;
    float windDepthHigh = 6000.0F;
    float windSpeedLow = 7.0F;
    float windSpeedHigh = 31.0F;
};

// Hydrostatic horizontally uniform base state sampled on the cell centres and
// faces of a uniform vertical grid.
struct BaseState
{
    std::vector<float> centreHeight;
    std::vector<float> theta;     // potential temperature, centres
    std::vector<float> vapor;     // water-vapour mixing ratio, centres
    std::vector<float> exner;     // Exner function, centres
    std::vector<float> pressure;  // Pa, centres
    std::vector<float> density;   // kg/m^3, centres
    std::vector<float> faceDensity; // kg/m^3, nz + 1 faces
    std::vector<float> windU;     // m/s, centres
    std::vector<float> windV;     // m/s, centres
};

[[nodiscard]] BaseState BuildSupercellBaseState(
    const SupercellSoundingParams& params,
    std::uint32_t layers,
    float layerThickness);
} // namespace orbit::weather_lab
