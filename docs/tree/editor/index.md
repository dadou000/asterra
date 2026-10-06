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
"button/shortcut/toolbar change" = "/rules/placement"
"profiler, performance trace" = "/legacy/orbit-profiler"
"shading assets, shader contract" = "/legacy/orbit-shading"
+++

Studio modules (all under `engine/`): `studio_ui` (panels, viewport renderer, render-view set,
RPC for views), `studio_session` (session and terrain runtime bridge), `editor_model` (UI-independent
authoring model), `editor_ui`, `editor_rpc` (JSON-RPC authoring API), `commands` (command registry),
`selection`, `schema`, `scene`, `documents`. The executable lives in `apps/editor`.

The authoring rule is: a UI element **requests**, a domain owner **executes**, and the same
operation is reachable over RPC and MCP (`/editor/mcp-rpc`).
