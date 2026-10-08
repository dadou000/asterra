+++
path = "/apps/studio"
title = "Orbit Studio application (Orbit.exe)"
kind = "subsystem"
status = "stable"
summary = "The one unified Studio executable: composition root for the editor (project browser, worlds, viewports, panels, plugins, profiler, reports), the loopback JSON-RPC server (default port 4320, configurable with ORBIT_RPC_PORT), hot-reload bootstrap and the relaunch path used when a project is opened."
keywords = ["studio", "orbit.exe", "editor app", "main", "composition root", "relaunch", "hot reload bootstrap", "terrain ui smoke", "orbit_renderdoc", "orbit_profiler"]
sources = [
  "apps/editor/src/Main.cpp",
  "apps/editor/src/StudioApplication.cpp",
  "apps/editor/src/StudioApplication.hpp",
  "apps/editor/src/StudioWorkspaceRpc.cpp",
  "apps/editor/src/RelaunchStudio.cpp",
  "apps/editor/src/EditorAppSupport.hpp",
  "apps/editor/src/EditorAppSupport.cpp",
  "apps/editor/src/StudioPanels.hpp",
  "apps/editor/src/StudioViewportPanel.cpp",
  "apps/editor/src/StudioExplorerPanel.cpp",
  "apps/editor/src/StudioPropertiesPanel.cpp",
  "apps/editor/src/StudioPluginsPanel.cpp",
  "apps/editor/src/StudioMaterialServicePanel.cpp",
  "apps/editor/src/StudioPlatformServicesPanel.cpp",
  "apps/editor/src/StudioOutputPanel.cpp",
  "apps/editor/src/StudioBuildHost.hpp",
  "apps/editor/src/StudioBuildHost.cpp",
  "apps/editor/src/StudioPathNetworkHost.hpp",
  "apps/editor/src/StudioPathNetworkHost.cpp",
  "apps/editor/src/HotReloadBootstrap.cpp",
  "apps/editor/src/RelaunchStudio.cpp",
  "apps/editor/CMakeLists.txt",
]
symbols = []
invariants = [
  "The Explorer presents world objects and project assets together in one searchable hierarchy under one panel titled Explorer. All/World/Assets filters narrow that same tree; distinct vector icons mark semantic object and asset kinds in a consistent row gutter, tinted by the shared EditorUi element-category palette. Explorer tree rows, selectable rows, and search input use modest extra vertical padding for easier pointer targeting. The internal Explorer Source panel is composed into the canonical Explorer surface and is not shown as a separate workspace. Selection, object drag/drop reparenting, context actions, and asset discovery stay owned by the existing explorer/content services.",
  "Orbit.exe is the Studio application itself, not a launcher for another UI process; there is no separate launcher, project manager or alternate editor front end (/rules/ui).",
  "A successful local build publishes the current Studio executable to <repo-root>/Orbit.exe; failing to refresh it is a build failure.",
  "Hot reload bootstrap is a development acceleration layer: any host-side failure falls back to the statically linked production path; it applies only to the root development Orbit.exe, the build-tree Studio and staged generation copies, packaged executables stay self-contained.",
  "RelaunchStudio starts a fresh Studio process opened on a project manifest; the caller exits its own event loop afterwards and project-bound runtime state is intentionally rebuilt from scratch rather than hot-swapped.",
  "The RPC server listens on 127.0.0.1, defaulting to port 4320; ORBIT_RPC_PORT can select another local port for isolated MCP clients or smoke tests (/editor/mcp-rpc).",
  "Main.cpp delegates to StudioApplication::Run, the composition and frame-loop owner in StudioApplication.cpp; the helpers it calls that do not capture its locals (path-network queries and routing profiles, project and initial-body bootstrap, id encoding for drag payloads, plugin panel sync, material preview colour, RPC notification toasts, FindPlayerExecutable, the per-frame CpuFrameTelemetry) live in EditorAppSupport.{hpp,cpp}, namespace orbit::editor_app::support, so editing one recompiles a small unit instead of the whole composition root.",
  "Panels and hosts that used to be lambdas inside main() are classes with the same names for everything they captured: StudioPanelBase (StudioPanels.hpp) carries the application references (ui, worldSession, content, ...) and the world-session accessors (objects(), selection(), ...), and each panel class (Viewport, Explorer, Properties, Plugins, Material Service, Platform Services, Output) owns the state its draw callback keeps between frames and registers itself with Register(). StudioBuildHost owns the BuildService, the validate / cook / package actions, the build.* RPC methods and the Build panel; StudioPathNetworkHost owns route planning, the derived path products, their route/derived events and the path.route / path.geometry RPC queries. The frame loop and the menus call them (viewportPanel.pathPlacementMode, buildHost.PackageProjectBuild(...), pathNetwork.PollRoutes()); the process entry point is main() in HotReloadBootstrap.cpp, which calls the thin OrbitStudioMain in Main.cpp. Project lifetime and workspace RPC registration live in StudioWorkspaceRpc.cpp.",
]
related = ["/rules/ui", "/rules/hot-iteration", "/editor/mcp-rpc", "/editor/viewport"]
depends_on = ["/apps/build-service", "/authoring/commands", "/authoring/content", "/authoring/content-wic", "/authoring/documents", "/authoring/plugins", "/authoring/scene", "/authoring/schema", "/authoring/selection", "/editor/mcp-rpc", "/editor/model", "/editor/studio-session", "/editor/studio-ui", "/editor/ui-toolkit", "/foundation/core", "/foundation/frames", "/foundation/hot-reload", "/foundation/jobs", "/foundation/platform", "/foundation/platform-services", "/foundation/runtime-session", "/rendering/post-process", "/rendering/render-graph", "/rendering/render-view", "/rendering/volumes/fields", "/rendering/volumes/solver", "/tools/dev-server", "/world/path-geometry", "/world/path-routing", "/world/paths", "/world/universe", "/world/world-model"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
  "python tools/tests/mcp_live_smoke.py --exe Orbit.exe: live Windows/Vulkan smoke; every MCP tool and each unique adapter RPC method must return without terminating Studio.",
]
verified = "329654cd3290274cd945e9daec3fd214267ef2c8"
+++
