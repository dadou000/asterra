+++
path = "/rules/hot-iteration"
title = "Hot iteration (save-to-reflect)"
kind = "rule"
status = "stable"
summary = """
Every Orbit-owned development file must have an automatic reflection path: saving a change must \
not require closing Studio, a full rebuild and a relaunch. Pick the fastest safe mechanism \
(resource refresh, script reload, shader/pipeline swap, in-process DLL generation swap, \
automatic Studio generation handoff) and never retire a working generation before its \
replacement has built, loaded, validated and been accepted."""
keywords = ["hot reload", "hot iteration", "save to reflect", "generation", "dll", "restart", "rebuild", "watcher"]
owner_module = "OrbitHotReload"
sources = [
  "docs/ORBIT_HOT_ITERATION.md",
  "engine/hot_reload/include/orbit/hot_reload/ChangeClassifier.hpp",
  "engine/hot_reload/include/orbit/hot_reload/HotReloadHost.hpp",
  "engine/hot_reload/include/orbit/hot_reload/HotIterationService.hpp",
]
symbols = ["ChangeClassifier", "HotReloadHost", "HotIterationService"]
applies_to = ["engine/**", "apps/**", "cmake/**"]
invariants = [
  "Preference order: direct resource refresh, script reload, shader/pipeline replacement, in-process native DLL generation swap, automatic incremental Studio-generation handoff.",
  "A candidate generation must compile, load, validate and accept activation before the working generation is retired; a failed hot build or load leaves the running generation usable.",
  "Never keep raw function pointers into a DLL beyond the lifetime of the pinned generation; use HotReloadHost interface visits/leases.",
  "GPU resources of an old generation are retired only after GPU completion (deferred retirement); never destroy live Vulkan resources just because CPU code was replaced.",
  "Use the central watcher/classifier; do not add per-subsystem watcher threads. New source/resource extensions are added to central classification and tests in the same change.",
  "Unknown files under engine/, apps/ or cmake/ fall back to the automatic generation refresh; they are never silently ignored.",
  "Orbit.exe is the one Studio entry point; do not introduce a separate hot-reload launcher.",
  "New C++ subsystems are hot-reloadable by default (stable host-owned state + versioned reloadable module); do not grow the immutable host to avoid designing the boundary.",
]
related = ["/rules/completion-check", "/rules/architecture"]
verify = [
  "Save the implementation file while Studio runs: the change must appear without a manual restart.",
  "Break the patch deliberately: the previous generation must stay alive and usable.",
]
+++

Source of truth: `docs/ORBIT_HOT_ITERATION.md` (normative) and rules 1-18 of `AGENTS.md`.

## Routing table: what must happen when a file is saved

| Change | Required behaviour |
| --- | --- |
| texture, material, LUT, mesh, decal, imported content | refresh the owning content service, no restart |
| Luau/Lua/plugin script | reload the script/plugin in place |
| project/content shader | compile the changed shader and swap the live pipeline safely |
| reloadable C++ subsystem | incremental build of the affected target, swap the DLL generation in-process |
| editor/native code not yet a DLL island | incremental rebuild, automatic generation handoff |
| CMake/build metadata, immutable host ABI | automatic reconfigure/build and handoff |
| unknown file under `engine/`, `apps/`, `cmake/` | conservative automatic generation refresh |

Documentation-only files may be ignored by the watcher, but docs blocks are still re-read on every
docs-tool call, so doc edits are reflected immediately.

## When you add a subsystem

Document its hot-iteration class and add a test; a subsystem is not complete until both exist
(`/rules/completion-check`).
