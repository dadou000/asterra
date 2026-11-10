#include <orbit/weather_lab/FastStormSolver.hpp>
#include <orbit/weather_lab/NestedStormSolver.hpp>
#include <orbit/weather_lab/StormMetrics.hpp>
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

void TestIceScheme()
{
    FastStormConfig c = SmallConfig();
    c.ice = true;
    FastStormSolver solver(c);
    solver.Advance(2400.0);
    const FastStormDiagnostics& d = solver.Diagnostics();
    // The storm still initiates, and total water (now including cloud ice, snow and graupel) stays conserved.
    Check(d.maxUpdraft > 15.0F);
    Check(std::fabs(d.totalWater - d.initialTotalWater) < 1.0e-4 * d.initialTotalWater);
    // Ice forms in the cold anvil and graupel/snow reach substantial mixing ratios; nothing is negative or non-finite.
    float maxIce = 0.0F, maxSnow = 0.0F, maxGraupel = 0.0F;
    for (const char* name : {"qi", "qs", "qg"})
    {
        std::vector<float> q;
        solver.CopyField(name, q);
        for (const float value : q)
        {
            Check(std::isfinite(value) && value >= 0.0F);
        }
        const float top = *std::max_element(q.begin(), q.end());
        (name[1] == 'i' ? maxIce : name[1] == 's' ? maxSnow : maxGraupel) = top;
    }
    Check(maxIce > 1.0e-5F);
    Check(maxSnow > 1.0e-5F || maxGraupel > 1.0e-5F);
    // The export list gains the ice fields only when the scheme is on.
    const auto fields = solver.ActiveExportFields();
    Check(std::find(fields.begin(), fields.end(), "qg") != fields.end());
    Check(FastStormSolver(SmallConfig()).ActiveExportFields().size() == FastStormSolver::ExportFields().size());

    // Deterministic across thread counts with ice on.
    FastStormConfig a = c;
    a.threads = 1;
    FastStormConfig b = c;
    b.threads = 4;
    FastStormSolver sa(a);
    FastStormSolver sb(b);
    sa.Advance(600.0);
    sb.Advance(600.0);
    std::vector<float> ga, gb;
    sa.CopyField("qs", ga);
    sb.CopyField("qs", gb);
    Check(ga == gb);
}

NestConfig SmallNest()
{
    NestConfig n;
    n.parent = SmallConfig();
    n.parent.bubble.amplitude = 2.0F;
    n.ratio = 2;
    n.cells = 16;       // child 32 x 32 at half the spacing
    n.relaxCells = 6;
    return n;
}

void TestNesting()
{
    // Validation.
    NestConfig bad = SmallNest();
    Check(NestedStormSolver::Validate(bad).empty());
    bad.ratio = 3;
    Check(!NestedStormSolver::Validate(bad).empty());
    bad = SmallNest();
    bad.cells = 12;   // 24 child cells: not a power of two
    Check(!NestedStormSolver::Validate(bad).empty());
    bad = SmallNest();
    bad.cells = 32;   // no margin left inside a 32-cell parent
    Check(!NestedStormSolver::Validate(bad).empty());

    // One-way: the child starts as the parent's interpolation, stays finite and divergence-free, develops its own storm.
    NestedStormSolver oneWay(SmallNest());
    std::vector<float> childTheta0;
    oneWay.Child().CopyField("th", childTheta0);
    const NestConfig layout = SmallNest();
    Check(childTheta0.size() == 32U * 32U * 40U);
    oneWay.Advance(1500.0);
    const FastStormDiagnostics& dc = oneWay.Child().Diagnostics();
    Check(std::isfinite(dc.maxUpdraft) && dc.maxUpdraft > 3.0F);
    Check(dc.maxDivergence < 1.0e-5F);
    Check(std::fabs(oneWay.Child().Diagnostics().time - oneWay.Parent().Diagnostics().time) < 1.0e-3);
    Check(oneWay.SubStepRatio() >= 1.0);

    // At the child's outer edge the child follows the parent: compare the relaxed edge column with the parent's value there.
    std::vector<float> childTheta, parentTheta;
    oneWay.Child().CopyField("th", childTheta);
    oneWay.Parent().CopyField("th", parentTheta);
    const std::uint32_t oi = (layout.parent.nx - layout.cells) / 2U;
    const std::uint32_t oj = (layout.parent.ny - layout.cells) / 2U;
    double worst = 0.0;
    for (std::uint32_t k = 0; k < 40U; ++k)
    {
        for (std::uint32_t j = 0; j < 32U; ++j)
        {
            const float child = childTheta[(static_cast<std::size_t>(k) * 32U + j) * 32U + 0U];
            const float parent = parentTheta[(static_cast<std::size_t>(k) * layout.parent.ny + oj + j / 2U) * layout.parent.nx + oi];
            worst = std::max(worst, static_cast<double>(std::fabs(child - parent)));
        }
    }
    Check(worst < 0.6);

    // Two-way: finite, and the parent really changes relative to the one-way run.
    NestConfig two = SmallNest();
    two.feedback = true;
    NestedStormSolver twoWay(two);
    twoWay.Advance(1500.0);
    Check(std::isfinite(twoWay.Parent().Diagnostics().maxUpdraft));
    Check(twoWay.Child().Diagnostics().maxDivergence < 1.0e-5F);
    std::vector<float> a, b;
    oneWay.Parent().CopyField("th", a);
    twoWay.Parent().CopyField("th", b);
    double difference = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        Check(std::isfinite(b[i]));
        difference = std::max(difference, static_cast<double>(std::fabs(a[i] - b[i])));
    }
    Check(difference > 1.0e-3);
}

