#include <orbit/weather_lab/FastStormSolver.hpp>
#include <orbit/weather_lab/StormMetrics.hpp>
#include <orbit/weather_lab/CloudVolume.hpp>
#include <orbit/weather_lab/SliceColor.hpp>
#include <orbit/weather_lab/Thermo.hpp>
#include <orbit/weather_lab/WeatherLabSession.hpp>
#include <orbit/weather_lab/WxFormat.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <chrono>
#include <source_location>
#include <thread>
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

bool WaitFor(WeatherLabSession& session, const SessionState wanted, const double seconds)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (session.Status().state == wanted)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

WeatherLabSettings SessionSettings()
{
    WeatherLabSettings s;
    s.solver = SmallConfig();
    s.solver.nx = 16;
    s.solver.ny = 16;
    s.solver.dx = 4000.0F;
    s.solver.dy = 4000.0F;
    s.solver.maxTimeStep = 24.0F;
    s.solver.maxCourant = 2.5F;
    s.targetMinutes = 20.0;
    s.frameIntervalSeconds = 300.0;
    return s;
}

void TestSessionLifecycle()
{
    WeatherLabSession session;
    Check(session.Status().state == SessionState::Idle);
    Check(!session.GetSlice({}).valid);
    Check(session.Configure(SessionSettings()).empty());
    Check(session.Start().empty());
    Check(WaitFor(session, SessionState::Finished, 120.0));
    const WeatherLabStatus status = session.Status();
    Check(std::fabs(status.simTime - 1200.0) < 0.01);
    Check(status.liveFrames == 5U); // 0, 300, 600, 900, 1200 s
    const auto metrics = session.LiveMetrics();
    Check(metrics.size() == 5U);
    Check(std::fabs(metrics[2].time - 600.0F) < 0.01F);
    Check(status.realTimeRatio > 1.0);
    Check(!session.Start().empty()); // finished runs need a Reset

    const Slice plan = session.GetSlice({.kind = SliceKind::Plan, .field = "w", .height = 3000.0F});
    Check(plan.valid && plan.width == 16U && plan.height == 16U);
    Check(plan.values.size() == 256U);
    const Slice section = session.GetSlice({.kind = SliceKind::Section, .field = "qr"});
    Check(section.valid && section.height == 40U);
    const Slice column = session.GetSlice({.kind = SliceKind::ColumnMax, .field = "condensate"});
    Check(column.valid && column.maxValue >= column.minValue);
    Check(!session.GetSlice({.field = "bogus"}).valid);

    session.Reset();
    Check(session.Status().state == SessionState::Idle);
    Check(session.LiveMetrics().empty());
}

void TestSessionPauseStepAndConfigure()
{
    WeatherLabSession session;
    Check(session.Configure(SessionSettings()).empty());
    Check(session.Step(100.0).empty());
    Check(WaitFor(session, SessionState::Paused, 30.0));
    // Step returns immediately; wait for the budget to be consumed.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (session.Status().simTime < 99.9
        && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    Check(std::fabs(session.Status().simTime - 100.0) < 0.5);
    // Settings are frozen while a run exists.
    Check(!session.Configure(SessionSettings()).empty());
    session.Reset();
    Check(session.Configure(SessionSettings()).empty());
    WeatherLabSettings bad = SessionSettings();
    bad.solver.nx = 48;
    Check(!session.Configure(bad).empty());
}

void TestSessionPlaybackAndCompare()
{
    const auto path = std::filesystem::temp_directory_path()
        / "orbit_weather_lab_session_test.orbitwx";
    WeatherLabSettings settings = SessionSettings();
    settings.recordPath = path;
    WeatherLabSession recorder;
    Check(recorder.Configure(settings).empty());
    Check(recorder.Start().empty());
    Check(WaitFor(recorder, SessionState::Finished, 120.0));
    Check(!recorder.Status().recording);

    WeatherLabSession session;
    Check(session.Configure(SessionSettings()).empty());
    Check(!session.LoadPlayback(path.string() + ".missing").empty());
    Check(session.LoadPlayback(path).empty());
    Check(session.Status().hasPlayback && session.Status().playbackFrames == 5U);
    Check(session.SelectPlaybackFrame(4).empty());
    Check(!session.SelectPlaybackFrame(9).empty());
    const Slice playback = session.GetSlice({.source = DisplaySource::Playback,
        .kind = SliceKind::Plan, .field = "thp", .height = 250.0F});
    Check(playback.valid && std::fabs(playback.time - 1200.0) < 0.01);

    // Same settings, same machine: a rerun must reproduce the recording, so
    // the comparison rows match to round-off.
    Check(session.Start().empty());
    Check(WaitFor(session, SessionState::Finished, 120.0));
    const auto rows = session.Compare();
    Check(rows.size() == 5U);
    for (const ComparisonRow& row : rows)
    {
        Check(row.reference.maxUpdraft == row.live.maxUpdraft);
    }
    session.ClearPlayback();
    Check(!session.Status().hasPlayback);
    std::filesystem::remove(path);
}

