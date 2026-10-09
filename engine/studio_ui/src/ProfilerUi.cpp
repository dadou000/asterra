#include <orbit/studio_ui/ProfilerUi.hpp>

#include <orbit/math/Vector.hpp>
#include <orbit/profiler/Profiler.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::studio_ui
{
namespace
{
using editor_ui::CanvasInteraction;
using editor_ui::PanelContext;
using editor_ui::UiSize;

constexpr f32 kGutter = 176.0F;
constexpr f32 kRuler = 20.0F;
constexpr f32 kRowHeight = 15.0F;
constexpr f32 kLaneGap = 4.0F;
constexpr f32 kStripHeight = 54.0F;
constexpr std::array<std::string_view, 4> kViewportCaptureResolutions{
    "720p", "1080p", "1440p", "2160p"};
constexpr std::array<std::string_view, 5> kViewportCaptureScenarios{
    "static", "walk_1_94_mps", "surface_200_kmh",
    "flight_2000_mps_5000m", "ground_to_orbit_20s"};
constexpr std::array<std::string_view, 5> kViewportCaptureScenarioLabels{
    "Static", "Walking (1.94 m/s)", "Surface travel (200 km/h)",
    "Flight (2,000 m/s at 5 km)", "Ground to 500 km orbit (20 s)"};

[[nodiscard]] f64 SteadySeconds()
{
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<f64>(Clock::now().time_since_epoch()).count();
}

[[nodiscard]] std::string FormatMs(const f64 ms)
{
    if (ms >= 1000.0)
    {
        return std::format("{:.2f} s", ms / 1000.0);
    }
    if (ms >= 10.0)
    {
        return std::format("{:.1f} ms", ms);
    }
    if (ms >= 1.0)
    {
        return std::format("{:.2f} ms", ms);
    }
    return std::format("{:.0f} us", ms * 1000.0);
}

// A ruler label with just enough digits to tell neighbouring ticks apart: whole
// seconds for coarse steps, otherwise milliseconds since the snapshot start.
[[nodiscard]] std::string FormatRulerTime(const f64 ms, const f64 stepMs)
{
    if (stepMs >= 1000.0)
    {
        return std::format("{:.0f} s", ms / 1000.0);
    }
    const int decimals = stepMs >= 1.0
        ? 0
        : std::clamp(static_cast<int>(std::ceil(-std::log10(stepMs))), 1, 4);
    return std::format("{:.{}f} ms", ms, decimals);
}

[[nodiscard]] math::Float4 HueColor(const char* name, const f32 alpha)
{
    const std::size_t hash = std::hash<std::string_view>{}(name);
    const f32 hue = static_cast<f32>(hash % 360U) / 60.0F;
    constexpr f32 saturation = 0.50F;
    constexpr f32 value = 0.80F;
    const f32 chroma = value * saturation;
    const f32 x = chroma * (1.0F - std::abs(std::fmod(hue, 2.0F) - 1.0F));
    f32 r = 0.0F;
    f32 g = 0.0F;
    f32 b = 0.0F;
    if (hue < 1.0F) { r = chroma; g = x; }
    else if (hue < 2.0F) { r = x; g = chroma; }
    else if (hue < 3.0F) { g = chroma; b = x; }
    else if (hue < 4.0F) { g = x; b = chroma; }
    else if (hue < 5.0F) { r = x; b = chroma; }
    else { r = chroma; b = x; }
    const f32 m = value - chroma;
    return {r + m, g + m, b + m, alpha};
}

[[nodiscard]] f64 NiceStep(const f64 minimum)
{
    const f64 magnitude = std::pow(10.0, std::floor(std::log10(std::max(minimum, 1.0e-6))));
    for (const f64 factor : {1.0, 2.0, 5.0, 10.0})
    {
        if (magnitude * factor >= minimum)
        {
            return magnitude * factor;
        }
    }
    return magnitude * 10.0;
}

[[nodiscard]] std::string SliceTooltip(const ProfilerSliceInfo& info)
{
    return std::format(
        "{}\nthread {}   core {}\nat {}   duration {}\nself {}   {} nested",
        info.name,
        info.lane,
        info.core,
        FormatMs(info.startMs),
        FormatMs(info.durationMs),
        FormatMs(info.selfMs),
        info.children);
}
} // namespace

ProfilerUi::ProfilerUi() = default;

void ProfilerUi::Register(editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanelId,
        .title = "Profiler",
        .defaultOpen = false,
        .defaultDock = editor_ui::DockRegion::Bottom,
        .dockOrder = 50,
        .minSize = {.width = 420.0F, .height = 240.0F},
        .defaultSize = {.width = 1100.0F, .height = 520.0F},
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                Draw(context);
            }});
}

