+++
path = "/editor/studio-ui/world-and-project"
title = "World Documents, Project Browser and Project Settings panels"
kind = "subsystem"
status = "stable"
summary = """
ProjectAuthoringUi (the Project Browser), WorldDocumentsUi and ProjectSettingsUi (session-bound, \
wrapping ProjectSettingsUiBase with project lighting/display defaults) are presentation over studio_session models, ProjectDocument and StudioSession. \
This block maps each button to its model call and to the project.* / world.* RPC and MCP tools, and lists the project operations that have none."""
owner_module = "OrbitStudioUi"
keywords = ["project browser", "world documents", "project settings", "recent projects", "create project", "open project", "startup world", "create world", "rename world", "close world", "project switch", "relaunch", "lighting display defaults", "LightingDisplay.orbitcfg", "project.open", "world.open", "ProjectBrowserModel"]
sources = [
  "engine/studio_ui/src/ProjectAuthoringUi.cpp",
  "engine/studio_ui/include/orbit/studio_ui/ProjectAuthoringUi.hpp",
  "engine/studio_ui/src/WorldDocumentsUi.cpp",
  "engine/studio_ui/include/orbit/studio_ui/WorldDocumentsUi.hpp",
  "engine/studio_ui/src/ProjectSettingsUi.cpp",
  "engine/studio_ui/src/ProjectSettingsUiBase.cpp",
  "engine/studio_ui/include/orbit/studio_ui/ProjectSettingsUi.hpp",
  "engine/studio_session/include/orbit/studio_session/ProjectBrowserModel.hpp",
  "engine/studio_session/src/StudioSession.cpp",
  "engine/editor_session/src/WorldDocumentsModel.cpp",
  "engine/editor_rpc/src/EditorSessionRpcHost.cpp",
  "apps/editor/src/Main.cpp",
]
symbols = [
  "ProjectAuthoringUi",
  "WorldDocumentsUi",
  "ProjectSettingsUi",
  "ProjectBrowserModel",
  "SetWorkspaceChangedCallback",
  "DispatchWorldLifecycle",
  "WorldDocumentsModel",
  "RelaunchStudioWithProject",
]
invariants = [
  "The panels own presentation buffers only (paths, names, selection, status text). Authority is StudioWorkspace / ProjectDocument / EditorWorldSession; WorldDocumentsUi never reopens project authority and routes every operation through the one StudioSession that Explorer, the runtime and RPC use.",
  "World create / rename / set-startup in the UI end in the same ProjectDocument calls as world.create / world.set_display_name / world.set_startup (WorldDocumentsModel::Create/Rename/SetStartup versus EditorSessionRpcHost). Open World and Close Active World ARE the world.open / world.close RPC: StudioSession::OpenWorld/CloseWorld call DispatchWorldLifecycle through the session's own dispatcher and clear pending terrain invalidations, physical pages and volume output.",
  "The world catalog is diagnostic, not all-or-nothing: an unreadable world document appears as valid=false with its diagnostic and an [Invalid] label, and gets no Open / Set Startup / Rename controls; it never makes the catalog or settings panel unavailable.",
  "Project create/open/close in the Project Browser go through ProjectBrowserModel into StudioWorkspace, which builds and validates a complete candidate project + session before replacing the current one; a failed open leaves the workspace and the recent-projects (MRU) order unchanged. The only persisted browser state is the MRU list of Project.orbit.toml paths.",
  "ProjectAuthoringUi::CreateProject / OpenProject are the single callable operations behind the buttons and the project.create / project.open RPC registered in apps/editor/src/Main.cpp; both run NotifyWorkspaceChanged. In the editor the browser's own StudioWorkspace is separate from the editing one: a switch saves the project, checkpoints the world, closes the browser workspace and relaunches Studio on the chosen project (RPC result carries relaunching=true), so clients must poll project.info for the new project id.",
  "ProjectSettingsUi.cpp includes ProjectSettingsUiBase.cpp with `Register` and `Draw` macro-renamed to RegisterBase / DrawBase and appends the 'Lighting / Display Defaults' section; the base file is not its own CMake source and must stay includable. Lighting and display defaults are project-owned (LightingDisplay.orbitcfg in the project root), clamped on input, and applied through lighting::SetStudioLightingRuntimeConfig and PublishStudioDisplayDefaultsRuntime; Display Diagnostics overrides them per session without rewriting the file.",
  "Project display name is written through ProjectDocument; startup-world changes go through StudioSession::SetStartupWorld. ProjectSettingsUi::SynchronizeAuthority re-reads the manifest name each draw and resets the edit buffer when the persisted name changes.",
  "Project Settings and World Documents are registered only by ProjectSettingsUi and WorldDocumentsUi (session-bound); ProjectAuthoringUi registers only the Project Browser (RegisterProjectBrowser). Its workspace-bound copies of the settings and world panels, and StudioUiBundle which registered them, were removed in 0.0.9 because nothing used them.",
  "A project operation reachable only through a button is a parity gap (AGENTS.md): see 'Known gaps' before adding UI-only project actions.",
]
related = ["/editor/studio-ui", "/editor/mcp-rpc", "/editor/studio-session", "/editor/session", "/authoring/documents", "/apps/studio"]
depends_on = ["/editor/studio-session", "/editor/session", "/authoring/documents"]
used_by = ["/editor/studio-ui"]
verify = [
  "ctest -R Orbit.ProjectBrowserModel",
  "ctest -R Orbit.WorldDocumentsModel",
  "ctest -R Orbit.StudioSession",
]
verified = "55d48117"

