+++
path = "/editor/reports"
title = "Issue reports (reproducible problem captures)"
kind = "subsystem"
status = "stable"
summary = "ReportStore keeps many problems per session as IssueReports: status (unresolved, pending, resolved), any combination of scopes (performance, visual quality, bug, crash, other), tags, text and captured conditions (a JSON snapshot of clock, camera, selection, diagnostics) with a screenshot, saved in the project so they travel with it."
owner_module = "OrbitStudioReports"
keywords = ["report", "issue report", "bug report", "condition", "snapshot", "screenshot", "transient", "reproduce", "reports panel", "reports rpc"]
sources = [
  "engine/studio_reports/include/orbit/studio_reports/IssueReport.hpp",
  "engine/studio_reports/CMakeLists.txt",
]
symbols = ["ReportCondition"]
invariants = [
  "A report captures a condition: simulation time and rate, project, world, workspace, selection, camera pose, target body, coordinates and heights, view diagnostics text, surface debug mode and frame-time statistics; the store treats the snapshot as opaque and the Studio capture decides what goes in it.",
  "A persistent problem has one condition; a transient problem has a starting condition and, once it stops, an ending condition; a non-transient report has no end condition and rejects one.",
  "Reports are saved to <project>/Reports/reports.json on every change so a crash never loses one; a damaged file is left alone and reported in the log; deleting a report deletes its screenshots.",
  "reports.restore pauses the clock at the captured time and rate, selects the captured body and puts the camera back on the captured pose.",
]
related = ["/editor/mcp-rpc", "/legacy/orbit-mcp/issue-reports"]
depends_on = ["/foundation/core", "/foundation/rpc"]
used_by = ["/editor/studio-ui"]
verify = [
  "ctest -R Orbit.StudioReports",
]
verified = "b0a0de7f"
+++

RPC/MCP: reports.list/get/create/update/capture/restore and their orbit_reports_* tools (`/legacy/orbit-mcp/issue-reports`).