void ProfilerUi::Draw(PanelContext& context)
{
    const f64 now = SteadySeconds();
    model_.Tick(now);

    DrawToolbar(context, now);
    DrawFrameStrip(context, now);
    DrawTimeline(context, now);
    DrawOptions(context);
    DrawDetails(context, now);
    DrawScopes(context);
    DrawStalls(context);
    DrawHitches(context);
}

void ProfilerUi::DrawToolbar(PanelContext& context, const f64 now)
{
    if (model_.Paused())
    {
        if (context.PrimaryButton(
                model_.Loaded() ? "Back to live##profiler-resume"
                                : "Resume live##profiler-resume"))
        {
            model_.SetPaused(false, now);
        }
    }
    else if (context.Button("Pause##profiler-pause"))
    {
        model_.SetPaused(true, now);
    }
    context.SameLine();
    if (context.Button("Reset view##profiler-reset-view"))
    {
        model_.ResetView();
    }
    context.SameLine();
    if (context.Button("Write trace file##profiler-capture"))
    {
        try
        {
            const auto path = profiler::DefaultCapturePath();
            static_cast<void>(
                profiler::Capture(path, model_.Options().windowMs));
            lastCapturePath_ = path.generic_string();
            captureError_.clear();
        }
        catch (const std::exception& exception)
        {
            captureError_ = exception.what();
        }
    }
    ProfilerOptions captureOptions = model_.Options();
    i32 captureScenarioIndex = static_cast<i32>(std::distance(
        kViewportCaptureScenarios.begin(),
        std::find(
            kViewportCaptureScenarios.begin(),
            kViewportCaptureScenarios.end(),
            captureOptions.viewportCaptureScenario)));
    if (context.Combo(
            "Capture workload##profiler-capture-scenario",
            kViewportCaptureScenarioLabels,
            captureScenarioIndex))
    {
        captureOptions.viewportCaptureScenario =
            kViewportCaptureScenarios[static_cast<std::size_t>(captureScenarioIndex)];
        model_.SetOptions(captureOptions);
    }
    if (context.PrimaryButton("Capture viewport only##profiler-viewport-capture") &&
        viewportOnlyCaptureAction_)
    {
        const auto& options = model_.Options();
        if (!viewportOnlyCaptureAction_(
                options.viewportCaptureDurationMs,
                options.viewportCaptureResolution,
                options.viewportCaptureScenario))
        {
            captureError_ = "Capture settings are invalid or this scenario needs a targeted camera.";
        }
        else
        {
            captureError_.clear();
        }
    }

    const auto summary = profiler::Frames();
    const auto& snapshot = model_.Snapshot();
    std::string state = "LIVE";
    if (model_.Loaded())
    {
        state = "INSPECTING " + snapshot.source;
    }
    else if (model_.Paused())
    {
        state = std::format("PAUSED (last {:.1f} s)", model_.SnapshotSpanMs() / 1000.0);
    }
    context.Text(std::format(
        "{}   |   {} avg ({:.0f} fps), worst {}, {} hitches   |   {} slices, {} lanes, {} stack samples",
        state,
        FormatMs(summary.averageMs),
        summary.averageMs > 0.0 ? 1000.0 / summary.averageMs : 0.0,
        FormatMs(summary.worstMs),
        summary.hitches,
        snapshot.sliceCount,
        snapshot.lanes.size(),
        snapshot.stalls.size()));

    if (model_.Loading())
    {
        context.MutedText("Loading trace file...");
    }
    if (!model_.LoadError().empty())
    {
        context.ErrorText("Could not load the trace: " + model_.LoadError());
    }
    if (!captureError_.empty())
    {
        context.ErrorText("Could not write the trace: " + captureError_);
    }
    if (!lastCapturePath_.empty())
    {
        context.MutedText(
            "Wrote " + lastCapturePath_ + " (open it at ui.perfetto.dev).");
    }
}

