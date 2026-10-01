#include <orbit/editor_rpc/ProfilerRpc.hpp>

#include <orbit/profiler/Profiler.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace orbit::editor_rpc
{
namespace
{
using rpc::Value;

constexpr i64 kFailed = 1080;

[[nodiscard]] Value ConfigToValue(const profiler::Config& config)
{
    return Value(Value::Object{
        {"enabled", config.enabled},
        {"hitch_threshold_ms", config.hitchThresholdMs},
        {"stall_threshold_ms", config.stallThresholdMs},
        {"capture_window_ms", config.captureWindowMs},
        {"max_hitch_files", static_cast<i64>(config.maxHitchFiles)},
        {"output_directory", profiler::OutputDirectory().generic_string()}});
}

[[nodiscard]] Value HitchToValue(const profiler::HitchInfo& hitch)
{
    Value::Array frames;
    for (const std::string& frame : hitch.topFrames)
    {
        frames.emplace_back(frame);
    }
    return Value(Value::Object{
        {"path", hitch.path.generic_string()},
        {"frame", static_cast<i64>(hitch.frame)},
        {"frame_ms", hitch.frameMs},
        {"time", hitch.time},
        {"stack_samples", static_cast<i64>(hitch.stackSamples)},
        {"top_stack_frames", Value(std::move(frames))}});
}

[[nodiscard]] std::optional<f64> OptionalNumber(
    const Value& params,
    const char* key)
{
    if (!params.IsObject())
    {
        return std::nullopt;
    }
    const auto found = params.AsObject().find(key);
    if (found == params.AsObject().end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    if (!found->second.IsNumber())
    {
        throw rpc::Error(
            -32602, std::string(key) + " must be a number.");
    }
    return found->second.AsNumber();
}
} // namespace

void RegisterProfilerRpc(rpc::Dispatcher& dispatcher)
{
    dispatcher.Register(
        {
            .name = "profiler.status",
            .description =
                "CPU micro-profiler state: configuration, frame-time summary "
                "over the last 240 frames, the last 120 frame times, and the "
                "recent hitch captures (each with the sampled stack frames "
                "that dominated the stall). Hitch files are Chrome/Perfetto "
                "traces with one lane per thread and one per CPU core.",
            .mutating = false
        },
        [](const Value&)
        {
            const auto summary = profiler::Frames();
            Value::Array recent;
            for (const f32 ms : profiler::RecentFrameMilliseconds(120U))
            {
                recent.emplace_back(static_cast<f64>(ms));
            }
            Value::Array hitches;
            for (const auto& hitch : profiler::RecentHitches())
            {
                hitches.push_back(HitchToValue(hitch));
            }
            return Value(Value::Object{
                {"config", ConfigToValue(profiler::CurrentConfig())},
                {"frames", static_cast<i64>(summary.frames)},
                {"last_ms", summary.lastMs},
                {"average_ms", summary.averageMs},
                {"worst_ms", summary.worstMs},
                {"hitch_count", static_cast<i64>(summary.hitches)},
                {"recent_frame_ms", Value(std::move(recent))},
                {"hitches", Value(std::move(hitches))}});
        });

    dispatcher.Register(
        {
            .name = "profiler.configure",
            .description =
                "Changes the micro-profiler: enabled, hitch_threshold_ms "
                "(frames longer than this write a capture), "
                "stall_threshold_ms (stack sampling starts past this), "
                "capture_window_ms (history kept per capture), "
                "max_hitch_files. Omitted fields keep their value. Returns "
                "the new configuration.",
            .mutating = true
        },
        [](const Value& params)
        {
            auto config = profiler::CurrentConfig();
            if (params.IsObject())
            {
                const auto& values = params.AsObject();
                if (const auto found = values.find("enabled");
                    found != values.end())
                {
                    if (!found->second.IsBool())
                    {
                        throw rpc::Error(
                            -32602, "enabled must be a boolean.");
                    }
                    config.enabled = found->second.AsBool();
                }
            }
            if (const auto value = OptionalNumber(params, "hitch_threshold_ms"))
            {
                config.hitchThresholdMs = std::max(*value, 1.0);
            }
            if (const auto value = OptionalNumber(params, "stall_threshold_ms"))
            {
                config.stallThresholdMs = std::max(*value, 10.0);
            }
            if (const auto value = OptionalNumber(params, "capture_window_ms"))
            {
                config.captureWindowMs = std::clamp(*value, 100.0, 30000.0);
            }
            if (const auto value = OptionalNumber(params, "max_hitch_files"))
            {
                config.maxHitchFiles =
                    static_cast<u32>(std::clamp(*value, 1.0, 500.0));
            }
            profiler::Configure(config);
            return ConfigToValue(profiler::CurrentConfig());
        });

    dispatcher.Register(
        {
            .name = "profiler.capture",
            .description =
                "Writes the last window_ms (default: the configured capture "
                "window) of every thread as a Chrome/Perfetto trace and "
                "returns its path, event count and thread count. Open it at "
                "https://ui.perfetto.dev or chrome://tracing. Optional path "
                "overrides the output file.",
            .mutating = true
        },
        [](const Value& params)
        {
            const f64 window =
                OptionalNumber(params, "window_ms")
                    .value_or(profiler::CurrentConfig().captureWindowMs);
            std::filesystem::path path;
            if (params.IsObject())
            {
                const auto found = params.AsObject().find("path");
                if (found != params.AsObject().end() &&
                    found->second.IsString())
                {
                    path = found->second.AsString();
                }
            }
            if (path.empty())
            {
                path = profiler::DefaultCapturePath();
            }
            try
            {
                const auto info = profiler::Capture(path, window);
                return Value(Value::Object{
                    {"path", info.path.generic_string()},
                    {"events", static_cast<i64>(info.events)},
                    {"threads", static_cast<i64>(info.threads)},
                    {"window_ms", info.windowMs}});
            }
            catch (const std::exception& exception)
            {
                throw rpc::Error(kFailed, exception.what());
            }
        });
}
} // namespace orbit::editor_rpc
