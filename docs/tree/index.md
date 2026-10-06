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
"editor, Studio UI, viewport, panels, camera, toolbar" = "/editor"
"add or change an editor capability, RPC method, MCP tool" = "/editor/mcp-rpc"
"hot reload, save-to-reflect, rebuild, restart" = "/rules/hot-iteration"
"where should this change go, avoiding duplicate systems" = "/rules/placement"
"module dependencies, ownership, what may include what" = "/rules/architecture"
"how are these docs organised, how do I write a block" = "/docs-system"
"an old design document, milestone spec or research note" = "/legacy"
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
| `/rendering` | Terrain (generation, clipmaps, GPU caches, water), atmosphere and clouds, lighting, shading, performance work. |
| `/editor` | Studio: viewport, navigation, panels, the RPC/MCP automation layer. |
| `/rules` | Normative rules: architecture, hot iteration, MCP parity, change placement, UI, completion checks. |
| `/docs-system` | The block format, the docs tools and how to keep blocks fresh. |
| `/legacy` | Existing long documents, indexed per section until they are migrated. |

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