void ProfilerUi::DrawOptions(PanelContext& context)
{
    if (!context.Section("Options##profiler-options", false))
    {
        return;
    }
    ProfilerOptions options = model_.Options();
    bool changed = false;

    f64 windowSeconds = options.windowMs / 1000.0;
    if (context.SliderDouble("History (s)##profiler-window", windowSeconds, 1.0, 30.0))
    {
        options.windowMs = windowSeconds * 1000.0;
        changed = true;
    }

    f64 captureDurationSeconds = options.viewportCaptureDurationMs / 1000.0;
    if (context.SliderDouble(
            "Viewport capture duration (s)##profiler-capture-duration",
            captureDurationSeconds,
            1.0,
            30.0))
    {
        options.viewportCaptureDurationMs = captureDurationSeconds * 1000.0;
        changed = true;
    }
    i32 captureResolutionIndex = static_cast<i32>(std::distance(
        kViewportCaptureResolutions.begin(),
        std::find(
            kViewportCaptureResolutions.begin(),
            kViewportCaptureResolutions.end(),
            options.viewportCaptureResolution)));
    if (context.Combo(
            "Viewport capture resolution##profiler-capture-resolution",
            kViewportCaptureResolutions,
            captureResolutionIndex))
    {
        options.viewportCaptureResolution =
            kViewportCaptureResolutions[static_cast<std::size_t>(captureResolutionIndex)];
        changed = true;
    }
    static constexpr std::array<std::string_view, 2> kGroupings{
        "By thread", "By core"};
    i32 grouping = options.grouping == ProfilerGrouping::Threads ? 0 : 1;
    if (context.SegmentedControl("profiler-grouping", kGroupings, grouping))
    {
        options.grouping =
            grouping == 0 ? ProfilerGrouping::Threads : ProfilerGrouping::Cores;
        changed = true;
    }
    if (options.grouping == ProfilerGrouping::Cores)
    {
        context.MutedText(
            "Each scope sits on the core it started on. Nested scopes stack downward.");
    }

    if (context.Checkbox("Pause on hitch##profiler-freeze", options.freezeOnHitch))
    {
        changed = true;
    }
    f64 minMicroseconds = options.minSliceMs * 1000.0;
    if (context.SliderDouble(
            "Hide slices shorter than (us)##profiler-min", minMicroseconds, 0.0, 2000.0))
    {
        options.minSliceMs = minMicroseconds / 1000.0;
        changed = true;
    }
    if (context.InputText("Highlight scope##profiler-filter", options.filter))
    {
        changed = true;
    }
    if (changed)
    {
        model_.SetOptions(options);
    }
}

void ProfilerUi::DrawFrameStrip(PanelContext& context, const f64 now)
{
    const f32 width = std::max(context.ContentAvailable().width, 420.0F);
    const auto frames = model_.Frames();
    const f64 span = model_.SnapshotSpanMs();

    const CanvasInteraction in =
        context.Canvas("profiler-frame-strip", UiSize{width, kStripHeight});

    f64 worst = 0.0;
    for (const auto& frame : frames)
    {
        worst = std::max(worst, frame.durationMs);
    }
    const f64 scale = std::max(worst, 20.0);
    const f64 hitchMs = profiler::CurrentConfig().hitchThresholdMs;

    const auto y = [&](const f64 ms)
    {
        return static_cast<f32>(1.0 - std::min(ms / scale, 1.0));
    };
    for (const f64 reference : {16.7, 33.3})
    {
        context.CanvasLine(
            {0.0F, y(reference)},
            {1.0F, y(reference)},
            {0.55F, 0.6F, 0.68F, 0.35F});
    }
    context.CanvasText({0.004F, y(16.7) - 0.02F}, {0.6F, 0.66F, 0.74F, 0.8F}, "16.7 ms");

    std::optional<std::size_t> hovered;
    for (std::size_t i = 0U; i < frames.size(); ++i)
    {
        const auto& frame = frames[i];
        const f32 x0 = static_cast<f32>(frame.startMs / span);
        const f32 x1 = std::max(
            static_cast<f32>((frame.startMs + frame.durationMs) / span),
            x0 + 1.5F / width);
        math::Float4 color{0.36F, 0.72F, 0.46F, 0.95F};
        if (frame.durationMs > hitchMs)
        {
            color = {0.88F, 0.30F, 0.28F, 1.0F};
        }
        else if (frame.durationMs > 20.0)
        {
            color = {0.92F, 0.72F, 0.26F, 1.0F};
        }
        context.CanvasRect({x0, y(frame.durationMs)}, {x1, 1.0F}, color);
        if (in.hovered && in.u >= x0 && in.u <= x1)
        {
            hovered = i;
        }
    }

    // The part of the snapshot the timeline below is showing.
    const f32 v0 = static_cast<f32>((model_.ViewBeginMs() - model_.SnapshotBeginMs()) / span);
    const f32 v1 = v0 + static_cast<f32>(model_.ViewSpanMs() / span);
    context.CanvasRect({v0, 0.0F}, {v1, 1.0F}, {0.45F, 0.65F, 1.0F, 0.18F});
    context.CanvasRect({v0, 0.0F}, {v1, 1.0F}, {0.55F, 0.75F, 1.0F, 0.9F}, false);

    if (in.hovered && hovered.has_value())
    {
        context.CanvasTooltip(std::format(
            "Frame took {}. Click to zoom to it.", FormatMs(frames[*hovered].durationMs)));
    }
    if (in.clicked && hovered.has_value())
    {
        model_.ZoomToFrame(*hovered, now);
    }
    else if (in.dragging && in.leftDown)
    {
        const f64 center = model_.SnapshotBeginMs() + static_cast<f64>(in.u) * span;
        model_.SetView(center - model_.ViewSpanMs() * 0.5, model_.ViewSpanMs(), now);
    }
}

