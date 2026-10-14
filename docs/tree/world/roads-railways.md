+++
path = "/world/roads-railways"
title = "Roads, railways and supported infrastructure: canonical specification"
kind = "subsystem"
status = "planned"
summary = "Canonical architectural contract for Orbit's unified, editable road/rail/path infrastructure: authored networks, alignment, terrain and structural construction, frame-aware dynamics, persistence, editor automation and planet-scale LOD."
keywords = ["roads", "railways", "infrastructure", "corridors", "alignment", "terrain grading", "moving frames", "bridges", "tunnels"]
related = ["/world/paths", "/world/path-geometry", "/world/path-routing", "/editor/studio-ui/debug-center", "/world/roads-railways/alignment", "/world/roads-railways/construction", "/world/roads-railways/runtime-validation"]
+++

# Status and provenance

This is the **single canonical design specification**, consolidating user requirements from the Orbit roads/railways discussions and existing engine architecture. This is **not** an assertion that every feature below is implemented. The subdocuments distinguish architectural requirements from implementation sequencing. Any new road/rail feature should update these blocks and the owning module's documentation. User-established constraints take priority over implementation suggestions.

## Product intent / non-negotiables

1. **One unified corridor/network system** for roads, railways and reusable path infrastructure, not separate incompatible generators. Shared alignment, references, cross-section machinery, construction layers, terrain integration, caching, and authoring; mode-specific engineering rules, operating logic and assets.
2. **Works on terrain AND on supported construction.** A road may lie on bedrock, a bridge, a viaduct, an upper deck, a moving ship or vehicle platform. Surface ownership and support are explicit—not inferred from a terrain raycast.
3. **Authoring by nodes and curves.** Direct lines, editable Bézier/spline handles, terrain-aware routed paths, controllable arcs, smooth joins, split/merge/branch/junction edits. Curve evaluation and arc length must be stable.
4. **Engineering assistance without loss of control.** User-selectable width, cross-section, grade, curve radius, transition, banking/superelevation, railway cant, surface and wear. Automatic speed-aware radius/slope transitions, curve easing and rail cant with useful previews/warnings; advanced override remains available.
5. **Real physical geometry and contact.** Render surfaces, collision/contact and practical operating geometry must agree. Rail profile, wheel/rail contact, road cambers, ruts, cracks, potholes, sleepers and transitions can change actual contact where scale requires it; visual-only decoration is explicitly marked.
6. **Construction changes the world coherently.** Cut/fill, embankments, retaining systems, bridge piers, tunnel excavation, foundation support, drainage, water interaction and adjacent terrain must share one authoritative composition order with nondestructive authored edits.
7. **Player- and NPC-usable at game scale.** Vehicles, trains, AI routing, crossings, stations, signals, carriageways, sidewalks, parking, maintenance/damage and evolving infrastructure. Game-sensible simulation LOD rather than uniform expensive physical simulation.
8. **Planet-scale streaming.** Authored network persists while disposable visual/physical outputs stream according to observer and activity. Far geometry is cheap; local fidelity stays high. No global re-bake for a local road edit.
9. **Moving hierarchical reference frames.** A car aboard a cruise ship, a passenger within a train, a vehicle on a ferry or movable platform must behave coherently through support-frame changes, velocity and acceleration. Never assume every road is fixed to a planet.
10. **Studio-first usability.** Fast contextual UI, previews, undo/redo, draggable control points, quality-of-life automation, tests in **Debug**, plus parity through supported RPC/MCP commands. Never a separate editor/launcher.

## Domain ownership and data flow

**Persistent source of truth:** `engine/paths` semantic `PathNetworkRecord`, profiles, nodes, edges, anchors, revisions and operations. An infrastructure record adds mode rules and construction intent; do not duplicate coordinates across runtime and render subsystems.

**Derived caches:** `engine/path_geometry` alignment samples, lane/track references, swept cross-sections, junction patches; `path_routing` terrain-aware preferred alignment; `studio_session` construction registry, rebuild scheduling and residency; `terrain` composed material/surface changes; `terrain_gpu` physical-page products; distinct collision/nav and render caches. Derived outputs must be reproducible and disposable.

Conceptual dependency chain:
`Authored network + profile + support/constraints + time` → `resolved alignment & engineering checks` → `construction plan (cut/fill/structures)` → `authoritative surface/contact and drainage` → `meshes / physics / lane-rail graph / navigation / LOD` → `runtime and viewport`.

The renderer never authors its own road geometry. Network graph, route and collision are separate representations with shared IDs and revision fingerprints. Stage results atomically: old valid generation remains visible while a replacement builds.

## Explicit status boundary (Harmony 0.1.1)

**Source modules exist:** `engine/paths`, `engine/path_geometry` (including `CorridorSweep`, `CorridorTerrainSource`), `studio_session/CorridorConstructionRegistry`, and physical-page GPU terrain integration. They are partial foundations, **not proof** of end-to-end roads/railways, structural/floating support, traffic, vehicle dynamics, junctions or full test UI. A prior Debug terrain sanity test only checks page publication/revision/one finite sample; it does not validate a road, rail or collision outcome. Integration compilation and performance validation are still mandatory.

## Canonical detailed blocks

- [Alignment, network and design rules](roads-railways/alignment.md)
- [Construction, surface, infrastructure and representation](roads-railways/construction.md)
- [Runtime, frame physics, editor UX, validation and staged delivery](roads-railways/runtime-validation.md)

No independent road/rail specification may silently supersede this root. New component documents should link back here and state scope.
