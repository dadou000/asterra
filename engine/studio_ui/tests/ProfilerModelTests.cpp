#include <orbit/core/ThreadName.hpp>
#include <orbit/profiler/Profiler.hpp>
#include <orbit/studio_ui/ProfilerModel.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace
{
using namespace orbit;
using studio_ui::ProfilerGrouping;
using studio_ui::ProfilerModel;
using studio_ui::ProfilerOptions;

int gFailures = 0;

void Check(const bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << "\n";
        ++gFailures;
    }
}

bool Near(const double a, const double b, const double tolerance = 1.0e-6)
{
    return std::abs(a - b) <= tolerance;
}

profiler::SnapshotSlice Slice(
    const double start,
    const double duration,
    const char* name,
    const unsigned depth,
    const unsigned core)
{
    return {
        .startMs = start,
        .durationMs = duration,
        .name = name,
        .depth = static_cast<u16>(depth),
        .core = static_cast<u16>(core)};
}

// 0..400 ms. Frames 5, 6 and a 150 ms hitch; a main thread with nested scopes;
// two workers.
profiler::Snapshot MakeSnapshot()
{
    profiler::Snapshot snapshot;
    snapshot.source = "live";
    snapshot.beginMs = 1000.0;
    snapshot.endMs = 1400.0;

    profiler::SnapshotLane frames;
    frames.name = "Frames";
    frames.synthetic = true;
    frames.threadId = 0x7F000000U;
    frames.slices = {
        Slice(1010.0, 5.0, "frame", 0, 0),
        Slice(1020.0, 6.0, "frame", 0, 0),
        Slice(1100.0, 150.0, "frame", 0, 0)};

    profiler::SnapshotLane main;
    main.name = "Orbit.Main";
    main.threadId = 10U;
    main.slices = {
        Slice(1100.0, 150.0, "compose", 0, 2),
        Slice(1110.0, 100.0, "dxc.compile main", 1, 2),
        Slice(1120.0, 20.0, "parse", 2, 2)};

    profiler::SnapshotLane workerTen;
    workerTen.name = "Orbit.Jobs.10";
    workerTen.threadId = 12U;
    workerTen.slices = {Slice(1050.0, 30.0, "Jobs.job", 0, 5)};

    profiler::SnapshotLane workerTwo;
    workerTwo.name = "Orbit.Jobs.2";
    workerTwo.threadId = 11U;
    workerTwo.slices = {
        Slice(1050.0, 40.0, "Jobs.job", 0, 5),
        Slice(1300.0, 10.0, "Jobs.job", 0, 6)};

    // Deliberately out of display order.
    snapshot.lanes = {workerTen, main, workerTwo, frames};
    for (const auto& lane : snapshot.lanes)
    {
        snapshot.sliceCount += lane.slices.size();
    }
    snapshot.stalls = {
        {.timeMs = 1150.0, .frame = 3U, .label = "dxcompiler.dll+0x1",
         .stack = {"dxcompiler.dll+0x1", "Orbit.exe+0x2"}},
        {.timeMs = 1200.0, .frame = 3U, .label = "dxcompiler.dll+0x1",
         .stack = {"dxcompiler.dll+0x1", "Orbit.exe+0x2"}}};
    return snapshot;
}

void TestRowsAndAnalysis()
{
    ProfilerModel model([](double) { return MakeSnapshot(); }, [] { return 0ULL; });
    model.Tick(0.0);

    const auto& rows = model.Rows();
    Check(rows.size() == 4U, "four lanes");
    if (rows.size() == 4U)
    {
        Check(rows[0].name == "Frames", "Frames lane first");
        Check(rows[1].name == "Orbit.Main", "main thread after synthetic lanes");
        Check(rows[2].name == "Orbit.Jobs.2" && rows[3].name == "Orbit.Jobs.10",
              "worker lanes in natural order (2 before 10)");
        Check(rows[1].rowCount == 3U, "main lane needs three nested rows");
    }

    const auto stats = model.ScopeStats(10U);
    Check(!stats.empty() && stats.front().name == "compose" &&
              Near(stats.front().totalMs, 150.0),
          "heaviest scope is compose at 150 ms");
    const auto jobs = std::find_if(
        stats.begin(), stats.end(),
        [](const auto& stat) { return stat.name == "Jobs.job"; });
    Check(jobs != stats.end() && jobs->count == 3U && Near(jobs->totalMs, 80.0) &&
              Near(jobs->maxMs, 40.0),
          "job scope aggregated over both workers");

    const auto slowest = model.SlowestSlices(2U);
    Check(slowest.size() == 2U && slowest[0].name == "compose" &&
              slowest[1].name == "dxc.compile main",
          "slowest slices ordered by duration");
    if (!slowest.empty())
    {
        Check(Near(slowest[0].startMs, 100.0), "start is relative to the snapshot");
        Check(Near(slowest[0].selfMs, 50.0) && slowest[0].children == 1U,
              "compose self time excludes its nested compile");
        Check(slowest[0].core == 2U, "core is reported");
    }

    const auto frames = model.Frames();
    Check(frames.size() == 3U && Near(frames[2].durationMs, 150.0) &&
              Near(frames[2].startMs, 100.0),
          "frames come from the Frames lane");

    // Filtering by minimum slice length hides short scopes from the analysis.
    ProfilerOptions options = model.Options();
    options.minSliceMs = 35.0;
    model.SetOptions(options);
    const auto filtered = model.ScopeStats(10U);
    Check(std::none_of(filtered.begin(), filtered.end(),
                       [](const auto& stat) { return stat.name == "parse"; }),
          "short slices are excluded");

    const auto found = model.FindSlice("Orbit.Main", 1125.0);
    Check(found.has_value() && model.Describe(*found)->name == "parse",
          "FindSlice returns the deepest scope at a time");
    Check(!model.FindSlice("Orbit.Main", 1399.0).has_value(), "nothing at an idle time");
}