void ProfilerUi::DrawTimeline(PanelContext& context, const f64 now)
{
    const auto& rows = model_.Rows();
    if (rows.empty())
    {
        context.MutedText("No profiler data yet.");
        return;
    }

    // Follow the font: rows must be at least one text line tall or the lane
    // names spill into the next lane.
    const f32 rowHeight = std::max(kRowHeight, context.TextHeightPixels() + 2.0F);
    const f32 rulerHeight = std::max(kRuler, context.TextHeightPixels() + 6.0F);
    const f32 width = std::max(context.ContentAvailable().width, 420.0F);
    const f32 plotWidth = width - kGutter - 4.0F;

    std::vector<f32> laneTop;
    laneTop.reserve(rows.size());
    f32 cursor = rulerHeight;
    for (const auto& row : rows)
    {
        laneTop.push_back(cursor);
        cursor += static_cast<f32>(row.rowCount) * rowHeight + kLaneGap;
    }
    const f32 height = cursor + 2.0F;

    const CanvasInteraction in =
        context.Canvas("profiler-timeline", UiSize{width, height});

    // Zoom, pan and click-vs-drag are resolved before drawing so the frame shows
    // the new view.
    if (in.clicked)
    {
        pressTravelPixels_ = 0.0F;
    }
    if (in.hovered && in.wheel != 0.0F && in.pixelX >= kGutter)
    {
        const f64 anchor = model_.ViewBeginMs() +
            static_cast<f64>(in.pixelX - kGutter) / plotWidth * model_.ViewSpanMs();
        model_.ZoomAt(anchor, std::pow(0.8, static_cast<f64>(in.wheel)), now);
    }
    if (in.dragDeltaX != 0.0F || in.dragDeltaY != 0.0F)
    {
        pressTravelPixels_ += std::abs(in.dragDeltaX) + std::abs(in.dragDeltaY);
        if (in.dragDeltaX != 0.0F && pressTravelPixels_ > 3.0F)
        {
            model_.Pan(
                -static_cast<f64>(in.dragDeltaX) / plotWidth * model_.ViewSpanMs(), now);
        }
    }

    const f64 begin = model_.ViewBeginMs();
    const f64 span = model_.ViewSpanMs();
    const f64 snapshotBegin = model_.SnapshotBeginMs();
    const auto xOf = [&](const f64 t)
    {
        return kGutter + static_cast<f32>((t - begin) / span) * plotWidth;
    };
    const auto nx = [&](const f32 x) { return x / width; };
    const auto ny = [&](const f32 y) { return y / height; };
    const auto rect = [&](const f32 x0, const f32 y0, const f32 x1, const f32 y1,
                          const math::Float4 color, const bool filled = true,
                          const f32 thickness = 1.0F)
    {
        context.CanvasRect({nx(x0), ny(y0)}, {nx(x1), ny(y1)}, color, filled, thickness);
    };

    // Lane stripes and names.
    for (std::size_t r = 0U; r < rows.size(); ++r)
    {
        const f32 top = laneTop[r];
        const f32 bottom = top + static_cast<f32>(rows[r].rowCount) * rowHeight;
        rect(0.0F, top - 1.0F, width, bottom + 1.0F,
             r % 2U == 0U ? math::Float4{0.10F, 0.12F, 0.15F, 1.0F}
                          : math::Float4{0.07F, 0.085F, 0.11F, 1.0F});
        context.CanvasTextClipped(
            {nx(6.0F), ny(top)},
            nx(kGutter - 4.0F),
            rows[r].synthetic ? math::Float4{0.62F, 0.78F, 1.0F, 1.0F}
                              : math::Float4{0.86F, 0.88F, 0.92F, 1.0F},
            rows[r].name);
    }
    rect(kGutter - 2.0F, 0.0F, kGutter - 1.0F, height, {0.25F, 0.3F, 0.36F, 1.0F});

    // Ruler with grid lines.
    {
        const f64 step = NiceStep(90.0 / (plotWidth / span));
        const f64 first = std::ceil((begin - snapshotBegin) / step) * step + snapshotBegin;
        for (f64 t = first; t <= begin + span; t += step)
        {
            const f32 x = xOf(t);
            rect(x, rulerHeight - 4.0F, x + 1.0F, height, {0.5F, 0.55F, 0.62F, 0.16F});
            context.CanvasTextClipped(
                {nx(x + 3.0F), ny(2.0F)},
                1.0F,
                {0.72F, 0.76F, 0.82F, 1.0F},
                FormatRulerTime(t - snapshotBegin, step));
        }
    }

    // Slices.
    std::optional<ProfilerSliceRef> hovered;
    const bool filtering = !model_.Options().filter.empty();
    const std::optional<ProfilerSliceRef> selected = model_.Selected();
    for (std::size_t r = 0U; r < rows.size(); ++r)
    {
        std::array<f32, 16> lastEnd{};
        lastEnd.fill(-1.0e9F);
        const auto& slices = rows[r].slices;
        for (std::size_t i = 0U; i < slices.size(); ++i)
        {
            const auto& slice = slices[i];
            if (!model_.SliceVisible(slice))
            {
                continue;
            }
            const f32 x0 = std::max(xOf(slice.startMs), kGutter);
            f32 x1 = std::min(xOf(slice.startMs + slice.durationMs), width - 2.0F);
            if (x1 < kGutter || x0 > width)
            {
                continue;
            }
            const std::size_t sub = std::min<std::size_t>(slice.row, 15U);
            if (x1 - x0 < 1.0F)
            {
                if (x0 < lastEnd[sub])
                {
                    continue;
                }
                x1 = x0 + 1.0F;
            }
            lastEnd[sub] = x1;

            const f32 top = laneTop[r] + static_cast<f32>(sub) * rowHeight;
            const f32 bottom = top + rowHeight - 1.0F;
            const bool matches = !filtering || model_.SliceMatchesFilter(slice);
            rect(x0, top, x1, bottom, HueColor(slice.name, matches ? 1.0F : 0.22F));

            const bool isSelected =
                selected.has_value() && selected->row == r && selected->index == i;
            if (isSelected)
            {
                rect(x0, top, x1, bottom, {1.0F, 1.0F, 1.0F, 1.0F}, false, 2.0F);
            }
            if (in.hovered && in.pixelX >= x0 && in.pixelX <= x1 &&
                in.pixelY >= top && in.pixelY <= bottom)
            {
                hovered = ProfilerSliceRef{.row = r, .index = i};
            }
            if (x1 - x0 >= 34.0F && matches)
            {
                std::string label = slice.name;
                if (x1 - x0 >= 130.0F)
                {
                    label += "  " + FormatMs(slice.durationMs);
                }
                context.CanvasTextClipped(
                    {nx(x0 + 3.0F), ny(top)},
                    nx(x1 - 1.0F),
                    {0.06F, 0.07F, 0.09F, 1.0F},
                    label);
            }
        }
    }

    if (hovered.has_value() && !(selected.has_value() && *selected == *hovered))
    {
        const auto& slice = rows[hovered->row].slices[hovered->index];
        const f32 top = laneTop[hovered->row] +
            static_cast<f32>(std::min<u16>(slice.row, 15U)) * rowHeight;
        rect(std::max(xOf(slice.startMs), kGutter), top,
             std::min(std::max(xOf(slice.startMs + slice.durationMs),
                               xOf(slice.startMs) + 1.0F), width - 2.0F),
             top + rowHeight - 1.0F, {1.0F, 1.0F, 1.0F, 0.9F}, false, 1.0F);
    }

    // Stack samples taken while the main thread was stalled.
    std::optional<std::size_t> hoveredStall;
    const auto& stalls = model_.Snapshot().stalls;
    for (std::size_t i = 0U; i < stalls.size(); ++i)
    {
        const f32 x = xOf(stalls[i].timeMs);
        if (x < kGutter || x > width)
        {
            continue;
        }
        rect(x, 0.0F, x + 1.0F, height, {0.95F, 0.3F, 0.3F, 0.55F});
        rect(x - 2.0F, rulerHeight - 7.0F, x + 3.0F, rulerHeight - 1.0F, {0.95F, 0.3F, 0.3F, 1.0F});
        if (in.hovered && std::abs(in.pixelX - x) <= 3.0F && in.pixelY < rulerHeight + 4.0F)
        {
            hoveredStall = i;
        }
    }

    if (in.hovered)
    {
        if (hoveredStall.has_value())
        {
            const auto& stall = stalls[*hoveredStall];
            std::string text = "Stalled in " + stall.label + "\n";
            for (std::size_t i = 0U; i < stall.stack.size() && i < 10U; ++i)
            {
                text += "  " + stall.stack[i] + "\n";
            }
            context.CanvasTooltip(text);
        }
        else if (hovered.has_value())
        {
            if (const auto info = model_.Describe(*hovered); info.has_value())
            {
                context.CanvasTooltip(SliceTooltip(*info));
            }
        }
    }

    // A press that barely moved is a click; a drag already panned.
    if (in.hovered && in.leftReleased && pressTravelPixels_ < 4.0F && in.pixelX >= kGutter)
    {
        model_.Select(hovered, now);
    }
    if (in.hovered && in.doubleClicked && in.pixelX >= kGutter)
    {
        if (hovered.has_value())
        {
            model_.ZoomToSlice(*hovered, now);
        }
        else
        {
            model_.ResetView();
        }
    }
}

