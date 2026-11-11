// The Weather Lab fast-core storm has no ice phase. This test checks the temperature-based ice proxy: an adapter unit
// check, and an end-to-end run showing the real solver's storm electrifies only once it has matured.
#include <orbit/storm_electrification/LeaderSimulation.hpp>
#include <orbit/storm_electrification/WarmRainAdapter.hpp>
#include <orbit/weather_lab/FastStormSolver.hpp>

#include <algorithm>
#include <cstdio>
#include <stdexcept>

using namespace orbit;
using namespace orbit::storm_electrification;

namespace
{
int failures = 0;
void Check(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

struct Outcome { double maxRatio; int flashes; double maxUpdraft; };

Outcome Electrify(const weather_lab::FastStormSolver& solver, const weather_lab::FastStormConfig& c, double seconds)
{
    const auto& base = solver.Base();
    WarmRainInputs in;
    solver.CopyField("th", in.potentialTemperatureK);
    solver.CopyField("qc", in.cloudWater);
    solver.CopyField("qr", in.rainWater);
    solver.CopyField("w", in.updraft);
    const std::size_t n = in.potentialTemperatureK.size(), layer = static_cast<std::size_t>(c.nx) * c.ny;
    in.exner.resize(n);
    in.airDensity.resize(n);
    for (std::size_t i = 0; i < n; ++i) { in.exner[i] = base.exner[i / layer]; in.airDensity[i] = base.density[i / layer]; }
    const StormFields fields = StormFieldsFromWarmRain(in);
    ChargeGrid grid(c.nx, c.ny, c.nz, c.dx);
    const ElectrificationParams electrification;
    const LeaderParams leader;
    Outcome out{0.0, 0, *std::max_element(in.updraft.begin(), in.updraft.end())};
    for (double t = 0.0; t < seconds; t += 5.0)
    {
        StepElectrification(grid, fields, electrification, 5.0);
        grid.SolvePotential();
        const double ratio = MaxBreakdownRatio(grid, leader);
        out.maxRatio = std::max(out.maxRatio, ratio);
        if (ratio >= 1.0 && TryFlash(grid, leader, t, static_cast<std::uint64_t>(out.flashes)).occurred) { ++out.flashes; }
    }
    return out;
}
} // namespace

int main()
{
    // Unit: phases follow temperature.
    {
        WarmRainInputs in;
        in.potentialTemperatureK = {300.0F, 300.0F, 300.0F, 300.0F};
        in.exner = {1.0F, 0.9F, 0.89F, 0.8F};   // T = 300, 270, 267, 240
        in.airDensity = {1.0F, 1.0F, 1.0F, 1.0F};
        in.updraft = {5.0F, 5.0F, 5.0F, 5.0F};
        in.cloudWater = {1e-3F, 1e-3F, 1e-3F, 1e-3F};
        in.rainWater = {2e-3F, 2e-3F, 2e-3F, 2e-3F};
        const StormFields f = StormFieldsFromWarmRain(in);
        Check(f.graupel[0] == 0.0F && f.ice[0] == 0.0F && f.liquid[0] == 0.0F, "warm cell has no ice phase or supercooled water");
        Check(f.graupel[1] == 0.0F && f.liquid[1] > 0.0F, "270 K: supercooled cloud water, rain not yet frozen");
        Check(f.graupel[2] > 0.0F && f.liquid[2] > 0.0F, "267 K: graupel and supercooled water coexist");
        Check(f.ice[3] > 1e-3F && f.liquid[3] == 0.0F, "240 K: cloud water has glaciated");
        in.rainWater.pop_back();
        bool threw = false;
        try { (void)StormFieldsFromWarmRain(in); } catch (const std::invalid_argument&) { threw = true; }
        Check(threw, "mismatched array sizes are rejected");
    }

    weather_lab::FastStormConfig config;
    config.nx = 64; config.ny = 64; config.nz = 40;
    config.dx = 500.0F; config.dy = 500.0F; config.dz = 500.0F;
    config.bubble.horizontalRadius = 5000.0F;
    weather_lab::FastStormSolver solver(config);
    solver.Advance(600.0);
    const Outcome young = Electrify(solver, config, 600.0);
    solver.Advance(900.0);   // 25 minutes of storm time
    const Outcome mature = Electrify(solver, config, 600.0);
    std::printf("10 min storm: w %.1f m/s, ratio %.2f, %d flashes; 25 min storm: w %.1f m/s, ratio %.2f, %d flashes\n",
                young.maxUpdraft, young.maxRatio, young.flashes, mature.maxUpdraft, mature.maxRatio, mature.flashes);
    Check(young.flashes == 0 && young.maxRatio < 1.0, "a 10-minute-old storm does not flash");
    Check(mature.flashes > 0 && mature.maxRatio >= 1.0, "a mature storm reaches breakdown and flashes");
    std::printf(failures == 0 ? "WarmRainAdapter tests passed\n" : "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
