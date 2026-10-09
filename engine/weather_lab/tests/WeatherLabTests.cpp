#include <orbit/weather_lab/FastStormSolver.hpp>
#include <orbit/weather_lab/StormMetrics.hpp>
#include <orbit/weather_lab/Thermo.hpp>
#include <orbit/weather_lab/WxFormat.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <vector>

namespace
{
void Check(
    const bool condition,
    const std::source_location location = std::source_location::current())
{
    if (!condition)
    {
        std::cerr << "Weather lab test failed at " << location.file_name()
                  << ':' << location.line() << '\n';
        std::exit(1);
    }
}

using namespace orbit::weather_lab;

FastStormConfig SmallConfig()
{
    FastStormConfig c;
    c.nx = 32;
    c.ny = 32;
    c.nz = 40;
    c.threads = 2;
    return c;
}

void TestBaseState()
{
    const BaseState base = BuildSupercellBaseState({}, 40U, 500.0F);
    Check(base.density.size() == 40U && base.faceDensity.size() == 41U);
    Check(base.density.front() > 1.0F && base.density.front() < 1.3F);
    // Density and pressure fall monotonically; theta rises (stable aloft).
    for (std::size_t k = 1; k < 40U; ++k)
    {
        Check(base.density[k] < base.density[k - 1U]);
        Check(base.pressure[k] < base.pressure[k - 1U]);
        Check(base.theta[k] > base.theta[k - 1U]);
    }
    // The boundary layer is moist and capped at the WK mixing ratio.
    Check(std::fabs(base.vapor.front() - 0.014F) < 1.0e-4F);
    // Quarter-circle hodograph: westerly shear through 6 km, southerly
    // component saturating at 7 m/s.
    Check(base.windU.back() > 30.0F && base.windU.front() < 1.0F);
    Check(std::fabs(base.windV.back() - 7.0F) < 1.0e-3F);
}

void TestValidation()
{
    FastStormConfig c = SmallConfig();
    Check(FastStormSolver::Validate(c).empty());
    c.nx = 48;
    Check(!FastStormSolver::Validate(c).empty());
}

void TestRestAtmosphereStaysAtRest()
{
    FastStormConfig c = SmallConfig();
    c.bubble.amplitude = 0.0F;
    c.moisture = false;
    FastStormSolver solver(c);
    solver.Advance(600.0);
    // Without a perturbation the sheared, stably stratified base state must
    // not spontaneously generate vertical motion.
    Check(solver.Diagnostics().maxUpdraft < 0.05F);
    Check(solver.Diagnostics().maxDowndraft > -0.05F);
}

void TestProjectionAndWaterBudget()
{
    FastStormConfig c = SmallConfig();
    FastStormSolver solver(c);
    solver.Advance(1500.0);
    const FastStormDiagnostics& d = solver.Diagnostics();
    // The FFT/tridiagonal projection is a direct solve: divergence stays at
    // round-off level even in a vigorous updraft.
    Check(d.maxUpdraft > 5.0F);
    Check(d.maxDivergence < 1.0e-6F);
    // Semi-Lagrangian advection is made conservative by the mass fixer;
    // total water (vapour + condensate + fallen rain) must stay within 1e-4.
    Check(std::fabs(d.totalWater - d.initialTotalWater)
        < 1.0e-4 * d.initialTotalWater);
}

void TestStormInitiates()
{
    FastStormConfig c = SmallConfig();
    FastStormSolver solver(c);
    solver.Advance(2400.0);
    // A 2 K warm bubble in the Weisman-Klemp sounding must produce a deep
    // convective updraft with rain, and a downdraft.
    Check(solver.Diagnostics().maxUpdraft > 15.0F);
    Check(solver.Diagnostics().maxDowndraft < -3.0F);
    std::vector<float> qr;
    solver.CopyField("qr", qr);
    Check(*std::max_element(qr.begin(), qr.end()) > 1.0e-3F);
}

void TestDeterministicAcrossThreadCounts()
{
    FastStormConfig a = SmallConfig();
    a.threads = 1;
    FastStormConfig b = SmallConfig();
    b.threads = 4;
    FastStormSolver sa(a);
    FastStormSolver sb(b);
    sa.Advance(600.0);
    sb.Advance(600.0);
    std::vector<float> wa;
    std::vector<float> wb;
    sa.CopyField("w", wa);
    sb.CopyField("w", wb);
    Check(wa == wb);
}

void TestFormatRoundTripAndMetrics()
{
    FastStormSolver solver(SmallConfig());
    const auto path = std::filesystem::temp_directory_path()
        / "orbit_weather_lab_test.orbitwx";
    {
        WxWriter writer;
        std::string error;
        Check(writer.Open(path, solver.MakeHeader({0.0F, 600.0F}), &error));
        Check(solver.WriteFrame(writer));
        solver.Advance(600.0);
        Check(solver.WriteFrame(writer));
        writer.Close();
    }
    WxReader reader;
    std::string error;
    Check(reader.Open(path, &error));
    Check(reader.FrameCount() == 2U);
    Check(reader.Header().nx == 32U && reader.Header().source == "fastcore");
    std::vector<float> w;
    Check(reader.ReadField(1, "w", w));
    std::vector<float> expected;
    solver.CopyField("w", expected);
    Check(w == expected);
    Check(!reader.ReadField(1, "missing", w));

    std::vector<StormMetrics> metrics;
    Check(ComputeStormMetrics(reader, metrics, &error));
    Check(metrics.size() == 2U);
    Check(metrics[1].maxUpdraft > metrics[0].maxUpdraft);
    std::filesystem::remove(path);
}
} // namespace

int main()
{
    TestBaseState();
    TestValidation();
    TestRestAtmosphereStaysAtRest();
    TestProjectionAndWaterBudget();
    TestStormInitiates();
    TestDeterministicAcrossThreadCounts();
    TestFormatRoundTripAndMetrics();
    std::cout << "Weather lab tests passed\n";
    return 0;
}
