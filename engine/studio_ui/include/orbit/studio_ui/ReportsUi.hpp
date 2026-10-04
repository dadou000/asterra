#pragma once

#include <orbit/editor_ui/EditorUi.hpp>
#include <orbit/rpc/JsonRpc.hpp>
#include <orbit/studio_reports/IssueReport.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace orbit::studio_ui
{
class ReportThumbnailCache;

// The headless operations behind the Reports panel: capture a snapshot of the
// situation Studio is in, raise and update reports, and put Studio back in the
// situation a report was captured in. The panel and the reports.* RPC / MCP
// methods both go through this one object.
//
// A snapshot is gathered by calling the same RPC methods an agent would
// (time.get, project.info, world.active, studio.workspace_get, viewport.get,
// view.text_diagnostics, viewport.pose_get, profiler.status), so everything
// reachable that way is recorded and nothing here duplicates engine state.
class ReportsController
{
public:
    ReportsController(
        studio_reports::ReportStore& store,
        rpc::Dispatcher& dispatcher,
        std::string viewId = "studio.primary");

    [[nodiscard]] studio_reports::ReportStore& Store() noexcept
    {
        return *store_;
    }
    [[nodiscard]] const studio_reports::ReportStore& Store() const noexcept
    {
        return *store_;
    }

    // Snapshot of the situation right now: simulation time and rate, project,
    // world, workspace, camera, target body, location, view settings and
    // frame-time statistics. Sections that cannot be read hold {"error": ...}.
    [[nodiscard]] studio_reports::ReportCondition CaptureCondition() const;

    // Writes a screenshot of the primary viewport to the given file (PNG);
    // false when it cannot be taken right now.
    using ScreenshotHook = std::function<bool(const std::filesystem::path&)>;
    void SetScreenshotHook(ScreenshotHook hook);

    // Takes a screenshot of the viewport as it is now and attaches it to a
    // report's starting or ending condition (replacing the earlier one), saved
    // next to the reports file as Screenshots/<label>-start|end.png. A report
    // then shows the problem at a glance, with no test to launch. False when no
    // screenshot could be taken (the error is in ScreenshotError()).
    bool CaptureScreenshot(u64 id, bool endCondition);

    // Shows a file in the platform file browser.
    using OpenHook = std::function<void(const std::filesystem::path&)>;
    void SetOpenHook(OpenHook hook);
    // Shows a condition's screenshot file in the file browser. Throws when the
    // report has none or the file is gone.
    std::filesystem::path OpenScreenshot(u64 id, bool endCondition);
    [[nodiscard]] const std::string& ScreenshotError() const noexcept
    {
        return screenshotError_;
    }

    // New report whose starting condition (and screenshot) is captured now.
    [[nodiscard]] u64 CreateReport(
        std::string title,
        u32 scopes,
        bool transient);

    void CaptureStart(u64 id, bool screenshot = true);
    // Throws for a report that is not transient.
    void CaptureEnd(u64 id, bool screenshot = true);

    struct RestoreResult
    {
        bool timeRestored{false};
        // "restored", "pending" (waiting for the target body to load, then
        // finished by Tick), "unavailable" (no pose in the snapshot) or
        // "failed".
        std::string pose{"unavailable"};
        std::string message;
    };

    // Puts the simulation clock (paused, at the captured time and rate) and
    // the camera back where the condition was captured.
    [[nodiscard]] RestoreResult Restore(u64 id, bool endCondition);

    // Per frame: finishes a camera restore that was waiting for its target
    // body.
    void Tick();

    [[nodiscard]] const std::string& RestoreStatus() const noexcept
    {
        return restoreStatus_;
    }

    // One-paragraph description of a condition for lists and tooltips.
    [[nodiscard]] static std::string Summarize(
        const studio_reports::ReportCondition& condition);

private:
    [[nodiscard]] rpc::Value Call(
        std::string_view method,
        rpc::Value params = rpc::Value(rpc::Value::Object{})) const;
    [[nodiscard]] bool TryRestorePose(const rpc::Value& pose);
    [[nodiscard]] const studio_reports::ReportCondition& RequireCondition(
        u64 id,
        bool endCondition) const;

    struct PendingPose
    {
        rpc::Value pose;
        u32 framesLeft{0};
    };

    ScreenshotHook screenshotHook_;
    OpenHook openHook_;
    std::string screenshotError_;
    studio_reports::ReportStore* store_{nullptr};
    rpc::Dispatcher* dispatcher_{nullptr};
    std::string viewId_;
    std::optional<PendingPose> pending_;
    std::string restoreStatus_;
};

// The Reports panel (View menu > Reports): list, filter and edit reports,
// capture the starting and ending condition with a button each, and restore
// either one. Every control has a reports.* RPC / MCP equivalent
// (RegisterReportsRpc).
class ReportsUi
{
public:
    explicit ReportsUi(ReportsController& controller) noexcept;

    void Register(editor_ui::EditorUi& ui);

    // Lets the panel show each condition's screenshot.
    void SetThumbnails(ReportThumbnailCache* thumbnails) noexcept
    {
        thumbnails_ = thumbnails;
    }

    // Raises a new report from the current situation and opens the panel on
    // it.
    void NewReportAndShow();

    // Opens the panel on an existing report. False when there is no such report.
    bool Show(u64 id);

    inline static constexpr editor_ui::PanelId kPanelId{
        .high = 0x4f5242495452504fULL,
        .low = 0x5254534953535545ULL
    };

private:
    void Draw(editor_ui::PanelContext& context);
    void DrawList(editor_ui::PanelContext& context);
    void DrawDetail(editor_ui::PanelContext& context);
    void LoadEditBuffers();

    void DrawScreenshot(
        editor_ui::PanelContext& context,
        u64 id,
        bool endCondition,
        const studio_reports::ReportCondition& condition);

    ReportsController* controller_{nullptr};
    editor_ui::EditorUi* ui_{nullptr};
    ReportThumbnailCache* thumbnails_{nullptr};

    u64 selected_{0};
    i32 statusFilter_{0};
    i32 scopeFilter_{0};
    std::string textFilter_;

    // Text being edited; reloaded when the selection or the store changes
    // from somewhere else (an RPC call).
    u64 editId_{0};
    u64 editRevision_{0};
    std::string title_;
    std::string description_;
    std::string resolution_;
    std::string tags_;

    std::string status_;
    bool statusError_{false};
};

// reports.show: opens the panel on a report (the click on its row in the list).
void RegisterReportsUiRpc(rpc::Dispatcher& dispatcher, ReportsUi& ui);

// reports.list / .get / .create / .update / .capture / .restore / .delete /
// .export: the RPC face of the panel.
void RegisterReportsRpc(
    rpc::Dispatcher& dispatcher,
    ReportsController& controller);
} // namespace orbit::studio_ui
