#pragma once

#include <orbit/core/Types.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Orbit's micro-profiler.
//
// Every thread that records a scope gets a fixed-size ring buffer, so recording
// is a timestamp, a core number and a store (no locks, no allocation). Scopes are
// always on by default, which is what makes a hitch capturable after the fact:
// when a frame runs long, the last few seconds of every thread's events are
// written as a Chrome/Perfetto trace with one lane per thread and one lane per
// CPU core. A watchdog thread also samples the watched thread's call stack while
// a frame is stalled, so even a hang with no instrumentation shows where it is
// stuck. See docs/ORBIT_PROFILER.md.
namespace orbit::profiler
{
struct Config
{
    bool enabled{true};
    // A frame longer than this is written to disk as a hitch capture.
    f64 hitchThresholdMs{100.0};
    // While the watched thread is still inside a frame past this, its call stack
    // is sampled every ~50 ms.
    f64 stallThresholdMs{250.0};
    // How much history a hitch or manual capture keeps.
    f64 captureWindowMs{4000.0};
    u32 maxHitchFiles{24U};
    // Empty: %LOCALAPPDATA%\Orbit\profiler (or ORBIT_PROFILER_DIR).
    std::filesystem::path outputDirectory;
};

// High-resolution monotonic clock shared by every recorded event.
[[nodiscard]] u64 NowTicks() noexcept;
[[nodiscard]] f64 TicksToMilliseconds(u64 ticks) noexcept;
[[nodiscard]] f64 TicksPerMillisecond() noexcept;

[[nodiscard]] bool Enabled() noexcept;

// Scopes. `name` must outlive the process: pass a string literal or the result
// of Intern(). Nesting is tracked per thread.
void BeginScope(const char* name) noexcept;
void EndScope() noexcept;

// An interval measured elsewhere, recorded on the calling thread.
void RecordSpan(const char* name, u64 beginTicks, u64 endTicks) noexcept;

// An interval on a named synthetic lane (for example the editor's per-phase
// timings). Each lane must be written from a single thread.
void RecordLaneSpan(
    const char* lane,
    const char* name,
    u64 beginTicks,
    u64 endTicks) noexcept;

// Records the calling thread's display name. core::SetCurrentThreadName calls
// this, so a thread keeps its name in captures after it has exited.
void NoteThreadName(std::string_view name);

// A stable pointer for a dynamic name.
[[nodiscard]] const char* Intern(std::string_view name);

class Scope
{
public:
    explicit Scope(const char* name) noexcept
        : active_(Enabled())
    {
        if (active_)
        {
            BeginScope(name);
        }
    }

    ~Scope()
    {
        if (active_)
        {
            EndScope();
        }
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    // Captured at construction so toggling the profiler mid-scope cannot
    // unbalance the per-thread nesting.
    bool active_{false};
};

#define ORBIT_PROFILE_CONCAT_INNER(a, b) a##b
#define ORBIT_PROFILE_CONCAT(a, b) ORBIT_PROFILE_CONCAT_INNER(a, b)
#define ORBIT_PROFILE_SCOPE(name) \
    ::orbit::profiler::Scope ORBIT_PROFILE_CONCAT(orbitProfileScope, __LINE__)(name)

// Frames. Call BeginFrame/EndFrame on the thread that drives the frame loop.
void BeginFrame() noexcept;
void EndFrame() noexcept;
// Drops the frame in progress without recording it (a minimised window that
// skips rendering would otherwise look like a stall).
void CancelFrame() noexcept;

struct FrameSummary
{
    u64 frames{0U};
    f64 lastMs{0.0};
    f64 averageMs{0.0};
    f64 worstMs{0.0};
    u64 hitches{0U};
};

// Summary over the last 240 frames (and the lifetime hitch count).
[[nodiscard]] FrameSummary Frames() noexcept;
[[nodiscard]] std::vector<f32> RecentFrameMilliseconds(u32 count);

void Configure(const Config& config);
[[nodiscard]] Config CurrentConfig();

// Starts the hitch writer and stall watchdog. Call it on the thread whose frames
// are watched (the main thread): that thread is the one whose stack is sampled.
void StartWatchdog();
void StopWatchdog();

struct CaptureInfo
{
    std::filesystem::path path;
    u64 events{0U};
    u32 threads{0U};
    f64 windowMs{0.0};
};

// Writes the last `windowMs` of every thread as a Chrome/Perfetto trace.
[[nodiscard]] CaptureInfo Capture(
    const std::filesystem::path& path,
    f64 windowMs);

// OutputDirectory()/capture-<unix ms>.json, the file name manual captures use.
[[nodiscard]] std::filesystem::path DefaultCapturePath();

struct HitchInfo
{
    std::filesystem::path path;
    u64 frame{0U};
    f64 frameMs{0.0};
    std::string time;
    u32 stackSamples{0U};
    // The frames most often at the top of the sampled stacks, most frequent first.
    std::vector<std::string> topFrames;
};

// An in-memory copy of recent activity, for the Studio profiler panel and the
// profiler.snapshot RPC. Times are milliseconds on the profiler clock (see
// NowMilliseconds). Slice names point at string literals or interned strings and
// stay valid for the life of the process.
struct SnapshotSlice
{
    f64 startMs{0.0};
    f64 durationMs{0.0};
    const char* name{""};
    u16 depth{0U};
    // The core the slice started on.
    u16 core{0U};
};

struct SnapshotLane
{
    std::string name;
    u32 threadId{0U};
    // A lane such as "Frames" or "Main loop phases" that is not a real thread.
    bool synthetic{false};
    // Sorted by startMs.
    std::vector<SnapshotSlice> slices;
};

// A stack sample taken while the watched thread was stalled.
struct SnapshotStall
{
    f64 timeMs{0.0};
    u64 frame{0U};
    // The first frame outside system modules.
    std::string label;
    std::vector<std::string> stack;
};

struct Snapshot
{
    // "live", or the file a snapshot was loaded from.
    std::string source;
    f64 beginMs{0.0};
    f64 endMs{0.0};
    std::vector<SnapshotLane> lanes;
    std::vector<SnapshotStall> stalls;
    u64 sliceCount{0U};
};

// Copies the last `windowMs` of every thread and lane. Cheap enough to call a few
// times per second; symbolising stall stacks is cached per address.
[[nodiscard]] Snapshot TakeSnapshot(f64 windowMs, bool includeStalls = true);
// The profiler clock in milliseconds (the time base of Snapshot and traces).
[[nodiscard]] f64 NowMilliseconds() noexcept;

[[nodiscard]] std::vector<HitchInfo> RecentHitches();
[[nodiscard]] std::filesystem::path OutputDirectory();
} // namespace orbit::profiler
