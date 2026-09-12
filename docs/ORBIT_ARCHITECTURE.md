# Orbit architecture rules

Orbit is being built for Asterra's unusual requirements: a planet-scale persistent world, runtime authoring, procedural blueprints, simulation LOD, GPU-driven rendering and simulation, and a world representation that can collapse to compact semantic data.

The engine must stay modular as it grows. These rules are architectural constraints, not suggestions.

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
