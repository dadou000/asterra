+++
path = "/"
title = "Orbit engine documentation"
kind = "root"
status = "stable"
summary = """
Orbit is the C++23 engine and unified Studio editor for Asterra: a planet-scale, persistent, \
runtime-authored world with GPU-driven terrain, atmosphere/clouds, lighting and simulation. \
Start at the section that matches your task; every node summarises everything below it."""
keywords = ["orbit", "engine", "overview", "start", "index", "architecture", "agent"]
related = ["/docs-system"]

[routes]
"rendering, terrain, clipmaps, GPU, shaders, lighting, atmosphere, clouds" = "/rendering"
"terrain looks wrong, holes, cracks, rings, popping, wrong detail level" = "/rendering/terrain/clipmaps/debugging"
"terrain edit rebuilds too much, GPU terrain cache, regeneration" = "/rendering/terrain/invalidation"
"planets, stars, orbits, rotation, rings, gas giants, black holes" = "/celestial"
"editor, Studio UI, viewport, panels, camera, toolbar, reports" = "/editor"
"add or change an editor capability, RPC method, MCP tool" = "/editor/mcp-rpc"
"projects, worlds, scene, commands, undo, assets, plugins" = "/authoring"
"planet coordinates, universe, fields, paths, procedural graph" = "/world"
"types, math, jobs, time, frames, platform, window, hot reload core" = "/foundation"
"executables: Studio, player, sandbox, build CLI" = "/apps"
"dev server, probes, python tooling" = "/tools"
"hot reload, save-to-reflect, rebuild, restart" = "/rules/hot-iteration"
"where should this change go, avoiding duplicate systems" = "/rules/placement"
"module dependencies, ownership, what may include what" = "/rules/architecture"
"how are these docs organised, how do I write a block" = "/docs-system"
"old specs, milestone records, release state, known problems" = "/history"
"an old long document that has no structured node yet" = "/legacy"
+++

## How to use this tree

1. **Orient.** Read the summary of the section that matches your task, then follow its
   *where to go* hints. Do not read whole documents when a node answers the question.
2. **Before editing code**, call `docs.for_target(<file or symbol>)`. It returns the
   invariants that must keep holding, what depends on the code, and the rules that apply.
3. **Before changing behaviour**, write the placement note required by `/rules/placement`.
4. **After editing**, run the `verify` steps of the nodes you touched and update their
   `invariants` and `verified` commit if the facts changed.

## Sections

| Section | What lives there |
| --- | --- |
| `/rendering` | Terrain (generation, clipmaps, GPU caches, invalidation, water), atmosphere, clouds, lighting, post-process, RHI and render graph, volumes, shading. |
| `/celestial` | Bodies as astronomical objects: orbits, rotation, gravity, stars, eclipses, representation ladder, globes, rings, giants, compact objects, small bodies. |
| `/editor` | Studio: viewport, session, model, UI toolkit, issue reports, the RPC/MCP automation layer. |
| `/authoring` | Documents, schema, semantic scene, commands/undo, selection, plugins, content/assets, cooked projects. |
| `/world` | Planet coordinates, universe and frames, fields, surfaces, authored constraints, procedural graph, paths. |
| `/foundation` | Core, math, jobs, time, frames, platform, runtime session, JSON-RPC core, hot reload. |
| `/apps` | Studio (Orbit.exe), Player, Sandbox, build CLI and BuildService. |
| `/tools` | Dev server, hot-reload probe/module, Python tooling. |
| `/rules` | Normative rules: architecture, hot iteration, MCP parity, change placement, UI, completion checks. |
| `/docs-system` | The block format, the docs tools and how to keep blocks fresh. |
| `/history` | Version specs and milestone records (V0.0.3-V0.0.7), problem tracker, Godot-era documents. Records, not rules. |
| `/legacy` | Remaining long documents indexed per section. |

## Repository map

```text
apps/        editor (Orbit.exe Studio), player, sandbox, build CLI
engine/      one directory per module: include/orbit/<module>, src, tests, docs
tools/       MCP servers (live Studio + docs), validators, benchmarks
docs/        long-form specs, research and the docs tree (docs/tree)
code_change/ placement notes written before changing existing behaviour
```

Modules own their types and talk through public headers, IDs and data objects
(see `/rules/architecture`). Documentation for a module lives in `engine/<module>/docs/`
and is attached to the tree by the `path` in its front matter.
