# Orbit Hot Iteration Contract

Status: **Canonical engine-wide development contract**

Orbit is designed around save-to-reflect iteration. A developer should be able to change engine code, editor code, shaders, scripts, content, build metadata, or project data and see the result with the least disruption technically possible.

This document defines the architecture and rules that make that possible. It is normative. New systems must comply with it together with [ORBIT_ARCHITECTURE.md](ORBIT_ARCHITECTURE.md) and [ORBIT_UI_RULES.md](ORBIT_UI_RULES.md).

## 1. Core invariant

> **Every development-relevant file owned by Orbit must have an automatic reflection path. A normal edit must not require a developer to manually close Orbit, run a full rebuild, find a new executable, and reopen the editor.**

“Hot patchable” does not mean every file uses the same mechanism. It means every change is routed to the fastest safe mechanism available.

The intended order of preference is:

1. direct resource refresh;
2. direct script reload;
3. shader compile plus live pipeline replacement;
4. in-process native DLL generation replacement;
5. automatic incremental Studio generation build and handoff for the tiny immutable boundary.

The fifth path is a compatibility/safety fallback, not the desired steady-state architecture for ordinary engine systems.

## 2. Save-to-reflect routing

A saved file must resolve to one of these paths:

| Change | Required development behavior |
| --- | --- |
| texture, material, LUT, mesh, decal, imported content | refresh the owning content service without restarting Studio |
| Luau/Lua/plugin script | reload the script/plugin while Studio remains live |
| project/content shader | compile the changed shader and replace the live shader/pipeline safely |
| reloadable C++ subsystem | incrementally build only the affected native target and swap its DLL generation in-process |
| editor/native code not yet migrated to a DLL island | incrementally rebuild the affected Studio target and perform automatic generation handoff |
| CMake/build metadata or immutable host ABI | automatic incremental reconfigure/build and generation handoff |
| unknown Orbit-owned file under `engine/`, `apps/`, or `cmake/` | conservative automatic generation refresh; never silently ignore it |

Files that are genuinely documentation-only or unrelated to a running build may remain ignored.

## 3. The permanent host must stay tiny

`Orbit.exe` is the unified Studio entry point. It must not become a second launcher application.

The long-term hot architecture is:

```text
Orbit.exe / permanent process shell
    |
    +-- process/window lifetime
    +-- hot-module loader and ABI
    +-- persistent service handles needed for safe replacement
    +-- minimal platform bootstrap that cannot replace itself
    |
    +-- reloadable editor generation
    +-- reloadable renderer generation
    +-- reloadable terrain generation
    +-- reloadable simulation generations
    +-- reloadable tools/plugins
```

The immutable boundary is a liability and must remain as small as practical. Do not move ordinary features into it because doing so is convenient.

## 4. New native systems are hot by default

A new C++ subsystem must be designed for live replacement from its first implementation.

Preferred structure:

```text
stable host-owned state
        |
        v
versioned module interface
        |
        v
reloadable implementation DLL
```

Do not first build a large statically linked subsystem and promise to make it hot later unless there is a concrete bootstrap dependency that prevents modularization.

When a system cannot yet be an in-process module, it must at minimum participate in the engine-wide automatic incremental generation fallback.

## 5. State survives implementation replacement

Reloadable implementation code must not unnecessarily own authoritative state.

Prefer host-owned or service-owned state with generation code operating on explicit state records/handles. Examples include exposure state, world state, editor document state, terrain authority data, simulation state, cached handles, and user workspace state.

If a module must own private state, it must provide versioned serialization/migration through the hot-reload lifecycle.

A successful hot patch should preserve, where applicable:

- open project and world;
- camera position and viewport state;
- editor selection;
- unsaved authoring state when safe;
- simulation state when compatible;
- persistent GPU/resource ownership through stable handles;
- diagnostics and developer context.

A minor implementation edit must not reset the whole editor merely because the code changed.

## 6. ABI boundaries are explicit and versioned

Native module boundaries use explicit versioned APIs.

Rules:

