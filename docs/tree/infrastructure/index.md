+++
path = "/infrastructure"
title = "Unified roads, railways and paths — M0 architecture"
kind = "subsystem"
status = "planned"
summary = "Catalog-driven infrastructure corridors share a semantic graph, alignment and cross-section evaluator; road, rail and path rules specialize engineering constraints and generated details."
keywords = ["infrastructure", "road", "rail", "tram", "corridor", "alignment", "cant", "condition", "contact", "MCP"]
sources = [
  "engine/path_geometry/src/PathDerived.cpp",
  "engine/path_routing/src/RoutePlanner.cpp",
  "engine/terrain_dependency/src/TerrainDependencyGraph.cpp",
  "engine/frames/src/FrameGraph.cpp",
]
invariants = [
  "The infrastructure graph and catalog are authoritative; rendering meshes, collision patches and LOD are derived caches.",
  "Road, rail, tram and path instances share one corridor/alignment representation while retaining type-specific design and junction rules.",
  "Terrain and supported construction are independent, mixable corridor-segment properties.",
  "Vehicle physics queries canonical geometry/condition independently of visual camera LOD; unchanged contact geometry is reused.",
  "Editor actions, RPC and MCP call identical validated commands with undo/redo and diagnostic return values.",
  "No terrain or collision geometry regeneration is required for an unchanged corridor at every simulation tick.",
]
depends_on = ["/rules/architecture"]
verify = [
  "Audit public path geometry/routing headers and tests prior to implementation.",
  "Create road and railway with identical shared corridor APIs; save/reopen and compare semantic state.",
  "Test road/rail geometry, continuity, cant, targeted invalidation and MCP command parity.",
  "Run python tools/orbit_docs_cli.py check and coverage --require-modules after introducing new modules.",
]
+++

## M0: verified repository starting points (Harmony)

The branch `0.1.1-Harmony` registers `path_geometry`, `path_routing`, `paths`, `frames`, `terrain_dependency`, `terrain_cache`, `terrain_gpu`, `procedural_graph`, `jobs`, `schema`, `commands`, `editor_rpc` and Studio UI modules in `engine/CMakeLists.txt`. `path_geometry` builds PathDerived/PathSource and depends on Frames, Math, PathRouting, Paths, Scene, Time and Universe. `path_routing` builds RoutePlanner/RouteDomains and depends on Terrain, Surface, Frames and Jobs. `RoutePlanner.cpp` hashes path profile width, lanes, minimum radius, maximum grade, bridge/tunnel permissions and terrain cut cost. `terrain_dependency` contains a spatial tile invalidation graph. `frames` contains a FrameGraph.

These observations **do not prove** existing modules implement cubic Bézier editing, clothoids, railway cant, continuous contact geometry, a complete road/rail catalog or the required studio tooling. Inspect public headers, tests, call sites and generated data before selecting integration points. Avoid shadow path, terrain or frame authority.

## Proposed ownership and construction sequence

1. **M0 audit:** run docs task/target routing, inspect existing path/profile, terrain evaluator and RPC/Studio command registration. Establish module dependencies, tests, serialization constraints and benchmark scenes.
2. **M1 semantic corridor/catalog:** typed network/segment/junction IDs; planet/frame IDs; versioned catalog and profile inheritance; authored anchor constraints; segment-level construction type (terrain / supported / mixed); composable bands. Add schema, transactions, RPC/MCP immediately.
3. **M2 alignment/meshing:** reuse existing path services where possible. Introduce station-based curve evaluation, tangent/normal frame, swept cross-section profiles, adaptive error-bounded mesh tessellation; generate road and rail from the same data contract. Retain deterministic canonical evaluation.
4. **M3 engineering:** road design/posted speeds distinct; friction, grade, superelevation and transition constraints. Rail gauge, design speeds, curvature continuity, rail cant and cant deficiency/excess, twists and turnout-specific constraints. Fit clothoids where feasible; expose diagnostic violations and locked control nodes, never silently claim compliance.
5. **M4 terrain:** nondestructive corridor deformation, cuts/fills, ballast beds, ditches, surface masks, localized dependency invalidation. Support crossing terrain / elevated / tunnels without topology corruption.
6. **M5 specialized generation:** shoulders, pavement, markings, sidewalks, rail heads, sleepers, ballast, switches, intersections, overhead equipment, supports, and rule-driven accessories; mixed tram/road corridors and grade-separated crossings.
7. **M6 condition:** shared station/lateral/time physical condition fields; macroscopic settlement, mesoscopic potholes/joints/rail geometry defects, microscopic friction/roughness effects. Distinguish structural/material condition from displacement; seed and revision persistent.
8. **M7 physics integration:** cached analytic/patch geometry and per-contact material sampling; independently selected physical accuracy, predictor-based warm prefetch, immutable revision handles. No GPU per-step readback or per-tick re-meshing.
9. **M8 planetary performance:** discrete geometry/condition revisions; bounded async generation, selective cache invalidation, contact and render LOD independent, orbital aggregate representation, distant physics frames, timewarp policy.
10. **M9 tooling:** comprehensive discoverable inspector, node snapping, non-blocking previews, reversible auto-repair, precise parameters; batch, preview, validate, query, explain, revision guard, job status and schema discovery over RPC/MCP.
11. **M10 acceptance:** canonical save/reload equivalence, live hot-iteration, LOD seam and geometry error tests, distant vehicle contact, damage correctness, benchmark scenes and deterministic procedural seed behavior.

## Entry slice and hard gates

**First thin vertical slice:** create and modify two-node road and railway semantic corridors; choose construction mode and catalog profile; render an adaptive curved road strip and two rail profiles with sleepers; inspect/edit via shared command service and RPC/MCP; undo/redo, save/reopen. No new physics solver or terrain refactor is authorized before a verified adapter contract.

**API policy:** identify a single authoritative mutation path for Studio and agents; allow atomic batches, dry-run alignment previews, granular diagnostics (object/segment/station, measured and target values, severity and advice), revision-aware jobs and idempotent request IDs.

**Runtime policy:** state is owned outside hot-reload generations, jobs are cancellable or pinned, old GPU resources retire behind fences, failed candidate builds keep the current Studio generation. New native modules require docs cards, targeted tests and hot-iteration classification.

## Open investigations

- Whether current `paths` and `path_geometry` already expose an editable station-based curve and sampling contract, including precision near celestial-scale positions.
- How terrain authored modifiers compose with current physical terrain pages and GPU clipmaps, and how revision invalidation traverses both.
- How Studio RPC commands bind to the editor's authoritative document/transaction service.
- Appropriate rail cant and turn-transition limit tables for selected catalogs; never embed universal constants as a legal standard.
- Cross-section continuity and mesh/collision consistency around junctions, turnouts, cracks, topology changes and structurally dynamic supports.
