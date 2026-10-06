+++
path = "/editor"
title = "Editor (Orbit Studio)"
kind = "section"
status = "stable"
summary = """
Orbit Studio is the single unified editor application (Orbit.exe): project browser, scene and \
world authoring, viewports, diagnostics, profiler, build/export. Every capability is one \
command/service reachable from the UI, plugins, RPC and MCP. Start with the viewport for \
camera/navigation/diagnostic-overlay work and with mcp-rpc for automation."""
keywords = ["editor", "studio", "ui", "viewport", "panels", "automation", "rpc", "mcp"]
related = ["/rules/ui", "/rules/placement"]

[routes]
"viewport, navigation, camera, diagnostic overlays, text HUD" = "viewport"
"add an RPC method or an MCP tool, drive Studio from an agent" = "mcp-rpc"
"explorer, inspector, command surfaces, recipes, shortcuts" = "model"
"switching worlds, active body, world list" = "session"
"terrain runtime bridge, rebuild scheduler, project browser, bezier handle drag" = "studio-session"
"ImGui shell, panel extensions, DPI scale, previews" = "ui-toolkit"
"issue reports, capturing a reproducible problem" = "reports"
"button/shortcut/toolbar change" = "/rules/placement"
"profiler, performance trace" = "/legacy/orbit-profiler"
"shading assets, shader contract" = "/rendering/shading"
+++

Studio modules (all under `engine/`): `studio_ui` (panels, viewport renderer, render-view set, RPC for views; `/editor/viewport`),
`studio_session` (`/editor/studio-session`), `editor_session` (`/editor/session`), `editor_model` (`/editor/model`), `editor_ui` (`/editor/ui-toolkit`),
`editor_rpc` (`/editor/mcp-rpc`), `studio_reports` (`/editor/reports`) and the authoring layer in `/authoring`. The executable is `apps/editor` (`/apps/studio`).

The authoring rule is: a UI element **requests**, a domain owner **executes**, and the same
operation is reachable over RPC and MCP (`/editor/mcp-rpc`).
