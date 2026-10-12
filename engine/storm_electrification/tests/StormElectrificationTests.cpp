#include <orbit/storm_electrification/ChargeGrid.hpp>
#include <orbit/storm_electrification/Electrification.hpp>
#include <orbit/storm_electrification/FlashSchedule.hpp>
#include <orbit/storm_electrification/LeaderSimulation.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

using namespace orbit::storm_electrification;
using orbit::optical_transport::FlashKind;

namespace
{
int failures = 0;
void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
bool Near(double a, double b, double rel) { return std::abs(a - b) <= rel * std::max(std::abs(a), std::abs(b)) + 1.0e-300; }

// Synthetic supercell: a Gaussian updraft column with ice and graupel proxies above the freezing level.
StormFields MakeStorm(const ChargeGrid& g, double peakUpdraft)
{
    StormFields f;
    const std::size_t n = g.CellCount();
    f.temperatureK.resize(n); f.airDensity.resize(n); f.updraft.resize(n);
    f.graupel.resize(n); f.ice.resize(n); f.liquid.resize(n);
    const double cx = g.Nx() * 0.5 * g.Spacing(), cy = g.Ny() * 0.5 * g.Spacing();
    for (std::uint32_t k = 0; k < g.Nz(); ++k)
        for (std::uint32_t j = 0; j < g.Ny(); ++j)
            for (std::uint32_t i = 0; i < g.Nx(); ++i)
            {
                const std::size_t c = g.Index(i, j, k);
                const double z = g.Height(k);
                const double r = std::hypot((i + 0.5) * g.Spacing() - cx, (j + 0.5) * g.Spacing() - cy);
                const double column = std::exp(-r * r / (2.0 * 4000.0 * 4000.0));
                const double vertical = std::sin(std::numbers::pi * std::clamp((z - 500.0) / 12000.0, 0.0, 1.0));
                const double temperature = 300.0 - 6.5e-3 * z;
                f.temperatureK[c] = static_cast<float>(temperature);
                f.airDensity[c] = static_cast<float>(1.2 * std::exp(-z / 8400.0));
                f.updraft[c] = static_cast<float>(peakUpdraft * column * vertical);
                const bool cold = temperature < 268.0, veryCold = temperature < 253.0;
                // Condensate (and so ice and graupel) grows with the updraft that lifts the vapour: reference 25 m/s.
                const double strength = peakUpdraft / 25.0;
                f.graupel[c] = static_cast<float>(cold ? 2.0e-3 * column * strength : 0.0);
                f.ice[c] = static_cast<float>((cold ? 1.0e-3 : 0.0) * std::exp(-r * r / (2.0 * 6000.0 * 6000.0)) * (veryCold ? 2.0 : 1.0) * strength);
                // Supercooled liquid exists from freezing down to about -20 C and thins with temperature; colder than that an
                // updraft is glaciated, so there is no riming charge there. (Letting liquid reach -28 C puts negative ice aloft.)
                const double liquidFraction = std::clamp((temperature - 253.0) / (273.15 - 253.0), 0.0, 1.0);
                f.liquid[c] = static_cast<float>(1.5e-3 * column * std::min(1.0, strength) * liquidFraction);
            }
    return f;
}

struct StormRun
{
    int flashes = 0;
    int cloudToGround = 0;
    double flashCharge = 0.0;
    double worstChargeImbalance = 0.0;
    bool energyAlwaysDropped = true;
};

StormRun RunStorm(double peakUpdraft, double seconds, std::uint64_t seed)
{
    ChargeGrid grid(40, 40, 28, 600.0);
    const StormFields fields = MakeStorm(grid, peakUpdraft);
    const ElectrificationParams electrification;
    LeaderParams leader;
    leader.seed = seed;
    StormRun run;
    for (double t = 0.0; t < seconds; t += 5.0)
    {
        StepElectrification(grid, fields, electrification, 5.0);
        grid.SolvePotential();
        for (int guard = 0; guard < 5 && MaxBreakdownRatio(grid, leader) >= 1.0; ++guard)
        {
            const FlashResult flash = TryFlash(grid, leader, t, static_cast<std::uint64_t>(run.flashes));
            if (!flash.occurred) break;
            ++run.flashes;
            run.cloudToGround += flash.kind == FlashKind::CloudToGround ? 1 : 0;
            run.flashCharge += flash.positiveRemoved;
            if (flash.kind != FlashKind::CloudToGround)
                run.worstChargeImbalance = std::max(run.worstChargeImbalance, std::abs(flash.positiveRemoved - flash.negativeRemoved));
            run.energyAlwaysDropped = run.energyAlwaysDropped && flash.energyAfter < flash.energyBefore;
        }
    }
    return run;
}
} // namespace

