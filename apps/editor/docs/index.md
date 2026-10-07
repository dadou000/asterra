+++
path = "/apps/studio"
title = "Orbit Studio application (Orbit.exe)"
kind = "subsystem"
status = "stable"
summary = "The one unified Studio executable: composition root for the editor (project browser, worlds, viewports, panels, plugins, profiler, reports), the JSON-RPC server on port 4320, hot-reload bootstrap and the relaunch path used when a project is opened."
keywords = ["studio", "orbit.exe", "editor app", "main", "composition root", "relaunch", "hot reload bootstrap", "terrain ui smoke", "orbit_renderdoc", "orbit_profiler"]
sources = [
  "apps/editor/src/Main.cpp",
  "apps/editor/src/EditorAppSupport.hpp",
  "apps/editor/src/EditorAppSupport.cpp",
  "apps/editor/src/HotReloadBootstrap.cpp",
  "apps/editor/src/RelaunchStudio.cpp",
  "apps/editor/CMakeLists.txt",
]
symbols = []
invariants = [
  "Orbit.exe is the Studio application itself, not a launcher for another UI process; there is no separate launcher, project manager or alternate editor front end (/rules/ui).",
  "A successful local build publishes the current Studio executable to <repo-root>/Orbit.exe; failing to refresh it is a build failure.",
  "Hot reload bootstrap is a development acceleration layer: any host-side failure falls back to the statically linked production path; it applies only to the root development Orbit.exe, the build-tree Studio and staged generation copies, packaged executables stay self-contained.",
  "RelaunchStudio starts a fresh Studio process opened on a project manifest; the caller exits its own event loop afterwards and project-bound runtime state is intentionally rebuilt from scratch rather than hot-swapped.",
  "The RPC server listens on 127.0.0.1:4320 (/editor/mcp-rpc).",
  "Main.cpp is the composition root; the helpers it calls that do not capture its locals (path-network queries and routing profiles, project and initial-body bootstrap, id encoding for drag payloads, plugin panel sync, material preview colour, RPC notification toasts, FindPlayerExecutable, the per-frame CpuFrameTelemetry) live in EditorAppSupport.{hpp,cpp}, namespace orbit::editor_app::support, so editing one recompiles a small unit instead of the whole composition root.",
]
related = ["/rules/ui", "/rules/hot-iteration", "/editor/mcp-rpc", "/editor/viewport"]
depends_on = ["/apps/build-service", "/authoring/commands", "/authoring/content", "/authoring/content-wic", "/authoring/documents", "/authoring/plugins", "/authoring/scene", "/authoring/schema", "/authoring/selection", "/editor/mcp-rpc", "/editor/model", "/editor/studio-session", "/editor/studio-ui", "/editor/ui-toolkit", "/foundation/core", "/foundation/frames", "/foundation/hot-reload", "/foundation/jobs", "/foundation/platform", "/foundation/platform-services", "/foundation/runtime-session", "/rendering/post-process", "/rendering/render-graph", "/rendering/render-view", "/rendering/volumes/fields", "/rendering/volumes/solver", "/tools/dev-server", "/world/path-geometry", "/world/path-routing", "/world/paths", "/world/universe", "/world/world-model"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++