void TestGpuPassStats()
{
    profiler::Snapshot snapshot = MakeSnapshot();
    profiler::SnapshotLane gpu;
    gpu.name = "GPU passes";
    gpu.synthetic = true;
    gpu.threadId = 0x7F000005U;
    gpu.slices = {
        Slice(1100.0, 120.0, "terrain.composite", 0, 0),
        Slice(1230.0, 2.0, "post", 0, 0),
        Slice(1240.0, 1.0, "post", 0, 0)};
    snapshot.lanes.push_back(gpu);
    snapshot.sliceCount += gpu.slices.size();

    ProfilerModel model([snapshot](double) { return snapshot; }, [] { return 0ULL; });
    model.Tick(0.0);

    const auto passes = model.GpuScopeStats(5U);
    Check(passes.size() == 2U && passes[0].name == "terrain.composite" &&
              Near(passes[0].totalMs, 120.0),
          "GPU passes are ranked by total GPU time");
    Check(passes.size() == 2U && passes[1].count == 2U && Near(passes[1].totalMs, 3.0),
          "repeated GPU passes are aggregated");

    const auto cpu = model.ScopeStats(20U);
    Check(std::none_of(cpu.begin(), cpu.end(),
                       [](const auto& stat) { return stat.name == "terrain.composite"; }),
          "GPU passes are not counted as CPU scopes");
}

void TestPauseAndInspection()
{
    int calls = 0;
    ProfilerModel model(
        [&calls](double)
        {
            ++calls;
            return MakeSnapshot();
        },
        [] { return 0ULL; });

    model.Tick(0.0);
    Check(calls == 1 && !model.Paused() && model.FollowingLive(), "starts live");
    model.Tick(0.1);
    Check(calls == 1, "live refreshes are throttled");
    model.Tick(0.3);
    Check(calls == 2, "live refreshes a few times a second");

    model.SetPaused(true, 0.35);
    const int pausedCalls = calls;
    Check(model.Paused() && !model.FollowingLive(), "pause freezes the view");
    model.Tick(5.0);
    model.Tick(10.0);
    Check(calls == pausedCalls, "a paused model never refreshes");

    model.ZoomAt(1200.0, 0.5, 10.0);
    Check(Near(model.ViewSpanMs(), 200.0), "zoom halves the visible span");
    Check(model.ViewBeginMs() >= 1000.0 && model.ViewBeginMs() + model.ViewSpanMs() <= 1400.0,
          "zoom stays inside the snapshot");
    model.Pan(10000.0, 10.0);
    Check(Near(model.ViewBeginMs() + model.ViewSpanMs(), 1400.0), "pan clamps at the end");
    model.ResetView();
    Check(Near(model.ViewSpanMs(), 400.0), "reset shows the whole snapshot");

    model.SetPaused(false, 11.0);
    Check(!model.Paused() && model.FollowingLive() && calls == pausedCalls + 1,
          "resume goes live with a fresh snapshot");

    // Interacting with a live view pauses it.
    model.ZoomAt(1100.0, 0.5, 12.0);
    Check(model.Paused(), "zooming a live view pauses it");

    model.SetPaused(false, 13.0);
    const auto slice = model.FindSlice("Orbit.Main", 1115.0);
    model.Select(slice, 13.5);
    Check(model.Paused() && model.Selected().has_value(), "selecting pauses and selects");
    model.ZoomToSlice(*slice, 14.0);
    Check(model.ViewSpanMs() < 200.0 && model.ViewBeginMs() <= 1110.0 &&
              model.ViewBeginMs() + model.ViewSpanMs() >= 1210.0,
          "zoom to slice frames the slice");
}

