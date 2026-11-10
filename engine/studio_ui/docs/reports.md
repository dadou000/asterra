+++
path = "/editor/studio-ui/reports"
title = "Reports panel, report capture and restore"
kind = "subsystem"
status = "stable"
summary = "The Studio side of issue reports: ReportsController captures a snapshot of the running Studio by calling its own RPC methods, saves a viewport screenshot beside it, and can put clock and camera back; ReportsUi is the Reports panel and ReportThumbnailCache shows the screenshots. Every control has a reports.* RPC method and an orbit_reports_* MCP tool."
owner_module = "OrbitStudioUi"
keywords = ["reports", "issue report", "report issue", "reports.json", "report screenshot", "thumbnail", "reports.restore", "reports.create", "reports.show", "capture condition", "ReportsController", "ReportsUi", "ReportThumbnailCache", "R-0001"]
sources = [
  "engine/studio_ui/include/orbit/studio_ui/ReportsUi.hpp",
  "engine/studio_ui/src/ReportsUi.cpp",
  "engine/studio_ui/include/orbit/studio_ui/ReportThumbnailCache.hpp",
  "engine/studio_ui/src/ReportThumbnailCache.cpp",
  "engine/studio_reports/src/IssueReport.cpp",
  "tools/mcp_server/orbit_editor_mcp_server.py",
]
symbols = ["ReportsController", "ReportsUi", "ReportThumbnailCache", "RegisterReportsRpc", "RegisterReportsUiRpc", "CaptureCondition", "kPoseRestoreFrames", "kMaxEntries", "kRetireFrames"]
invariants = [
  "ReportsController is the only place that captures, restores or screenshots; the Reports panel, the 'Report issue' button of the simulation transport band and the reports.* RPC methods all call it, so a click and an agent do the same thing.",
  "A snapshot is not read from engine state directly: CaptureCondition dispatches these RPC methods through the dispatcher and stores each result under a key - time.get (simulation), project.info (project), world.active (world), studio.workspace_get (workspace), selection.get (selection), viewport.get (camera), viewport.pose_get (pose), view.text_diagnostics (view_text), view.surface_debug_get (surface_debug), profiler.status (performance) - plus a build string. A section that cannot be read holds {\"error\": ...} instead of failing the capture.",
  "A report's screenshot is best effort: CaptureScreenshot returns false and sets ScreenshotError() instead of throwing when there is no hook, no report file or the viewport cannot be captured, and the report is still created. The hook installed in apps/editor/src/Main.cpp refuses while ViewportCaptureService::Active(), because a high-resolution capture has the view resized.",
  "Screenshots are saved as Screenshots/<label>-start.png or -end.png next to the reports file (ReportStore::AssetPath), where <label> is R-NNNN (four digits); the report stores only that relative path, and deleting a report deletes its screenshot files.",
  "The store is opened at <project>/Reports/reports.json in apps/editor/src/Main.cpp; if opening fails (for example a damaged file) a warning is logged, the store stays unopened so the file is never overwritten, and reports are not saved that session.",
  "reports.restore (ReportsController::Restore) sets the clock paused (playing=false) at the captured time_microseconds and rate through time.set. If the viewport is not already on the captured target_object with the same terrain flag it selects that object with selection.set and finishes the camera later: pose is 'pending' and Tick() (called once per frame by Main) retries for up to kPoseRestoreFrames (240) frames before trying anyway.",
  "ReportThumbnailCache keys entries by path and file modification time (a retaken screenshot reloads), shrinks images by an integer box filter to at most kMaxWidth (640) px wide, keeps at most kMaxEntries (24) textures, and keeps a dropped texture alive for kRetireFrames (8) Tick() calls because earlier frames may still draw it. Tick() must be called once per frame.",
  "A file that cannot be read is remembered as failed and is not retried every frame; it is reloaded only when its modification time changes.",
]
related = ["/editor/reports", "/editor/mcp-rpc", "/editor/viewport", "/editor/profiler", "/foundation/time", "/editor/studio-ui/validation-and-capture"]
depends_on = ["/editor/reports", "/foundation/rpc", "/editor/ui-toolkit", "/rendering/render-view"]
used_by = ["/editor/studio-ui"]
verify = [
  "ctest -R Orbit.StudioReports covers the report store (model, persistence); ReportsController and ReportThumbnailCache have no test target, so verify them by hand: reports.create then reports.get and check screenshot_path exists on disk, then reports.restore and read viewport.pose_get.",
  "Over MCP: orbit_reports_create, orbit_reports_get, orbit_reports_restore, orbit_reports_export.",
]
verified = "55d48117"

[routes]
"what a stored report contains or how the file is persisted" = "/editor/reports"
"how to capture a large screenshot or tiled 16K capture" = "/editor/studio-ui/validation-and-capture"
"which numbers the report's view_text section holds" = "/editor/studio-ui/diagnostics-hud"

