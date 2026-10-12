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
    // How the channel grows. The defaults grow it as a random mesh: every cell of the channel may extend, in proportion to the field excess,
    // and the optical energy is the same in every segment. These make it a tree with a trunk, like a dielectric breakdown model:
    //   growthExponent       the chance of a candidate cell goes with excess^exponent (1 = linear; 3 or more concentrates growth where the
    //                        field is strongest, so the channel runs toward the opposite charge instead of filling the volume);
    //   sideBranchChance     the chance that a cell with more than one channel neighbour (not a tip) may extend in a round (1 = always);
    //   picksDivisor         cells added per round = 1 + channel cells / picksDivisor (smaller = faster, bushier growth);
    //   currentWeighted      the energy radiated by the intracloud channel is shared over its segments in proportion to the number of
    //                        segments downstream of each (the charge each carries back to the root), so the trunk is bright and the twigs dim.
    double growthExponent = 1.0;
    double sideBranchChance = 1.0;
    double picksDivisor = 8.0;
    bool currentWeighted = false;
    // The channel may step to any of the 26 neighbours of a cell (diagonals included) instead of the 6 that share a face, so it is not made of
    // axis-aligned runs of the grid.
    bool diagonalGrowth = false;
    // A candidate step is multiplied by 1 + directionPersistence * max(0, cos) where cos is the angle between the step and the heading of the
    // segment that reached its parent: the channel keeps its heading, so it runs across the cloud instead of curling into a ball.
    double directionPersistence = 0.0;
    // A conducting channel screens the field around it: a candidate cell is multiplied by screening^(channel cells touching it - 1), so cells
    // that hug the channel are unlikely and the tip, which touches only its parent, is the favourite (1 = no screening).
    double screening = 1.0;
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