[routes]
"project or world file on disk is wrong, not the panel" = "/authoring/documents"
"project switch, workspace generation or session rebuild misbehaves" = "/editor/studio-session"
"world open/close leaves stale terrain or body state" = "/editor/session"
"Studio relaunch or RPC port handoff after project.open" = "/apps/studio"

[[diagnose]]
symptom = "Project Browser lists a recent project as '(unavailable)' or opening it fails"
steps = [
  "Call project.recent (orbit_project_recent): `available` and `error` come from ProjectBrowserModel::RecentProjects (for example 'Project manifest is missing.').",
  "Try project.open with the manifest path; a failed open leaves the current project and the MRU order untouched, so the error text in the Project Browser status line is the whole story.",
]

[[diagnose]]
symptom = "After project.open or project.create over RPC the old project keeps answering, or the connection drops"
steps = [
  "This is the designed handoff: Studio relaunches into the target project (result has relaunching=true). Poll project.info until its id equals the id returned by project.open.",
  "orbit_project_open / orbit_project_create do this polling when wait=True (default timeout 120 s).",
  "If it never switches, RelaunchStudioWithProject failed in apps/editor/src/Main.cpp and the pending switch is dropped.",
]
docs = ["/apps/studio"]

[[diagnose]]
symptom = "A world shows [Invalid] and cannot be opened, renamed or made startup"
steps = [
  "Call world.describe {path}: the error (1021) carries the same diagnostic the catalog shows under 'Validation:'.",
  "Fix or restore the .orbitworld file under Worlds/; the catalog is rebuilt from ProjectDocument::WorldPaths() on every draw.",
]
docs = ["/authoring/documents"]

[[diagnose]]
symptom = "Rename, Set Startup or Create World succeeds but the panel keeps showing old text"
steps = [
  "Call world.list: it is the authoritative ProjectDocument catalog the panel renders each frame.",
  "The panel's selection and name buffer are presentation state; they reset when the workspace generation changes or the selected path disappears from the catalog.",
]

[[diagnose]]
symptom = "Project Settings shows the old project name, or 'Save Project Name' does nothing visible"
steps = [
  "The name field is bound to ProjectDocument manifest displayName and refreshed by SynchronizeAuthority; project.info reports the persisted value.",
  "The editor app has no RPC for renaming the project (see Known gaps); only the button writes it.",
]

[[diagnose]]
symptom = "Lighting / Display Defaults do not persist or do not apply"
steps = [
  "They are saved only by the 'Save Lighting / Display Defaults' button, which writes LightingDisplay.orbitcfg in the project root and applies it at once; 'Adopt Session Lighting' only copies the session override into the unsaved panel state.",
  "'Reload Project Defaults' re-reads the file and re-applies it, discarding unsaved edits.",
  "Session overrides made in Display Diagnostics are separate and never rewrite the file (panel text: they apply to the current Studio session only).",
]

[[diagnose]]
symptom = "Two 'Project Settings' or 'World Documents' panels, or a panel is missing"
steps = [
  "apps/editor/src/Main.cpp registers the browser from ProjectAuthoringUi and the other two from WorldDocumentsUi / ProjectSettingsUi; check studio.panel_list (orbit_panel_list) for what is registered.",
]
+++

