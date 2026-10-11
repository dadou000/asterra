#pragma once

#include <orbit/optical_transport/DischargeGraph.hpp>
#include <orbit/storm_electrification/ChargeGrid.hpp>

#include <cstdint>

namespace orbit::storm_electrification
{

// Stochastic bidirectional-leader flash on a ChargeGrid after Mansell et al. (2002): the flash starts where the field
// exceeds an altitude-scaled initiation threshold, then the channel (an equipotential conductor) grows cell by cell
// into neighbours whose potential differs from it by more than a propagation threshold times the cell size, with
// probability proportional to the excess. Charge near the channel is then neutralised. Thresholds and optical
// energies are configurable defaults of the right order of magnitude, not fitted values.
struct LeaderParams
{
    double initiationField = 1.5e5;        // V/m at sea level
    double densityScaleHeight = 8400.0;    // m; thresholds fall with air density
    double propagationField = 6.0e4;       // V/m at sea level
    std::uint32_t maxChannelCells = 6000;
    // A flash stops growing once the charge it could neutralise (of the smaller polarity, near the channel) reaches
    // this many coulombs. With the channel held at a fixed potential the potential criterion alone keeps being
    // satisfied across ever larger volumes, so without a budget every flash would fill maxChannelCells.
    double maxFlashCharge = 25.0;   // also the most charge of either polarity one flash neutralises
    // ...or once this many consecutive growth rounds reach no new charge (the channel is crossing empty air).
    std::uint32_t stagnationRounds = 16;
    // The charge budget only applies once the channel has this many cells; a strongly charged storm holds more than
    // the budget around a single cell, and a flash must still span a region to connect opposite polarities.
    std::uint32_t minChannelCells = 12;
    std::uint32_t neutralizationRadiusCells = 2;
    double neutralizationFraction = 0.5;   // share of local charge removed near the channel
    double groundContactHeight = 1.0;      // channel cells below this many cell heights touch the ground
    double crossCloudSpanMeters = 15000.0; // an ungrounded channel wider than this is cloud-to-cloud
    double leaderSpeed = 2.0e5;            // m/s, stepped-leader front
    double returnStrokeSpeed = 1.0e8;      // m/s
    double returnStrokeEnergy = 3.0e5;     // J of optical energy radiated by a first return stroke
    double leaderEnergyPerMeter = 0.2;     // J/m of faint leader luminosity (a stepped leader is far dimmer than its stroke)
    double intracloudEnergyPerMeter = 40.0;// J/m radiated as the channel conducts (no return stroke)
    double pulseDuration = 3.0e-5;         // s
    std::uint64_t seed = 1;
};

struct FlashResult
{
    bool occurred = false;
    optical_transport::FlashKind kind = optical_transport::FlashKind::IntraCloud;
    optical_transport::DischargeGraph graph{optical_transport::FlashKind::IntraCloud};
    std::uint32_t channelCells = 0;
    double positiveRemoved = 0.0;   // C of positive space charge neutralised
    double negativeRemoved = 0.0;   // C of negative space charge neutralised (magnitude)
    double groundCharge = 0.0;      // C delivered to ground (signed: net charge the channel removed from the cloud)
    double energyBefore = 0.0;      // J field energy
    double energyAfter = 0.0;
    double startTime = 0.0;
    double endTime = 0.0;
};

// Tries to start one flash at `startTime`. The grid's potential is (re)solved first and again afterwards. Changes the
// grid's charge only when a flash occurs.
FlashResult TryFlash(ChargeGrid& grid, const LeaderParams& params, double startTime, std::uint64_t flashIndex);

// Largest ratio |E| / initiationThreshold(z) over the grid (>= 1 means a flash can start). Uses the current potential.
double MaxBreakdownRatio(const ChargeGrid& grid, const LeaderParams& params, std::uint32_t* i = nullptr,
                         std::uint32_t* j = nullptr, std::uint32_t* k = nullptr);

} // namespace orbit::storm_electrification
