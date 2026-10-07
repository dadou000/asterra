# Dead code removal (0.0.9 cleanup)

## Behavior
No behaviour change. Removes code that no production executable (Studio, Player, Build) links, found by relinking each with `/OPT:REF` and diffing the `/MAP` output against `/OPT:NOREF`, plus the stale documentation and tooling of retired versions.

Removed in this pass:
- Docs and tooling of V0.0.3-V0.0.6 and the Godot-era Asterra docs, V0.0.4 tooling, redundant CI.
- `StudioUiBundle`, `OceanRenderer`, `ScreenSpaceVisibility`, `ProjectAuthoringUi`'s duplicate Project Settings / World Documents panels.
- 35 never-linked translation units with their tests: `studio_session` (BezierHandleEditor, ProjectSettingsModel, StudioAuthoringBinding, StudioWorkspaceRpcHost, StudioCelestial*), `lighting` (PlanetaryEmission*, ReflectionPolicy), the stateful `terrain_gpu` page passes and their embedded compute shaders, `MultiScaleTerrain`, `RiverCarvedTerrainSource`, `Local3DPromotion`, `SurfaceMaterial`, `PhysicalPropertySolver`, `OceanMesh`, `V007PerformanceCapture`, `WorldAssetsBrowserModel`, `terrain_relief`, engine/time `SimulationClock`.
- The `OrbitSandbox` app and what only it used: `debug_render`, `map_render`, `water_render`, `UniformPlanetRenderer`/`UniformPlanetMesh`, `GpuElevationQuery`, the port-4319 MCP bridge.
- The `orbit-v003-clean` and `orbit-v003-final` workflows (folded into `orbit-windows.yml`) and the unused `ORBIT_GIT_COMMIT` / `ORBIT_VERSION_STRING` definitions (the commit hash changed every compile flag set when HEAD moved).

Kept on purpose: units whose `.cpp` is unlinked but whose header types live code uses (EmissiveSampling, SurfaceData, ImpactField, Stratigraphy, the three TOML loaders), `PrimitiveBinding` (active feature), `platform_services/steam` (compiled out by configuration), `terrain_cache` and two `terrain_region` sources (test-only now).

## Existing owner
- Module: each removed unit's own module; no new owner was created.
- Canonical state: unchanged; nothing live referenced the removed code.

## Primary insertion point
- File: the deleted sources, their `CMakeLists.txt` entries, tests, and the docs blocks that listed them
- Symbol/function: see the commit messages of `Remove 35 never-linked translation units...` and `Remove the OrbitSandbox app...`
- Reason: a unit is removed only when the linker shows no object of it in any production exe and no live header/source includes it.

## Secondary touch points
- Docs blocks (`sources`, `symbols`, `verify`, `used_by`) updated in the same change; `python tools/orbit_docs_cli.py check` and the docs tooling tests pass.
- V0.0.4 validation and gate tests lose only their checks of the removed units.

## Must not be implemented in
- `assets/` (explicitly out of scope) and the user's work in progress.

## Data/control flow
`/OPT:REF` map diff -> candidate list -> include/usage classification -> removal with tests, CMake and docs

## Validation
- [x] Studio, Player and Build build from scratch (clean-worktree build) and all 260 ctest tests pass
- [x] No duplicate state/controller was introduced
- [x] Studio smoke: every panel registered once, `project.create` / `project.open` work
- [x] `python tools/orbit_docs_cli.py check` passes