void TestGroupingByCore()
{
    ProfilerModel model([](double) { return MakeSnapshot(); }, [] { return 0ULL; });
    model.Tick(0.0);
    ProfilerOptions options = model.Options();
    options.grouping = ProfilerGrouping::Cores;
    model.SetOptions(options);

    std::vector<std::string> names;
    for (const auto& row : model.Rows())
    {
        names.push_back(row.name);
    }
    Check(names.size() == 4U && names[0] == "Frames" && names[1] == "Core 2" &&
              names[2] == "Core 5" && names[3] == "Core 6",
          "cores: synthetic lanes, then one lane per core in order");
    for (const auto& row : model.Rows())
    {
        if (row.name == "Core 5")
        {
            Check(row.slices.size() == 2U && row.rowCount == 2U,
                  "overlapping jobs on one core stack into two rows");
        }
        if (row.name == "Core 2")
        {
            Check(row.rowCount == 3U, "nested scopes on a core stack downward");
        }
    }
    const auto found = model.FindSlice("Orbit.Jobs.2", 1060.0);
    Check(found.has_value(), "a thread name still finds its slice when grouped by core");
}

void TestFreezeOnHitch()
{
    unsigned long long hitches = 0U;
    ProfilerModel model(
        [](double) { return MakeSnapshot(); }, [&hitches] { return hitches; });
    model.Tick(0.0);
    hitches = 1U;
    model.Tick(0.05);
    Check(model.Paused() && !model.Loaded(), "a recorded hitch pauses the live view");
    const double frameStart = 1100.0;
    Check(model.ViewBeginMs() < frameStart &&
              model.ViewBeginMs() + model.ViewSpanMs() > frameStart + 150.0 &&
              model.ViewSpanMs() < 400.0,
          "the view is framed on the longest frame");

    ProfilerModel quiet(
        [](double) { return MakeSnapshot(); }, [&hitches] { return hitches; });
    ProfilerOptions options = quiet.Options();
    options.freezeOnHitch = false;
    quiet.SetOptions(options);
    quiet.Tick(0.0);
    hitches = 2U;
    quiet.Tick(1.0);
    Check(!quiet.Paused(), "freeze on hitch can be turned off");
}

void TestTraceRoundTrip()
{
    namespace fs = std::filesystem;
    // Unique per run, and cleanup never throws: a scanner can hold a fresh file
    // for a moment on Windows.
    const fs::path directory = fs::temp_directory_path() /
        ("orbit_profiler_model_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ignored;
    fs::remove_all(directory, ignored);
    profiler::Config config;
    config.outputDirectory = directory;
    profiler::Configure(config);

    {
        std::thread worker(
            []
            {
                core::SetCurrentThreadName("Orbit.ModelTest");
                ORBIT_PROFILE_SCOPE("model.outer");
                ORBIT_PROFILE_SCOPE("model.inner");
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            });
        worker.join();
    }
    const auto path = directory / "trace.json";
    static_cast<void>(profiler::Capture(path, 5000.0));

    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    const auto snapshot = ProfilerModel::ParseTrace(text.str(), "trace.json");

    Check(snapshot.source == "trace.json", "source is kept");
    const auto lane = std::find_if(
        snapshot.lanes.begin(), snapshot.lanes.end(),
        [](const auto& candidate) { return candidate.name == "Orbit.ModelTest"; });
    Check(lane != snapshot.lanes.end(), "the thread lane survives a write/parse round trip");
    if (lane != snapshot.lanes.end())
    {
        Check(lane->slices.size() == 2U, "both scopes were parsed");
        if (lane->slices.size() == 2U)
        {
            Check(std::string(lane->slices[0].name) == "model.outer" &&
                      lane->slices[0].depth == 0U && lane->slices[1].depth == 1U,
                  "names and nesting depth are preserved");
            Check(lane->slices[0].durationMs >= 2.0, "durations are in milliseconds");
        }
    }
    Check(snapshot.endMs > snapshot.beginMs, "time range spans the events");

    // The same file loads through the async path and installs paused.
    ProfilerModel model([](double) { return MakeSnapshot(); }, [] { return 0ULL; });
    model.LoadTraceFileAsync(path);
    for (int i = 0; i < 200 && model.Loading() && model.LoadError().empty() && !model.Loaded(); ++i)
    {
        model.Tick(static_cast<double>(i));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(model.Loaded() && model.Paused() && model.Snapshot().source == "trace.json",
          "a loaded trace is shown paused");
    model.SetPaused(false, 100.0);
    Check(!model.Loaded() && model.Snapshot().source == "live", "resume drops the loaded trace");

    bool threw = false;
    try
    {
        static_cast<void>(ProfilerModel::ParseTrace("{\"nothing\":1}", "bad"));
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    Check(threw, "a file with no trace events is rejected");
    fs::remove_all(directory, ignored);
}
} // namespace

int main()
{
    try
    {
        TestRowsAndAnalysis();
        TestGpuPassStats();
        TestPauseAndInspection();
        TestGroupingByCore();
        TestFreezeOnHitch();
        TestTraceRoundTrip();
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAILED: unexpected exception: " << exception.what() << '\n';
        return 1;
    }
    if (gFailures == 0)
    {
        std::cout << "Orbit profiler model tests passed.\n";
    }
    return gFailures == 0 ? 0 : 1;
}
