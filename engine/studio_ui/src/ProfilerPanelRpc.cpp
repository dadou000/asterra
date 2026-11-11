#include <orbit/studio_ui/ProfilerUi.hpp>

#include <orbit/profiler/Profiler.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;

constexpr i64 kInvalid = 1090;

[[nodiscard]] f64 SteadySeconds()
{
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<f64>(Clock::now().time_since_epoch()).count();
}

[[nodiscard]] const char* GroupingName(const ProfilerGrouping grouping) noexcept
{
    return grouping == ProfilerGrouping::Threads ? "threads" : "cores";
}

[[nodiscard]] Value SliceToValue(const ProfilerSliceInfo& info)
{
    return Value(Value::Object{
        {"name", info.name},
        {"thread", info.lane},
        {"start_ms", info.startMs},
        {"duration_ms", info.durationMs},
        {"self_ms", info.selfMs},
        {"nested_scopes", static_cast<i64>(info.children)},
        {"core", static_cast<i64>(info.core)},
        {"depth", static_cast<i64>(info.depth)}});
}

[[nodiscard]] Value ViewToValue(const ProfilerModel& model)
{
    return Value(Value::Object{
        {"begin_ms", model.ViewBeginMs() - model.SnapshotBeginMs()},
        {"span_ms", model.ViewSpanMs()},
        {"snapshot_span_ms", model.SnapshotSpanMs()},
        {"following_live", model.FollowingLive()}});
}

[[nodiscard]] Value StateToValue(const ProfilerModel& model)
{
    const auto& options = model.Options();
    const auto& snapshot = model.Snapshot();
    Value::Object state{
        {"paused", model.Paused()},
        {"loaded_from_file", model.Loaded()},
        {"loading", model.Loading()},
        {"load_error", model.LoadError()},
        {"source", snapshot.source},
        {"window_ms", options.windowMs},
        {"grouping", GroupingName(options.grouping)},
        {"freeze_on_hitch", options.freezeOnHitch},
        {"viewport_capture_duration_ms", options.viewportCaptureDurationMs},
        {"viewport_capture_resolution", options.viewportCaptureResolution},
        {"viewport_capture_scenario", options.viewportCaptureScenario},
        {"min_slice_ms", options.minSliceMs},
        {"filter", options.filter},
        {"lanes", static_cast<i64>(snapshot.lanes.size())},
        {"slices", static_cast<i64>(snapshot.sliceCount)},
        {"stack_samples", static_cast<i64>(snapshot.stalls.size())},
        {"view", ViewToValue(model)}};
    if (model.Selected().has_value())
    {
        if (const auto info = model.Describe(*model.Selected()); info.has_value())
        {
            state.emplace("selected", SliceToValue(*info));
        }
    }
    return Value(std::move(state));
}

[[nodiscard]] std::optional<f64> Number(const Value::Object& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    if (!found->second.IsNumber())
    {
        throw rpc::Error(-32602, std::string(key) + " must be a number.");
    }
    return found->second.AsNumber();
}

[[nodiscard]] std::optional<bool> Boolean(const Value::Object& object, const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    if (!found->second.IsBool())
    {
        throw rpc::Error(-32602, std::string(key) + " must be a boolean.");
    }
    return found->second.AsBool();
}
} // namespace