int main()
{
    // --- potential solver against an analytic case: uniform charge between two grounded planes.
    // The grounded planes sit half a cell outside the first/last cell centres, so the plate gap is (nz + 1) cells.
    {
        ChargeGrid g(64, 64, 16, 100.0);
        const double rho = 1.0e-9;
        for (std::size_t c = 0; c < g.CellCount(); ++c) g.IceCharge()[c] = rho;
        g.SolvePotential(1.0e-9);
        Check(g.Converged(), "Poisson solve converges");
        const double gap = (g.Nz() + 1) * g.Spacing();
        double worst = 0.0;
        for (std::uint32_t k = 1; k + 1 < g.Nz(); ++k)
        {
            const double z = g.Height(k) + 0.5 * g.Spacing();   // measured from the lower plate
            const double analytic = rho / (2.0 * kVacuumPermittivity) * z * (gap - z);
            worst = std::max(worst, std::abs(g.Potential()[g.Index(32, 32, k)] - analytic) / analytic);
        }
        std::printf("slab potential: worst relative error %.3f\n", worst);
        Check(worst < 0.06, "charged slab matches the analytic parabola (6%, grounded side walls 3.2 km away)");
        double e[3];
        g.Field(32, 32, 0, e);
        Check(e[2] < 0.0, "under positive space charge the field points down toward the grounded plane");
        Check(g.TotalCharge() > 0.0 && Near(g.TotalCharge(), rho * g.CellCount() * 1.0e6, 1.0e-12), "total charge counts every cell");
    }
    {
        // Linearity and symmetry: doubling the charge doubles phi; a mirrored charge gives the mirrored potential.
        ChargeGrid g(33, 33, 33, 50.0);
        g.IceCharge()[g.Index(10, 16, 16)] = 1.0e-8;
        g.SolvePotential(1.0e-10);
        const double a = g.Potential()[g.Index(16, 16, 16)];
        const double mirrored = [&] { ChargeGrid m(33, 33, 33, 50.0); m.IceCharge()[m.Index(22, 16, 16)] = 1.0e-8; m.SolvePotential(1.0e-10); return m.Potential()[m.Index(16, 16, 16)]; }();
        Check(Near(a, mirrored, 1.0e-6), "mirrored charge gives the mirrored potential");
        g.IceCharge()[g.Index(10, 16, 16)] = 2.0e-8;
        g.SolvePotential(1.0e-10);
        Check(Near(g.Potential()[g.Index(16, 16, 16)], 2.0 * a, 1.0e-5), "potential is linear in charge");
    }

    // --- charging is neutral and transport conserves charge.
    {
        ChargeGrid g(48, 48, 32, 500.0);
        const StormFields storm = MakeStorm(g, 25.0);
        const ElectrificationParams params;
        double top = 0.0, ground = 0.0, absolute = 0.0;
        for (int step = 0; step < 60; ++step)
        {
            const ChargeBudget b = StepElectrification(g, storm, params, 5.0);
            top += b.leftThroughTop;
            ground += b.groundDeposit;
        }
        for (std::size_t c = 0; c < g.CellCount(); ++c) absolute += std::abs(g.SpaceCharge(c));
        absolute *= std::pow(g.Spacing(), 3);
        Check(absolute > 5.0, "charging separates a few coulombs of charge in five minutes");
        // total + outflow = created = 0
        Check(std::abs(g.TotalCharge() + top + ground) < 1.0e-9 * absolute, "charging + transport conserve charge (internal + outflows = 0)");
    }
    {
        // Pure transport: no charging fields, a charged blob carried by the wind keeps its charge.
        ChargeGrid g(8, 8, 40, 100.0);
        StormFields still;
        const std::size_t n = g.CellCount();
        still.temperatureK.assign(n, 250.0F); still.airDensity.assign(n, 1.0F); still.updraft.assign(n, 5.0F);
        still.graupel.assign(n, 0.0F); still.ice.assign(n, 0.0F); still.liquid.assign(n, 0.0F);
        for (std::uint32_t k = 5; k < 10; ++k) for (std::uint32_t j = 0; j < 8; ++j) for (std::uint32_t i = 0; i < 8; ++i)
            g.IceCharge()[g.Index(i, j, k)] = 1.0e-9;
        const double before = g.TotalCharge();
        ElectrificationParams params;
        double top = 0.0, ground = 0.0;
        for (int step = 0; step < 200; ++step)
        {
            const ChargeBudget b = StepElectrification(g, still, params, 5.0);
            top += b.leftThroughTop; ground += b.groundDeposit;
        }
        Check(std::abs(g.TotalCharge() + top + ground - before) < 1.0e-9 * std::abs(before), "advection conserves charge including outflow");
        Check(top > 0.5 * before, "an updraft carries the charge out of the top of the block");
    }
    {
        // Falling graupel with no updraft delivers its charge to the ground.
        ChargeGrid g(4, 4, 20, 100.0);
        StormFields calm;
        const std::size_t n = g.CellCount();
        calm.temperatureK.assign(n, 250.0F); calm.airDensity.assign(n, 1.0F); calm.updraft.assign(n, 0.0F);
        calm.graupel.assign(n, 0.0F); calm.ice.assign(n, 0.0F); calm.liquid.assign(n, 0.0F);
        for (std::uint32_t k = 3; k < 6; ++k) for (std::uint32_t j = 0; j < 4; ++j) for (std::uint32_t i = 0; i < 4; ++i)
            g.GraupelCharge()[g.Index(i, j, k)] = -2.0e-9;
        const double before = g.TotalCharge();
        ElectrificationParams params;
        double ground = 0.0;
        for (int step = 0; step < 100; ++step) ground += StepElectrification(g, calm, params, 5.0).groundDeposit;
        // First-order upwind leaves a small decaying numerical tail aloft; what is delivered plus what remains is exact.
        Check(ground < 0.0 && Near(ground, before, 2.0e-3), "rain-out delivers the graupel charge to the ground");
        Check(std::abs(g.TotalCharge()) < 2.0e-3 * std::abs(before), "almost nothing is left aloft");
        Check(std::abs(g.TotalCharge() + ground - before) < 1.0e-9 * std::abs(before), "delivered plus remaining equals the initial charge exactly");
    }

    // --- charge layering of the default synthetic storm: a normal dipole (positive above negative), main negative in the
    // mixed-phase region between about -5 and -20 C, centroids a few kilometres apart, net charge zero.
    {
        ChargeGrid g(40, 40, 28, 600.0);
        const StormFields storm = MakeStorm(g, 25.0);
        const ElectrificationParams params;
        for (int step = 0; step < 30; ++step) StepElectrification(g, storm, params, 5.0);
        double positive = 0.0, negative = 0.0, zPositive = 0.0, zNegative = 0.0;
        double negativeTemperature = 0.0;
        for (std::uint32_t k = 0; k < g.Nz(); ++k)
        {
            double layer = 0.0;
            for (std::uint32_t j = 0; j < g.Ny(); ++j)
                for (std::uint32_t i = 0; i < g.Nx(); ++i) layer += g.SpaceCharge(g.Index(i, j, k));
            layer *= std::pow(g.Spacing(), 3);
            if (layer > 0.0) { positive += layer; zPositive += layer * g.Height(k); }
            else { negative += layer; zNegative += layer * g.Height(k); negativeTemperature += layer * (300.0 - 6.5e-3 * g.Height(k)); }
        }
        const double separation = zPositive / positive - zNegative / negative;
        const double negativeT = negativeTemperature / negative;
        std::printf("layering: %.1f C each, centroid separation %.0f m, negative centroid at %.0f K\n", positive, separation, negativeT);
        Check(positive > 5.0 && Near(positive, -negative, 1.0e-6), "the storm separates charge into equal and opposite halves");
        Check(separation > 1500.0 && separation < 4500.0, "the charge centres are 1.5 to 4.5 km apart");
        Check(negativeT > 255.0 && negativeT < 270.0, "the main negative charge sits in the mixed-phase region (-18 to -3 C)");
    }

    // --- one flash: threshold, conservation, energy, determinism.
    {
        ChargeGrid g(48, 48, 32, 500.0);
        const StormFields storm = MakeStorm(g, 25.0);
        const ElectrificationParams electrification;
        LeaderParams leader;
        // Before the storm has charged no flash can start.
        Check(!TryFlash(g, leader, 0.0, 0).occurred, "no flash in an uncharged cloud");
        // Charge until the field first reaches the initiation threshold (at most 20 minutes of storm time).
        const auto chargeUntilBreakdown = [&](ChargeGrid& target) {
            double seconds = 0.0;
            for (; seconds < 1200.0; seconds += 5.0)
            {
                StepElectrification(target, storm, electrification, 5.0);
                target.SolvePotential();
                if (MaxBreakdownRatio(target, leader) >= 1.0) break;
            }
            return seconds;
        };
        const double chargingTime = chargeUntilBreakdown(g);
        std::printf("first breakdown after %.0f s of charging\n", chargingTime);
        const double ratio = MaxBreakdownRatio(g, leader);
        Check(ratio >= 1.0 && chargingTime > 60.0 && chargingTime < 1200.0, "a storm reaches the initiation threshold after minutes of charging, not instantly");
        ChargeGrid copy = g;
        const double totalBefore = g.TotalCharge();
        const FlashResult flash = TryFlash(g, leader, chargingTime, 7);
        Check(flash.occurred && flash.channelCells >= 3, "a flash starts and grows a channel");
        Check(flash.kind == FlashKind::IntraCloud, "a flash confined aloft is intracloud");
        Check(std::abs(g.TotalCharge() - totalBefore) < 1.0e-9 * flash.positiveRemoved, "an ungrounded flash conserves charge exactly");
        Check(Near(flash.positiveRemoved, flash.negativeRemoved, 1.0e-9), "equal and opposite charge is neutralised");
        Check(flash.positiveRemoved > 1.0 && flash.positiveRemoved <= leader.maxFlashCharge * 1.5, "flash charge is of the order of the charge budget");
        Check(flash.energyAfter < flash.energyBefore, "a flash dissipates field energy");
        Check(flash.graph.Segments().size() + 1 == flash.channelCells, "one segment per added cell");
        // Determinism: same grid, seed and index give the same channel; another seed differs.
        const FlashResult again = TryFlash(copy, leader, chargingTime, 7);
        Check(again.channelCells == flash.channelCells && again.graph.Segments().size() == flash.graph.Segments().size() &&
                  again.graph.EnergyInWindow(0.0, 1000.0) == flash.graph.EnergyInWindow(0.0, 1000.0),
              "same seed and flash index reproduce the same flash");
        ChargeGrid other(48, 48, 32, 500.0);
        chargeUntilBreakdown(other);
        LeaderParams reseeded = leader;
        reseeded.seed = 99;
        const FlashResult different = TryFlash(other, reseeded, chargingTime, 7);
        const auto fingerprint = [](const FlashResult& r) {
            double sum = 0.0;
            for (const auto& s : r.graph.Segments()) sum += s.to[0] * 1.0 + s.to[1] * 3.0 + s.to[2] * 7.0 + s.formedAt;
            return sum;
        };
        Check(different.occurred && (different.channelCells != flash.channelCells || fingerprint(different) != fingerprint(flash)),
              "a different seed grows a different channel");
        // A tree with a trunk: with the growth made to concentrate on strong fields, rare side branches and energy by downstream
        // charge, the channel is far fewer cells, the root-side segments radiate far more than the twigs, and the energy of the flash is
        // the same as an equal sharing would give.
        {
            LeaderParams tree = leader;
            tree.growthExponent = 3.0;
            tree.sideBranchChance = 0.03;
            tree.picksDivisor = 24.0;
            tree.currentWeighted = true;
            ChargeGrid treeGrid(48, 48, 32, 500.0);
            chargeUntilBreakdown(treeGrid);
            const FlashResult grown = TryFlash(treeGrid, tree, chargingTime, 7);
            Check(grown.occurred && grown.channelCells >= 3, "a tree-grown flash occurs");
            double strongest = 0.0, weakest = 1.0e300, sum = 0.0;
            for (const auto& s : grown.graph.Segments())
            {
                double e = 0.0;
                for (const auto& pulse : s.pulses) e = std::max(e, pulse.radiantEnergy);
                strongest = std::max(strongest, e);
                weakest = std::min(weakest, e);
                sum += e;
            }
            Check(grown.graph.Segments().size() > 8 && strongest > 4.0 * weakest, "the trunk radiates several times more than the twigs");
            // With equal sharing every segment has the same bright pulse, so the strongest is not many times the mean.
            double equalStrongest = 0.0, equalMean = 0.0;
            for (const auto& s : flash.graph.Segments())
            {
                double e = 0.0;
                for (const auto& pulse : s.pulses) e = std::max(e, pulse.radiantEnergy);
                equalStrongest = std::max(equalStrongest, e);
                equalMean += e;
            }
            equalMean /= static_cast<double>(flash.graph.Segments().size());
            Check(equalStrongest < 1.5 * equalMean, "the default sharing is equal");
            (void)sum;
        }
        // Timing and energy: the channel grows at the leader speed and the optical energy is finite and bounded.
        const double energy = flash.graph.EnergyInWindow(0.0, 1.0e6);
        Check(energy > 0.0 && energy < 1.0e9, "optical energy of an intracloud flash is finite and bounded");
        Check(flash.endTime > flash.startTime, "a channel takes time to grow");
        const auto clusters = flash.graph.ClusterEmission(0.0, 1.0e6, 16);
        double clusterEnergy = 0.0;
        for (const auto& c : clusters) clusterEnergy += c.energy;
        Check(clusters.size() <= 16 && Near(clusterEnergy, energy, 1.0e-9), "emission clusters preserve the flash energy within a bounded count");
    }

    // --- a ground flash: low positive pocket under a negative layer.
    {
        ChargeGrid g(41, 41, 24, 250.0);
        for (std::uint32_t k = 2; k <= 4; ++k) for (std::uint32_t j = 18; j <= 22; ++j) for (std::uint32_t i = 18; i <= 22; ++i)
            g.IceCharge()[g.Index(i, j, k)] = 4.0e-9;
        for (std::uint32_t k = 7; k <= 10; ++k) for (std::uint32_t j = 16; j <= 24; ++j) for (std::uint32_t i = 16; i <= 24; ++i)
            g.GraupelCharge()[g.Index(i, j, k)] = -3.0e-9;
        g.SolvePotential();
        LeaderParams leader;
        const double totalBefore = g.TotalCharge();
        const FlashResult flash = TryFlash(g, leader, 10.0, 1);
        Check(flash.occurred && flash.kind == FlashKind::CloudToGround, "a channel that reaches the ground is cloud-to-ground");
        Check(flash.groundCharge < -1.0, "a negative cloud-to-ground flash lowers negative charge to the ground");
        Check(std::abs((g.TotalCharge() - totalBefore) + flash.groundCharge) < 1.0e-9 * std::abs(totalBefore) + 1.0e-12 ||
                  Near(g.TotalCharge() - totalBefore, flash.groundCharge * -1.0, 1.0e-6),
              "charge accounting: what the cloud lost is what the ground received");
        // The return stroke is a fast wave: much brighter and much shorter than the leader stage that preceded it.
        const double leaderEnergy = flash.graph.EnergyInWindow(10.0, flash.endTime - 1.0e-9);
        const double strokeEnergy = flash.graph.EnergyInWindow(flash.endTime - 1.0e-9, flash.endTime + 1.0);
        Check(Near(strokeEnergy, leader.returnStrokeEnergy, 0.02), "the return stroke radiates its configured optical energy");
        Check(strokeEnergy > leaderEnergy, "return stroke outshines the leader stage");
        double lastPulse = 0.0;
        for (const auto& s : flash.graph.Segments()) for (const auto& p : s.pulses) lastPulse = std::max(lastPulse, p.startTime + p.duration);
        Check(lastPulse - flash.endTime < 0.01, "the return stroke finishes within milliseconds");
        // A microsecond-scale pulse still contributes fully to a frame-length exposure.
        Check(Near(flash.graph.EnergyInWindow(flash.endTime - 0.008, flash.endTime + 0.008), strokeEnergy + flash.graph.EnergyInWindow(flash.endTime - 0.008, flash.endTime - 1.0e-9), 1.0e-9),
              "a 16 ms exposure integrates the whole stroke");
    }

    // --- flash rate follows the updraft (stronger updraft, more flashes), without charge errors.
    {
        const StormRun weak = RunStorm(8.0, 900.0, 1), medium = RunStorm(18.0, 900.0, 1), strong = RunStorm(30.0, 900.0, 1);
        std::printf("flashes in 15 min: w=8 -> %d, w=18 -> %d, w=30 -> %d\n", weak.flashes, medium.flashes, strong.flashes);
        Check(weak.flashes < medium.flashes && medium.flashes < strong.flashes, "stronger updrafts produce more flashes");
        Check(strong.worstChargeImbalance < 1.0e-6, "every ungrounded flash in a running storm conserved charge");
        Check(strong.energyAlwaysDropped, "every flash in a running storm lowered the field energy");
        const StormRun repeat = RunStorm(30.0, 900.0, 1);
        Check(repeat.flashes == strong.flashes && repeat.flashCharge == strong.flashCharge, "a whole storm run is reproducible");
    }

    // --- deterministic flash schedule for timewarp.
    {
        const FlashSchedule schedule(42, {{0.0, 0.0}, {600.0, 1.0}, {1200.0, 0.2}, {3600.0, 0.0}});
        const double expected = schedule.ExpectedCount(3600.0);
        Check(Near(expected, 0.5 * 600.0 * 1.0 + 0.5 * 600.0 * 1.2 + 0.5 * 2400.0 * 0.2, 1.0e-9), "expected count is the integral of the rate");
        const auto whole = schedule.FlashesIn(0.0, 3600.0);
        std::vector<double> pieces;
        for (double t = 0.0; t < 3600.0; t += 137.0)
        {
            const auto part = schedule.FlashesIn(t, std::min(t + 137.0, 3600.0));
            pieces.insert(pieces.end(), part.begin(), part.end());
        }
        Check(whole.size() == pieces.size() && std::equal(whole.begin(), whole.end(), pieces.begin()), "splitting a window never changes the flashes");
        Check(std::is_sorted(whole.begin(), whole.end()), "flash times ascend");
        // Counts over many independent seeds average to the expected count.
        double mean = 0.0;
        constexpr int seeds = 400;
        for (int s = 0; s < seeds; ++s) mean += static_cast<double>(FlashSchedule(1000 + s, {{0.0, 0.0}, {600.0, 1.0}, {1200.0, 0.2}, {3600.0, 0.0}}).FlashesIn(0.0, 3600.0).size());
        mean /= seeds;
        std::printf("schedule: expected %.1f, mean over %d seeds %.1f\n", expected, seeds, mean);
        Check(std::abs(mean - expected) < 0.05 * expected, "mean flash count over many seeds matches the integrated rate");
        Check(schedule.FlashesIn(0.0, 3600.0) == whole, "same seed gives the same flashes");
        Check(FlashSchedule(43, {{0.0, 0.0}, {600.0, 1.0}, {1200.0, 0.2}, {3600.0, 0.0}}).FlashesIn(0.0, 3600.0) != whole, "a different seed gives different flashes");
        Check(FlashSchedule(1, {{0.0, 0.0}}).FlashesIn(0.0, 1.0e9).empty(), "a zero rate never flashes");
        // Timewarp: a window a year away is answered directly.
        const double year = 365.25 * 86400.0;
        const FlashSchedule storm(7, {{year, 0.5}, {year + 3600.0, 0.5}});
        const auto far = storm.FlashesIn(year + 1000.0, year + 1100.0);
        Check(far.size() < 200 && std::all_of(far.begin(), far.end(), [&](double t) { return t >= year + 1000.0 && t < year + 1100.0; }),
              "a window a year ahead is answered without simulating the interval");
    }

    if (failures == 0) std::printf("StormElectrification: all tests passed\n");
    return failures;
}
