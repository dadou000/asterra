#pragma once

#include <orbit/optical_transport/DischargeGraph.hpp>

#include <cstdint>

namespace orbit::storm_electrification
{
// What a lightning flash looks like in time, colour and brightness, from the structure of its channel (a tree of segments with the time each
// formed). Replaces the pulses of the graph with a sequence of emission events, each cooling from a peak temperature:
//
//   - the stepped leader: each segment glows as it forms (about 9000 K, 2e4 W/m, the return stroke being 20 to 100 times brighter) and fades
//     and cools within milliseconds; a branch that carries no current afterwards is never lit again;
//   - the attachment (to the ground, or the end of the growth of an intracloud channel): a return stroke that runs along the main path
//     (the path from the root to the ground contact, or to the end of the heaviest chain) at 1.5e8 m/s, and a weaker one in each side branch
//     that carries a noticeable share of the charge, starting where it leaves the main path; the twigs are dark;
//   - for a flash to the ground, the later strokes: a dim dart leader down the main path (2e6 m/s) and a return stroke along it, 35 ms apart
//     (log-normal), about three of them on average, some followed by a continuing current (a steady glow of the whole path for milliseconds,
//     sometimes hundreds); for an intracloud flash, a few recoil (K) events along the main path ten milliseconds or so apart;
//   - every event cools: temperature T(t) = (Tpeak - Tslow) exp(-t / tau1) + Tslow exp(-t / tau2), power P(t) = Ppeak (T / Tpeak)^4 (a
//     radiating plasma), so the hot phase is a few hundred microseconds of blue-white light and the cooled channel a faint orange glow whose
//     length grows with the current (tau2 from 0.5 ms at 1 kA to tens of ms at 100 kA).
//
// The numbers are from field measurements (see /world/weather-lab/global-system-status-p10-emission): first return stroke 30 kA and 1e6 W/m of
// optical power, subsequent 12 kA and 2.5e5 W/m (power grows as current^1.5), peak temperatures 28 to 36 kK.
struct EmissionParams
{
    double returnStrokeSpeed = 1.5e8;      // m/s
    double dartLeaderSpeed = 2.0e6;        // m/s (measured 1e6 to 2e6, quoted up to 1e7)
    double recoilSpeed = 5.0e6;            // m/s
    double firstStrokeCurrent = 30.0e3;    // A (median, log-normal)
    double subsequentCurrent = 12.0e3;     // A
    double intracloudCurrent = 8.0e3;      // A, the discharge of an intracloud channel when its growth ends
    double currentSpread = 0.5;            // sigma of ln(current)
    double referenceCurrent = 30.0e3;      // A
    double referencePowerPerMeter = 1.0e6; // W/m optical at the reference current, power ~ (I / I_ref)^1.5
    double interStrokeMedian = 0.035;      // s
    double interStrokeSpread = 0.6;        // sigma of ln(interval)
    double meanSubsequentStrokes = 3.0;
    double continuingCurrentChance = 0.5;  // per stroke; 7 % of strokes have one longer than 40 ms
    double branchMinimumShare = 0.012;     // of the charge of the flash: a branch with less is not lit by the return stroke
};

// Replaces the pulses of `graph` (its segments' parents and formedAt define the tree). `grounded` and `groundSegment` give the contact of a
// flash to the ground; `attachTime` is when the leader reached it (or stopped growing). Deterministic in `seed`.
void AssignPhysicalEmission(optical_transport::DischargeGraph& graph, bool grounded, std::int32_t groundSegment, double attachTime,
                            std::uint64_t seed, const EmissionParams& params = {});
} // namespace orbit::storm_electrification
