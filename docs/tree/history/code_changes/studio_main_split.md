# Studio `Main.cpp` split

## Behavior
No behaviour change. `apps/editor/src/Main.cpp` was a 10,014-line `main()`; it is now about 3,900 lines of composition root (setup, RPC wiring, the frame loop). The rest moved into units that can be edited and compiled on their own, so a change to the Explorer panel no longer recompiles the whole root.

## Existing owner
- Module: `apps/editor` (Orbit Studio composition root)
- Class/service: `main()` / `OrbitStudioMain`; the moved code kept its owners (`StudioSession`, `EditorWorldSession`, `ContentService`, `EditorSessionRpcHost`)
- Canonical state: unchanged. Application-level objects stay locals of `OrbitStudioMain` and are passed by reference.

## Primary insertion point
- File: `apps/editor/src/EditorAppSupport.{hpp,cpp}` - helpers that capture none of `main()`'s locals (namespace `orbit::editor_app::support`)
- File: `apps/editor/src/StudioPathNetworkHost.{hpp,cpp}` - route planning, derived path products, their events and the `path.route` / `path.geometry` RPC queries
- File: `apps/editor/src/StudioPanels.hpp` + `StudioViewportPanel.cpp`, `StudioExplorerPanel.cpp`, `StudioPropertiesPanel.cpp`, `StudioPluginsPanel.cpp`, `StudioMaterialServicePanel.cpp`, `StudioPlatformServicesPanel.cpp`, `StudioOutputPanel.cpp` - one class per panel
- File: `apps/editor/src/StudioBuildHost.{hpp,cpp}` - `BuildService`, validate / cook / package, the `build.*` RPC methods and the Build panel
- Reason: each group is closed over a small, nameable set of locals; the code was moved verbatim and the locals it captured became members of the same name.

## Secondary touch points
- `StudioPanelBase` carries the application references (`ui`, `worldSession`, `content`, ...) and the world-session accessors (`objects()`, `selection()`, ...); lambda capture lists that named those locals became `[this]`.
- `apps/editor/CMakeLists.txt` lists the new sources; `main()` is renamed to `OrbitStudioMain` directly instead of by a compile definition (the process entry point is in `HotReloadBootstrap.cpp`).
- State the frame loop also drives (path placement mode, right-button gesture, frame mouse delta, material preview) is a public member of its panel and spelled `viewportPanel.x` / `materialServicePanel.x` in the loop.
- `apps/editor/docs/index.md` documents the structure.

## Must not be implemented in
- A second composition root or a launcher: `Orbit.exe` stays the one Studio entry point.
- New UI-only actions: RPC parity rules from `AGENTS.md` still apply to every moved panel.

## Data/control flow
`OrbitStudioMain` builds the application objects -> `StudioPanelEnvironment` (references) -> panel / host objects -> `Register()` / `AttachRpc()` -> frame loop calls `pathNetwork.PollRoutes()`, `buildHost.*`, `viewportPanel.*`

## Validation
- [x] Every moved statement equals the original text modulo capture lists (checked by script against `HEAD`)
- [x] Studio builds; 260/260 ctest tests pass
- [x] Studio smoke: all panels registered exactly once, build RPC answers, project create/open work
- [ ] Remaining in `Main.cpp`: the frame loop (about 1,350 lines, 90 locals), the project-switch/relaunch block and the agent tab-opening RPC block
