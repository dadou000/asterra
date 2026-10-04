#include <orbit/studio_ui/ReportsUi.hpp>

#include <orbit/studio_ui/ReportThumbnailCache.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orbit::studio_ui
{
namespace
{
using rpc::Value;
using studio_reports::IssueReport;
using studio_reports::ReportCondition;
using studio_reports::ReportScope;
using studio_reports::ReportStatus;

constexpr i64 kFailed = 1100;
constexpr u32 kPoseRestoreFrames = 240U;

[[nodiscard]] const Value* Child(const Value* parent, const char* key)
{
    return parent != nullptr && parent->IsObject() ? parent->Find(key) : nullptr;
}

[[nodiscard]] bool HasError(const Value* section)
{
    return Child(section, "error") != nullptr;
}

[[nodiscard]] std::optional<f64> NumberAt(const Value* parent, const char* key)
{
    const Value* found = Child(parent, key);
    if (found != nullptr && found->IsNumber())
    {
        return found->AsNumber();
    }
    return std::nullopt;
}

[[nodiscard]] std::string StringAt(const Value* parent, const char* key)
{
    const Value* found = Child(parent, key);
    return found != nullptr && found->IsString() ? found->AsString()
                                                 : std::string{};
}

[[nodiscard]] std::vector<std::string> SplitTags(const std::string& text)
{
    std::vector<std::string> tags;
    std::string current;
    const auto flush = [&tags, &current]()
    {
        const auto first = current.find_first_not_of(" \t");
        if (first != std::string::npos)
        {
            const auto last = current.find_last_not_of(" \t");
            tags.push_back(current.substr(first, last - first + 1U));
        }
        current.clear();
    };
    for (const char c : text)
    {
        if (c == ',')
        {
            flush();
        }
        else
        {
            current += c;
        }
    }
    flush();
    return tags;
}

[[nodiscard]] std::string JoinTags(const std::vector<std::string>& tags)
{
    std::string joined;
    for (std::size_t index = 0; index < tags.size(); ++index)
    {
        joined += (index == 0 ? "" : ", ") + tags[index];
    }
    return joined;
}

constexpr std::array<ReportScope, 5> kScopeOrder{
    ReportScope::Performance,
    ReportScope::VisualQuality,
    ReportScope::Bug,
    ReportScope::Crash,
    ReportScope::Other};

constexpr std::array<std::string_view, 5> kScopeLabels{
    "Performance issue",
    "Visual quality problem",
    "Bug",
    "Crash",
    "Other"};

constexpr std::array<std::string_view, 4> kStatusFilterLabels{
    "Any status", "Unresolved", "Pending", "Resolved"};

constexpr std::array<std::string_view, 6> kScopeFilterLabels{
    "Any scope",
    "Performance issue",
    "Visual quality problem",
    "Bug",
    "Crash",
    "Other"};

constexpr std::array<std::string_view, 3> kStatusLabels{
    "Unresolved", "Pending", "Resolved"};

[[nodiscard]] i32 StatusIndex(const ReportStatus status) noexcept
{
    return static_cast<i32>(status);
}

[[nodiscard]] ReportStatus StatusFromIndex(const i32 index) noexcept
{
    return static_cast<ReportStatus>(std::clamp(index, 0, 2));
}

[[nodiscard]] std::string ScopeText(const u32 scopes)
{
    std::string text;
    for (std::size_t index = 0; index < kScopeOrder.size(); ++index)
    {
        if ((scopes & studio_reports::ScopeBit(kScopeOrder[index])) != 0U)
        {
            text += (text.empty() ? "" : " / ");
            text += kScopeLabels[index];
        }
    }
    return text.empty() ? "no scope" : text;
}
} // namespace

ReportsController::ReportsController(
    studio_reports::ReportStore& store,
    rpc::Dispatcher& dispatcher,
    std::string viewId)
    : store_(&store),
      dispatcher_(&dispatcher),
      viewId_(std::move(viewId))
{
}

Value ReportsController::Call(const std::string_view method, Value params) const
{
    const Value request(Value::Object{
        {"jsonrpc", "2.0"},
        {"id", static_cast<i64>(1)},
        {"method", std::string(method)},
        {"params", std::move(params)}});

    const auto reply = dispatcher_->Dispatch(rpc::Serialize(request));
    if (!reply.has_value())
    {
        throw std::runtime_error(std::string(method) + " gave no reply.");
    }

    const Value parsed = rpc::ParseValue(*reply);
    if (const Value* error = parsed.Find("error"))
    {
        std::string message = StringAt(error, "message");
        throw std::runtime_error(
            message.empty() ? std::string(method) + " failed." : message);
    }
    const Value* result = parsed.Find("result");
    return result != nullptr ? *result : Value();
}

ReportCondition ReportsController::CaptureCondition() const
{
    Value::Object state;
    const auto section =
        [this, &state](const char* key, const std::string_view method, Value params)
    {
        try
        {
            state[key] = Call(method, std::move(params));
        }
        catch (const std::exception& exception)
        {
            state[key] = Value(Value::Object{{"error", exception.what()}});
        }
    };

    const auto viewParams = [this]()
    {
        return Value(Value::Object{{"id", viewId_}});
    };

    section("simulation", "time.get", Value(Value::Object{}));
    section("project", "project.info", Value(Value::Object{}));
    section("world", "world.active", Value(Value::Object{}));
    section("workspace", "studio.workspace_get", Value(Value::Object{}));
    section("selection", "selection.get", Value(Value::Object{}));
    section("camera", "viewport.get", Value(Value::Object{}));
    section("pose", "viewport.pose_get", viewParams());
    section("view_text", "view.text_diagnostics", viewParams());
    section("surface_debug", "view.surface_debug_get", viewParams());
    section("performance", "profiler.status", Value(Value::Object{}));
    state["build"] = std::string(__DATE__) + " " + __TIME__;

    return {
        .capturedAt = studio_reports::NowIsoUtc(),
        .state = Value(std::move(state))};
}

void ReportsController::SetScreenshotHook(ScreenshotHook hook)
{
    screenshotHook_ = std::move(hook);
}

bool ReportsController::CaptureScreenshot(const u64 id, const bool endCondition)
{
    const IssueReport* report = store_->Find(id);
    if (report == nullptr)
    {
        throw std::out_of_range(std::format("No report with id {}.", id));
    }
    if (!screenshotHook_ || store_->Path().empty())
    {
        screenshotError_ = "Screenshots are not available (no viewport or no report file).";
        return false;
    }

    const std::string relative = std::format(
        "Screenshots/{}-{}.png", report->Label(), endCondition ? "end" : "start");
    try
    {
        if (!screenshotHook_(store_->AssetPath(relative)))
        {
            screenshotError_ = "The viewport could not be captured right now.";
            return false;
        }
        store_->SetScreenshot(id, endCondition, relative);
        screenshotError_.clear();
        return true;
    }
    catch (const std::exception& exception)
    {
        screenshotError_ = exception.what();
        return false;
    }
}

void ReportsController::SetOpenHook(OpenHook hook)
{
    openHook_ = std::move(hook);
}

std::filesystem::path ReportsController::OpenScreenshot(
    const u64 id,
    const bool endCondition)
{
    const ReportCondition& condition = RequireCondition(id, endCondition);
    if (condition.screenshot.empty())
    {
        throw std::invalid_argument("That condition has no screenshot.");
    }
    if (!openHook_)
    {
        throw std::logic_error("No file browser is attached.");
    }
    const std::filesystem::path file = store_->AssetPath(condition.screenshot);
    std::error_code error;
    if (file.empty() || !std::filesystem::exists(file, error))
    {
        throw std::runtime_error("The screenshot file is missing: " + condition.screenshot);
    }
    openHook_(file);
    return file;
}

u64 ReportsController::CreateReport(
    std::string title,
    const u32 scopes,
    const bool transient)
{
    const u64 id = store_->Create(
        std::move(title), scopes, transient, CaptureCondition());
    CaptureScreenshot(id, false);
    return id;
}

void ReportsController::CaptureStart(const u64 id, const bool screenshot)
{
    store_->CaptureStart(id, CaptureCondition());
    if (screenshot)
    {
        CaptureScreenshot(id, false);
    }
}

void ReportsController::CaptureEnd(const u64 id, const bool screenshot)
{
    store_->CaptureEnd(id, CaptureCondition());
    if (screenshot)
    {
        CaptureScreenshot(id, true);
    }
}

const ReportCondition& ReportsController::RequireCondition(
    const u64 id,
    const bool endCondition) const
{
    const IssueReport* report = store_->Find(id);
    if (report == nullptr)
    {
        throw std::out_of_range(std::format("No report with id {}.", id));
    }
    const auto& condition = endCondition ? report->end : report->start;
    if (!condition.has_value())
    {
        throw std::invalid_argument(std::format(
            "{} has no {} condition captured.",
            report->Label(),
            endCondition ? "ending" : "starting"));
    }
    return *condition;
}

bool ReportsController::TryRestorePose(const Value& pose)
{
    Value::Object params = pose.AsObject();
    params["id"] = viewId_;
    const Value result = Call("viewport.pose_set", Value(std::move(params)));
    const Value* restored = result.Find("restored");
    return restored != nullptr && restored->IsBool() && restored->AsBool();
}

ReportsController::RestoreResult ReportsController::Restore(
    const u64 id,
    const bool endCondition)
{
    const ReportCondition& condition = RequireCondition(id, endCondition);
    const Value& state = condition.state;
    RestoreResult result;
    pending_.reset();

    if (const Value* simulation = state.Find("simulation");
        simulation != nullptr && !HasError(simulation))
    {
        Value::Object params{{"playing", false}};
        if (const auto time = NumberAt(simulation, "time_microseconds"))
        {
            params["time_microseconds"] = *time;
        }
        if (const auto rate = NumberAt(simulation, "rate"))
        {
            params["rate"] = *rate;
        }
        try
        {
            static_cast<void>(Call("time.set", Value(std::move(params))));
            result.timeRestored = true;
        }
        catch (const std::exception& exception)
        {
            result.message += std::string("Time not restored: ") +
                exception.what() + ". ";
        }
    }

    const Value* pose = state.Find("pose");
    if (pose == nullptr || HasError(pose) || !pose->IsObject())
    {
        result.pose = "unavailable";
        restoreStatus_ = result.message + "No camera pose in this snapshot.";
        result.message = restoreStatus_;
        return result;
    }

    const std::string desiredTarget = StringAt(pose, "target_object");
    const Value* desiredTerrain = Child(pose, "terrain");
    const bool wantTerrain = desiredTerrain != nullptr &&
        desiredTerrain->IsBool() && desiredTerrain->AsBool();

    bool onTarget = false;
    try
    {
        const Value current = Call(
            "viewport.pose_get", Value(Value::Object{{"id", viewId_}}));
        const Value* terrain = current.Find("terrain");
        onTarget = StringAt(&current, "target_object") == desiredTarget &&
            terrain != nullptr && terrain->IsBool() &&
            terrain->AsBool() == wantTerrain;
    }
    catch (const std::exception&)
    {
        onTarget = false;
    }

    if (onTarget)
    {
        try
        {
            result.pose = TryRestorePose(*pose) ? "restored" : "failed";
        }
        catch (const std::exception& exception)
        {
            result.pose = "failed";
            result.message += exception.what();
        }
    }
    else
    {
        try
        {
            static_cast<void>(Call(
                "selection.set",
                Value(Value::Object{
                    {"ids", Value(Value::Array{Value(desiredTarget)})}})));
            pending_ = PendingPose{
                .pose = *pose,
                .framesLeft = kPoseRestoreFrames};
            result.pose = "pending";
        }
        catch (const std::exception& exception)
        {
            result.pose = "failed";
            result.message += std::string("Cannot select the captured body: ") +
                exception.what();
        }
    }

    restoreStatus_ = std::format(
        "{}Time {}; camera {}.",
        result.message.empty() ? "" : result.message + " ",
        result.timeRestored ? "restored (paused)" : "not restored",
        result.pose);
    result.message = restoreStatus_;
    return result;
}

void ReportsController::Tick()
{
    if (!pending_.has_value())
    {
        return;
    }

    bool ready = false;
    try
    {
        const Value current = Call(
            "viewport.pose_get", Value(Value::Object{{"id", viewId_}}));
        const Value* wantTerrain = Child(&pending_->pose, "terrain");
        const Value* haveTerrain = current.Find("terrain");
        ready =
            StringAt(&current, "target_object") ==
                StringAt(&pending_->pose, "target_object") &&
            haveTerrain != nullptr && wantTerrain != nullptr &&
            haveTerrain->IsBool() && wantTerrain->IsBool() &&
            haveTerrain->AsBool() == wantTerrain->AsBool();
    }
    catch (const std::exception&)
    {
        ready = false;
    }

    const bool timedOut = pending_->framesLeft == 0U;
    if (!ready && !timedOut)
    {
        --pending_->framesLeft;
        return;
    }

    bool restored = false;
    try
    {
        restored = TryRestorePose(pending_->pose);
    }
    catch (const std::exception&)
    {
        restored = false;
    }
    pending_.reset();
    restoreStatus_ = restored
        ? "Camera restored."
        : "Camera could not be restored: the captured body did not become "
          "the viewport target.";
}

std::string ReportsController::Summarize(const ReportCondition& condition)
{
    const Value& state = condition.state;
    std::string text;

    if (const Value* simulation = state.Find("simulation");
        simulation != nullptr && !HasError(simulation))
    {
        const Value* playing = Child(simulation, "playing");
        const bool isPlaying =
            playing != nullptr && playing->IsBool() && playing->AsBool();
        text += std::format(
            "Simulation {} ({}",
            StringAt(simulation, "time_text"),
            isPlaying ? "playing" : "paused");
        if (const auto rate = NumberAt(simulation, "rate"))
        {
            text += std::format(", x{:g}", *rate);
        }
        text += ")\n";
    }

    if (const Value* view = state.Find("view_text");
        view != nullptr && !HasError(view))
    {
        text += std::format(
            "View {} ({})", StringAt(view, "view_mode"), StringAt(view, "navigation"));
        if (const Value* camera = Child(view, "camera"))
        {
            if (const auto heading = NumberAt(camera, "heading_degrees"))
            {
                text += std::format(", heading {:.0f}", *heading);
            }
            if (const auto pitch = NumberAt(camera, "pitch_degrees"))
            {
                text += std::format(", pitch {:+.0f}", *pitch);
            }
        }
        if (const auto height = NumberAt(view, "height_above_terrain_meters"))
        {
            text += std::format(", {:.1f} m above terrain", *height);
        }
        text += "\n";
        if (const Value* below = Child(view, "below_camera"))
        {
            const auto latitude = NumberAt(below, "latitude_degrees");
            const auto longitude = NumberAt(below, "longitude_degrees");
            if (latitude.has_value() && longitude.has_value())
            {
                text += std::format(
                    "Over lat {:.4f}, lon {:.4f}\n", *latitude, *longitude);
            }
        }
    }

    if (const Value* world = state.Find("world");
        world != nullptr && !HasError(world))
    {
        const std::string path = StringAt(world, "path");
        if (!path.empty())
        {
            text += "World " + path + "\n";
        }
    }

    if (text.empty())
    {
        return "(nothing readable was captured)";
    }
    text.pop_back();
    return text;
}

ReportsUi::ReportsUi(ReportsController& controller) noexcept
    : controller_(&controller)
{
}

void ReportsUi::Register(editor_ui::EditorUi& ui)
{
    ui_ = &ui;
    ui.RegisterPanel({
        .id = kPanelId,
        .title = "Reports",
        .defaultOpen = false,
        .defaultDock = editor_ui::DockRegion::Bottom,
        .dockOrder = 60,
        .minSize = {.width = 420.0F, .height = 260.0F},
        .defaultSize = {.width = 900.0F, .height = 560.0F},
        .draw =
            [this](editor_ui::PanelContext& context)
            {
                Draw(context);
            }});
}

void ReportsUi::NewReportAndShow()
{
    try
    {
        selected_ = controller_->CreateReport(
            "New report",
            studio_reports::ScopeBit(ReportScope::Bug),
            false);
        status_ = "Report raised with the current situation.";
        statusError_ = false;
    }
    catch (const std::exception& exception)
    {
        status_ = exception.what();
        statusError_ = true;
    }
    if (ui_ != nullptr)
    {
        static_cast<void>(ui_->SetPanelOpen(kPanelId, true));
        static_cast<void>(ui_->FocusPanel(kPanelId));
    }
}

bool ReportsUi::Show(const u64 id)
{
    if (controller_->Store().Find(id) == nullptr)
    {
        return false;
    }
    selected_ = id;
    if (ui_ != nullptr)
    {
        static_cast<void>(ui_->SetPanelOpen(kPanelId, true));
        static_cast<void>(ui_->FocusPanel(kPanelId));
    }
    return true;
}

void ReportsUi::DrawScreenshot(
    editor_ui::PanelContext& context,
    const u64 id,
    const bool endCondition,
    const ReportCondition& condition)
{
    if (condition.screenshot.empty())
    {
        context.MutedText("No screenshot.");
        return;
    }
    // The file itself, in the file browser (for sharing, or a bigger look).
    if (context.Button("Show file##shot-" + condition.screenshot))
    {
        try
        {
            static_cast<void>(controller_->OpenScreenshot(id, endCondition));
            status_ = "Showing " + condition.screenshot;
            statusError_ = false;
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
            statusError_ = true;
        }
    }
    context.SameLine();
    context.MutedText(condition.screenshot);
    if (thumbnails_ == nullptr)
    {
        return;
    }

    const auto thumbnail = thumbnails_->Get(
        controller_->Store().AssetPath(condition.screenshot));
    if (thumbnail.texture == nullptr)
    {
        context.MutedText("Screenshot missing: " + condition.screenshot);
        return;
    }

    // Fit the panel width without upscaling.
    const f32 available = std::max(context.ContentAvailable().width, 64.0F);
    const f32 scale = std::min(
        1.0F, available / static_cast<f32>(thumbnail.width));
    static_cast<void>(context.Image(
        *thumbnail.texture,
        {.width = static_cast<f32>(thumbnail.width) * scale,
         .height = static_cast<f32>(thumbnail.height) * scale}));
}

void ReportsUi::LoadEditBuffers()
{
    const IssueReport* report = controller_->Store().Find(selected_);
    editId_ = selected_;
    editRevision_ = controller_->Store().Revision();
    if (report == nullptr)
    {
        title_.clear();
        description_.clear();
        resolution_.clear();
        tags_.clear();
        return;
    }
    title_ = report->title;
    description_ = report->description;
    resolution_ = report->resolutionNote;
    tags_ = JoinTags(report->tags);
}

void ReportsUi::Draw(editor_ui::PanelContext& context)
{
    controller_->Tick();
    DrawList(context);
    context.Separator();
    DrawDetail(context);
}

void ReportsUi::DrawList(editor_ui::PanelContext& context)
{
    auto& store = controller_->Store();

    if (context.PrimaryButton("New report##reports-new"))
    {
        NewReportAndShow();
    }
    context.SameLine();
    static_cast<void>(context.Combo(
        "##reports-status-filter", kStatusFilterLabels, statusFilter_));
    context.SameLine();
    static_cast<void>(context.Combo(
        "##reports-scope-filter", kScopeFilterLabels, scopeFilter_));
    context.SameLine();
    static_cast<void>(
        context.InputText("##reports-text-filter", textFilter_));

    std::array<u32, 3> counts{};
    for (const IssueReport& report : store.Reports())
    {
        ++counts[static_cast<std::size_t>(StatusIndex(report.status))];
    }
    context.MutedText(std::format(
        "{} unresolved  -  {} pending  -  {} resolved",
        counts[0],
        counts[1],
        counts[2]));
    if (!store.LastSaveError().empty())
    {
        context.ErrorText("Reports could not be saved: " + store.LastSaveError());
    }

    studio_reports::ReportFilter filter;
    if (statusFilter_ > 0)
    {
        filter.status = StatusFromIndex(statusFilter_ - 1);
    }
    if (scopeFilter_ > 0)
    {
        filter.scopes = studio_reports::ScopeBit(
            kScopeOrder[static_cast<std::size_t>(scopeFilter_ - 1)]);
    }
    filter.text = textFilter_;

    if (context.BeginChild("##reports-list", {.width = 0.0F, .height = 170.0F}, true))
    {
        const auto matches = store.Query(filter);
        if (matches.empty())
        {
            context.MutedText(
                store.Reports().empty()
                    ? "No reports yet. New report captures where you are."
                    : "No report matches the filter.");
        }
        // Newest first.
        for (auto iterator = matches.rbegin(); iterator != matches.rend();
             ++iterator)
        {
            const IssueReport& report = **iterator;
            const std::string label = std::format(
                "{}  [{}]  {}{}  {}##report-row-{}",
                report.Label(),
                studio_reports::StatusName(report.status),
                report.transient ? "(transient) " : "",
                ScopeText(report.scopes),
                report.title,
                report.id);
            if (context.Selectable(label, selected_ == report.id))
            {
                selected_ = report.id;
            }
        }
    }
    context.EndChild();
}

void ReportsUi::DrawDetail(editor_ui::PanelContext& context)
{
    auto& store = controller_->Store();
    const IssueReport* report = store.Find(selected_);
    if (report == nullptr)
    {
        context.MutedText("Select a report to edit it.");
        return;
    }

    if (editId_ != selected_ || editRevision_ != store.Revision())
    {
        LoadEditBuffers();
    }

    const u64 id = report->id;
    const std::string suffix = std::format("##report-{}", id);
    studio_reports::ReportPatch patch;
    bool changed = false;

    context.Heading(report->Label());

    if (context.InputText("Title" + suffix + "-title", title_))
    {
        patch.title = title_;
        changed = true;
    }

    i32 statusIndex = StatusIndex(report->status);
    if (context.SegmentedControl(
            "report-status" + suffix, kStatusLabels, statusIndex))
    {
        patch.status = StatusFromIndex(statusIndex);
        changed = true;
    }

    // Scope: any combination.
    u32 scopes = report->scopes;
    for (std::size_t index = 0; index < kScopeOrder.size(); ++index)
    {
        const u32 bit = studio_reports::ScopeBit(kScopeOrder[index]);
        bool on = (scopes & bit) != 0U;
        if (index != 0)
        {
            context.SameLine();
        }
        if (context.Checkbox(
                std::string(kScopeLabels[index]) + suffix + "-scope-" +
                    std::to_string(index),
                on))
        {
            scopes = on ? (scopes | bit) : (scopes & ~bit);
            patch.scopes = scopes;
            changed = true;
        }
    }

    bool transient = report->transient;
    if (context.Checkbox(
            "Transient: it comes and goes, so it has a start and an end" +
                suffix + "-transient",
            transient))
    {
        patch.transient = transient;
        changed = true;
    }

    if (context.InputText("Tags (comma separated)" + suffix + "-tags", tags_))
    {
        patch.tags = SplitTags(tags_);
        changed = true;
    }

    if (context.InputTextMultiline(
            "Description" + suffix + "-description",
            description_,
            {.width = 0.0F, .height = 90.0F}))
    {
        patch.description = description_;
        changed = true;
    }

    context.Separator();

    // Starting condition. For a persistent problem this is the only one.
    context.Heading(transient ? "Starting condition" : "Condition");
    if (report->start.has_value())
    {
        context.MutedText("Captured " + report->start->capturedAt);
        DrawScreenshot(context, id, false, *report->start);
        context.Text(ReportsController::Summarize(*report->start));
    }
    else
    {
        context.MutedText("Not captured yet.");
    }
    if (context.Button(
            (transient ? "Capture starting condition now" : "Capture condition now") +
            suffix + "-capture-start"))
    {
        try
        {
            controller_->CaptureStart(id);
            status_ = "Condition captured.";
            statusError_ = false;
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
            statusError_ = true;
        }
    }
    if (report->start.has_value())
    {
        context.SameLine();
        if (context.Button("Go to this situation" + suffix + "-restore-start"))
        {
            try
            {
                status_ = controller_->Restore(id, false).message;
                statusError_ = false;
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
                statusError_ = true;
            }
            }
    }

    // Ending condition: transient problems only.
    if (transient)
    {
        context.Separator();
        context.Heading("Ending condition");
        if (report->end.has_value())
        {
            context.MutedText("Captured " + report->end->capturedAt);
            DrawScreenshot(context, id, true, *report->end);
            context.Text(ReportsController::Summarize(*report->end));
        }
        else
        {
            context.MutedText(
                "Not captured yet. Capture it when the problem stops.");
        }
        if (context.Button("Capture ending condition now" + suffix + "-capture-end"))
        {
            try
            {
                controller_->CaptureEnd(id);
                status_ = "Ending condition captured.";
                statusError_ = false;
            }
            catch (const std::exception& exception)
            {
                status_ = exception.what();
                statusError_ = true;
            }
            }
        if (report->end.has_value())
        {
            context.SameLine();
            if (context.Button("Go to this situation" + suffix + "-restore-end"))
            {
                try
                {
                    status_ = controller_->Restore(id, true).message;
                    statusError_ = false;
                }
                catch (const std::exception& exception)
                {
                    status_ = exception.what();
                    statusError_ = true;
                }
                    }
        }
    }

    context.Separator();
    if (context.InputText(
            "Resolution note" + suffix + "-resolution", resolution_))
    {
        patch.resolutionNote = resolution_;
        changed = true;
    }

    bool deleted = false;
    if (context.Button("Export as Markdown" + suffix + "-export"))
    {
        try
        {
            std::filesystem::path path = store.Path().empty()
                ? std::filesystem::temp_directory_path()
                : store.Path().parent_path();
            path /= report->Label() + ".md";
            std::ofstream(path, std::ios::binary | std::ios::trunc)
                << studio_reports::ReportToMarkdown(*report);
            status_ = "Exported to " + path.generic_string();
            statusError_ = false;
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
            statusError_ = true;
        }
    }
    context.SameLine();
    if (context.Button("Delete report" + suffix + "-delete"))
    {
        deleted = true;
    }

    const std::string& restoreStatus = controller_->RestoreStatus();
    if (!restoreStatus.empty())
    {
        context.MutedText(restoreStatus);
    }
    if (!status_.empty())
    {
        if (statusError_)
        {
            context.ErrorText("Error: " + status_);
        }
        else
        {
            context.MutedText(status_);
        }
    }

    // Applied after the widgets so `report` is not used once it may change.
    if (changed)
    {
        try
        {
            store.Update(id, patch);
            // The edit buffers already hold what was typed.
            editRevision_ = store.Revision();
        }
        catch (const std::exception& exception)
        {
            status_ = exception.what();
            statusError_ = true;
        }
    }
    if (deleted)
    {
        static_cast<void>(store.Remove(id));
        selected_ = 0;
        status_ = "Report deleted.";
        statusError_ = false;
    }
}

namespace
{
[[nodiscard]] const Value::Object& RequireObject(const Value& params)
{
    if (!params.IsObject())
    {
        throw rpc::Error(-32602, "Params must be an object.");
    }
    return params.AsObject();
}

[[nodiscard]] std::optional<std::string> OptionalString(
    const Value::Object& object,
    const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    if (!found->second.IsString())
    {
        throw rpc::Error(-32602, std::string(key) + " must be a string.");
    }
    return found->second.AsString();
}

[[nodiscard]] std::optional<bool> OptionalBool(
    const Value::Object& object,
    const char* key)
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

[[nodiscard]] u32 ScopesFromValue(const Value& value, const char* key)
{
    const auto parseOne = [key](const Value& entry)
    {
        const auto scope = entry.IsString()
            ? studio_reports::ParseScope(entry.AsString())
            : std::nullopt;
        if (!scope.has_value())
        {
            throw rpc::Error(
                -32602,
                std::string(key) +
                    " must name scopes: performance, visual_quality, bug, "
                    "crash, other.");
        }
        return studio_reports::ScopeBit(*scope);
    };

    u32 scopes = 0U;
    if (value.IsArray())
    {
        for (const Value& entry : value.AsArray())
        {
            scopes |= parseOne(entry);
        }
    }
    else
    {
        scopes = parseOne(value);
    }
    return scopes;
}

[[nodiscard]] std::optional<u32> OptionalScopes(
    const Value::Object& object,
    const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    return ScopesFromValue(found->second, key);
}

[[nodiscard]] std::optional<ReportStatus> OptionalStatus(
    const Value::Object& object)
{
    const auto text = OptionalString(object, "status");
    if (!text.has_value())
    {
        return std::nullopt;
    }
    const auto status = studio_reports::ParseStatus(*text);
    if (!status.has_value())
    {
        throw rpc::Error(
            -32602, "status must be unresolved, pending or resolved.");
    }
    return status;
}

[[nodiscard]] std::optional<std::vector<std::string>> OptionalTags(
    const Value::Object& object)
{
    const auto found = object.find("tags");
    if (found == object.end() || found->second.IsNull())
    {
        return std::nullopt;
    }
    if (!found->second.IsArray())
    {
        throw rpc::Error(-32602, "tags must be an array of strings.");
    }
    std::vector<std::string> tags;
    for (const Value& tag : found->second.AsArray())
    {
        if (!tag.IsString())
        {
            throw rpc::Error(-32602, "tags must be an array of strings.");
        }
        tags.push_back(tag.AsString());
    }
    return tags;
}

// 7, "7" or "R-0007".
[[nodiscard]] u64 RequireId(const Value::Object& object)
{
    const auto found = object.find("id");
    if (found == object.end())
    {
        throw rpc::Error(-32602, "id is required (7 or \"R-0007\").");
    }
    if (found->second.IsInteger() && found->second.AsInteger() > 0)
    {
        return static_cast<u64>(found->second.AsInteger());
    }
    if (found->second.IsString())
    {
        std::string text = found->second.AsString();
        if (text.starts_with("R-") || text.starts_with("r-"))
        {
            text.erase(0, 2);
        }
        try
        {
            std::size_t used = 0;
            const unsigned long long parsed = std::stoull(text, &used);
            if (used == text.size() && parsed > 0)
            {
                return static_cast<u64>(parsed);
            }
        }
        catch (const std::exception&)
        {
        }
    }
    throw rpc::Error(-32602, "id must be a report number such as 7 or \"R-0007\".");
}

[[nodiscard]] Value SummaryToValue(const IssueReport& report)
{
    Value value = studio_reports::ReportToValue(report);
    Value::Object& object = value.AsObject();
    // Lists stay light: the snapshots are fetched with reports.get.
    const bool hasStart = object.erase("start") > 0U;
    const bool hasEnd = object.erase("end") > 0U;
    object.emplace("has_start", hasStart);
    object.emplace("has_end", hasEnd);
    object.emplace(
        "has_screenshot",
        (report.start.has_value() && !report.start->screenshot.empty()) ||
            (report.end.has_value() && !report.end->screenshot.empty()));
    return value;
}

// The report as JSON, with each screenshot also given as an absolute path
// (screenshot_path) so a caller can open the picture straight away.
[[nodiscard]] Value ReportResult(
    const studio_reports::ReportStore& store,
    const IssueReport& report)
{
    Value value = studio_reports::ReportToValue(report);
    for (const char* key : {"start", "end"})
    {
        const auto found = value.AsObject().find(key);
        if (found == value.AsObject().end() || !found->second.IsObject())
        {
            continue;
        }
        const Value* relative = found->second.Find("screenshot");
        if (relative != nullptr && relative->IsString())
        {
            found->second.AsObject()["screenshot_path"] =
                store.AssetPath(relative->AsString()).generic_string();
        }
    }
    return value;
}

[[nodiscard]] const IssueReport& RequireReport(
    const studio_reports::ReportStore& store,
    const u64 id)
{
    const IssueReport* report = store.Find(id);
    if (report == nullptr)
    {
        throw rpc::Error(kFailed, std::format("No report with id {}.", id));
    }
    return *report;
}

[[nodiscard]] bool EndFlag(const Value::Object& object)
{
    const auto which = OptionalString(object, "which");
    if (!which.has_value() || *which == "start")
    {
        return false;
    }
    if (*which == "end")
    {
        return true;
    }
    throw rpc::Error(-32602, "which must be 'start' or 'end'.");
}

template <typename Fn>
[[nodiscard]] Value Guarded(Fn&& fn)
{
    try
    {
        return fn();
    }
    catch (const rpc::Error&)
    {
        throw;
    }
    catch (const std::exception& exception)
    {
        throw rpc::Error(kFailed, exception.what());
    }
}
} // namespace

void RegisterReportsUiRpc(rpc::Dispatcher& dispatcher, ReportsUi& ui)
{
    dispatcher.Register(
        {
            .name = "reports.show",
            .description =
                "Opens the Reports panel on a report, the same as clicking its "
                "row: its screenshot, conditions and notes are shown. id is 7 "
                "or \"R-0007\".",
            .mutating = true
        },
        [&ui](const Value& params)
        {
            const u64 id = RequireId(RequireObject(params));
            if (!ui.Show(id))
            {
                throw rpc::Error(kFailed, std::format("No report with id {}.", id));
            }
            return Value(Value::Object{{"shown", true}, {"id", static_cast<i64>(id)}});
        });
}

void RegisterReportsRpc(
    rpc::Dispatcher& dispatcher,
    ReportsController& controller)
{
    dispatcher.Register(
        {
            .name = "reports.list",
            .description =
                "Issue reports raised in this project (newest first). "
                "Optional filters: status (unresolved | pending | resolved), "
                "scope (performance | visual_quality | bug | crash | other, "
                "one or an array), transient (bool), text (substring of "
                "title, description or a tag). Each entry has id, label, "
                "title, status, scopes, tags, transient, has_start, has_end "
                "and timestamps; fetch snapshots with reports.get.",
            .mutating = false
        },
        [&controller](const Value& params)
        {
            return Guarded(
                [&]()
                {
                    studio_reports::ReportFilter filter;
                    if (params.IsObject())
                    {
                        const auto& object = params.AsObject();
                        filter.status = OptionalStatus(object);
                        filter.scopes =
                            OptionalScopes(object, "scope").value_or(0U);
                        filter.transient = OptionalBool(object, "transient");
                        filter.text =
                            OptionalString(object, "text").value_or("");
                    }
                    Value::Array reports;
                    const auto matches = controller.Store().Query(filter);
                    for (auto it = matches.rbegin(); it != matches.rend(); ++it)
                    {
                        reports.push_back(SummaryToValue(**it));
                    }
                    return Value(std::move(reports));
                });
        });

    dispatcher.Register(
        {
            .name = "reports.get",
            .description =
                "One report with everything needed to recreate the "
                "situation: the starting condition (and, for a transient "
                "report, the ending condition) each holding the simulation "
                "time/rate, project, world, camera pose, view diagnostics "
                "text and frame-time statistics, plus the screenshot of the "
                "viewport taken with it (screenshot, and screenshot_path as "
                "an absolute file path). id is 7 or \"R-0007\".",
            .mutating = false
        },
        [&controller](const Value& params)
        {
            return Guarded(
                [&]()
                {
                    return ReportResult(controller.Store(), RequireReport(
                        controller.Store(), RequireId(RequireObject(params))));
                });
        });

    dispatcher.Register(
        {
            .name = "reports.create",
            .description =
                "Raises a report. Fields (all optional): title, description, "
                "scopes (array of performance | visual_quality | bug | crash "
                "| other), tags, transient (the problem comes and goes: it "
                "gets a starting and later an ending condition), status, "
                "capture_start (default true: capture the situation now, "
                "with a screenshot of the viewport), screenshot (default "
                "true; false skips the screenshot). Returns the report.",
            .mutating = true
        },
        [&controller](const Value& params)
        {
            return Guarded(
                [&]()
                {
                    const Value::Object empty;
                    const auto& object =
                        params.IsObject() ? params.AsObject() : empty;
                    const bool capture =
                        OptionalBool(object, "capture_start").value_or(true);
                    const u64 id = controller.Store().Create(
                        OptionalString(object, "title").value_or(""),
                        OptionalScopes(object, "scopes").value_or(0U),
                        OptionalBool(object, "transient").value_or(false),
                        capture ? std::optional<studio_reports::ReportCondition>(
                                      controller.CaptureCondition())
                                : std::nullopt);
                    if (capture && OptionalBool(object, "screenshot").value_or(true))
                    {
                        controller.CaptureScreenshot(id, false);
                    }

                    studio_reports::ReportPatch patch;
                    patch.description = OptionalString(object, "description");
                    patch.status = OptionalStatus(object);
                    patch.tags = OptionalTags(object);
                    controller.Store().Update(id, patch);
                    return ReportResult(controller.Store(), RequireReport(controller.Store(), id));
                });
        });

    dispatcher.Register(
        {
            .name = "reports.update",
            .description =
                "Edits a report: title, description, resolution_note, "
                "status (unresolved | pending | resolved), scopes, tags, "
                "transient. Making a report non-transient drops its ending "
                "condition. Returns the report.",
            .mutating = true
        },
        [&controller](const Value& params)
        {
            return Guarded(
                [&]()
                {
                    const auto& object = RequireObject(params);
                    const u64 id = RequireId(object);
                    studio_reports::ReportPatch patch;
                    patch.title = OptionalString(object, "title");
                    patch.description = OptionalString(object, "description");
                    patch.resolutionNote =
                        OptionalString(object, "resolution_note");
                    patch.status = OptionalStatus(object);
                    patch.scopes = OptionalScopes(object, "scopes");
                    patch.tags = OptionalTags(object);
                    patch.transient = OptionalBool(object, "transient");
                    controller.Store().Update(id, patch);
                    return ReportResult(controller.Store(), RequireReport(controller.Store(), id));
                });
        });

    dispatcher.Register(
        {
            .name = "reports.capture",
            .description =
                "Captures the situation Studio is in now into a report: "
                "which = 'start' (default) replaces the starting condition, "
                "'end' sets the ending condition of a transient report (a "
                "non-transient report has none). A screenshot of the viewport "
                "is attached to the condition unless screenshot is false. "
                "Returns the report.",
            .mutating = true
        },
        [&controller](const Value& params)
        {
            return Guarded(
                [&]()
                {
                    const auto& object = RequireObject(params);
                    const u64 id = RequireId(object);
                    const bool screenshot =
                        OptionalBool(object, "screenshot").value_or(true);
                    if (EndFlag(object))
                    {
                        controller.CaptureEnd(id, screenshot);
                    }
                    else
                    {
                        controller.CaptureStart(id, screenshot);
                    }
                    return ReportResult(controller.Store(), RequireReport(controller.Store(), id));
                });
        });

    dispatcher.Register(
        {
            .name = "reports.open_screenshot",
            .description =
                "Shows a condition's screenshot file in the platform file "
                "browser, the same as the Show file button in the Reports "
                "panel. which = 'start' (default) or 'end'. Returns the path.",
            .mutating = true
        },
        [&controller](const Value& params)
        {
            return Guarded(
                [&]()
                {
                    const auto& object = RequireObject(params);
                    const auto file = controller.OpenScreenshot(
                        RequireId(object), EndFlag(object));
                    return Value(Value::Object{{"path", file.generic_string()}});
                });
        });

    dispatcher.Register(
        {
            .name = "reports.restore",
            .description =
                "Recreates the situation a report was captured in: the "
                "simulation clock goes to the captured time and rate (and "
                "pauses), the captured body is selected and the camera "
                "returns to the captured pose. which = 'start' (default) or "
                "'end'. Returns time_restored and pose ('restored', "
                "'pending' while the body loads, 'unavailable' or 'failed').",
            .mutating = true
        },
        [&controller](const Value& params)
        {
            return Guarded(
                [&]()
                {
                    const auto& object = RequireObject(params);
                    const auto result =
                        controller.Restore(RequireId(object), EndFlag(object));
                    return Value(Value::Object{
                        {"time_restored", result.timeRestored},
                        {"pose", result.pose},
                        {"message", result.message}});
                });
        });

    dispatcher.Register(
        {
            .name = "reports.delete",
            .description = "Deletes a report. Ids are never reused.",
            .mutating = true
        },
        [&controller](const Value& params)
        {
            return Guarded(
                [&]()
                {
                    const u64 id = RequireId(RequireObject(params));
                    return Value(Value::Object{
                        {"deleted", controller.Store().Remove(id)}});
                });
        });

    dispatcher.Register(
        {
            .name = "reports.export",
            .description =
                "A Markdown write-up of one report (id) or, without id, of "
                "every report matching the optional status filter. With "
                "path, also writes it to that file. Returns the markdown.",
            .mutating = false
        },
        [&controller](const Value& params)
        {
            return Guarded(
                [&]()
                {
                    const Value::Object empty;
                    const auto& object =
                        params.IsObject() ? params.AsObject() : empty;

                    std::string markdown;
                    if (object.contains("id"))
                    {
                        markdown = studio_reports::ReportToMarkdown(
                            RequireReport(controller.Store(), RequireId(object)));
                    }
                    else
                    {
                        studio_reports::ReportFilter filter;
                        filter.status = OptionalStatus(object);
                        for (const IssueReport* report :
                             controller.Store().Query(filter))
                        {
                            markdown += studio_reports::ReportToMarkdown(*report);
                            markdown += "\n---\n\n";
                        }
                    }

                    Value::Object result{{"markdown", markdown}};
                    if (const auto path = OptionalString(object, "path"))
                    {
                        const std::filesystem::path target(*path);
                        if (target.has_parent_path())
                        {
                            std::filesystem::create_directories(
                                target.parent_path());
                        }
                        std::ofstream stream(
                            target, std::ios::binary | std::ios::trunc);
                        stream << markdown;
                        if (!stream)
                        {
                            throw std::runtime_error(
                                "Cannot write " + target.generic_string());
                        }
                        result.emplace("path", target.generic_string());
                    }
                    return Value(std::move(result));
                });
        });
}
} // namespace orbit::studio_ui