[[diagnose]]
symptom = "a report has no screenshot, or the panel says the screenshot is missing"
steps = [
  "Call reports.get {id}: screenshot (relative path) and screenshot_path (absolute) tell whether one was attached; an empty value means CaptureScreenshot returned false when the report was raised.",
  "Call viewport.capture_status: a screenshot is refused while a high-resolution capture is active (the hook in apps/editor/src/Main.cpp checks ViewportCaptureService::Active()). Wait for state 'idle', then call reports.capture {id, which: 'start'}.",
  "If screenshot is set but the panel shows 'Screenshot missing', check that the file exists under <project>/Reports/Screenshots/; ReportThumbnailCache::Get returns a null texture for a missing or unreadable file.",
]
docs = ["/editor/reports"]

[[diagnose]]
symptom = "reports.restore leaves the camera or clock somewhere else"
steps = [
  "Read the returned pose: 'restored', 'pending' (waiting for the captured body to become the viewport target), 'unavailable' (the snapshot has no pose section, or it holds an error) or 'failed' (message says why).",
  "For 'pending', poll viewport.pose_get; Tick() gives up waiting after kPoseRestoreFrames (240) frames and tries the pose once, so a body that never loads leaves the camera unchanged.",
  "The clock is always left paused: time_restored true means time.set succeeded; read time.get to confirm time_microseconds and rate.",
]
docs = ["/editor/viewport"]

[[diagnose]]
symptom = "reports are not saved or disappear after restarting"
steps = [
  "Look in the Studio log for 'Reports: cannot open the report file, reports will not be saved'; the store was left unopened on purpose.",
  "The panel shows 'Reports could not be saved: ...' from the store's LastSaveError when a save fails.",
  "Check that <project>/Reports/reports.json exists and parses.",
]
docs = ["/editor/reports"]
+++

## What a report is

A report is an `IssueReport` (model in `engine/studio_reports`, see `/editor/reports`): title, status, scopes, tags,
and one or two captured conditions. This block is the Studio half: how a condition is captured, shown and restored.

## Flow

1. Raise one: the **Report issue** button in the simulation transport band (`SimulationControlsUi::SetReportIssueHandler`
   is wired to `ReportsUi::NewReportAndShow` in `apps/editor/src/Main.cpp`), the Reports panel, `reports.create`, or
   `orbit_reports_create`. The panel path creates a non-transient report titled "New report" with the Bug scope.
2. `ReportsController::CreateReport` stores the report with `CaptureCondition()` and then takes the screenshot.
   `reports.create` does the same by default (`capture_start` and `screenshot` default to true) and then applies
   description, status and tags.
3. A transient report gets an ending condition later with `CaptureEnd` (`reports.capture` with `which: "end"`); a
   non-transient report rejects it.
4. `ReportsUi` shows the list (status, scope and text filters), the detail editor and, under each condition, a thumbnail
   from `ReportThumbnailCache` plus a **Show file** button (`reports.open_screenshot`).

## Where things live

| Concern | Owner |
| --- | --- |
| capture, screenshot, restore, summary text | `ReportsController` (`engine/studio_ui/src/ReportsUi.cpp`) |
| panel (id `ReportsUi::kPanelId`, title "Reports", docked bottom, closed by default) | `ReportsUi` |
| thumbnails | `ReportThumbnailCache`, set on the panel with `SetThumbnails` |
| RPC | `RegisterReportsRpc` (list, get, create, update, capture, open_screenshot, restore, delete, export) and `RegisterReportsUiRpc` (show) |
| wiring, screenshot hook, open hook, per-frame `Tick` calls | `apps/editor/src/Main.cpp` |

## RPC and MCP

| RPC | MCP tool | Notes |
| --- | --- | --- |
| `reports.list` | `orbit_reports_list` | filters: status, scope, transient, text; newest first |
| `reports.get` | `orbit_reports_get` | conditions plus `screenshot` and absolute `screenshot_path` |
| `reports.create` | `orbit_reports_create` | optional title, description, scopes, tags, transient, status, capture_start, screenshot |
| `reports.update` | `orbit_reports_update` | making a report non-transient drops its ending condition |
| `reports.capture` | `orbit_reports_capture` | `which` is `start` (default) or `end` |
| `reports.restore` | `orbit_reports_restore` | returns `time_restored`, `pose`, `message` |
| `reports.show` | `orbit_reports_show` | opens the panel on a report |
| `reports.open_screenshot` | `orbit_reports_open_screenshot` | shows the file in the platform file browser |
| `reports.delete` | `orbit_reports_delete` | ids are never reused |
| `reports.export` | `orbit_reports_export` | Markdown for one report or all matching; optional `path` also writes a file |

An id is `7` or `"R-0007"`. Failures from the controller surface as RPC error 1100; a bad `which` is -32602.
