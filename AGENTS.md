# Orbit contributor and agent rules

These rules apply to automated coding agents and human contributors working on the `orbit` branch.

Start from the documentation tree instead of reading whole documents. With the docs MCP server registered
(`claude mcp add orbit-docs -- python tools/mcp_server/orbit_docs_mcp_server.py`) call `orbit_docs_for_task("<what you are about to do>")`,
and `orbit_docs_for_target("<file or symbol>")` before editing a file. Without MCP use `python tools/orbit_docs_cli.py task "<task>"`
and `python tools/orbit_docs_cli.py target <file>`. See `docs/ORBIT_DOCS.md`.

Before changing engine/editor/runtime architecture, read (or fetch the matching nodes `/rules/architecture`, `/rules/hot-iteration`, `/rules/ui`):

- `docs/ORBIT_ARCHITECTURE.md`
- `docs/ORBIT_HOT_ITERATION.md`
- `docs/ORBIT_UI_RULES.md` for UI/editor work

## Hot-iteration rules

1. **Every Orbit-owned development change must have an automatic reflection path.** Normal iteration must not require manually closing Orbit, running a full rebuild, locating a new executable, and reopening the editor.
2. Prefer the fastest safe path in this order: direct resource refresh, script reload, shader/pipeline replacement, in-process native DLL generation swap, automatic incremental Studio-generation handoff.
3. New C++ subsystems are hot-reloadable by default. Prefer stable host-owned state plus a versioned reloadable implementation module.
4. Do not expand the immutable host with ordinary engine functionality merely to avoid designing a reloadable boundary.
5. A candidate generation must compile, load, validate, and accept activation before the current working generation is retired.
6. Failed hot builds or loads must leave the running generation alive and usable.
7. Never keep raw function pointers into a DLL beyond the lifetime of the pinned generation. Use `HotReloadHost` interface visits/leases or another explicit lifetime mechanism.
8. Preserve authoritative state across implementation reloads when practical. If module-private state is required, provide versioned migration.
9. GPU resources from an old generation must use safe deferred retirement after GPU completion; do not destroy live Vulkan resources just because CPU code was replaced.
10. Use the central hot-iteration watcher/classifier. Do not add ad-hoc watcher threads for individual subsystems.
11. New source/resource extensions must be added to central classification and tests in the same change.
12. Unknown files under `engine/`, `apps/`, or `cmake/` must conservatively use the automatic native/generation fallback rather than being silently ignored.
13. Do not rebuild Studio for changes that can be handled as content, script, shader, or module reloads.
14. Keep translation units, public headers, and build targets narrow enough that one-file edits remain cheap.
15. Do not make release packaging or root executable copying part of the save-to-reflect inner loop.
16. `Orbit.exe` remains the one unified Studio entry point. Do not introduce a separate hot-reload launcher.
17. The automatic process-generation fallback is acceptable for the minimal host/ABI boundary and as a temporary migration path, but frequently edited systems should move to true in-process hot modules.
18. A new subsystem is not complete until its hot-iteration path is documented and testable.

## MCP parity

Read `docs/ORBIT_MCP.md` before adding or changing editor capabilities.

- Anything reachable in Orbit Studio must also be reachable over RPC and MCP. If a workflow step exists only as a UI click, extract the operation the button runs, make the button call it, register an RPC method for it, add a dedicated tool in `tools/mcp_server/orbit_editor_mcp_server.py`, and document it in `docs/ORBIT_MCP.md` in the same change.
- Do not drive Studio by simulating mouse or keyboard input to reach a goal. A missing RPC/MCP path is a gap to implement.

## Documentation upkeep

- Documentation is a tree of small blocks (`docs/ORBIT_DOCS.md`). Blocks live next to the code in `engine/<module>/docs/`.
- If a change makes a block's `invariants`, `diagnose` steps or `sources` untrue, update the block in the same change and set its `verified` commit.
- `python tools/orbit_docs_cli.py check` must pass (CI runs it). New long-form notes should be blocks, not new monolithic Markdown files.

## Completion check for code changes

Before considering a feature complete, verify:

- What happens when its implementation file is saved while Orbit is running?
- What happens when its shader/script/content file is saved?
- Does a failed patch preserve the active generation?
- Does state survive the patch where users would expect it to?
- Can any thread/GPU submission still reference the old generation when it is retired?
- Did the change accidentally add a manual restart requirement or separate launcher?

If the answer to the first two questions is “nothing until the developer rebuilds manually,” the feature violates the Orbit architecture.
