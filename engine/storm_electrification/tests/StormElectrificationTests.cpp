#include <orbit/storm_electrification/ChargeGrid.hpp>
#include <orbit/storm_electrification/Electrification.hpp>
#include <orbit/storm_electrification/FlashSchedule.hpp>
#include <orbit/optical_transport/Blackbody.hpp>
#include <orbit/storm_electrification/LeaderSimulation.hpp>
#include <orbit/storm_electrification/LightningEmission.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

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
        // Diagonal growth: the channel may step to the 26 neighbours, so some of its segments are not along an axis, and it still grows as a
        // tree with tips (it did not stop after a few cells when "tip" meant a cell with few neighbours).
        {
            const auto axisAligned = [](const FlashResult& r) {
                std::size_t aligned = 0;
                for (const auto& seg : r.graph.Segments())
                {
                    int moving = 0;
                    for (int a = 0; a < 3; ++a) { moving += std::fabs(seg.to[a] - seg.from[a]) > 1.0; }
                    aligned += moving == 1;
                }
                return aligned;
            };
            LeaderParams grown = leader;
            grown.growthExponent = 3.0;
            grown.sideBranchChance = 0.03;
            grown.picksDivisor = 24.0;
            grown.currentWeighted = true;
            grown.maxChannelCells = 400;
            grown.maxFlashCharge = 1.0e9;   // not the limit here: the channel cap is
            ChargeGrid faces(48, 48, 32, 500.0), diagonals(48, 48, 32, 500.0);
            chargeUntilBreakdown(faces);
            chargeUntilBreakdown(diagonals);
            const FlashResult onFaces = TryFlash(faces, grown, chargingTime, 7);
            grown.diagonalGrowth = true;
            const FlashResult onDiagonals = TryFlash(diagonals, grown, chargingTime, 7);
            Check(onFaces.occurred && onFaces.graph.Segments().size() > 50 && axisAligned(onFaces) == onFaces.graph.Segments().size(),
                  "growing to the 6 face neighbours makes only axis-aligned segments");
            Check(onDiagonals.occurred && onDiagonals.graph.Segments().size() > 50 &&
                      axisAligned(onDiagonals) < onDiagonals.graph.Segments().size() * 7 / 10,
                  "growing to the 26 neighbours makes a good share of diagonal segments and a full-size tree");
        }
        // A channel that crosses the cloud instead of packing it: screening (a conducting channel weakens the field at the cells that hug it) and
        // direction persistence make the same number of cells reach much farther than the plain growth does.
        {
            const auto extent = [&](const double screening, const double persistence) {
                LeaderParams t = leader;
                t.growthExponent = 4.0;
                t.sideBranchChance = 0.004;
                t.picksDivisor = 1.0e9;
                t.diagonalGrowth = true;
                t.maxFlashCharge = 1.0e9;
                t.maxChannelCells = 400;
                t.screening = screening;
                t.directionPersistence = persistence;
                ChargeGrid tg(48, 48, 32, 500.0);
                chargeUntilBreakdown(tg);
                const FlashResult r = TryFlash(tg, t, chargingTime, 7);
                double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
                for (const auto& sg : r.graph.Segments())
                    for (int ax = 0; ax < 3; ++ax) { lo[ax] = std::min(lo[ax], sg.to[ax]); hi[ax] = std::max(hi[ax], sg.to[ax]); }
                return std::make_pair(r.channelCells, std::max(hi[0] - lo[0], hi[1] - lo[1]));
            };
            const auto packed = extent(1.0, 0.0);
            const auto crossing = extent(0.2, 6.0);
            Check(packed.first >= 300 && crossing.first >= 300, "both grow to the cell cap");
            Check(crossing.second > 1.4 * packed.second, "screening and persistence make a channel reach much farther with the same cells");
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

    // --- the physical emission of a flash: leader glow, return stroke, dark twigs, later strokes, cooling, colour.
    {
        using orbit::optical_transport::DischargeGraph;
        using orbit::optical_transport::Segment;
        // A channel: the main path 0..8 (grounded at 8), a heavy branch 9..13 from segment 3, a twig 14 from segment 2. Segments are 500 m.
        const auto build = [] {
            DischargeGraph g(FlashKind::CloudToGround);
            const auto add = [&](int parent, double x0, double z0, double x1, double z1, double formed) {
                Segment s;
                s.from[0] = x0; s.from[1] = 0.0; s.from[2] = z0;
                s.to[0] = x1; s.to[1] = 0.0; s.to[2] = z1;
                s.parent = parent;
                s.formedAt = formed;
                return g.AddSegment(s);
            };
            for (int i = 0; i < 9; ++i) add(i - 1, 0.0, 9000.0 - 500.0 * i, 0.0, 9000.0 - 500.0 * (i + 1), 1.0e-4 * i);
            int parent = 3;
            for (int i = 0; i < 5; ++i) parent = add(parent, 500.0 * i, 7500.0, 500.0 * (i + 1), 7500.0, 1.0e-3 + 1.0e-4 * i);
            add(2, 0.0, 8000.0, -500.0, 8000.0, 2.0e-3);
            return g;
        };
        const double attach = 3.0e-3;
        EmissionParams scaled;   // this channel has 15 segments, so a twig is 7 % of it: the share below which a branch stays dark is set above that
        scaled.branchMinimumShare = 0.1;
        double peakKelvinMain = 0.0, peakWattsPerMeterMain = 0.0, leaderWattsPerMeterMax = 0.0, twigAfterAttachKelvin = 0.0;
        double coolTail = 0.0, coolTailKelvin = 1.0e9;
        bool anyLaterStroke = false;
        for (std::uint64_t seed = 1; seed <= 40; ++seed)
        {
            DischargeGraph g = build();
            AssignPhysicalEmission(g, true, 8, attach, seed, scaled);
            for (std::size_t i = 0; i < g.Segments().size(); ++i)
            {
                const auto& seg = g.Segments()[i];
                const double length = 500.0;
                for (const auto& p : seg.pulses)
                {
                    const double wattsPerMeter = p.radiantEnergy / p.duration / length;
                    if (p.startTime < seg.formedAt + 3.0e-4) leaderWattsPerMeterMax = std::max(leaderWattsPerMeterMax, wattsPerMeter);
                    if (i <= 8)
                    {
                        peakKelvinMain = std::max(peakKelvinMain, p.temperatureKelvin);
                        peakWattsPerMeterMain = std::max(peakWattsPerMeterMain, wattsPerMeter);
                        if (p.startTime > attach + 1.0e-2 && p.temperatureKelvin > 15000.0) anyLaterStroke = true;
                        if (seed == 1 && i == 8 && p.startTime > attach && p.startTime < attach + 2.0e-2)
                        {
                            coolTail = std::max(coolTail, p.startTime + p.duration - attach);
                            if (p.startTime > attach + 1.0e-3) coolTailKelvin = std::min(coolTailKelvin, p.temperatureKelvin);
                        }
                    }
                    if (i == 14 && p.startTime > attach) twigAfterAttachKelvin = std::max(twigAfterAttachKelvin, p.temperatureKelvin);
                }
            }
        }
        Check(peakKelvinMain > 20000.0 && peakKelvinMain <= 36001.0, "the return stroke heats the main path to 20 to 36 kK");
        Check(peakWattsPerMeterMain > 1.0e5 && peakWattsPerMeterMain < 2.0e7, "a return stroke radiates about 1e6 W per metre of channel");
        Check(leaderWattsPerMeterMax < 2.5e4 && leaderWattsPerMeterMax > 1.0e4, "the stepped leader is some 50 times fainter (2e4 W/m)");
        Check(twigAfterAttachKelvin < 9100.0, "a twig that carries no charge is not heated by the return stroke (only its own leader glow cools after it)");
        Check(anyLaterStroke, "a flash to the ground has later strokes, tens of milliseconds after the first");
        Check(coolTail > 1.0e-3 && coolTailKelvin < 6000.0, "the channel cools to a few thousand kelvin over milliseconds after the stroke");
        // Colour: hot plasma is blue-white, cooled plasma orange.
        const auto hot = orbit::optical_transport::BlackbodyLinearRgb(30000.0);
        const auto warm = orbit::optical_transport::BlackbodyLinearRgb(6500.0);
        const auto cool = orbit::optical_transport::BlackbodyLinearRgb(3500.0);
        Check(hot[2] >= hot[0] && hot[2] > 0.99, "30 kK is blue-white");
        Check(std::abs(warm[0] - warm[2]) < 0.15 && warm[1] > 0.9, "6500 K is white");
        Check(cool[0] > 0.99 && cool[2] < 0.6 * cool[0], "3500 K is orange");
        // The first stroke also lights the heavy branch, dimmer than the main channel.
        DischargeGraph g = build();
        AssignPhysicalEmission(g, true, 8, attach, 7, scaled);
        double mainPeak = 0.0, branchPeak = 0.0;
        for (std::size_t i = 0; i < g.Segments().size(); ++i)
            for (const auto& p : g.Segments()[i].pulses)
            {
                if (p.startTime < attach) continue;
                const double w = p.radiantEnergy / p.duration;
                if (i <= 8) mainPeak = std::max(mainPeak, w);
                if (i >= 9 && i <= 13) branchPeak = std::max(branchPeak, w);
            }
        Check(branchPeak > 0.0 && branchPeak < mainPeak, "the heavy branch flashes too, dimmer than the main channel");
    }

    // Descending leaders: a flash that seeks the ground reaches it (a return stroke), the same grid without the bias mostly does not.
    {
        const auto groundedCount = [&](const double chance) {
            int grounded = 0;
            for (std::uint64_t seed = 1; seed <= 3; ++seed)
            {
                ChargeGrid g(48, 48, 32, 500.0);
                const StormFields storm = MakeStorm(g, 25.0);
                const ElectrificationParams electrification;
                LeaderParams t;
                for (double seconds = 0.0; seconds < 1200.0; seconds += 5.0)
                {
                    StepElectrification(g, storm, electrification, 5.0);
                    g.SolvePotential();
                    if (MaxBreakdownRatio(g, t) >= 1.0) break;
                }
                t.growthExponent = 4.0;
                t.sideBranchChance = 0.004;
                t.picksDivisor = 1.0e9;
                t.diagonalGrowth = true;
                t.screening = 0.15;
                t.directionPersistence = 6.0;
                t.maxFlashCharge = 1.0e9;
                t.maxChannelCells = 400;
                t.groundSeekChance = chance;
                t.seed = seed;
                const FlashResult r = TryFlash(g, t, 100.0, seed);
                grounded += r.occurred && r.kind == FlashKind::CloudToGround ? 1 : 0;
            }
            return grounded;
        };
        const int seeking = groundedCount(1.0);
        const int plain = groundedCount(0.0);
        std::printf("ground strikes of 3 flashes: seeking %d, plain %d\n", seeking, plain);
        Check(seeking >= 2 && seeking > plain, "flashes that seek the ground reach it far more often than the plain growth");
    }

    // --- coherent channels, many small flashes, and waves that follow the temperature of the channel.
    {
        // Wave speed follows the temperature: a hot channel carries a wave at return stroke speed, a cold one at dart leader speed.
        const EmissionParams speeds;
        Check(WaveSpeedForTemperature(15000.0, speeds) == speeds.returnStrokeSpeed && WaveSpeedForTemperature(3000.0, speeds) == speeds.dartLeaderSpeed,
              "a hot channel carries a wave at the return stroke speed, a cold one at the dart leader speed");
        Check(WaveSpeedForTemperature(6000.0, speeds) > WaveSpeedForTemperature(5000.0, speeds), "the wave speed grows with the temperature");
        // No pulse is written for a plasma too cold to glow.
        double coldest = 1.0e9;
        orbit::optical_transport::DischargeGraph chain(FlashKind::CloudToGround);
        for (int i = 0; i < 12; ++i)
        {
            orbit::optical_transport::Segment s;
            s.from[2] = 6000.0 - 500.0 * i;
            s.to[2] = 6000.0 - 500.0 * (i + 1);
            s.parent = i - 1;
            s.formedAt = 1.0e-4 * i;
            chain.AddSegment(s);
        }
        for (std::uint64_t seed = 1; seed <= 20; ++seed)
        {
            orbit::optical_transport::DischargeGraph g = chain;
            AssignPhysicalEmission(g, true, 11, 2.0e-3, seed);
            for (const auto& seg : g.Segments())
                for (const auto& pulse : seg.pulses) coldest = std::min(coldest, pulse.temperatureKelvin);
        }
        Check(coldest >= 2800.0, "the emission stops where the plasma is too cold to glow");
    }
    {
        // Small flashes: short, never to the ground, and they start at different cells although the potential is not solved between them.
        ChargeGrid g(48, 48, 32, 500.0);
        const StormFields storm = MakeStorm(g, 25.0);
        const ElectrificationParams electrification;
        LeaderParams t;
        for (double seconds = 0.0; seconds < 1200.0; seconds += 5.0)
        {
            StepElectrification(g, storm, electrification, 5.0);
            g.SolvePotential();
            if (MaxBreakdownRatio(g, t) >= 1.0) break;
        }
        t.smallFlashFraction = 1.0;
        t.initiationSpread = 0.4;
        t.diagonalGrowth = true;
        t.groundSeekChance = 1.0;   // a small flash does not seek the ground whatever this says
        t.maxFlashCharge = 1.0e9;
        std::size_t largest = 0;
        std::vector<std::array<double, 3>> starts;
        int grounded = 0, occurred = 0;
        for (std::uint64_t i = 0; i < 6; ++i)
        {
            const FlashResult r = TryFlash(g, t, 100.0, i);
            if (!r.occurred) continue;
            ++occurred;
            largest = std::max(largest, r.graph.Segments().size());
            grounded += r.kind == FlashKind::CloudToGround ? 1 : 0;
            const auto& first = r.graph.Segments().front();
            starts.push_back({first.from[0], first.from[1], first.from[2]});
        }
        std::size_t distinct = 0;
        for (std::size_t a = 0; a < starts.size(); ++a)
        {
            bool unique = true;
            for (std::size_t b = 0; b < a; ++b)
            {
                const double dx = starts[a][0] - starts[b][0], dy = starts[a][1] - starts[b][1], dz = starts[a][2] - starts[b][2];
                if (std::sqrt(dx * dx + dy * dy + dz * dz) < 1000.0) unique = false;
            }
            distinct += unique ? 1U : 0U;
        }
        std::printf("small flashes: %d occurred, largest %zu cells, %zu distinct starts\n", occurred, largest, distinct);
        // (This test grid has one hot spot, so after the first small flash the cells around it are set aside and few flashes follow; a real storm has
        // many.) What is checked: they are short, they do not reach the ground, and no two start in the same place.
        Check(occurred >= 1 && largest <= 91U && grounded == 0, "small flashes are short and do not reach the ground");
        Check(distinct == static_cast<std::size_t>(occurred), "successive small flashes start at different places");
    }
    {
        // A goal makes the channel run to where it is going: the same cells reach farther from the start.
        const auto reach = [&](const double goal) {
            double total = 0.0;
            for (std::uint64_t seed = 1; seed <= 3; ++seed)
            {
                ChargeGrid g(48, 48, 32, 500.0);
                const StormFields storm = MakeStorm(g, 25.0);
                const ElectrificationParams electrification;
                LeaderParams t;
                for (double seconds = 0.0; seconds < 1200.0; seconds += 5.0)
                {
                    StepElectrification(g, storm, electrification, 5.0);
                    g.SolvePotential();
                    if (MaxBreakdownRatio(g, t) >= 1.0) break;
                }
                t.growthExponent = 4.0;
                t.sideBranchChance = 0.004;
                t.picksDivisor = 1.0e9;
                t.diagonalGrowth = true;
                t.screening = 0.15;
                t.directionPersistence = 6.0;
                t.maxFlashCharge = 1.0e9;
                t.maxChannelCells = 200;
                t.goalBias = goal;
                t.seed = seed;
                const FlashResult r = TryFlash(g, t, 100.0, seed);
                if (!r.occurred) continue;
                const auto& root = r.graph.Segments().front();
                double far = 0.0;
                for (const auto& sg : r.graph.Segments())
                {
                    const double dx = sg.to[0] - root.from[0], dy = sg.to[1] - root.from[1], dz = sg.to[2] - root.from[2];
                    far = std::max(far, std::sqrt(dx * dx + dy * dy + dz * dz));
                }
                total += far;
            }
            return total;
        };
        const double without = reach(0.0), with = reach(8.0);
        std::printf("reach of 200 cells from the start: without a goal %.1f km, with %.1f km (3 flashes)\n", without / 1000.0, with / 1000.0);
        Check(with > 1.15 * without, "a channel with a goal reaches farther from its start than one without");
    }

    if (failures == 0) std::printf("StormElectrification: all tests passed\n");
    return failures;
}
