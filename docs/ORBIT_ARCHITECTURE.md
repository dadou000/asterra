# Orbit architecture rules

Orbit is being built for Asterra's unusual requirements: a planet-scale persistent world, runtime authoring, procedural blueprints, simulation LOD, GPU-driven rendering and simulation, and a world representation that can collapse to compact semantic data.

The engine must stay modular as it grows. These rules are architectural constraints, not suggestions.

For the V0.0.3 celestial/editor expansion, [V0.0.3_SPEC.md](V0.0.3_SPEC.md) is the implementation specification layered on top of these rules. New world, editor, plugin, asset, path, MCP, build and platform-service work must follow both documents.

## 1. Dependency direction

Dependencies are one-way.

```text
apps / game / editor
        |
        v
high-level engine systems
        |
        +--> world
        +--> physics
        +--> render
        +--> assets
        |
        v
      core

render --> RHI interface --> backend (D3D12, later Vulkan)
platform -----------------> native OS implementation
```

A lower layer must never include a higher layer to make something convenient.

## 2. No God object

There is no global `Engine` object that owns every subsystem and exposes arbitrary access to all of them. Systems receive narrow dependencies through constructors or explicit context objects.

## 3. No service locator

Do not add `GetRenderer()`, `GetWorld()`, `GetPhysics()` global accessors. Global access creates invisible coupling and makes jobs, tests, multiple worlds, dedicated servers, and editor sessions harder.

## 4. World state is not render state

The persistent world must never contain D3D12/Vulkan handles, GPU descriptor indices, renderer-owned pointers, or transient draw objects.

```text
persistent world
      |
      v
active simulation
      |
      v
render extraction
      |
      v
GPU scene
```

## 5. Persistent world is not the ECS

The planet is stored as compact authoritative data. Only relevant regions/entities are promoted into active runtime representations. Distant houses, roads, machines, forests and cities must not remain live engine objects merely because they exist.

## 6. Modules own their types

A type has one owning module. Cross-module interaction uses public interfaces, IDs, handles and data transfer objects. Do not pass private implementation types through module boundaries.

## 7. Third-party libraries stay behind adapters

Jolt, DLSS/Streamline, DirectX libraries, future CFD libraries, audio middleware and other dependencies must not leak across the codebase. Orbit owns the public interface; the third-party library is a backend.

## 8. No feature dumping into Core

`OrbitCore` contains only broadly reusable primitives. Terrain helpers, vehicle math, rendering utilities and gameplay concepts do not belong in Core just because multiple files want them.

## 9. Data over inheritance

Prefer composition, compact records and explicit systems. Avoid deep object hierarchies such as `Object -> Actor -> WorldActor -> BuildingActor -> HouseActor`.

## 10. Generated geometry is disposable

For procedural assets:

```text
semantic blueprint = authoritative
generated mesh     = cache
collision          = cache
navigation data    = cache
HLOD               = cache
```

Never make generated geometry the source of truth if semantic construction data exists.

## 11. Explicit simulation boundaries

CPU rigid-body physics, mechanical simulation, CFD, water, weather and GPU field simulation communicate through small explicit interfaces. One solver must not reach into another solver's internal state.

## 12. Threads are assumed

Engine APIs should not quietly require main-thread ownership unless there is a concrete platform/API reason. Expensive generation, streaming, compilation and simulation work must be expressible as jobs.

## 13. Runtime editor uses engine APIs

The editor is a privileged runtime client, not a parallel implementation of the world. Player construction, developer editing and future agent tools should ultimately issue the same validated world commands with different permissions.

## 14. Keep translation units small

Avoid umbrella headers that include most of the engine. Public headers expose the minimum required surface and prefer forward declarations where practical.

## 15. Architectural violations are bugs

If implementing a feature requires a circular dependency or broad global access, redesign the boundary rather than patching around it.


## 16. UI simplicity must not reduce capability

All Orbit Studio, runtime-authoring, developer-tool, plugin, and agent-facing UI work must follow [ORBIT_UI_RULES.md](ORBIT_UI_RULES.md).

The UI uses progressive disclosure: common actions stay simple and contextual, while legitimate advanced controls remain accessible through inspectors, advanced sections, command/search surfaces, shortcuts, plugins, scripting, or MCP tooling.

UI convenience must never create a second authority path or impose arbitrary authoring limits that do not exist in the engine.


## 17. One unified Studio surface; no separate launchers

Orbit has one primary user-facing Studio application. Project browsing/creation, editing, play/test, build/export, settings, plugins, diagnostics, platform integration, and developer tools are views/workspaces inside that same application.

Do not introduce a separate launcher, project-manager app, configuration shell, or alternate editor front end for normal Orbit workflows. A project-browser/start screen is a Studio view, not a separate executable.

Background/helper processes remain allowed when technically useful, but they must stay implementation details rather than become competing user-facing applications.

This rule is defined in detail by [ORBIT_UI_RULES.md](ORBIT_UI_RULES.md).


## 18. Root executable publishing is a build contract

A successful local Orbit build must publish the current unified Studio executable to `<repo-root>/Orbit.exe`.

The canonical interactive developer entry point is therefore always easy to find and run from the repository root. Internal CMake output layout may remain configuration-specific, but developers must not need to browse those directories to launch Orbit.

`Orbit.exe` is the Studio application itself, not a launcher for another UI process. Failure to refresh the root executable after a successful build is considered a build failure.