void RegisterProfilerPanelRpc(rpc::Dispatcher& dispatcher, ProfilerModel& model)
{
    dispatcher.Register(
        {
            .name = "profiler.panel_get",
            .description =
                "State of the Studio Profiler panel: paused or live, where the "
                "snapshot came from (live or a loaded hitch file), options "
                "(history window, viewport-only capture duration and resolution, "
                "grouping, freeze on hitch, minimum slice, "
                "highlight filter), the visible time range and the selected slice.",
            .mutating = false
        },
        [&model](const Value&)
        {
            model.Tick(SteadySeconds());
            return StateToValue(model);
        });

    dispatcher.Register(
        {
            .name = "profiler.panel_set",
            .description =
                "Drives the Profiler panel like its controls. Fields (all "
                "optional): paused (Pause / Resume live), window_ms, grouping "
                "('threads' | 'cores'), freeze_on_hitch, min_slice_ms, filter, "
                "viewport_capture_duration_ms (1000-30000) and "
                "viewport_capture_resolution ('720p' | '1080p' | '1440p' | '2160p'), "
                "viewport_capture_scenario ('static' | 'walk_1_94_mps' | "
                "'surface_200_kmh' | 'flight_2000_mps_5000m' | 'ground_to_orbit_20s'), "
                "reset_view, view_begin_ms + view_span_ms (relative to the "
                "snapshot start; pauses), zoom_frame (frame index in the "
                "snapshot, -1 for the longest), select {thread, time_ms} "
                "(relative to the snapshot start; null clears), "
                "zoom_to_selection, load_trace (path of a hitch or capture "
                "file, loaded paused). Returns the new panel state.",
            .mutating = true
        },
        [&model](const Value& params)
        {
            const f64 now = SteadySeconds();
            model.Tick(now);
            if (!params.IsObject())
            {
                return StateToValue(model);
            }
            const auto& values = params.AsObject();

            ProfilerOptions options = model.Options();
            bool optionsChanged = false;
            if (const auto value = Number(values, "window_ms"))
            {
                options.windowMs = *value;
                optionsChanged = true;
            }
            if (const auto found = values.find("grouping"); found != values.end())
            {
                if (!found->second.IsString() ||
                    (found->second.AsString() != "threads" &&
                     found->second.AsString() != "cores"))
                {
                    throw rpc::Error(
                        -32602, "grouping must be 'threads' or 'cores'.");
                }
                options.grouping = found->second.AsString() == "threads"
                    ? ProfilerGrouping::Threads
                    : ProfilerGrouping::Cores;
                optionsChanged = true;
            }
            if (const auto value = Boolean(values, "freeze_on_hitch"))
            {
                options.freezeOnHitch = *value;
                optionsChanged = true;
            }
            if (const auto value = Number(values, "min_slice_ms"))
            {
                options.minSliceMs = *value;
                optionsChanged = true;
            }
            if (const auto value = Number(values, "viewport_capture_duration_ms"))
            {
                options.viewportCaptureDurationMs = *value;
                optionsChanged = true;
            }
            if (const auto found = values.find("viewport_capture_resolution"); found != values.end())
            {
                if (!found->second.IsString())
                {
                    throw rpc::Error(-32602, "viewport_capture_resolution must be a preset string.");
                }
                if (found->second.AsString() != "720p" &&
                    found->second.AsString() != "1080p" &&
                    found->second.AsString() != "1440p" &&
                    found->second.AsString() != "2160p")
                {
                    throw rpc::Error(-32602, "viewport_capture_resolution must be 720p, 1080p, 1440p, or 2160p.");
                }
                options.viewportCaptureResolution = found->second.AsString();
                optionsChanged = true;
            }
            if (const auto found = values.find("viewport_capture_scenario"); found != values.end())
            {
                if (!found->second.IsString())
                {
                    throw rpc::Error(-32602, "viewport_capture_scenario must be a scenario string.");
                }
                options.viewportCaptureScenario = found->second.AsString();
                if (options.viewportCaptureScenario != "static" &&
                    options.viewportCaptureScenario != "walk_1_94_mps" &&
                    options.viewportCaptureScenario != "surface_200_kmh" &&
                    options.viewportCaptureScenario != "flight_2000_mps_5000m" &&
                    options.viewportCaptureScenario != "ground_to_orbit_20s")
                {
                    throw rpc::Error(-32602, "unsupported viewport_capture_scenario.");
                }
                optionsChanged = true;
            }
            if (const auto found = values.find("filter"); found != values.end())
            {
                if (!found->second.IsString())
                {
                    throw rpc::Error(-32602, "filter must be a string.");
                }
                options.filter = found->second.AsString();
                optionsChanged = true;
            }
            if (optionsChanged)
            {
                model.SetOptions(options);
            }

            if (const auto found = values.find("load_trace"); found != values.end())
            {
                if (!found->second.IsString() || found->second.AsString().empty())
                {
                    throw rpc::Error(-32602, "load_trace must be a file path.");
                }
                model.LoadTraceFileAsync(found->second.AsString());
            }
            if (const auto value = Boolean(values, "paused"))
            {
                model.SetPaused(*value, now);
            }
            if (Boolean(values, "reset_view").value_or(false))
            {
                model.ResetView();
            }
            if (const auto begin = Number(values, "view_begin_ms"))
            {
                const auto span = Number(values, "view_span_ms")
                                      .value_or(model.ViewSpanMs());
                model.SetView(model.SnapshotBeginMs() + *begin, span, now);
            }
            if (const auto frame = Number(values, "zoom_frame"))
            {
                const auto frames = model.Frames();
                std::size_t index = 0U;
                if (*frame < 0.0)
                {
                    for (std::size_t i = 0U; i < frames.size(); ++i)
                    {
                        if (frames[i].durationMs > frames[index].durationMs)
                        {
                            index = i;
                        }
                    }
                }
                else
                {
                    index = static_cast<std::size_t>(*frame);
                }
                if (index >= frames.size())
                {
                    throw rpc::Error(kInvalid, "zoom_frame is outside the snapshot's frames.");
                }
                model.ZoomToFrame(index, now);
            }
            if (const auto found = values.find("select"); found != values.end())
            {
                if (found->second.IsNull())
                {
                    model.Select(std::nullopt, now);
                }
                else if (found->second.IsObject())
                {
                    const auto& target = found->second.AsObject();
                    const auto thread = target.find("thread");
                    const auto time = Number(target, "time_ms");
                    if (thread == target.end() || !thread->second.IsString() ||
                        !time.has_value())
                    {
                        throw rpc::Error(
                            -32602, "select needs a thread name and time_ms.");
                    }
                    const auto slice = model.FindSlice(
                        thread->second.AsString(),
                        model.SnapshotBeginMs() + *time);
                    if (!slice.has_value())
                    {
                        throw rpc::Error(
                            kInvalid,
                            "No slice on that thread at that time in the snapshot.");
                    }
                    model.Select(slice, now);
                }
                else
                {
                    throw rpc::Error(-32602, "select must be an object or null.");
                }
            }
            if (Boolean(values, "zoom_to_selection").value_or(false))
            {
                if (!model.Selected().has_value())
                {
                    throw rpc::Error(kInvalid, "Nothing is selected.");
                }
                model.ZoomToSlice(*model.Selected(), now);
            }
            return StateToValue(model);
        });

    dispatcher.Register(
        {
            .name = "profiler.snapshot",
            .description =
                "Analysis of what the Profiler panel is showing (the live "
                "window, the frozen snapshot, or a loaded hitch file), limited "
                "to the visible time range: frame statistics, per-lane busy "
                "time, the heaviest scopes (inclusive, summed over threads), "
                "the heaviest GPU passes (top_gpu_passes), "
                "the longest individual slices with thread, core and self time, "
                "and the stack samples taken during stalls. Params: top_scopes "
                "(default 15), slowest (default 15).",
            .mutating = false
        },
        [&model](const Value& params)
        {
            model.Tick(SteadySeconds());
            std::size_t topScopes = 15U;
            std::size_t slowest = 15U;
            if (params.IsObject())
            {
                if (const auto value = Number(params.AsObject(), "top_scopes"))
                {
                    topScopes = static_cast<std::size_t>(std::clamp(*value, 0.0, 200.0));
                }
                if (const auto value = Number(params.AsObject(), "slowest"))
                {
                    slowest = static_cast<std::size_t>(std::clamp(*value, 0.0, 200.0));
                }
            }

            Value::Object result{
                {"state", StateToValue(model)}};

            // Frames in the snapshot.
            {
                const auto frames = model.Frames();
                f64 total = 0.0;
                f64 worst = 0.0;
                for (const auto& frame : frames)
                {
                    total += frame.durationMs;
                    worst = std::max(worst, frame.durationMs);
                }
                std::vector<ProfilerFrameInfo> longest = frames;
                std::sort(
                    longest.begin(),
                    longest.end(),
                    [](const ProfilerFrameInfo& a, const ProfilerFrameInfo& b)
                    {
                        return a.durationMs > b.durationMs;
                    });
                Value::Array longestValues;
                for (std::size_t i = 0U; i < longest.size() && i < 5U; ++i)
                {
                    longestValues.emplace_back(Value::Object{
                        {"start_ms", longest[i].startMs},
                        {"duration_ms", longest[i].durationMs}});
                }
                result.emplace(
                    "frames",
                    Value(Value::Object{
                        {"count", static_cast<i64>(frames.size())},
                        {"average_ms", frames.empty() ? 0.0 : total / static_cast<f64>(frames.size())},
                        {"worst_ms", worst},
                        {"longest", Value(std::move(longestValues))}}));
            }

            Value::Array lanes;
            for (const auto& row : model.Rows())
            {
                lanes.emplace_back(Value::Object{
                    {"name", row.name},
                    {"synthetic", row.synthetic},
                    {"slices", static_cast<i64>(row.slices.size())},
                    {"rows", static_cast<i64>(row.rowCount)},
                    {"busy_ms", row.busyMs}});
            }
            result.emplace("lanes", Value(std::move(lanes)));

            Value::Array scopes;
            for (const auto& stat : model.ScopeStats(topScopes))
            {
                scopes.emplace_back(Value::Object{
                    {"name", stat.name},
                    {"count", static_cast<i64>(stat.count)},
                    {"total_ms", stat.totalMs},
                    {"max_ms", stat.maxMs}});
            }
            result.emplace("top_scopes", Value(std::move(scopes)));

            Value::Array gpuPasses;
            for (const auto& stat : model.GpuScopeStats(topScopes))
            {
                gpuPasses.emplace_back(Value::Object{
                    {"name", stat.name},
                    {"count", static_cast<i64>(stat.count)},
                    {"total_ms", stat.totalMs},
                    {"max_ms", stat.maxMs}});
            }
            result.emplace("top_gpu_passes", Value(std::move(gpuPasses)));

            Value::Array slices;
            for (const auto& info : model.SlowestSlices(slowest))
            {
                slices.push_back(SliceToValue(info));
            }
            result.emplace("slowest_slices", Value(std::move(slices)));

            const auto& stalls = model.Snapshot().stalls;
            Value::Object stallInfo{{"count", static_cast<i64>(stalls.size())}};
            if (!stalls.empty())
            {
                std::map<std::string, u32> counts;
                for (const auto& stall : stalls)
                {
                    ++counts[stall.label];
                }
                std::vector<std::pair<u32, std::string>> ranked;
                for (const auto& [label, count] : counts)
                {
                    ranked.emplace_back(count, label);
                }
                std::sort(ranked.begin(), ranked.end(), std::greater<>());
                Value::Array top;
                for (std::size_t i = 0U; i < ranked.size() && i < 8U; ++i)
                {
                    top.emplace_back(Value::Object{
                        {"label", ranked[i].second},
                        {"samples", static_cast<i64>(ranked[i].first)}});
                }
                stallInfo.emplace("top_frames", Value(std::move(top)));
                Value::Array stack;
                const auto& sample = stalls[stalls.size() / 2U];
                for (std::size_t i = 0U; i < sample.stack.size() && i < 30U; ++i)
                {
                    stack.emplace_back(sample.stack[i]);
                }
                stallInfo.emplace("representative_stack", Value(std::move(stack)));
            }
            result.emplace("stalls", Value(std::move(stallInfo)));
            return Value(std::move(result));
        });
}
} // namespace orbit::studio_ui
