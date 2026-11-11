#pragma once

#include <orbit/storm_electrification/Electrification.hpp>

#include <vector>

namespace orbit::storm_electrification
{
// Plain per-cell inputs from a warm-rain storm solver (x fastest, then y, then z; same layout as ChargeGrid). The caller
// copies them out of its solver, so this library does not depend on any one model.
struct WarmRainInputs
{
    std::vector<float> potentialTemperatureK;   // full theta
    std::vector<float> exner;                   // Exner function per cell
    std::vector<float> airDensity;              // kg/m^3 per cell
    std::vector<float> updraft;                 // m/s
    std::vector<float> cloudWater;              // qc, kg/kg
    std::vector<float> rainWater;               // qr, kg/kg
};

// Parameters of the ice-phase PROXY. A warm-rain solver has no ice, so the ice phase is assumed from temperature:
// rain carried above the freezing level is taken to be frozen graupel, cloud water colder than `glaciationK` is ice
// crystals, and cloud water between that and 0 C stays supercooled liquid. These are hypotheses, not microphysics:
// they fix where the charging regions are, not how strong they are.
struct IceProxyParams
{
    double graupelBelowK{268.15};     // rain colder than this counts as graupel
    double glaciationK{253.15};       // cloud water colder than this counts as ice
    double freezingK{273.15};
    double graupelFraction{0.5};      // share of the rain mass assumed to be graupel (rest is slow-falling snow, ice)
};

[[nodiscard]] StormFields StormFieldsFromWarmRain(const WarmRainInputs& in, const IceProxyParams& params = {});
} // namespace orbit::storm_electrification