void ProfilerUi::DrawDetails(PanelContext& context, const f64 now)
{
    if (!context.Section("Selected slice##profiler-details", true))
    {
        return;
    }
    const auto& selected = model_.Selected();
    const auto info = selected.has_value()
        ? model_.Describe(*selected)
        : std::optional<ProfilerSliceInfo>{};
    if (!info.has_value())
    {
        context.MutedText(
            "Click a slice to inspect it. Scroll zooms, drag pans, double-click a slice "
            "zooms to it, double-click empty space resets the view. Changing the view "
            "pauses the live capture.");
        return;
    }
    context.KeyValue("Scope", info->name);
    context.KeyValue("Thread", info->lane);
    context.KeyValue("Core", std::to_string(info->core));
    context.KeyValue("Start", FormatMs(info->startMs) + " into the snapshot");
    context.KeyValue("Duration", FormatMs(info->durationMs));
    context.KeyValue(
        "Self time",
        std::format("{} ({} nested scopes)", FormatMs(info->selfMs), info->children));
    if (context.Button("Zoom to slice##profiler-zoom-slice"))
    {
        model_.ZoomToSlice(*selected, now);
    }
    context.SameLine();
    if (context.Button("Highlight this scope##profiler-highlight"))
    {
        ProfilerOptions options = model_.Options();
        options.filter = info->name;
        model_.SetOptions(options);
    }
    context.SameLine();
    if (context.Button("Clear selection##profiler-clear-selection"))
    {
        model_.Select(std::nullopt, now);
    }
}

