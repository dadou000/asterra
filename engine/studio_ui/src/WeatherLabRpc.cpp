#include <orbit/studio_ui/WeatherLabRpc.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;
using namespace weather_lab;

constexpr i64 kInvalid = 1100;
constexpr i64 kBadParams = -32602;

[[nodiscard]] const Value::Object& Params(const Value& params)
{
    static const Value::Object empty;
    return params.IsObject() ? params.AsObject() : empty;
}

[[nodiscard]] std::optional<f64> Number(const Value::Object& o, const char* key)
{
    const auto found = o.find(key);
    if (found == o.end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    if (!found->second.IsNumber() || !std::isfinite(found->second.AsNumber()))
    {
        throw rpc::Error(kBadParams, std::string(key) + " must be a finite number.");
    }
    return found->second.AsNumber();
}

[[nodiscard]] std::optional<bool> Boolean(const Value::Object& o, const char* key)
{
    const auto found = o.find(key);
    if (found == o.end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    if (!found->second.IsBool())
    {
        throw rpc::Error(kBadParams, std::string(key) + " must be a boolean.");
    }
    return found->second.AsBool();
}

[[nodiscard]] std::optional<std::string> Text(const Value::Object& o, const char* key)
{
    const auto found = o.find(key);
    if (found == o.end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    if (!found->second.IsString())
    {
        throw rpc::Error(kBadParams, std::string(key) + " must be a string.");
    }
    return found->second.AsString();
}

[[nodiscard]] std::string OneOf(
    const Value::Object& o,
    const char* key,
    std::initializer_list<const char*> allowed,
    const std::string& fallback)
{
    const auto value = Text(o, key);
    if (!value.has_value())
    {
        return fallback;
    }
    for (const char* option : allowed)
    {
        if (*value == option)
        {
            return *value;
        }
    }
    std::string list;
    for (const char* option : allowed)
    {
        list += (list.empty() ? "'" : ", '") + std::string(option) + "'";
    }
    throw rpc::Error(kBadParams, std::string(key) + " must be one of " + list + ".");
}

[[nodiscard]] std::string FieldName(
    const Value::Object& o, const char* key, const std::string& fallback)
{
    const auto value = Text(o, key);
    if (!value.has_value())
    {
        return fallback;
    }
    const auto& names = SliceFieldNames();
    if (std::find(names.begin(), names.end(), *value) == names.end())
    {
        std::string list;
        for (const auto& n : names)
        {
            list += (list.empty() ? "'" : ", '") + n + "'";
        }
        throw rpc::Error(kBadParams, std::string(key) + " must be one of " + list + ".");
    }
    return *value;
}

[[nodiscard]] u32 PositiveInteger(const f64 value, const char* key)
{
    if (value < 1.0 || value > 4096.0 || value != std::floor(value))
    {
        throw rpc::Error(kBadParams, std::string(key) + " must be an integer in 1..4096.");
    }
    return static_cast<u32>(value);
}

[[nodiscard]] Value MetricsToValue(const StormMetrics& m)
{
    return Value(Value::Object{
        {"time_s", static_cast<f64>(m.time)},
        {"max_updraft_ms", static_cast<f64>(m.maxUpdraft)},
        {"max_updraft_height_m", static_cast<f64>(m.maxUpdraftHeight)},
        {"max_downdraft_ms", static_cast<f64>(m.maxDowndraft)},
        {"max_rain_g_kg", static_cast<f64>(m.maxRainMixing)},
        {"cloud_top_m", static_cast<f64>(m.cloudTop)},
        {"max_low_vorticity_per_s", static_cast<f64>(m.maxLowVorticity)},
        {"updraft_helicity_m2_s2", static_cast<f64>(m.maxUpdraftHelicity)},
        {"cold_pool_deficit_k", static_cast<f64>(m.coldPoolDeficit)},
        {"rain_area_km2", static_cast<f64>(m.rainArea)}});
}

[[nodiscard]] Value SettingsToValue(const WeatherLabSettings& s)
{
    const FastStormConfig& c = s.solver;
    return Value(Value::Object{
        {"nx", static_cast<i64>(c.nx)},
        {"ny", static_cast<i64>(c.ny)},
        {"nz", static_cast<i64>(c.nz)},
        {"dx", static_cast<f64>(c.dx)},
        {"dz", static_cast<f64>(c.dz)},
        {"bubble_k", static_cast<f64>(c.bubble.amplitude)},
        {"max_time_step", static_cast<f64>(c.maxTimeStep)},
        {"max_courant", static_cast<f64>(c.maxCourant)},
        {"advection", c.advection == AdvectionScheme::MonotoneCubic ? "cubic" : "linear"},
        {"moisture", c.moisture},
        {"mass_fixer", c.massFixer},
        {"horizontal_mixing", static_cast<f64>(c.horizontalMixing)},
        {"vertical_mixing", static_cast<f64>(c.verticalMixing)},
        {"threads", static_cast<i64>(c.threads)},
        {"target_minutes", s.targetMinutes},
        {"frame_interval_s", s.frameIntervalSeconds},
        {"speed_limit", s.speedLimit},
        {"record_path", s.recordPath.string()}});
}

[[nodiscard]] Value StatusToValue(
    const WeatherLabStatus& s, const WeatherLabSettings& settings)
{
    const FastStormDiagnostics& d = s.diagnostics;
    const f64 drift = d.initialTotalWater > 0.0
        ? (d.totalWater - d.initialTotalWater) / d.initialTotalWater : 0.0;
    return Value(Value::Object{
        {"state", SessionStateName(s.state)},
        {"error", s.error},
        {"sim_time_s", s.simTime},
        {"target_s", s.targetSeconds},
        {"steps", static_cast<i64>(s.steps)},
        {"compute_wall_s", s.wallSeconds},
        {"realtime_ratio", s.realTimeRatio},
        {"resident_mib", static_cast<f64>(s.residentBytes) / (1024.0 * 1024.0)},
        {"live_frames", static_cast<i64>(s.liveFrames)},
        {"recording", s.recording},
        {"diagnostics", Value(Value::Object{
            {"max_updraft_ms", static_cast<f64>(d.maxUpdraft)},
            {"max_downdraft_ms", static_cast<f64>(d.maxDowndraft)},
            {"max_horizontal_speed_ms", static_cast<f64>(d.maxHorizontalSpeed)},
            {"max_divergence_per_s", static_cast<f64>(d.maxDivergence)},
            {"last_time_step_s", static_cast<f64>(d.lastTimeStep)},
            {"total_water_drift", drift},
            {"surface_rain_kg", d.surfaceRain}})},
        {"timings_ms", Value(Value::Object{
            {"advect", d.timings.advectMs},
            {"microphysics", d.timings.microphysicsMs},
            {"forcing", d.timings.forcingMs},
            {"projection", d.timings.projectionMs},
            {"step_total", d.timings.totalMs}})},
        {"settings", SettingsToValue(settings)},
        {"playback", Value(Value::Object{
            {"loaded", s.hasPlayback},
            {"path", s.playbackPath},
            {"source", s.playbackSource},
            {"frames", static_cast<i64>(s.playbackFrames)},
            {"frame", static_cast<i64>(s.playbackFrame)},
            {"time_s", s.playbackTime}})}});
}

[[nodiscard]] Value ViewToValue(const WeatherLabView& v)
{
    return Value(Value::Object{
        {"source", v.source == DisplaySource::Live ? "live" : "playback"},
        {"plan_field", v.planField},
        {"plan_height_m", static_cast<f64>(v.planHeightMeters)},
        {"section_field", v.sectionField},
        {"section_row", static_cast<i64>(v.sectionRow)},
        {"column_field", v.columnField},
        {"show_metrics", v.showMetrics},
        {"show_comparison", v.showComparison}});
}

[[nodiscard]] Value PlaybackToValue(
    const WeatherLabStatus& s, const std::size_t metricRows)
{
    return Value(Value::Object{
        {"loaded", s.hasPlayback},
        {"path", s.playbackPath},
        {"source", s.playbackSource},
        {"frames", static_cast<i64>(s.playbackFrames)},
        {"frame", static_cast<i64>(s.playbackFrame)},
        {"time_s", s.playbackTime},
        {"metric_rows", static_cast<i64>(metricRows)}});
}

void ThrowIfError(const std::string& error)
{
    if (!error.empty())
    {
        throw rpc::Error(kInvalid, error);
    }
}
} // namespace

void RegisterWeatherLabRpc(
    rpc::Dispatcher& dispatcher,
    WeatherLabSession& session,
    WeatherLabView& view)
{
    dispatcher.Register(
        {
            .name = "weather_lab.status",
            .description =
                "State of the Weather Lab (SC-01 storm experiment): state "
                "(idle, paused, running, finished, failed), simulated time vs "
                "target, steps, realtime_ratio (simulated seconds per compute "
                "second), solver diagnostics (max updraft/downdraft, divergence "
                "residual, total-water drift), per-stage step timings, resident "
                "memory, the settings and the loaded playback reference.",
            .mutating = false
        },
        [&session](const Value&)
        {
            return StatusToValue(session.Status(), session.Settings());
        });

    dispatcher.Register(
        {
            .name = "weather_lab.configure",
            .description =
                "Sets the fast-core experiment (only while no live run exists; "
                "Reset first). All fields optional: nx, ny (powers of two), nz, "
                "dx (m, cells are square), dz (m), bubble_k (warm bubble, K), "
                "max_time_step (s), max_courant, advection ('cubic' | "
                "'linear'), moisture, mass_fixer, horizontal_mixing and "
                "vertical_mixing (m2/s), threads (0 = all), target_minutes, "
                "frame_interval_s (metric/record cadence), speed_limit "
                "(simulated s per wall s, 0 = unlimited) and record_path (an "
                ".orbitwx file to write, '' to stop recording). Returns the "
                "settings.",
            .mutating = true
        },
        [&session](const Value& params)
        {
            const auto& o = Params(params);
            WeatherLabSettings s = session.Settings();
            FastStormConfig& c = s.solver;
            if (const auto v = Number(o, "nx")) { c.nx = PositiveInteger(*v, "nx"); }
            if (const auto v = Number(o, "ny")) { c.ny = PositiveInteger(*v, "ny"); }
            if (const auto v = Number(o, "nz")) { c.nz = PositiveInteger(*v, "nz"); }
            if (const auto v = Number(o, "dx")) { c.dx = static_cast<f32>(*v); c.dy = c.dx; }
            if (const auto v = Number(o, "dz")) { c.dz = static_cast<f32>(*v); }
            if (const auto v = Number(o, "bubble_k")) { c.bubble.amplitude = static_cast<f32>(*v); }
            if (const auto v = Number(o, "max_time_step")) { c.maxTimeStep = static_cast<f32>(*v); }
            if (const auto v = Number(o, "max_courant")) { c.maxCourant = static_cast<f32>(*v); }
            if (const auto v = Number(o, "horizontal_mixing")) { c.horizontalMixing = static_cast<f32>(*v); }
            if (const auto v = Number(o, "vertical_mixing")) { c.verticalMixing = static_cast<f32>(*v); }
            if (const auto v = Number(o, "threads"))
            {
                if (*v < 0.0 || *v > 256.0 || *v != std::floor(*v))
                {
                    throw rpc::Error(kBadParams, "threads must be an integer in 0..256.");
                }
                c.threads = static_cast<u32>(*v);
            }
            if (const auto v = Boolean(o, "moisture")) { c.moisture = *v; }
            if (const auto v = Boolean(o, "mass_fixer")) { c.massFixer = *v; }
            c.advection = OneOf(o, "advection", {"cubic", "linear"},
                c.advection == AdvectionScheme::MonotoneCubic ? "cubic" : "linear") == "cubic"
                ? AdvectionScheme::MonotoneCubic : AdvectionScheme::Linear;
            if (const auto v = Number(o, "target_minutes")) { s.targetMinutes = *v; }
            if (const auto v = Number(o, "frame_interval_s")) { s.frameIntervalSeconds = *v; }
            if (const auto v = Number(o, "speed_limit")) { s.speedLimit = *v; }
            if (const auto v = Text(o, "record_path")) { s.recordPath = *v; }
            ThrowIfError(session.Configure(s));
            return SettingsToValue(session.Settings());
        });

    dispatcher.Register(
        {
            .name = "weather_lab.control",
            .description =
                "Drives the live run like the panel's buttons. action: 'start' "
                "(create the solver if needed and run/resume), 'pause', "
                "'reset' (discard the live run and its metrics), 'step' "
                "(advance `seconds` of simulated time then pause; returns "
                "immediately, poll weather_lab.status) or 'speed' (set `limit`, "
                "simulated seconds per wall second, 0 = unlimited). Returns the "
                "status.",
            .mutating = true
        },
        [&session](const Value& params)
        {
            const auto& o = Params(params);
            const std::string action = OneOf(
                o, "action", {"start", "pause", "reset", "step", "speed"}, "");
            if (action.empty())
            {
                throw rpc::Error(kBadParams,
                    "action must be 'start', 'pause', 'reset', 'step' or 'speed'.");
            }
            if (action == "start") { ThrowIfError(session.Start()); }
            else if (action == "pause") { session.Pause(); }
            else if (action == "reset") { session.Reset(); }
            else if (action == "step")
            {
                const auto seconds = Number(o, "seconds");
                if (!seconds.has_value())
                {
                    throw rpc::Error(kBadParams, "step needs seconds.");
                }
                ThrowIfError(session.Step(*seconds));
            }
            else
            {
                const auto limit = Number(o, "limit");
                if (!limit.has_value() || *limit < 0.0)
                {
                    throw rpc::Error(kBadParams, "speed needs limit >= 0.");
                }
                session.SetSpeedLimit(*limit);
            }
            return StatusToValue(session.Status(), session.Settings());
        });

    dispatcher.Register(
        {
            .name = "weather_lab.playback",
            .description =
                "Loads and steps through a reference run (.orbitwx from "
                "tools/weather_lab/cm1_lab.py export or from a fast-core "
                "recording). action: 'load' (path; computes its metrics), "
                "'select' (frame index shown by source='playback' slices) or "
                "'clear'. Returns the playback state.",
            .mutating = true
        },
        [&session](const Value& params)
        {
            const auto& o = Params(params);
            const std::string action = OneOf(o, "action", {"load", "select", "clear"}, "");
            if (action.empty())
            {
                throw rpc::Error(kBadParams, "action must be 'load', 'select' or 'clear'.");
            }
            if (action == "load")
            {
                const auto path = Text(o, "path");
                if (!path.has_value() || path->empty())
                {
                    throw rpc::Error(kBadParams, "load needs path.");
                }
                ThrowIfError(session.LoadPlayback(*path));
            }
            else if (action == "select")
            {
                const auto frame = Number(o, "frame");
                if (!frame.has_value() || *frame < 0.0 || *frame != std::floor(*frame))
                {
                    throw rpc::Error(kBadParams, "select needs a frame index >= 0.");
                }
                ThrowIfError(session.SelectPlaybackFrame(static_cast<std::size_t>(*frame)));
            }
            else
            {
                session.ClearPlayback();
            }
            return PlaybackToValue(session.Status(), session.PlaybackMetrics().size());
        });

    dispatcher.Register(
        {
            .name = "weather_lab.metrics",
            .description =
                "Storm metrics per sampled frame (time, max updraft and its "
                "height, max downdraft, max rain mixing ratio, cloud top, max "
                "vorticity below 1 km, 2-5 km updraft helicity, cold-pool "
                "deficit, rain area). source: 'live' (default) or 'playback'.",
            .mutating = false
        },
        [&session](const Value& params)
        {
            const std::string source = OneOf(Params(params), "source", {"live", "playback"}, "live");
            const auto metrics = source == "live" ? session.LiveMetrics() : session.PlaybackMetrics();
            Value::Array rows;
            for (const StormMetrics& m : metrics)
            {
                rows.push_back(MetricsToValue(m));
            }
            return Value(Value::Object{{"source", source}, {"rows", Value(std::move(rows))}});
        });

    dispatcher.Register(
        {
            .name = "weather_lab.compare",
            .description =
                "Playback reference vs live run, paired by time (within 1 s): "
                "one row per reference frame that the live run has also "
                "sampled, each with 'reference' and 'live' metrics.",
            .mutating = false
        },
        [&session](const Value&)
        {
            Value::Array rows;
            for (const ComparisonRow& r : session.Compare())
            {
                rows.push_back(Value(Value::Object{
                    {"time_s", static_cast<f64>(r.time)},
                    {"reference", MetricsToValue(r.reference)},
                    {"live", MetricsToValue(r.live)}}));
            }
            return Value(Value::Object{{"rows", Value(std::move(rows))}});
        });

    dispatcher.Register(
        {
            .name = "weather_lab.slice",
            .description =
                "A 2-D field slice of the live run or the playback frame, as "
                "the panel draws it. kind: 'plan' (map at height_m), "
                "'section' (x-z cut at y row `row`, -1 = through the strongest "
                "updraft) or 'column_max'. field: w, qr, qc, qv (g/kg), thp "
                "(theta perturbation, K), zvort, speed, condensate. Returns "
                "size, min/max, the peak cell and time; pass include_values "
                "for the row-major grid (row 0 is y=0 or z=0), max_cells to "
                "decimate each axis to at most that many cells.",
            .mutating = false
        },
        [&session](const Value& params)
        {
            const auto& o = Params(params);
            SliceRequest request;
            request.source = OneOf(o, "source", {"live", "playback"}, "live") == "live"
                ? DisplaySource::Live : DisplaySource::Playback;
            const std::string kind = OneOf(o, "kind", {"plan", "section", "column_max"}, "plan");
            request.kind = kind == "plan" ? SliceKind::Plan
                : kind == "section" ? SliceKind::Section : SliceKind::ColumnMax;
            request.field = FieldName(o, "field", "w");
            if (const auto v = Number(o, "height_m")) { request.height = static_cast<f32>(*v); }
            if (const auto v = Number(o, "row")) { request.row = static_cast<i32>(*v); }
            const bool includeValues = Boolean(o, "include_values").value_or(false);
            const u32 maxCells = Number(o, "max_cells")
                ? PositiveInteger(*Number(o, "max_cells"), "max_cells") : 0U;

            const Slice slice = session.GetSlice(request);
            if (!slice.valid)
            {
                throw rpc::Error(kInvalid, slice.error);
            }
            Value::Object out{
                {"title", slice.title},
                {"field", slice.field},
                {"unit", slice.unit},
                {"kind", kind},
                {"width", static_cast<i64>(slice.width)},
                {"height", static_cast<i64>(slice.height)},
                {"cell_width_m", static_cast<f64>(slice.cellWidthMeters)},
                {"cell_height_m", static_cast<f64>(slice.cellHeightMeters)},
                {"min", static_cast<f64>(slice.minValue)},
                {"max", static_cast<f64>(slice.maxValue)},
                {"peak_column", static_cast<i64>(slice.peakColumn)},
                {"peak_row", static_cast<i64>(slice.peakRow)},
                {"time_s", slice.time}};
            if (includeValues)
            {
                const u32 strideX = maxCells == 0U ? 1U
                    : std::max(1U, (slice.width + maxCells - 1U) / maxCells);
                const u32 strideY = maxCells == 0U ? 1U
                    : std::max(1U, (slice.height + maxCells - 1U) / maxCells);
                Value::Array values;
                u32 outWidth = 0;
                u32 outHeight = 0;
                for (u32 y = 0; y < slice.height; y += strideY)
                {
                    ++outHeight;
                    outWidth = 0;
                    for (u32 x = 0; x < slice.width; x += strideX)
                    {
                        ++outWidth;
                        values.emplace_back(static_cast<f64>(
                            slice.values[static_cast<std::size_t>(y) * slice.width + x]));
                    }
                }
                out.emplace("values_width", static_cast<i64>(outWidth));
                out.emplace("values_height", static_cast<i64>(outHeight));
                out.emplace("values", Value(std::move(values)));
            }
            return Value(std::move(out));
        });

    dispatcher.Register(
        {
            .name = "weather_lab.view",
            .description =
                "Gets or sets what the Weather Lab panel shows. Fields (all "
                "optional): source ('live' | 'playback'), plan_field, "
                "plan_height_m, section_field, section_row (-1 = strongest "
                "updraft), column_field, show_metrics, show_comparison. "
                "Returns the view.",
            .mutating = true
        },
        [&view](const Value& params)
        {
            const auto& o = Params(params);
            WeatherLabView next = view;
            next.source = OneOf(o, "source", {"live", "playback"},
                next.source == DisplaySource::Live ? "live" : "playback") == "live"
                ? DisplaySource::Live : DisplaySource::Playback;
            next.planField = FieldName(o, "plan_field", next.planField);
            next.sectionField = FieldName(o, "section_field", next.sectionField);
            next.columnField = FieldName(o, "column_field", next.columnField);
            if (const auto v = Number(o, "plan_height_m")) { next.planHeightMeters = static_cast<f32>(*v); }
            if (const auto v = Number(o, "section_row")) { next.sectionRow = static_cast<i32>(*v); }
            if (const auto v = Boolean(o, "show_metrics")) { next.showMetrics = *v; }
            if (const auto v = Boolean(o, "show_comparison")) { next.showComparison = *v; }
            view = std::move(next);
            return ViewToValue(view);
        });
}
} // namespace orbit::studio_ui
