#pragma once

#include <orbit/storm_electrification/ChargeGrid.hpp>

#include <vector>

namespace orbit::storm_electrification
{

// Fields the storm supplies per cell (same layout and size as ChargeGrid). The current FastStormSolver has warm rain
// only (qv, qc, qr) and no ice phase, so `graupel` and `ice` must be derived by the caller (for example from
// supercooled cloud and rain water above the freezing level) until the solver gains ice microphysics.
struct StormFields
{
    std::vector<float> temperatureK;
    std::vector<float> airDensity;     // kg/m^3
    std::vector<float> updraft;        // m/s
    std::vector<float> graupel;        // kg/kg
    std::vector<float> ice;            // kg/kg (crystals and snow)
    std::vector<float> liquid;         // kg/kg supercooled cloud water
};

// Non-inductive graupel-ice charging with a temperature-dependent sign, then conservative vertical transport of the
// two carriers. The magnitudes are order-of-magnitude placeholders, NOT fitted to observations.
struct ElectrificationParams
{
    // Charge exchanged per (kg/m^3 graupel x kg/kg ice) per second, C m^-3 s^-1 per unit; sets how fast fields build.
    double chargingEfficiency = 2.0e-5;
    double reversalTemperatureK = 258.15;    // about -15 C: graupel is negative warmer than this, positive colder
    double reversalWidthK = 3.0;             // smooth sign change
    double iceOnsetK = 273.15;               // no riming charge above freezing
    double liquidRamp = 2.0e-4;              // kg/kg of supercooled water giving full charging
    double graupelFallSpeed = 3.0;           // m/s relative to the air
    double iceFallSpeed = 0.3;
};

struct ChargeBudget
{
    double leftThroughTop = 0.0;   // C carried out of the block upward
    double groundDeposit = 0.0;    // C delivered to the ground by falling graupel
};

// Advances charging and transport by dt (sub-stepped to the vertical CFL limit). Returns the charge budget of the step
// so a caller can verify  d(total charge) = -(top + ground).
ChargeBudget StepElectrification(
    ChargeGrid& grid, const StormFields& fields, const ElectrificationParams& params, double dt);

} // namespace orbit::storm_electrification