void ProfilerUi::DrawScopes(PanelContext& context)
{
    if (context.Section("Heaviest scopes in view##profiler-scopes", true))
    {
        context.MutedText(
            "Total time is inclusive and summed across threads. Click one to highlight it.");
        const auto stats = model_.ScopeStats(14U);
        for (std::size_t i = 0U; i < stats.size(); ++i)
        {
            const auto& stat = stats[i];
            const std::string line = std::format(
                "{}   {}x   {} total   {} longest##profiler-scope-{}",
                stat.name,
                stat.count,
                FormatMs(stat.totalMs),
                FormatMs(stat.maxMs),
                i);
            if (context.Selectable(line, model_.Options().filter == stat.name))
            {
                ProfilerOptions options = model_.Options();
                options.filter = options.filter == stat.name ? std::string() : stat.name;
                model_.SetOptions(options);
            }
        }
        if (stats.empty())
        {
            context.MutedText("Nothing recorded in the visible range.");
        }
    }

    if (context.Section("Heaviest GPU passes in view##profiler-gpu", true))
    {
        context.MutedText(
            "GPU time per render pass (from the GPU lanes above). A pass that holds the GPU "
            "for seconds is what the main thread's fence wait is waiting on.");
        const auto gpu = model_.GpuScopeStats(10U);
        for (std::size_t i = 0U; i < gpu.size(); ++i)
        {
            const auto& stat = gpu[i];
            const std::string line = std::format(
                "{}   {}x   {} total   {} longest##profiler-gpu-pass-{}",
                stat.name,
                stat.count,
                FormatMs(stat.totalMs),
                FormatMs(stat.maxMs),
                i);
            if (context.Selectable(line, model_.Options().filter == stat.name))
            {
                ProfilerOptions options = model_.Options();
                options.filter = options.filter == stat.name ? std::string() : stat.name;
                model_.SetOptions(options);
            }
        }
        if (gpu.empty())
        {
            context.MutedText(
                "No GPU pass took long enough to record in the visible range "
                "(passes under 50 us are not shown).");
        }
    }

    if (context.Section("Longest slices in view##profiler-slowest", false))
    {
        const auto refs = model_.SlowestRefs(12U);
        const f64 now = SteadySeconds();
        for (std::size_t i = 0U; i < refs.size(); ++i)
        {
            const auto info = model_.Describe(refs[i]);
            if (!info.has_value())
            {
                continue;
            }
            const std::string line = std::format(
                "{}   {}   on {}   at {}##profiler-slow-{}",
                FormatMs(info->durationMs),
                info->name,
                info->lane,
                FormatMs(info->startMs),
                i);
            if (context.Selectable(line, model_.Selected() == refs[i]))
            {
                model_.Select(refs[i], now);
                model_.ZoomToSlice(refs[i], now);
            }
        }
    }
}

