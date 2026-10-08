#include <orbit/studio_ui/ProfilerModel.hpp>

#include <orbit/rpc/JsonRpc.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace orbit::studio_ui
{
namespace
{
constexpr f64 kRefreshIntervalSeconds = 0.25;
constexpr f64 kMinSpanMs = 0.02;
constexpr u16 kMaxPackedRows = 10U;
constexpr u32 kSyntheticThreadIdBase = 0x7F000000U;

[[nodiscard]] bool NaturalLess(
    const std::string& a,
    const std::string& b) noexcept
{
    std::size_t i = 0U;
    std::size_t j = 0U;
    while (i < a.size() && j < b.size())
    {
        if (std::isdigit(static_cast<unsigned char>(a[i])) != 0 &&
            std::isdigit(static_cast<unsigned char>(b[j])) != 0)
        {
            u64 x = 0U;
            u64 y = 0U;
            while (i < a.size() &&
                   std::isdigit(static_cast<unsigned char>(a[i])) != 0)
            {
                x = x * 10U + static_cast<u64>(a[i++] - '0');
            }
            while (j < b.size() &&
                   std::isdigit(static_cast<unsigned char>(b[j])) != 0)
            {
                y = y * 10U + static_cast<u64>(b[j++] - '0');
            }
            if (x != y)
            {
                return x < y;
            }
            continue;
        }
        if (a[i] != b[j])
        {
            return a[i] < b[j];
        }
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

// Frames first, then the other synthetic lanes, the main thread, then the rest.
[[nodiscard]] int LaneRank(const std::string& name, const bool synthetic) noexcept
{
    if (name == "Frames")
    {
        return 0;
    }
    if (synthetic)
    {
        return 1;
    }
    return name == "Orbit.Main" ? 2 : 3;
}

// Stacks overlapping slices into the fewest rows; `row` is the first row whose
// previous slice has ended (a nested slice therefore lands below its parent).
void PackRows(std::vector<ProfilerSlice>& slices, u16& rowCount)
{
    std::sort(
        slices.begin(),
        slices.end(),
        [](const ProfilerSlice& a, const ProfilerSlice& b)
        {
            return a.startMs != b.startMs ? a.startMs < b.startMs
                                          : a.durationMs > b.durationMs;
        });
    std::vector<f64> rowEnds;
    for (auto& slice : slices)
    {
        u16 chosen = 0U;
        for (; chosen < rowEnds.size(); ++chosen)
        {
            if (rowEnds[chosen] <= slice.startMs + 1.0e-6)
            {
                break;
            }
        }
        if (chosen >= kMaxPackedRows)
        {
            chosen = static_cast<u16>(kMaxPackedRows - 1U);
        }
        if (chosen >= rowEnds.size())
        {
            rowEnds.resize(static_cast<std::size_t>(chosen) + 1U, -1.0e300);
        }
        rowEnds[chosen] =
            std::max(rowEnds[chosen], slice.startMs + slice.durationMs);
        slice.row = chosen;
    }
    rowCount = static_cast<u16>(std::max<std::size_t>(rowEnds.size(), 1U));
}

[[nodiscard]] std::string Lower(const std::string_view text)
{
    std::string result(text);
    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

[[nodiscard]] f64 Number(const rpc::Value& value, const f64 fallback = 0.0)
{
    return value.IsNumber() ? value.AsNumber() : fallback;
}

[[nodiscard]] const rpc::Value* Find(
    const rpc::Value::Object& object,
    const char* key) noexcept
{
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}
} // namespace

ProfilerModel::ProfilerModel(
    SnapshotProvider snapshots,
    HitchCountProvider hitches)
    : provider_(std::move(snapshots))
    , hitches_(std::move(hitches))
{
    if (!provider_)
    {
        provider_ = [](const f64 windowMs)
        {
            return profiler::TakeSnapshot(windowMs);
        };
    }
    if (!hitches_)
    {
        hitches_ = []
        {
            return profiler::Frames().hitches;
        };
    }
}

f64 ProfilerModel::SnapshotSpanMs() const noexcept
{
    return std::max(snapshot_.endMs - snapshot_.beginMs, kMinSpanMs);
}

f64 ProfilerModel::ViewBeginMs() const noexcept
{
    return followLive_ ? snapshot_.beginMs : viewBeginMs_;
}

f64 ProfilerModel::ViewSpanMs() const noexcept
{
    return followLive_ ? SnapshotSpanMs() : viewSpanMs_;
}

void ProfilerModel::Install(profiler::Snapshot snapshot, const bool loaded)
{
    snapshot_ = std::move(snapshot);
    loaded_ = loaded;
    selected_.reset();
    RebuildRows();
}

void ProfilerModel::Refresh(const f64 nowSeconds)
{
    lastRefreshSeconds_ = nowSeconds;
    Install(provider_(options_.windowMs), false);
}

void ProfilerModel::RebuildRows()
{
    rows_.clear();
    selected_.reset();

    std::vector<std::size_t> order;
    order.reserve(snapshot_.lanes.size());
    for (std::size_t i = 0U; i < snapshot_.lanes.size(); ++i)
    {
        if (!snapshot_.lanes[i].slices.empty())
        {
            order.push_back(i);
        }
    }
    std::stable_sort(
        order.begin(),
        order.end(),
        [this](const std::size_t a, const std::size_t b)
        {
            const auto& la = snapshot_.lanes[a];
            const auto& lb = snapshot_.lanes[b];
            const int ra = LaneRank(la.name, la.synthetic);
            const int rb = LaneRank(lb.name, lb.synthetic);
            return ra != rb ? ra < rb : NaturalLess(la.name, lb.name);
        });

    const auto toSlice = [](const profiler::SnapshotSlice& source,
                            const u32 lane)
    {
        return ProfilerSlice{
            .startMs = source.startMs,
            .durationMs = source.durationMs,
            .name = source.name,
            .row = source.depth,
            .depth = source.depth,
            .core = source.core,
            .sourceLane = lane};
    };

    std::map<u16, ProfilerRow> cores;

    for (const std::size_t index : order)
    {
        const auto& lane = snapshot_.lanes[index];
        const bool perCore =
            options_.grouping == ProfilerGrouping::Cores && !lane.synthetic;
        if (perCore)
        {
            for (const auto& source : lane.slices)
            {
                auto& row = cores[source.core];
                row.name = "Core " + std::to_string(source.core);
                row.threadId = source.core;
                row.slices.push_back(toSlice(source, static_cast<u32>(index)));
            }
            continue;
        }

        ProfilerRow row;
        row.name = lane.name;
        row.threadId = lane.threadId;
        row.synthetic = lane.synthetic;
        row.slices.reserve(lane.slices.size());
        for (const auto& source : lane.slices)
        {
            row.slices.push_back(toSlice(source, static_cast<u32>(index)));
        }
        if (lane.synthetic)
        {
            PackRows(row.slices, row.rowCount);
        }
        else
        {
            u16 deepest = 0U;
            for (auto& slice : row.slices)
            {
                slice.row = std::min<u16>(slice.row, 15U);
                deepest = std::max(deepest, slice.row);
            }
            row.rowCount = static_cast<u16>(deepest + 1U);
        }
        rows_.push_back(std::move(row));
    }

    for (auto& [core, row] : cores)
    {
        static_cast<void>(core);
        PackRows(row.slices, row.rowCount);
        rows_.push_back(std::move(row));
    }

    for (auto& row : rows_)
    {
        for (const auto& slice : row.slices)
        {
            if (slice.row == 0U)
            {
                row.busyMs += slice.durationMs;
            }
        }
    }
}

void ProfilerModel::ClampView()
{
    const f64 span = SnapshotSpanMs();
    viewSpanMs_ = std::clamp(viewSpanMs_, kMinSpanMs, span);
    viewBeginMs_ = std::clamp(
        viewBeginMs_,
        snapshot_.beginMs,
        std::max(snapshot_.endMs - viewSpanMs_, snapshot_.beginMs));
}

void ProfilerModel::Inspect(const f64 nowSeconds)
{
    if (!paused_)
    {
        SetPaused(true, nowSeconds);
    }
}

void ProfilerModel::SetPaused(const bool paused, const f64 nowSeconds)
{
    if (paused)
    {
        if (paused_)
        {
            return;
        }
        if (!loaded_)
        {
            Refresh(nowSeconds);
        }
        paused_ = true;
        followLive_ = false;
        viewBeginMs_ = snapshot_.beginMs;
        viewSpanMs_ = SnapshotSpanMs();
        return;
    }

    paused_ = false;
    followLive_ = true;
    if (loaded_)
    {
        loaded_ = false;
    }
    selected_.reset();
    Refresh(nowSeconds);
}

void ProfilerModel::SetOptions(const ProfilerOptions& options)
{
    const bool regroup = options.grouping != options_.grouping;
    const bool rewindow = options.windowMs != options_.windowMs;
    options_ = options;
    options_.windowMs = std::clamp(options_.windowMs, 500.0, 60000.0);
    options_.minSliceMs = std::max(options_.minSliceMs, 0.0);
    options_.viewportCaptureDurationMs =
        std::clamp(options_.viewportCaptureDurationMs, 1000.0, 30000.0);
    if (options_.viewportCaptureResolution != "720p" &&
        options_.viewportCaptureResolution != "1080p" &&
        options_.viewportCaptureResolution != "1440p" &&
        options_.viewportCaptureResolution != "2160p")
    {
        options_.viewportCaptureResolution = "1440p";
    }
    if (options_.viewportCaptureScenario != "static" &&
        options_.viewportCaptureScenario != "walk_1_94_mps" &&
        options_.viewportCaptureScenario != "surface_200_kmh" &&
        options_.viewportCaptureScenario != "flight_2000_mps_5000m" &&
        options_.viewportCaptureScenario != "ground_to_orbit_20s")
    {
        options_.viewportCaptureScenario = "static";
    }
    if (regroup)
    {
        RebuildRows();
    }
    if (rewindow && !paused_)
    {
        lastRefreshSeconds_ = -1.0e9;
    }
}

void ProfilerModel::Tick(const f64 nowSeconds)
{
    if (load_.valid() &&
        load_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        try
        {
            auto loadedSnapshot = load_.get();
            Install(std::move(loadedSnapshot), true);
            paused_ = true;
            followLive_ = false;
            viewBeginMs_ = snapshot_.beginMs;
            viewSpanMs_ = SnapshotSpanMs();
            loadError_.clear();
        }
        catch (const std::exception& exception)
        {
            loadError_ = exception.what();
        }
    }

    if (paused_)
    {
        return;
    }

    const u64 hitchCount = hitches_();
    if (!hitchCountKnown_)
    {
        lastHitchCount_ = hitchCount;
        hitchCountKnown_ = true;
    }
    const bool newHitch = hitchCount > lastHitchCount_;
    lastHitchCount_ = hitchCount;

    if (newHitch || nowSeconds - lastRefreshSeconds_ >= kRefreshIntervalSeconds)
    {
        Refresh(nowSeconds);
    }

    if (newHitch && options_.freezeOnHitch)
    {
        paused_ = true;
        followLive_ = false;
        viewBeginMs_ = snapshot_.beginMs;
        viewSpanMs_ = SnapshotSpanMs();
        const auto frames = Frames();
        if (!frames.empty())
        {
            const auto longest = std::max_element(
                frames.begin(),
                frames.end(),
                [](const ProfilerFrameInfo& a, const ProfilerFrameInfo& b)
                {
                    return a.durationMs < b.durationMs;
                });
            ZoomToFrame(
                static_cast<std::size_t>(longest - frames.begin()),
                nowSeconds);
        }
    }
}

void ProfilerModel::SetView(
    const f64 beginMs,
    const f64 spanMs,
    const f64 nowSeconds)
{
    Inspect(nowSeconds);
    viewBeginMs_ = beginMs;
    viewSpanMs_ = spanMs;
    ClampView();
}

void ProfilerModel::ZoomAt(
    const f64 anchorMs,
    const f64 factor,
    const f64 nowSeconds)
{
    Inspect(nowSeconds);
    const f64 span = ViewSpanMs();
    const f64 begin = ViewBeginMs();
    const f64 fraction =
        span > 0.0 ? std::clamp((anchorMs - begin) / span, 0.0, 1.0) : 0.5;
    const f64 newSpan =
        std::clamp(span * factor, kMinSpanMs, SnapshotSpanMs());
    viewSpanMs_ = newSpan;
    viewBeginMs_ = anchorMs - fraction * newSpan;
    ClampView();
}

void ProfilerModel::Pan(const f64 deltaMs, const f64 nowSeconds)
{
    Inspect(nowSeconds);
    viewBeginMs_ = ViewBeginMs() + deltaMs;
    viewSpanMs_ = ViewSpanMs();
    ClampView();
}

void ProfilerModel::ResetView()
{
    viewBeginMs_ = snapshot_.beginMs;
    viewSpanMs_ = SnapshotSpanMs();
    if (!paused_)
    {
        followLive_ = true;
    }
}

void ProfilerModel::ZoomToSlice(
    const ProfilerSliceRef& slice,
    const f64 nowSeconds)
{
    if (slice.row >= rows_.size() ||
        slice.index >= rows_[slice.row].slices.size())
    {
        return;
    }
    const auto& target = rows_[slice.row].slices[slice.index];
    const f64 span = std::max(target.durationMs * 1.4, 0.1);
    SetView(
        target.startMs - (span - target.durationMs) * 0.5,
        span,
        nowSeconds);
}

void ProfilerModel::ZoomToFrame(
    const std::size_t frameIndex,
    const f64 nowSeconds)
{
    const auto frames = Frames();
    if (frameIndex >= frames.size())
    {
        return;
    }
    const auto& frame = frames[frameIndex];
    const f64 padding = std::max(frame.durationMs * 0.2, 1.0);
    SetView(
        snapshot_.beginMs + frame.startMs - padding,
        frame.durationMs + padding * 2.0,
        nowSeconds);
}

void ProfilerModel::Select(
    std::optional<ProfilerSliceRef> slice,
    const f64 nowSeconds)
{
    if (slice.has_value())
    {
        Inspect(nowSeconds);
    }
    selected_ = slice;
}

std::vector<ProfilerFrameInfo> ProfilerModel::Frames() const
{
    std::vector<ProfilerFrameInfo> frames;
    for (const auto& row : rows_)
    {
        if (row.name != "Frames")
        {
            continue;
        }
        frames.reserve(row.slices.size());
        for (const auto& slice : row.slices)
        {
            frames.push_back({
                .startMs = slice.startMs - snapshot_.beginMs,
                .durationMs = slice.durationMs});
        }
        break;
    }
    return frames;
}

bool ProfilerModel::SliceVisible(const ProfilerSlice& slice) const noexcept
{
    if (slice.durationMs < options_.minSliceMs)
    {
        return false;
    }
    const f64 begin = ViewBeginMs();
    return slice.startMs + slice.durationMs >= begin &&
           slice.startMs <= begin + ViewSpanMs();
}

bool ProfilerModel::SliceMatchesFilter(const ProfilerSlice& slice) const noexcept
{
    if (options_.filter.empty())
    {
        return true;
    }
    const std::string_view name(slice.name);
    const auto same = [](const char a, const char b)
    {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };
    return std::search(
               name.begin(),
               name.end(),
               options_.filter.begin(),
               options_.filter.end(),
               same) != name.end();
}

std::optional<ProfilerSliceRef> ProfilerModel::FindSlice(
    const std::string_view laneName,
    const f64 timeMs) const
{
    std::optional<ProfilerSliceRef> best;
    u16 bestRow = 0U;
    for (std::size_t r = 0U; r < rows_.size(); ++r)
    {
        const auto& row = rows_[r];
        if (row.name != laneName)
        {
            continue;
        }
        for (std::size_t i = 0U; i < row.slices.size(); ++i)
        {
            const auto& slice = row.slices[i];
            if (timeMs >= slice.startMs &&
                timeMs <= slice.startMs + slice.durationMs &&
                (!best.has_value() || slice.row >= bestRow))
            {
                best = ProfilerSliceRef{.row = r, .index = i};
                bestRow = slice.row;
            }
        }
    }
    if (best.has_value())
    {
        return best;
    }
    // A thread name while grouped by core: look at each core row.
    for (std::size_t r = 0U; r < rows_.size(); ++r)
    {
        const auto& row = rows_[r];
        for (std::size_t i = 0U; i < row.slices.size(); ++i)
        {
            const auto& slice = row.slices[i];
            if (slice.sourceLane < snapshot_.lanes.size() &&
                snapshot_.lanes[slice.sourceLane].name == laneName &&
                timeMs >= slice.startMs &&
                timeMs <= slice.startMs + slice.durationMs &&
                (!best.has_value() || slice.depth >= bestRow))
            {
                best = ProfilerSliceRef{.row = r, .index = i};
                bestRow = slice.depth;
            }
        }
    }
    return best;
}

std::optional<ProfilerSliceInfo> ProfilerModel::Describe(
    const ProfilerSliceRef& ref) const
{
    if (ref.row >= rows_.size() || ref.index >= rows_[ref.row].slices.size())
    {
        return std::nullopt;
    }
    const auto& slice = rows_[ref.row].slices[ref.index];

    ProfilerSliceInfo info;
    info.name = slice.name;
    info.lane = slice.sourceLane < snapshot_.lanes.size()
        ? snapshot_.lanes[slice.sourceLane].name
        : rows_[ref.row].name;
    info.startMs = slice.startMs - snapshot_.beginMs;
    info.durationMs = slice.durationMs;
    info.core = slice.core;
    info.depth = slice.depth;

    f64 childMs = 0.0;
    if (slice.sourceLane < snapshot_.lanes.size())
    {
        const auto& lane = snapshot_.lanes[slice.sourceLane];
        const f64 end = slice.startMs + slice.durationMs;
        auto it = std::lower_bound(
            lane.slices.begin(),
            lane.slices.end(),
            slice.startMs - 1.0e-6,
            [](const profiler::SnapshotSlice& s, const f64 value)
            {
                return s.startMs < value;
            });
        for (; it != lane.slices.end() && it->startMs <= end; ++it)
        {
            if (it->depth == slice.depth + 1U &&
                it->startMs + it->durationMs <= end + 1.0e-6)
            {
                ++info.children;
                childMs += it->durationMs;
            }
        }
    }
    info.selfMs = std::max(slice.durationMs - childMs, 0.0);
    return info;
}

std::vector<ProfilerScopeStat> ProfilerModel::Aggregate(
    const std::function<bool(const ProfilerRow&)>& include,
    const std::size_t limit) const
{
    std::unordered_map<std::string_view, ProfilerScopeStat> stats;
    for (const auto& row : rows_)
    {
        if (!include(row))
        {
            continue;
        }
        for (const auto& slice : row.slices)
        {
            if (!SliceVisible(slice))
            {
                continue;
            }
            auto& stat = stats[slice.name];
            if (stat.name.empty())
            {
                stat.name = slice.name;
            }
            ++stat.count;
            stat.totalMs += slice.durationMs;
            stat.maxMs = std::max(stat.maxMs, slice.durationMs);
        }
    }
    std::vector<ProfilerScopeStat> result;
    result.reserve(stats.size());
    for (auto& [name, stat] : stats)
    {
        static_cast<void>(name);
        result.push_back(std::move(stat));
    }
    std::sort(
        result.begin(),
        result.end(),
        [](const ProfilerScopeStat& a, const ProfilerScopeStat& b)
        {
            return a.totalMs > b.totalMs;
        });
    if (result.size() > limit)
    {
        result.resize(limit);
    }
    return result;
}

std::vector<ProfilerScopeStat> ProfilerModel::ScopeStats(
    const std::size_t limit) const
{
    return Aggregate(
        [](const ProfilerRow& row) { return !row.synthetic; }, limit);
}

std::vector<ProfilerScopeStat> ProfilerModel::GpuScopeStats(
    const std::size_t limit) const
{
    return Aggregate(
        [](const ProfilerRow& row) { return row.name == "GPU passes"; }, limit);
}

std::vector<ProfilerSliceRef> ProfilerModel::SlowestRefs(
    const std::size_t limit) const
{
    std::vector<ProfilerSliceRef> refs;
    for (std::size_t r = 0U; r < rows_.size(); ++r)
    {
        if (rows_[r].synthetic)
        {
            continue;
        }
        for (std::size_t i = 0U; i < rows_[r].slices.size(); ++i)
        {
            if (SliceVisible(rows_[r].slices[i]))
            {
                refs.push_back({.row = r, .index = i});
            }
        }
    }
    const auto duration = [this](const ProfilerSliceRef& ref)
    {
        return rows_[ref.row].slices[ref.index].durationMs;
    };
    const std::size_t count = std::min(limit, refs.size());
    std::partial_sort(
        refs.begin(),
        refs.begin() + static_cast<std::ptrdiff_t>(count),
        refs.end(),
        [&](const ProfilerSliceRef& a, const ProfilerSliceRef& b)
        {
            return duration(a) > duration(b);
        });
    refs.resize(count);
    return refs;
}

std::vector<ProfilerSliceInfo> ProfilerModel::SlowestSlices(
    const std::size_t limit) const
{
    std::vector<ProfilerSliceInfo> result;
    for (const auto& ref : SlowestRefs(limit))
    {
        if (auto info = Describe(ref); info.has_value())
        {
            result.push_back(std::move(*info));
        }
    }
    return result;
}

profiler::Snapshot ProfilerModel::ParseTrace(
    const std::string_view json,
    std::string source)
{
    const rpc::Value root = rpc::ParseValue(json);
    if (!root.IsObject())
    {
        throw std::runtime_error("Trace file is not a JSON object.");
    }
    const rpc::Value* events = Find(root.AsObject(), "traceEvents");
    if (events == nullptr || !events->IsArray())
    {
        throw std::runtime_error("Trace file has no traceEvents array.");
    }

    profiler::Snapshot snapshot;
    snapshot.source = std::move(source);

    std::map<u32, profiler::SnapshotLane> lanes;
    f64 begin = 1.0e300;
    f64 end = -1.0e300;

    for (const rpc::Value& event : events->AsArray())
    {
        if (!event.IsObject())
        {
            continue;
        }
        const auto& object = event.AsObject();
        const rpc::Value* phase = Find(object, "ph");
        const rpc::Value* pid = Find(object, "pid");
        const rpc::Value* tid = Find(object, "tid");
        const rpc::Value* name = Find(object, "name");
        if (phase == nullptr || !phase->IsString() || pid == nullptr ||
            Number(*pid) != 1.0 || tid == nullptr || name == nullptr ||
            !name->IsString())
        {
            continue;
        }
        const u32 threadId = static_cast<u32>(Number(*tid));
        const std::string& kind = phase->AsString();

        if (kind == "M")
        {
            if (name->AsString() == "thread_name")
            {
                const rpc::Value* args = Find(object, "args");
                if (args != nullptr && args->IsObject())
                {
                    if (const auto* label = Find(args->AsObject(), "name");
                        label != nullptr && label->IsString())
                    {
                        auto& lane = lanes[threadId];
                        lane.threadId = threadId;
                        lane.name = label->AsString();
                        lane.synthetic = threadId >= kSyntheticThreadIdBase;
                    }
                }
            }
            continue;
        }

        const rpc::Value* timestamp = Find(object, "ts");
        if (timestamp == nullptr)
        {
            continue;
        }
        const f64 startMs = Number(*timestamp) / 1000.0;

        if (kind == "X")
        {
            const rpc::Value* dur = Find(object, "dur");
            const f64 durationMs = dur != nullptr ? Number(*dur) / 1000.0 : 0.0;
            auto& lane = lanes[threadId];
            lane.threadId = threadId;
            lane.synthetic = threadId >= kSyntheticThreadIdBase;
            profiler::SnapshotSlice slice;
            slice.startMs = startMs;
            slice.durationMs = durationMs;
            slice.name = profiler::Intern(name->AsString());
            if (const rpc::Value* args = Find(object, "args");
                args != nullptr && args->IsObject())
            {
                if (const auto* core = Find(args->AsObject(), "core"))
                {
                    slice.core = static_cast<u16>(Number(*core));
                }
                if (const auto* depth = Find(args->AsObject(), "depth"))
                {
                    slice.depth = static_cast<u16>(Number(*depth));
                }
            }
            lane.slices.push_back(slice);
            begin = std::min(begin, startMs);
            end = std::max(end, startMs + durationMs);
        }
        else if (kind == "i" && name->AsString().rfind("STALLED in ", 0U) == 0U)
        {
            profiler::SnapshotStall stall;
            stall.timeMs = startMs;
            stall.label = name->AsString().substr(11U);
            if (const rpc::Value* args = Find(object, "args");
                args != nullptr && args->IsObject())
            {
                if (const auto* stack = Find(args->AsObject(), "stack");
                    stack != nullptr && stack->IsArray())
                {
                    for (const auto& frame : stack->AsArray())
                    {
                        if (frame.IsString())
                        {
                            stall.stack.push_back(frame.AsString());
                        }
                    }
                }
            }
            snapshot.stalls.push_back(std::move(stall));
        }
    }

    if (begin > end)
    {
        throw std::runtime_error("Trace file contains no timed events.");
    }
    snapshot.beginMs = begin;
    snapshot.endMs = end;
    for (auto& [threadId, lane] : lanes)
    {
        static_cast<void>(threadId);
        if (lane.name.empty())
        {
            lane.name = "Thread " + std::to_string(lane.threadId);
        }
        std::sort(
            lane.slices.begin(),
            lane.slices.end(),
            [](const profiler::SnapshotSlice& a, const profiler::SnapshotSlice& b)
            {
                return a.startMs < b.startMs;
            });
        snapshot.sliceCount += lane.slices.size();
        if (!lane.slices.empty())
        {
            snapshot.lanes.push_back(std::move(lane));
        }
    }
    return snapshot;
}

void ProfilerModel::LoadTraceFileAsync(const std::filesystem::path& path)
{
    loadError_.clear();
    loadingName_ = path.filename().string();
    load_ = std::async(
        std::launch::async,
        [path]
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                throw std::runtime_error(
                    "Could not open " + path.string() + ".");
            }
            std::ostringstream text;
            text << in.rdbuf();
            return ParseTrace(text.str(), path.filename().string());
        });
}
} // namespace orbit::studio_ui
