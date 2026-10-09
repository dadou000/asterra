#pragma once

#include <orbit/core/Types.hpp>
#include <orbit/profiler/Profiler.hpp>

#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_ui
{
enum class ProfilerGrouping : u8
{
    // One lane per thread (plus the synthetic lanes), nested by call depth.
    Threads,
    // One lane per CPU core: what ran where, whichever thread it was.
    Cores
};

struct ProfilerSlice
{
    f64 startMs{0.0};
    f64 durationMs{0.0};
    const char* name{""};
    // Vertical row inside the lane: call depth for threads, packed for cores.
    u16 row{0U};
    // Call depth on the source thread (the same as `row` when grouped by thread).
    u16 depth{0U};
    u16 core{0U};
    // The thread lane this slice came from (an index into Snapshot::lanes).
    u32 sourceLane{0U};
};

struct ProfilerRow
{
    std::string name;
    u32 threadId{0U};
    bool synthetic{false};
    // Number of stacked rows the lane needs.
    u16 rowCount{1U};
    f64 busyMs{0.0};
    std::vector<ProfilerSlice> slices;
};

struct ProfilerScopeStat
{
    std::string name;
    u64 count{0U};
    // Inclusive: nested scopes are counted in their parents as well.
    f64 totalMs{0.0};
    f64 maxMs{0.0};
};

struct ProfilerSliceRef
{
    std::size_t row{0U};
    std::size_t index{0U};
    [[nodiscard]] bool operator==(const ProfilerSliceRef&) const = default;
};

struct ProfilerSliceInfo
{
    std::string name;
    std::string lane;
    // Milliseconds from the start of the snapshot.
    f64 startMs{0.0};
    f64 durationMs{0.0};
    // Duration minus the time spent in directly nested scopes.
    f64 selfMs{0.0};
    u64 children{0U};
    u16 core{0U};
    u16 depth{0U};
};

struct ProfilerFrameInfo
{
    // Milliseconds from the start of the snapshot.
    f64 startMs{0.0};
    f64 durationMs{0.0};
};

struct ProfilerOptions
{
    // History copied on each refresh (the ring buffers bound what exists).
    f64 windowMs{8000.0};
    ProfilerGrouping grouping{ProfilerGrouping::Threads};
    // Slices shorter than this are not drawn or counted. Zero shows everything.
    f64 minSliceMs{0.0};
    // Case-insensitive substring: matching slices are emphasised, others dim.
    std::string filter;
    // Pause by itself when a hitch is recorded, framed on the long frame.
    bool freezeOnHitch{true};
    // Default settings for the viewport-only Perfetto capture action.
    f64 viewportCaptureDurationMs{4000.0};
    std::string viewportCaptureResolution{"1440p"};
    std::string viewportCaptureScenario{"static"};
};

// State behind the Profiler panel and the profiler.* panel RPC methods: a live
// snapshot refreshed a few times a second, or a frozen one the user is
// inspecting (paused, or loaded from a hitch/capture trace file). No ImGui here.
class ProfilerModel
{
public:
    using SnapshotProvider =
        std::function<profiler::Snapshot(f64 windowMs)>;
    using HitchCountProvider = std::function<u64()>;

    explicit ProfilerModel(
        SnapshotProvider snapshots = {},
        HitchCountProvider hitches = {});

    // Call once per UI frame with a monotonic clock in seconds. Refreshes the
    // live snapshot when it is due and handles freeze-on-hitch and file loads.
    void Tick(f64 nowSeconds);

    // ---- pause / live ------------------------------------------------------
    [[nodiscard]] bool Paused() const noexcept { return paused_; }
    // Pausing freezes a fresh snapshot; resuming drops a loaded file and goes live.
    void SetPaused(bool paused, f64 nowSeconds);
    [[nodiscard]] bool Loaded() const noexcept { return loaded_; }

    // ---- options -------------------------------------------------------------
    [[nodiscard]] const ProfilerOptions& Options() const noexcept { return options_; }
    void SetOptions(const ProfilerOptions& options);

    // ---- data ------------------------------------------------------------------
    [[nodiscard]] const profiler::Snapshot& Snapshot() const noexcept { return snapshot_; }
    [[nodiscard]] const std::vector<ProfilerRow>& Rows() const noexcept { return rows_; }
    [[nodiscard]] std::vector<ProfilerFrameInfo> Frames() const;
    [[nodiscard]] f64 SnapshotBeginMs() const noexcept { return snapshot_.beginMs; }
    [[nodiscard]] f64 SnapshotSpanMs() const noexcept;

    // ---- view (absolute profiler-clock milliseconds) -----------------------------
    [[nodiscard]] f64 ViewBeginMs() const noexcept;
    [[nodiscard]] f64 ViewSpanMs() const noexcept;
    [[nodiscard]] bool FollowingLive() const noexcept { return followLive_; }
    // Interacting with a live view pauses it first, like any frame profiler.
    void SetView(f64 beginMs, f64 spanMs, f64 nowSeconds);
    void ZoomAt(f64 anchorMs, f64 factor, f64 nowSeconds);
    void Pan(f64 deltaMs, f64 nowSeconds);
    void ResetView();
    void ZoomToSlice(const ProfilerSliceRef& slice, f64 nowSeconds);
    void ZoomToFrame(std::size_t frameIndex, f64 nowSeconds);

    // ---- selection ----------------------------------------------------------------
    [[nodiscard]] const std::optional<ProfilerSliceRef>& Selected() const noexcept { return selected_; }
    void Select(std::optional<ProfilerSliceRef> slice, f64 nowSeconds);
    // The slice on `laneName` covering `timeMs` (absolute), deepest row first.
    [[nodiscard]] std::optional<ProfilerSliceRef> FindSlice(
        std::string_view laneName,
        f64 timeMs) const;
    [[nodiscard]] std::optional<ProfilerSliceInfo> Describe(
        const ProfilerSliceRef& slice) const;

    // ---- analysis (over the visible view range, real threads only) ----------------
    [[nodiscard]] std::vector<ProfilerScopeStat> ScopeStats(std::size_t limit) const;
    // The same over the "GPU passes" lane: GPU time per render pass in view.
    [[nodiscard]] std::vector<ProfilerScopeStat> GpuScopeStats(std::size_t limit) const;
    [[nodiscard]] std::vector<ProfilerSliceInfo> SlowestSlices(std::size_t limit) const;
    [[nodiscard]] std::vector<ProfilerSliceRef> SlowestRefs(std::size_t limit) const;
    [[nodiscard]] bool SliceVisible(const ProfilerSlice& slice) const noexcept;
    [[nodiscard]] bool SliceMatchesFilter(const ProfilerSlice& slice) const noexcept;

    // ---- trace files ---------------------------------------------------------------
    // Parses a Chrome/Perfetto trace written by the profiler (a hitch or capture
    // file) on a worker thread; Tick installs it, paused, when it is ready.
    void LoadTraceFileAsync(const std::filesystem::path& path);
    [[nodiscard]] bool Loading() const noexcept { return load_.valid(); }
    [[nodiscard]] const std::string& LoadError() const noexcept { return loadError_; }
    // Synchronous parse, exposed for tests and the loader.
    [[nodiscard]] static profiler::Snapshot ParseTrace(
        std::string_view json,
        std::string source);

private:
    [[nodiscard]] std::vector<ProfilerScopeStat> Aggregate(
        const std::function<bool(const ProfilerRow&)>& include,
        std::size_t limit) const;
    void Install(profiler::Snapshot snapshot, bool loaded);
    void Refresh(f64 nowSeconds);
    void RebuildRows();
    void ClampView();
    void Inspect(f64 nowSeconds);

    SnapshotProvider provider_;
    HitchCountProvider hitches_;
    ProfilerOptions options_;
    profiler::Snapshot snapshot_;
    std::vector<ProfilerRow> rows_;
    bool paused_{false};
    bool loaded_{false};
    bool followLive_{true};
    f64 viewBeginMs_{0.0};
    f64 viewSpanMs_{1000.0};
    f64 lastRefreshSeconds_{-1.0e9};
    u64 lastHitchCount_{0U};
    bool hitchCountKnown_{false};
    std::optional<ProfilerSliceRef> selected_;
    std::future<profiler::Snapshot> load_;
    std::string loadError_;
    std::string loadingName_;
};
} // namespace orbit::studio_ui