void TestSpongeTrackingAndAdaptiveNest()
{
    // Lateral sponge: rain placed in the edge band decays faster than in the fully periodic run.
    FastStormConfig open = SmallConfig();
    open.lateralSpongeCells = 6;
    FastStormConfig closed = SmallConfig();
    FastStormSolver sOpen(open);
    FastStormSolver sClosed(closed);
    const std::size_t bandCell = (static_cast<std::size_t>(5) * 32U + 16U) * 32U + 1U;   // level 5, column x = 1
    const std::size_t centreCell = (static_cast<std::size_t>(5) * 32U + 16U) * 32U + 16U;
    sOpen.MutableState().qr[bandCell] = 1.0e-3F;
    sClosed.MutableState().qr[bandCell] = 1.0e-3F;
    sOpen.MutableState().qr[centreCell] = 1.0e-3F;
    sClosed.MutableState().qr[centreCell] = 1.0e-3F;
    for (int i = 0; i < 4; ++i)
    {
        (void)sOpen.Step(12.0F);
        (void)sClosed.Step(12.0F);
    }
    std::vector<float> ro, rc;
    sOpen.CopyField("qr", ro);
    sClosed.CopyField("qr", rc);
    Check(ro[bandCell] < 0.92F * rc[bandCell]);           // absorbed in the band (about 13 percent in 48 s)
    Check(std::fabs(ro[centreCell] - rc[centreCell]) < 0.2F * rc[centreCell] + 1.0e-9F);   // the interior is not

    // Storm-following frame: starting from a frame that does not move with the storm, the tracker moves it toward the storm.
    FastStormConfig tracked = SmallConfig();
    tracked.frameU = 0.0F;
    tracked.frameV = 0.0F;
    tracked.trackStorm = true;
    tracked.trackSeconds = 240.0F;
    FastStormSolver follower(tracked);
    follower.Advance(3000.0);
    Check(follower.Config().frameU > 3.0F);
    Check(std::isfinite(follower.Diagnostics().maxUpdraft) && follower.Diagnostics().maxUpdraft > 10.0F);

    // Adaptive child: absent before the storm, created once the updraft passes the threshold, and shrunk by the budget.
    NestConfig adaptive = SmallNest();
    adaptive.adaptive = true;
    adaptive.maxChildCells = 20000;   // a 32 x 32 x 40 child (40960 cells) does not fit: it is halved to 16 x 16 x 40
    Check(NestedStormSolver::Validate(adaptive).empty());
    NestedStormSolver nest(adaptive);
    Check(!nest.HasChild());
    nest.Advance(300.0);
    Check(!nest.HasChild());
    nest.Advance(1500.0);
    Check(nest.HasChild());
    Check(nest.ChildCreations() == 1U);
    Check(nest.BudgetCoarsenings() == 1U);
    Check(nest.ChildCells() == 8U);
    Check(nest.TotalCells() == 32ULL * 32U * 40U + 16ULL * 16U * 40U);
    Check(std::fabs(nest.Child().Diagnostics().time - nest.Parent().Diagnostics().time) < 1.0e-3);
    Check(nest.Child().Diagnostics().maxDivergence < 1.0e-5F);
    NestConfig bad = adaptive;
    bad.releaseUpdraft = bad.createUpdraft;
    Check(!NestedStormSolver::Validate(bad).empty());
}

void TestOutflowDoesNotWrap()
{
    // A 2 K pulse of potential temperature at 14 km leaves through the east edge (the wind there is faster than the frame). In a fully
    // periodic domain it comes back in at the west edge; with the lateral sponge it is absorbed and does not.
    const auto run = [](const bool sponge)
    {
        FastStormConfig c;
        c.nx = 256;
        c.ny = 4;
        c.nz = 40;
        c.threads = 2;
        c.bubble.amplitude = 0.0F;
        c.lateralSpongeCells = sponge ? 16U : 0U;
        FastStormSolver solver(c);
        const FastStormState s = solver.MutableState();
        const std::uint32_t k = 28;
        for (std::uint32_t j = 0; j < c.ny; ++j)
        {
            for (std::uint32_t i = 236; i < 244; ++i)
            {
                s.theta[(static_cast<std::size_t>(k) * c.ny + j) * c.nx + i] += 2.0F;
            }
        }
        solver.Advance(4000.0);
        std::vector<float> theta;
        solver.CopyField("th", theta);
        const BaseState& base = solver.Base();
        float west = 0.0F;   // x from 40 km to 200 km: outside the west sponge band, well away from the pulse's exit
        for (std::uint32_t j = 0; j < c.ny; ++j)
        {
            for (std::uint32_t i = 20; i < 100; ++i)
            {
                west = std::max(west, std::fabs(theta[(static_cast<std::size_t>(k) * c.ny + j) * c.nx + i] - base.theta[k]));
            }
        }
        return west;
    };
    const float periodic = run(false);
    const float open = run(true);
    Check(periodic > 0.03F);             // it wrapped (the pulse disperses into gravity waves: about 0.07 K measured)
    Check(open < 0.02F * periodic);      // the sponge absorbed it (about 0.0004 K measured)
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
} // namespace

int main()
{
    TestBaseState();
    TestValidation();
    TestRestAtmosphereStaysAtRest();
    TestProjectionAndWaterBudget();
    TestStormInitiates();
    TestIceScheme();
    TestNesting();
    TestSpongeTrackingAndAdaptiveNest();
    TestOutflowDoesNotWrap();
    TestDeterministicAcrossThreadCounts();
    TestFormatRoundTripAndMetrics();
    TestSliceColors();
    TestSessionLifecycle();
    TestSessionPauseStepAndConfigure();
    TestSessionPlaybackAndCompare();
    std::cout << "Weather lab tests passed\n";
    return 0;
}