void ProfilerUi::DrawStalls(PanelContext& context)
{
    const auto& stalls = model_.Snapshot().stalls;
    if (stalls.empty())
    {
        return;
    }
    if (!context.Section(
            std::format("Stalled stack samples ({})###profiler-stalls", stalls.size()),
            true))
    {
        return;
    }
    context.MutedText(
        "Taken by the watchdog while the main thread was stuck in one frame. "
        "Red ticks on the timeline mark each sample; hover one for its stack.");
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
    for (std::size_t i = 0U; i < ranked.size() && i < 8U; ++i)
    {
        context.Text(std::format("{}x  {}", ranked[i].first, ranked[i].second));
    }
    const auto& sample = stalls[stalls.size() / 2U];
    context.Separator();
    context.MutedText("Stack of one sample, innermost first:");
    for (std::size_t i = 0U; i < sample.stack.size() && i < 24U; ++i)
    {
        context.MutedText(sample.stack[i]);
    }
}

void ProfilerUi::DrawHitches(PanelContext& context)
{
    const auto hitches = profiler::RecentHitches();
    if (!context.Section(
            std::format("Recorded hitches ({})###profiler-hitches", hitches.size()),
            !hitches.empty()))
    {
        return;
    }
    if (hitches.empty())
    {
        context.MutedText(
            "A frame longer than the hitch threshold (100 ms by default) is written to disk "
            "and listed here.");
        return;
    }
    const std::size_t shown = std::min<std::size_t>(hitches.size(), 12U);
    for (std::size_t n = 0U; n < shown; ++n)
    {
        const auto& hitch = hitches[hitches.size() - 1U - n];
        if (context.Button(std::format("Inspect##profiler-hitch-{}", n)))
        {
            model_.LoadTraceFileAsync(hitch.path);
        }
        context.SameLine();
        context.Text(std::format(
            "frame {}   {}   {}   {} stack samples",
            hitch.frame,
            FormatMs(hitch.frameMs),
            hitch.time,
            hitch.stackSamples));
        if (!hitch.topFrames.empty())
        {
            context.MutedText("    " + hitch.topFrames.front());
        }
    }
}
} // namespace orbit::studio_ui