- use fixed-width scalar types, POD records, opaque handles, or explicitly versioned structures at the ABI;
- do not pass private implementation classes across module boundaries;
- do not rely on STL container object layout across independently replaceable generations unless both sides are intentionally locked to the same ABI and lifetime;
- do not transfer ownership of memory without an explicit allocator/owner contract;
- do not leak RTTI/exceptions across a reload boundary;
- every queried interface has an interface name/id and version;
- incompatible interfaces must be rejected without taking down the active generation.

Changing the central module ABI is an immutable-boundary change and therefore uses automatic generation handoff.

## 7. A failed patch must not kill the working editor

The old generation remains authoritative until the replacement has compiled, loaded, validated, and accepted activation.

Required behavior:

```text
current generation stays live
        |
        +-- compile replacement
        |       |
        |       +-- failure -> log error, keep current generation
        |
        +-- load/validate replacement
                |
                +-- failure -> unload candidate, keep current generation
                |
                +-- success -> commit swap -> retire old generation safely
```

Never unload the current generation before the candidate is known to be usable.

Build errors are developer feedback, not an excuse to terminate Orbit.

## 8. Code executing inside a DLL must be pinned

A generation cannot be unloaded while any thread can still execute code from it.

Use host-mediated interface visits, generation leases, frame-boundary commits, job fences, or another explicit lifetime mechanism. A raw function pointer that can outlive its module generation is not acceptable.

Renderer and simulation modules must additionally account for asynchronous CPU jobs and GPU work before destroying old-generation resources.

## 9. GPU resources need deferred retirement

Vulkan objects cannot be destroyed simply because a CPU DLL was replaced.

Reloadable rendering code should prefer host-owned or RHI-owned stable handles. Generation-specific GPU resources must enter deferred destruction and remain alive until the appropriate fence/timeline value proves that no submitted work references them.

A renderer hot patch must never solve lifetime hazards with `vkDeviceWaitIdle()` on every edit unless used temporarily for diagnostics. The production iteration path should use precise synchronization.

## 10. Shader iteration is not a Studio rebuild

Engine-owned shaders must move toward the same direct path already expected for project/content shaders:

```text
save shader
  -> compile affected stage/variant
  -> validate reflection/layout compatibility
  -> create replacement pipeline
  -> switch at a safe frame boundary
  -> defer destruction of the previous pipeline
```

Embedding large shader sources as C++ string literals should be avoided when it prevents independent shader compilation/reload.

## 11. Content iteration is service-driven

Materials, textures, LUTs, decals, meshes, import metadata and similar resources are not native-code rebuild events.

They should enter `ContentService`/importer refresh paths and increment resource revisions so consumers can update selectively.

Do not rebuild Studio just to reflect data that can be reimported or re-bound.

## 12. File watching is centralized

Do not add one ad-hoc file watcher per subsystem.

The engine-wide hot iteration service owns development file observation and classification. Systems register roots/handlers with it, or register native hot roots with `HotReloadHost`.

This avoids duplicate builds, races, inconsistent debounce behavior, and multiple systems responding to the same save.

## 13. Classification must be conservative and exhaustive

Orbit-owned development files must never disappear into an unhandled classification hole.

If a file under the engine/editor/build source tree is not recognized as a direct resource type, route it through the safe native/generation fallback.

When adding a new extension or source domain, update the classifier and tests in the same change.

## 14. Optimize for one-file iteration

The common case is changing one implementation file and seeing the result quickly.

Development builds should therefore favor:

- incremental compilation;
- parallel compiler workers;
- small translation units;
- narrow header dependencies;
- granular CMake targets;
- incremental linking where safe;
- independently buildable reload DLLs;
- no unnecessary packaging/copy steps in the inner loop.

A header that causes half the engine to rebuild is an iteration defect and should be treated as architectural debt.

## 15. No manual build step in the normal inner loop

After the initial development build and launch, saving a supported file should trigger its reflection path automatically.

Manual `cmake --build`, `build_orbit.bat`, closing Studio, or copying executables is allowed for explicit clean/release/diagnostic workflows, but it is not the expected inner-loop workflow.