void TestSliceColors()
{
    Slice signedSlice;
    signedSlice.field = "w";
    signedSlice.minValue = -10.0F;
    signedSlice.maxValue = 40.0F;
    // Zero is white regardless of the asymmetric range; extremes are saturated.
    const auto zero = SliceColor(signedSlice, 0.0F);
    Check(zero[0] > 0.9F && zero[1] > 0.9F && zero[2] > 0.9F);
    const auto up = SliceColor(signedSlice, 40.0F);
    Check(up[0] > up[2]);
    const auto down = SliceColor(signedSlice, -40.0F);
    Check(down[2] > down[0] && down[0] < 0.2F);

    Slice sequential;
    sequential.field = "qr";
    sequential.minValue = 0.0F;
    sequential.maxValue = 8.0F;
    const auto low = SliceColor(sequential, 0.0F);
    const auto high = SliceColor(sequential, 8.0F);
    Check(high[0] + high[1] > low[0] + low[1]);
    // Out-of-range values clamp instead of wrapping.
    Check(SliceColor(sequential, 99.0F) == high);
    Check(IsSignedField("zvort") && !IsSignedField("qr"));
}

void TestCloudVolumeMapping()
{
    WxHeader h;
    h.nx = 4; h.ny = 3; h.nz = 5; h.dx = 1000.0F; h.dy = 2000.0F;
    for (std::uint32_t k = 0; k < h.nz; ++k) { h.centreHeight.push_back(250.0F + 500.0F * static_cast<float>(k)); }
    std::vector<float> q(h.CellCount(), 0.0F);
    // One cloudy cell: x=3, y=1, z=4 (the top layer), 4 g/kg.
    q[(4U * 3U + 1U) * 4U + 3U] = 0.004F;

    CloudVolumeRequest r;
    r.gainPerGramPerKg = 0.5F;
    r.upAxis = VolumeUpAxis::Z;
    CloudVolumeGrid z = BuildCloudVolumeGrid(h, q, r);
    Check(z.valid && z.resolutionX == 4U && z.resolutionY == 3U && z.resolutionZ == 5U);
    Check(z.sizeX == 4000.0 && z.sizeY == 6000.0 && z.sizeZ == 2500.0);
    Check(std::fabs(z.maxCondensateGramsPerKg - 4.0F) < 1.0e-4F);
    const float expected = 1.0F - std::exp(-2.0F);
    Check(std::fabs(z.density[(4U * 3U + 1U) * 4U + 3U] - expected) < 1.0e-5F);

    // Y-up swaps the vertical into the middle axis: cache (x, y, z) = sim (x, z, y).
    r.upAxis = VolumeUpAxis::Y;
    CloudVolumeGrid y = BuildCloudVolumeGrid(h, q, r);
    Check(y.resolutionX == 4U && y.resolutionY == 5U && y.resolutionZ == 3U);
    Check(y.sizeY == 2500.0 && y.sizeZ == 6000.0);
    Check(std::fabs(y.density[(1U * 5U + 4U) * 4U + 3U] - expected) < 1.0e-5F);

    // X-up: cache (x, y, z) = sim (z, x, y).
    r.upAxis = VolumeUpAxis::X;
    CloudVolumeGrid x = BuildCloudVolumeGrid(h, q, r);
    Check(x.resolutionX == 5U && x.resolutionY == 4U && x.resolutionZ == 3U);
    Check(std::fabs(x.density[(1U * 4U + 3U) * 5U + 4U] - expected) < 1.0e-5F);

    // Density stays in [0,1], the rest of the grid is clear, bad input is rejected.
    float sum = 0.0F;
    for (const float d : y.density) { Check(d >= 0.0F && d <= 1.0F); sum += d; }
    Check(std::fabs(sum - expected) < 1.0e-5F);
    Check(!BuildCloudVolumeGrid(h, std::vector<float>(3, 0.0F), r).valid);
    VolumeUpAxis parsed = VolumeUpAxis::Z;
    Check(ParseVolumeUpAxis("y", parsed) && parsed == VolumeUpAxis::Y);
    Check(!ParseVolumeUpAxis("w", parsed));
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
    TestCloudVolumeMapping();
    TestSliceColors();
    TestSessionLifecycle();
    TestSessionPauseStepAndConfigure();
    TestSessionPlaybackAndCompare();
    std::cout << "Weather lab tests passed\n";
    return 0;
}
