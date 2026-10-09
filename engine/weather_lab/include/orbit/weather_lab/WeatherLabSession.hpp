#pragma once

#include <orbit/weather_lab/FastStormSolver.hpp>
#include <orbit/weather_lab/StormMetrics.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// The headless operations behind the Studio Weather Lab panel and the
// weather_lab.* RPC / MCP methods. The panel, an agent and the tests all call
// this one object, so a button and an RPC method do exactly the same thing.
//
// A session owns at most one live FastStormSolver, advanced on a private
// compute thread (never the UI thread), and at most one loaded .orbitwx
// reference for playback. The compute thread publishes display snapshots a few
// times per second; every query reads the last snapshot, so the UI never waits
// on a solver step.

namespace orbit::weather_lab
{
enum class SliceKind : std::uint8_t
{
    Plan,       // horizontal map at a height
    Section,    // vertical x-z cross-section
    ColumnMax,  // horizontal map of the column maximum (reflectivity-like)
};

enum class DisplaySource : std::uint8_t
{
    Live,
    Playback,
};

struct SliceRequest
{
    DisplaySource source = DisplaySource::Live;
    SliceKind kind = SliceKind::Plan;
    // w qr qc qv thp zvort speed condensate
    std::string field = "w";
    float height = 1000.0F;   // m, Plan
    // Section: y row index, or -1 to cut through the strongest updraft.
    std::int32_t row = -1;
};

struct Slice
{
    bool valid = false;
    std::string error;
    std::string title;
    std::string field;
    std::string unit;
    std::uint32_t width = 0;
    std::uint32_t height = 0;    // rows; row 0 is y = 0 (Plan) or z = 0 (Section)
    float cellWidthMeters = 0.0F;
    float cellHeightMeters = 0.0F;
    float minValue = 0.0F;
    float maxValue = 0.0F;
    // Cell of the strongest value, for markers.
    std::uint32_t peakColumn = 0;
    std::uint32_t peakRow = 0;
    double time = 0.0;
    std::vector<float> values;
};

enum class SessionState : std::uint8_t
{
    Idle,      // no live solver
    Paused,
    Running,
    Finished,  // reached the target time
    Failed,
};

struct WeatherLabSettings
{
    FastStormConfig solver{};
    double targetMinutes = 120.0;
    // Simulated seconds between metric samples / recorded frames.
    double frameIntervalSeconds = 300.0;
    // 0 = as fast as the machine allows, otherwise a cap on simulated seconds
    // per wall-clock second (1 = real time).
    double speedLimit = 0.0;
    // Empty = do not record.
    std::filesystem::path recordPath;
};

struct WeatherLabStatus
{
    SessionState state = SessionState::Idle;
    std::string error;
    double simTime = 0.0;
    double targetSeconds = 0.0;
    std::uint64_t steps = 0;
    double wallSeconds = 0.0;       // compute-thread time spent stepping
    double realTimeRatio = 0.0;     // simulated / wall seconds while stepping
    FastStormDiagnostics diagnostics{};
    std::size_t residentBytes = 0;
    std::size_t liveFrames = 0;     // metric samples recorded
    bool recording = false;
    // Playback reference.
    bool hasPlayback = false;
    std::string playbackPath;
    std::string playbackSource;
    std::size_t playbackFrames = 0;
    std::size_t playbackFrame = 0;
    double playbackTime = 0.0;
};

struct ComparisonRow
{
    float time = 0.0F;
    StormMetrics reference{};
    StormMetrics live{};
};

[[nodiscard]] const char* SessionStateName(SessionState state) noexcept;
[[nodiscard]] const std::vector<std::string>& SliceFieldNames();
[[nodiscard]] std::string SliceFieldUnit(const std::string& field);

class WeatherLabSession
{
public:
    WeatherLabSession();
    ~WeatherLabSession();
    WeatherLabSession(const WeatherLabSession&) = delete;
    WeatherLabSession& operator=(const WeatherLabSession&) = delete;

    // Settings apply to the next Reset/Start; changing them while a live
    // solver exists returns an error (Reset first). Empty string = accepted.
    [[nodiscard]] std::string Configure(const WeatherLabSettings& settings);
    [[nodiscard]] WeatherLabSettings Settings() const;

    // Builds the solver when none exists and runs (or resumes). A finished
    // run needs Reset first.
    [[nodiscard]] std::string Start();
    void Pause();
    // Stops the compute thread and discards the live solver and its history.
    void Reset();
    // Advance by `seconds` of simulated time then pause. Works from Paused or
    // Idle; returns immediately (poll Status).
    [[nodiscard]] std::string Step(double seconds);
    void SetSpeedLimit(double simulatedSecondsPerWallSecond);

    [[nodiscard]] WeatherLabStatus Status() const;
    [[nodiscard]] std::vector<StormMetrics> LiveMetrics() const;

    // Loads a .orbitwx (CM1 export or fast-core run) as a reference. Computes
    // its metrics immediately. Empty string = loaded.
    [[nodiscard]] std::string LoadPlayback(const std::filesystem::path& path);
    void ClearPlayback();
    [[nodiscard]] std::string SelectPlaybackFrame(std::size_t frame);
    [[nodiscard]] std::vector<StormMetrics> PlaybackMetrics() const;
    // Playback metrics paired with the live sample at the same time (within
    // 1 s); frames without a live counterpart are omitted.
    [[nodiscard]] std::vector<ComparisonRow> Compare() const;

    [[nodiscard]] Slice GetSlice(const SliceRequest& request) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace orbit::weather_lab