## Who is who

| Class | Bound to | Registers | Notes |
| --- | --- | --- | --- |
| `ProjectAuthoringUi` | `StudioWorkspace` (+ recent-projects file) | Project Browser (default open, centre) via `RegisterProjectBrowser` | Owns `ProjectBrowserModel`. `SetOpenProject` lets the editor report the project it is really bound to, because the browser's own workspace is empty while editing. |
| `WorldDocumentsUi` | `StudioSession` (already-open project) | World Documents (left, default closed) | Create World, world list with ID and schema, Open Selected, Set Selected As Startup, Rename, Close Active World (hidden when `allowCloseWorld` is false). |
| `ProjectSettingsUi` | `ProjectDocument` + `StudioSession` | Project Settings (right, closed) | Sections: Project (name, folder, manifest, id, engine compatibility), Worlds (startup world, Set Startup), Developer Validation (terrain round trip, M15 scenario), Lighting / Display Defaults. |

The Project Browser has Recent Projects (filter box above six entries), Find Projects (background scan of `platform::UsualProjectFolders()` via `ProjectBrowserModel::DiscoverProjects`, up to depth 4, 64 results and a 4 s budget by default), New Project and Open From Disk.

## Calls behind the buttons

| Button | Call | Same operation over RPC |
| --- | --- | --- |
| Create Project | `ProjectAuthoringUi::CreateProject` -> `ProjectBrowserModel::CreateProject` -> `StudioWorkspace::CreateProject` | `project.create` |
| Open Project, recent or found entry | `ProjectAuthoringUi::OpenProject` -> `ProjectBrowserModel::OpenProject` | `project.open` |
| Scan Usual Folders | `ProjectBrowserModel::DiscoverProjects` | `project.discover` |
| Recent Projects list | `ProjectBrowserModel::RecentProjects` | `project.recent` |
| Create World | `StudioSession::CreateWorld` -> `WorldDocumentsModel::Create` -> `ProjectDocument::CreateWorld` | `world.create` |
| Open Selected World | `StudioSession::OpenWorld` -> dispatches `world.open` | `world.open` |
| Set (Selected As) Startup | `StudioSession::SetStartupWorld` | `world.set_startup` |
| Rename Selected World | `StudioSession::RenameWorld` -> `ProjectDocument::SetWorldDisplayName` | `world.set_display_name` |
| Close Active World | `StudioSession::CloseWorld` -> dispatches `world.close` | `world.close` |

## RPC / MCP parity

| Operation | RPC | MCP tool |
| --- | --- | --- |
| open project info | `project.info` | `orbit_project_info` |
| create / open project | `project.create`, `project.open` | `orbit_project_create`, `orbit_project_open` (wait for relaunch) |
| find / recent projects | `project.discover`, `project.recent` | `orbit_project_discover`, `orbit_project_recent` |
| world catalog | `world.active`, `world.list`, `world.describe` | `orbit_world_active`, `orbit_world_list`, `orbit_world_describe` |
| world lifecycle and metadata | `world.create`, `world.open`, `world.close`, `world.set_startup`, `world.set_display_name` | `orbit_world_create`, `orbit_world_open`, `orbit_world_close`, `orbit_world_set_startup`, `orbit_world_set_display_name` |

`world.open` and `world.close` must be standalone requests (`EditorSessionRpcHost` rejects them inside a batch). `project.create` / `project.open` are registered by `apps/editor/src/Main.cpp` over `ProjectAuthoringUi`. The in-process, transactional variants that used to live in `StudioWorkspaceRpcHost` were removed in 0.0.9 (no app instantiated them).

## Known gaps

Verified by searching every `.name = "..."` registration under `engine/` and `apps/` and every tool in `tools/mcp_server/orbit_editor_mcp_server.py`:

- Close Project (Project Browser): there is no `project.close` RPC or MCP tool.
- Save Project Name (Project Settings): there is no `project.set_display_name` RPC or MCP tool.
- Developer Validation buttons (Save, Reopen & Verify Terrain; Run M15 Terrain Validation Scenario): no RPC or MCP tool.
- Lighting / Display Defaults (Save, Reload Project Defaults, Adopt Session Lighting): no RPC or MCP tool reads or writes `LightingDisplay.orbitcfg`.

When closing one, extract the model call the button makes, register the RPC in the app host, add the MCP tool and the `docs/ORBIT_MCP.md` row in the same change, then delete the bullet here.
