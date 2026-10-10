#include <orbit/studio_ui/WeatherLabUi.hpp>

#include <orbit/math/Vector.hpp>
#include <orbit/weather_lab/SliceColor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

namespace orbit::studio_ui
{
namespace
{
using editor_ui::ActionPresentation;
using editor_ui::PanelContext;
using editor_ui::UiSize;
using namespace weather_lab;

// Slices are decimated to at most this many cells per axis before drawing so
// a 256x256 grid does not become 65k rectangles per canvas.
constexpr u32 kMaxDrawCells = 96U;

constexpr std::array<std::string_view, 2> kPresetLabels{
    "Quick: 2 km, 64x64x40 (validated)",
    "Standard: 1 km, 128x128x40 (experimental)"};
constexpr std::array<std::string_view, 2> kSourceLabels{"Live run", "Playback"};
constexpr std::array<std::string_view, 3> kAxisLabels{"X is up", "Y is up", "Z is up"};
constexpr std::array<std::string_view, 5> kGridSizes{"16", "32", "64", "128", "256"};

[[nodiscard]] math::Float4 ToColor(const std::array<f32, 4>& c)
{
    return math::Float4{c[0], c[1], c[2], c[3]};
}

[[nodiscard]] i32 FieldIndex(const std::string& field)
{
    const auto& names = SliceFieldNames();
    const auto it = std::find(names.begin(), names.end(), field);
    return it == names.end() ? 0 : static_cast<i32>(it - names.begin());
}

// Returns true and writes `field` when the user picks a different one.
[[nodiscard]] bool FieldCombo(
    PanelContext& context, std::string_view label, std::string& field)
{
    static const std::vector<std::string_view> views = [] {
        std::vector<std::string_view> v;
        for (const auto& name : SliceFieldNames())
        {
            v.emplace_back(name);
        }
        return v;
    }();
    i32 index = FieldIndex(field);
    if (context.Combo(label, views, index, 140.0F))
    {
        field = std::string(views[static_cast<std::size_t>(index)]);
        return true;
    }
    return false;
}

[[nodiscard]] std::string FormatDuration(const f64 seconds)
{
    const i64 total = static_cast<i64>(seconds + 0.5);
    return std::format("{}:{:02}:{:02}", total / 3600, (total / 60) % 60, total % 60);
}

void ApplyPreset(WeatherLabSettings& s, const i32 preset)
{
    FastStormConfig& c = s.solver;
    if (preset == 0)
    {
        c.nx = c.ny = 64;
        c.nz = 40;
        c.dx = c.dy = 2000.0F;
        c.dz = 500.0F;
        c.maxTimeStep = 24.0F;
        c.maxCourant = 2.5F;
    }
    else
    {
        c.nx = c.ny = 128;
        c.nz = 40;
        c.dx = c.dy = 1000.0F;
        c.dz = 500.0F;
        c.maxTimeStep = 12.0F;
        c.maxCourant = 1.5F;
    }
    c.bubble.amplitude = 1.0F;
}
} // namespace

WeatherLabUi::WeatherLabUi(
    WeatherLabSession& session, WeatherLabView& view, WeatherLabVolumeSink volumeSink)
    : session_(&session)
    , view_(&view)
    , volumeSink_(std::move(volumeSink))
{
}

void WeatherLabUi::Register(editor_ui::EditorUi& ui)
{
    ui.RegisterPanel({
        .id = kPanelId,
        .title = "Weather Lab",
        .defaultOpen = false,
        .defaultDock = editor_ui::DockRegion::Bottom,
        .dockOrder = 60,
        .minSize = {.width = 520.0F, .height = 320.0F},
        .defaultSize = {.width = 1100.0F, .height = 640.0F},
        .draw =
            [this](PanelContext& context)
            {
                Draw(context);
            }});
}

void WeatherLabUi::Draw(PanelContext& context)
{
    // Keep an attached storm volume following the displayed storm. This runs
    // only while the panel is drawn.
    if (view_->volumeAuto && volumeSink_)
    {
        const WeatherLabStatus status = session_->Status();
        const f64 shown = view_->source == weather_lab::DisplaySource::Live
            ? status.simTime : status.playbackTime;
        const u64 key = static_cast<u64>(shown * 1000.0)
            ^ (static_cast<u64>(status.playbackFrame) << 40U)
            ^ (view_->source == weather_lab::DisplaySource::Live ? 1ULL : 2ULL);
        if (key != lastVolumeKey_)
        {
            lastVolumeKey_ = key;
            volumeMessage_ = PushStormVolume(*session_, *view_, volumeSink_);
            if (!volumeMessage_.empty())
            {
                view_->volumeAuto = false;
            }
        }
    }
    if (!draftLoaded_)
    {
        draft_ = session_->Settings();
        draftLoaded_ = true;
    }
    DrawControls(context);
    DrawSettings(context);
    DrawPlayback(context);
    DrawViews(context);
    DrawVolume(context);
    DrawMetrics(context);
}

void WeatherLabUi::DrawControls(PanelContext& context)
{
    const WeatherLabStatus status = session_->Status();
    const bool running = status.state == SessionState::Running;
    const bool terminal = status.state == SessionState::Finished
        || status.state == SessionState::Failed;

    const std::array<ActionPresentation, 4> actions{{
        {.label = running ? "Pause" : (status.state == SessionState::Idle ? "Run" : "Resume"),
         .enabled = !terminal,
         .disabledReason = "The run is " + std::string(SessionStateName(status.state)) + "; Reset to start again.",
         .invoke = [this, running]
         {
             if (running)
             {
                 session_->Pause();
                 message_.clear();
             }
             else
             {
                 message_ = session_->Start();
             }
         }},
        {.label = "Step 60 s",
         .enabled = !running && !terminal,
         .disabledReason = running ? "Pause before stepping." : "Reset to start again.",
         .invoke = [this] { message_ = session_->Step(60.0); }},
        {.label = "Step 5 min",
         .enabled = !running && !terminal,
         .disabledReason = running ? "Pause before stepping." : "Reset to start again.",
         .invoke = [this] { message_ = session_->Step(300.0); }},
        {.label = "Reset",
         .enabled = status.state != SessionState::Idle,
         .disabledReason = "Nothing to reset.",
         .invoke = [this]
         {
             session_->Reset();
             message_.clear();
         }},
    }};
    context.Toolbar(actions);

    context.KeyValue("State", SessionStateName(status.state));
    context.KeyValue(
        "Simulated time",
        std::format("{} of {}", FormatDuration(status.simTime), FormatDuration(status.targetSeconds)));
    context.KeyValue(
        "Speed",
        std::format("{:.0f}x real time ({} steps, {:.1f} MiB resident)",
            status.realTimeRatio, status.steps,
            static_cast<f64>(status.residentBytes) / (1024.0 * 1024.0)));
    if (status.state != SessionState::Idle)
    {
        const auto& d = status.diagnostics;
        context.KeyValue(
            "Storm",
            std::format("updraft {:.1f} m/s, downdraft {:.1f} m/s, divergence {:.1e} /s",
                static_cast<f64>(d.maxUpdraft), static_cast<f64>(d.maxDowndraft),
                static_cast<f64>(d.maxDivergence)));
    }
    if (!status.error.empty())
    {
        context.ErrorText("Run failed: " + status.error);
    }
    if (!message_.empty())
    {
        context.ErrorText(message_);
    }
    context.Separator();
}

void WeatherLabUi::DrawSettings(PanelContext& context)
{
    if (!context.Section("Experiment settings", false))
    {
        return;
    }
    const bool locked = session_->Status().state != SessionState::Idle;
    if (locked)
    {
        context.MutedText("A run exists. Reset to change the experiment.");
    }
    if (context.Combo("Preset##weather-preset", kPresetLabels, presetIndex_, 280.0F) && !locked)
    {
        ApplyPreset(draft_, presetIndex_);
    }
    FastStormConfig& c = draft_.solver;
    i32 gridIndex = 2;
    for (std::size_t i = 0; i < kGridSizes.size(); ++i)
    {
        if (std::to_string(c.nx) == kGridSizes[i])
        {
            gridIndex = static_cast<i32>(i);
        }
    }
    if (context.Combo("Horizontal cells##weather-grid", kGridSizes, gridIndex, 120.0F) && !locked)
    {
        c.nx = c.ny = static_cast<u32>(std::stoul(std::string(kGridSizes[static_cast<std::size_t>(gridIndex)])));
    }
    f64 value = static_cast<f64>(c.dx);
    if (context.InputDouble("Cell size (m)##weather-dx", value, 120.0F) && !locked && value > 0.0)
    {
        c.dx = c.dy = static_cast<f32>(value);
    }
    value = static_cast<f64>(c.dz);
    if (context.InputDouble("Layer thickness (m)##weather-dz", value, 120.0F) && !locked && value > 0.0)
    {
        c.dz = static_cast<f32>(value);
    }
    value = static_cast<f64>(c.bubble.amplitude);
    if (context.InputDouble("Warm bubble (K)##weather-bubble", value, 120.0F) && !locked)
    {
        c.bubble.amplitude = static_cast<f32>(value);
    }
    value = draft_.targetMinutes;
    if (context.InputDouble("Run length (min)##weather-target", value, 120.0F) && !locked && value > 0.0)
    {
        draft_.targetMinutes = value;
    }
    value = draft_.frameIntervalSeconds;
    if (context.InputDouble("Sample every (s)##weather-frame", value, 120.0F) && !locked && value > 0.0)
    {
        draft_.frameIntervalSeconds = value;
    }
    value = draft_.speedLimit;
    if (context.InputDouble("Speed limit (x real time, 0 = max)##weather-speed", value, 120.0F) && value >= 0.0)
    {
        draft_.speedLimit = value;
        session_->SetSpeedLimit(value);
    }
    if (context.Checkbox("Water-mass fixer##weather-fixer", c.massFixer) && locked)
    {
        c.massFixer = session_->Settings().solver.massFixer;
    }
    (void)context.InputText("Record to .orbitwx (optional)##weather-record", recordPathText_);
    if (!locked && context.PrimaryButton("Apply settings##weather-apply"))
    {
        draft_.recordPath = recordPathText_;
        message_ = session_->Configure(draft_);
    }
}

void WeatherLabUi::DrawPlayback(PanelContext& context)
{
    if (!context.Section("Reference (CM1) playback", false))
    {
        return;
    }
    context.MutedText(
        "Load an .orbitwx file exported with tools/weather_lab/cm1_lab.py or recorded from a fast-core run.");
    (void)context.InputText("File##weather-playback-path", playbackPath_);
    if (context.Button("Load##weather-playback-load"))
    {
        message_ = session_->LoadPlayback(playbackPath_);
        if (message_.empty())
        {
            view_->source = DisplaySource::Playback;
        }
    }
    const WeatherLabStatus status = session_->Status();
    if (!status.hasPlayback)
    {
        return;
    }
    context.SameLine();
    if (context.Button("Clear##weather-playback-clear"))
    {
        session_->ClearPlayback();
        view_->source = DisplaySource::Live;
        return;
    }
    context.KeyValue("Loaded", std::format("{} ({}, {} frames)", status.playbackPath, status.playbackSource, status.playbackFrames));
    f64 frame = static_cast<f64>(status.playbackFrame);
    if (status.playbackFrames > 1U
        && context.SliderDouble("Frame##weather-playback-frame", frame, 0.0, static_cast<f64>(status.playbackFrames - 1U)))
    {
        message_ = session_->SelectPlaybackFrame(static_cast<std::size_t>(frame + 0.5));
    }
    context.KeyValue("Frame time", FormatDuration(status.playbackTime));
}

void WeatherLabUi::DrawSlice(
    PanelContext& context, const std::string_view id, const Slice& slice, const UiSize size)
{
    const editor_ui::CanvasInteraction in = context.Canvas(id, size);
    if (!slice.valid)
    {
        context.CanvasText({0.04F, 0.45F}, {0.7F, 0.74F, 0.8F, 1.0F}, slice.error);
        return;
    }
    const u32 strideX = std::max(1U, (slice.width + kMaxDrawCells - 1U) / kMaxDrawCells);
    const u32 strideY = std::max(1U, (slice.height + kMaxDrawCells - 1U) / kMaxDrawCells);
    const f32 w = static_cast<f32>(slice.width);
    const f32 h = static_cast<f32>(slice.height);
    for (u32 y = 0; y < slice.height; y += strideY)
    {
        for (u32 x = 0; x < slice.width; x += strideX)
        {
            const f32 value = slice.values[static_cast<std::size_t>(y) * slice.width + x];
            // Row 0 is south / the ground: draw it at the bottom.
            const f32 top = 1.0F - static_cast<f32>(std::min(y + strideY, slice.height)) / h;
            const f32 bottom = 1.0F - static_cast<f32>(y) / h;
            context.CanvasRect(
                {static_cast<f32>(x) / w, top},
                {static_cast<f32>(std::min(x + strideX, slice.width)) / w, bottom},
                ToColor(SliceColor(slice, value)));
        }
    }
    context.CanvasCircle(
        {(static_cast<f32>(slice.peakColumn) + 0.5F) / w,
         1.0F - (static_cast<f32>(slice.peakRow) + 0.5F) / h},
        4.0F, {1.0F, 1.0F, 1.0F, 0.9F}, false, 1.5F);
    context.CanvasText({0.01F, 0.01F}, {1.0F, 1.0F, 1.0F, 0.95F},
        std::format("{}  [{:.2f} .. {:.2f}] {}", slice.title,
            static_cast<f64>(slice.minValue), static_cast<f64>(slice.maxValue), slice.unit));
    if (in.hovered)
    {
        const u32 x = std::min(static_cast<u32>(in.u * w), slice.width - 1U);
        const u32 row = std::min(static_cast<u32>((1.0F - in.v) * h), slice.height - 1U);
        context.CanvasTooltip(std::format("{:.2f} {}  (cell {}, {})",
            static_cast<f64>(slice.values[static_cast<std::size_t>(row) * slice.width + x]),
            slice.unit, x, row));
    }
}

void WeatherLabUi::DrawViews(PanelContext& context)
{
    i32 source = view_->source == DisplaySource::Live ? 0 : 1;
    if (context.SegmentedControl("weather-source", kSourceLabels, source))
    {
        view_->source = source == 0 ? DisplaySource::Live : DisplaySource::Playback;
    }

    const f32 available = std::max(context.ContentAvailable().width, 360.0F);
    const f32 mapSize = std::min(available * 0.5F - 8.0F, 420.0F);

    SliceRequest plan{.source = view_->source, .kind = SliceKind::Plan,
        .field = view_->planField, .height = view_->planHeightMeters};
    SliceRequest section{.source = view_->source, .kind = SliceKind::Section,
        .field = view_->sectionField, .row = view_->sectionRow};
    SliceRequest column{.source = view_->source, .kind = SliceKind::ColumnMax,
        .field = view_->columnField};

    (void)FieldCombo(context, "Map field##weather-plan-field", view_->planField);
    f64 height = static_cast<f64>(view_->planHeightMeters);
    if (context.SliderDouble("Map height (m)##weather-plan-height", height, 0.0, 20000.0))
    {
        view_->planHeightMeters = static_cast<f32>(height);
    }
    DrawSlice(context, "weather-plan", session_->GetSlice(plan), {mapSize, mapSize});

    (void)FieldCombo(context, "Cross-section field##weather-section-field", view_->sectionField);
    DrawSlice(context, "weather-section", session_->GetSlice(section),
        {std::min(available, 900.0F), std::min(available, 900.0F) * 0.4F});

    (void)FieldCombo(context, "Column maximum field##weather-column-field", view_->columnField);
    DrawSlice(context, "weather-column", session_->GetSlice(column), {mapSize, mapSize});
    context.Separator();
}

void WeatherLabUi::DrawVolume(PanelContext& context)
{
    if (!context.Section("Storm as a Volume object", false))
    {
        return;
    }
    context.MutedText(
        "Shows the storm's cloud and rain water through a Volume object's baked-cache path, separate from the planet clouds. "
        "Create a Volume, set its Representation mode to Baked, size it to the extents shown below, then push.");
    (void)context.InputText("Volume object id##weather-volume-id", view_->volumeId);
    i32 axis = static_cast<i32>(view_->volumeUpAxis);
    if (context.Combo("Volume up axis##weather-volume-axis", kAxisLabels, axis, 120.0F))
    {
        view_->volumeUpAxis = static_cast<weather_lab::VolumeUpAxis>(axis);
    }
    f64 gain = static_cast<f64>(view_->volumeGain);
    if (context.SliderDouble("Density per g/kg##weather-volume-gain", gain, 0.05, 4.0))
    {
        view_->volumeGain = static_cast<f32>(gain);
    }
    if (context.PrimaryButton("Push to Volume##weather-volume-push"))
    {
        volumeMessage_ = PushStormVolume(*session_, *view_, volumeSink_, &volumeResult_);
        volumeHaveResult_ = volumeMessage_.empty();
    }
    context.SameLine();
    if (context.Button("Detach##weather-volume-clear") && volumeSink_ && !view_->volumeId.empty())
    {
        volumeMessage_ = volumeSink_(view_->volumeId, nullptr).error;
        view_->volumeAuto = false;
        volumeHaveResult_ = false;
    }
    (void)context.Checkbox("Keep updating while the panel is open##weather-volume-auto", view_->volumeAuto);
    if (!volumeMessage_.empty())
    {
        context.ErrorText(volumeMessage_);
    }
    if (volumeHaveResult_)
    {
        const auto& r = volumeResult_;
        context.KeyValue("Representation mode",
            r.representationOk ? r.representationMode : r.representationMode + " (set to Baked for the cloud to draw)");
        context.KeyValue("Volume half extents (m)", std::format("{:.0f} x {:.0f} x {:.0f}",
            r.currentHalfExtents[0], r.currentHalfExtents[1], r.currentHalfExtents[2]));
        context.KeyValue("Storm fits at (m)", std::format("{:.0f} x {:.0f} x {:.0f}",
            r.recommendedHalfExtents[0], r.recommendedHalfExtents[1], r.recommendedHalfExtents[2]));
    }
}

void WeatherLabUi::DrawMetrics(PanelContext& context)
{
    (void)context.Checkbox("Show metrics##weather-show-metrics", view_->showMetrics);
    if (!view_->showMetrics)
    {
        return;
    }
    const auto live = session_->LiveMetrics();
    const auto reference = session_->PlaybackMetrics();
    const bool compare = view_->showComparison && !reference.empty() && !live.empty();

    auto line = [&context](const std::string_view label, const StormMetrics& m)
    {
        context.KeyValue(label, std::format(
            "w {:.1f}/{:.1f} m/s, rain {:.1f} g/kg, top {:.1f} km, zeta<1km {:.4f}/s, UH {:.0f}, cold pool {:.1f} K",
            static_cast<f64>(m.maxUpdraft), static_cast<f64>(m.maxDowndraft),
            static_cast<f64>(m.maxRainMixing), static_cast<f64>(m.cloudTop) / 1000.0,
            static_cast<f64>(m.maxLowVorticity), static_cast<f64>(m.maxUpdraftHelicity),
            static_cast<f64>(m.coldPoolDeficit)));
    };
    if (!live.empty())
    {
        line(std::format("Live t={:.0f} min", static_cast<f64>(live.back().time) / 60.0), live.back());
    }
    if (compare)
    {
        const auto rows = session_->Compare();
        if (!rows.empty())
        {
            line(std::format("Reference t={:.0f} min", static_cast<f64>(rows.back().time) / 60.0),
                rows.back().reference);
        }
    }

    // Peak updraft over time: live in orange, reference in cyan.
    const UiSize size{std::max(context.ContentAvailable().width, 360.0F), 110.0F};
    (void)context.Canvas("weather-updraft-history", size);
    f32 maxTime = 1.0F;
    f32 maxW = 10.0F;
    for (const auto* series : {&live, &reference})
    {
        for (const StormMetrics& m : *series)
        {
            maxTime = std::max(maxTime, m.time);
            maxW = std::max(maxW, m.maxUpdraft);
        }
    }
    auto plot = [&](const std::vector<StormMetrics>& series, const math::Float4 color)
    {
        for (std::size_t i = 1; i < series.size(); ++i)
        {
            context.CanvasLine(
                {series[i - 1].time / maxTime, 1.0F - series[i - 1].maxUpdraft / maxW},
                {series[i].time / maxTime, 1.0F - series[i].maxUpdraft / maxW},
                color, 2.0F);
        }
    };
    plot(reference, {0.30F, 0.80F, 0.95F, 1.0F});
    plot(live, {0.98F, 0.62F, 0.22F, 1.0F});
    context.CanvasText({0.01F, 0.02F}, {0.75F, 0.8F, 0.86F, 1.0F},
        std::format("Peak updraft vs time (max {:.0f} m/s, {:.0f} min)",
            static_cast<f64>(maxW), static_cast<f64>(maxTime) / 60.0));
}
} // namespace orbit::studio_ui
