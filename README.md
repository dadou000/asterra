# Orbit

Orbit is the custom C++23 engine for **Asterra**.

The `orbit` branch is intentionally independent of Godot. The previous Godot implementation remains available on the other Asterra branches as a reference while Orbit is developed from first principles.

## M0 — World Kernel

The first milestone establishes a strict modular engine layout, a native Windows runtime, C++23 build configuration, explicit platform boundaries, and the foundation for D3D12, the planetary world database, concentric terrain clipmaps, runtime editing, and procedural blueprints.

## V0.0.3 — Celestial Authoring Scaffold

The next architecture milestone generalizes Orbit from a terrain-focused planetary runtime into a frame-aware celestial authoring engine with a permanent project/editor/plugin/MCP scaffold.

Read [docs/V0.0.3_SPEC.md](docs/V0.0.3_SPEC.md) before adding new world, editor, asset, path, plugin, build or platform-service systems. It defines the small implementation milestones, CPU/GPU residency rules, Orbit Studio editor architecture, Luau plugin model, project persistence, Material Service, dynamic path networks, MCP V2 and Steam integration boundary.

## Build

### One-click Windows build

Double-click `build_orbit.bat` from the repository root.

It configures CMake, builds the runtime, Studio and headless build CLI, and packages the developer build into:

```text
dist/Orbit-Windows-Release/
├── OrbitLauncher.exe
├── OrbitSandbox.exe
├── OrbitStudio.exe
├── OrbitBuild.exe
└── symbols/
```

Useful command-line options:

```bat
build_orbit.bat run
build_orbit.bat debug run
build_orbit.bat rebuild
build_orbit.bat release nopause
```

### Manual build

Requirements:

- Windows 10/11
- Visual Studio 2022 with Desktop development with C++
- CMake 3.28+

```powershell
cmake -S . -B build
cmake --build build --config Debug
.\build\apps\launcher\Debug\OrbitLauncher.exe
```

`OrbitLauncher` supervises the runtime and captures stdout/stderr into a timestamped session log. Logs are stored under `apps/launcher/<config>/logs` in a development build. If Orbit hits an unhandled Windows exception or `std::terminate`, the runtime also writes a crash report and minidump (`.dmp`) into the same log directory.

### Headless project validation and cooking

`OrbitBuild.exe` and Orbit Studio use the same `Orbit::Build` service. The current M21 path validates a project/build profile, resolves plugin manifests, cooks importer-backed target DDC products, compiles project Luau modules, copies the startup world and writes a deterministic `OrbitBuildManifest.toml`.

```powershell
.\build\apps\build\Release\OrbitBuild.exe C:\path\to\Project --validate
.\build\apps\build\Release\OrbitBuild.exe C:\path\to\Project --cook --profile "Development Windows"
```

Standalone player/package assembly is intentionally not exposed yet: `OrbitSandbox` is still a development terrain composition rather than a project-driven player.

Orbit Studio also exposes `build.profiles`, `build.validate`, and `build.cook` over its structured JSON-RPC/MCP bridge.

You can still launch `OrbitSandbox.exe` directly while developing; its crash handler falls back to a local `logs` directory when it is not started by the launcher.



### Sandbox camera controls

The terrain sandbox uses a free camera:

- `W/A/S/D` — move relative to camera heading
- Mouse — yaw/pitch look
- `Q/E` — move down/up
- `Left Shift` — movement boost
- `F3` — toggle the debug HUD (FPS, altitude, terrain/cache/ocean stats)
- `Esc` — quit

Mouse capture is released automatically when the window loses focus.

### Testing over MCP

While `OrbitSandbox` is running, it listens on a loopback-only TCP
dev server at `127.0.0.1:4319` (see `engine/dev_server`) that accepts
line-based commands: `PING`, `STATS`, `SCREENSHOT <path>`,
`TELEPORT <dirX> <dirY> <dirZ> <altitudeMeters>`,
`SLEW <dirX> <dirY> <dirZ> <altitudeMeters> <durationSeconds>`,
`DEBUG_OVERLAY <ON|OFF>`, `QUIT`.

`TELEPORT` jumps instantly, which is fine for reading stats but
leaves a stale, not-yet-restreamed frame on screen if you screenshot
right after -- use `SLEW` instead to fly there smoothly over real
frames when you actually need to see the clipmap streaming/morphing
while moving, e.g. across several `SCREENSHOT` calls spaced a second
or so apart during the flight.

`tools/mcp_server/orbit_mcp_server.py` exposes those as MCP tools so
an MCP client (Claude Code, Claude Desktop, ...) can drive and
inspect a running Orbit process for automated testing:

```powershell
pip install -r tools/mcp_server/requirements.txt
claude mcp add orbit -- python C:\path\to\asterra\tools\mcp_server\orbit_mcp_server.py
```

Launch `OrbitSandbox` separately first (the bridge only talks to an
already-running process, it doesn't launch one). Screenshots read
real desktop pixels, so the dev server briefly raises the Orbit
window's Z-order before capturing to make sure it isn't occluded.

Read [docs/ORBIT_ARCHITECTURE.md](docs/ORBIT_ARCHITECTURE.md) before adding engine systems.

Known issues and their fix status are tracked in [docs/PROBLEMS.md](docs/PROBLEMS.md).

### Mountain terrain and performance

The default terrain recipe combines domain-warped ridges, multifractal mountain
detail, and footprint-filtered hills, with elevations bounded below 8 km above
sea level. The sandbox starts over a mountain range with terrain-derived camera
clearance. The implementation, measured CPU costs, and optimization research
are documented in [the terrain research report](docs/research/TERRAIN_SYNTHESIS_AND_OPTIMIZATION.md).

To run the deterministic height survey and CPU benchmark:

```powershell
cmake --build build --config Release --target OrbitTerrainBenchmark
.\build\tests\Release\OrbitTerrainBenchmark.exe build/terrain-heightfield.csv
```

### Standing water

The sandbox renders oceans and lakes on the terrain clipmap itself, sharing its
sample cache, morphing and depth surface. Lake levels use coarse hydrology as
one authority while fine regions continue to refine rivers and surrounding land.
See [standing water rendering](docs/STANDING_WATER_RENDERING.md) for validation,
storage costs and remaining limitations. The HUD reports `WATER TERRAIN`; the
legacy ocean vertex and lake-cell counts no longer describe the active surface.

F3 also reports committed full-grid rebuilds as `REBASE`: cumulative count,
reason (`SOURCE`, `LOD`, or `MOVE`), number of levels, and elapsed seconds. `NOW`
stays visible for two seconds. Source refreshes preserve sampling grid placement;
coverage changes retain grids shared between the old and new resolutions.