## 16. The root executable contract still applies

The canonical developer entry point remains `<repo-root>/Orbit.exe`.

During a running hot session Windows may lock that file. Incremental builds must not fail merely because the running root executable cannot be overwritten. A staged generation may run from the hot runtime directory while preserving `Orbit.exe` as the canonical entry point for the next cold launch.

This does not permit a separate user-facing launcher.

## 17. Generation handoff is the fallback, not the target for everything

Automatic Studio-generation replacement gives every native file an immediate reflection route today. It is intentionally broader than the in-process DLL coverage.

However, systems with frequent iteration pressure should migrate to true in-process hot modules. Priority order is generally:

1. renderer/shaders/post process;
2. terrain and world-generation algorithms;
3. editor UI and tools;
4. lighting/GI/volumetrics;
5. physics/simulation;
6. celestial systems;
7. runtime/gameplay services.

A process-generation handoff that occurs for a frequently edited ordinary subsystem should be treated as a migration opportunity, not as the final architecture.

## 18. New subsystem acceptance checklist

A new subsystem is not complete until all applicable items are true:

- [ ] its files are classified by the central hot-iteration system;
- [ ] its project/content resources have direct reload handlers where appropriate;
- [ ] native implementation is in a reloadable module or has a documented temporary generation-fallback route;
- [ ] authoritative state survives implementation replacement where practical;
- [ ] interface ABI is explicit and versioned;
- [ ] active calls/jobs cannot race DLL unload;
- [ ] GPU resources, if any, retire safely after GPU completion;
- [ ] compile/load failure keeps the old generation live;
- [ ] a regression test covers classification or module loading where practical;
- [ ] documentation identifies the subsystem's hot path;
- [ ] no separate launcher or manual developer restart was introduced.

## 19. Forbidden patterns

Do not:

- add a feature whose normal iteration requires manually restarting Orbit;
- introduce a separate hot-reload launcher;
- put ordinary engine features into the immutable host to avoid module work;
- unload a native generation before the replacement is validated;
- keep raw callable DLL pointers past their generation lifetime;
- duplicate authoritative world/editor state inside reloadable implementations without migration;
- rebuild all of Studio for a texture/material/script change;
- create subsystem-specific directory watcher threads when the central watcher can route the event;
- classify a new Orbit source domain as ignored merely because its reload path is unfinished;
- make release packaging part of the save-to-reflect inner loop.

## 20. Validation requirements

The hot-iteration system must have automated coverage for at least:

- change classification;
- native module ABI/version rejection;
- successful native generation replacement;
- state transfer where used;
- candidate failure preserving the previous generation;
- duplicate event/debounce behavior;
- generation staging and cleanup;
- content/script/shader routing as those paths become production-ready.

CI may use clean builds for correctness. Local developer mode may use aggressive incremental compile/link settings. Neither mode should change runtime semantics.

## 21. Current implementation map

Primary implementation points:

- `engine/hot_reload/include/orbit/hot_reload/ModuleApi.hpp` — stable module ABI;
- `engine/hot_reload/include/orbit/hot_reload/HotReloadHost.hpp` — native module registration and pinned interface visits;
- `engine/hot_reload/include/orbit/hot_reload/HotIterationService.hpp` — engine-wide watcher/event/fallback service;
- `engine/hot_reload/src/ChangeClassifier.cpp` — save routing;
- `engine/hot_reload/src/HotReloadHost.cpp` — build/stage/load/swap lifecycle;
- `engine/hot_reload/src/HotIterationService.cpp` — Win32 source watching and generation fallback;
- `apps/editor/src/HotReloadBootstrap.cpp` — Studio integration;
- `engine/post_process_hot_reload/` — first production native hot-module seam;
- `engine/content/src/ContentHotReload.cpp` — content refresh integration;
- `build_orbit.bat` and `ORBIT_FAST_HOT_ITERATION` — low-latency local build mode.

These paths may evolve, but the rules above remain the contract.
