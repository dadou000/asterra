+++
path = "/world/roads-railways/runtime-validation"
title = "Road/rail runtime frames, authoring, tests and delivery gates"
kind = "design"
status = "planned"
summary = "Frame-relative vehicles and trains, NPC traffic, Studio authoring and MCP parity, diagnostics, physical test matrix and staged integration roadmap."
keywords = ["ship", "cruise ship", "car", "moving frame", "train", "physics", "npc", "editor", "mcp", "validation"]
related = ["/world/roads-railways", "/world/roads-railways/alignment", "/world/roads-railways/construction", "/editor/studio-ui/debug-center"]
+++

# Frame-aware operating environment: crucial difficult cases

A path is tied to an explicit reference frame, support entity or body surface at a timestamp; **do not assume planet-fixed geometry**. World transforms compose parent frames and local transforms with velocity and angular velocity. Car-on-cruise-ship is a canonical test, not a fringe exception:
- The ship has translational and angular motion, including roll and pitch. Car tires contact the ship's *moving deck*, with the correct relative contact velocities, normal force and friction.
- When parked, the car is carried by the ship without coordinate drift or spurious friction/impulses. Braking, driving or sliding uses relative deck velocity, not world-zero assumptions.
- If a car drives from shore over a ramp onto the ship and back, contact/support changes dynamically without teleportation, incorrect double transforms or velocity resets. Ramp endpoints can connect two moving frames; collisions and velocities use their instantaneous world states.
- Ship roll can cause sliding; player, cargo, passengers and vehicles can transfer support separately. Detachment conserves appropriate inertial velocity including angular contribution (`v_parent + omega × r + v_relative`), and reattachment avoids energy spikes.
- Moving train interiors, railcars, lifts, ferries and platforms use the same machinery for player/vehicle support, navigation and nested occupants. A passenger can walk inside a moving train, while the train follows planet-fixed rails; a carriage on a ship is a nested frame problem.
- Support contact is determined from real physics/surface identity, not viewport hover, camera proximity or a nearest-planetoid heuristic. Hierarchies support ownership changes, docking, partial overlaps and authority transfer in multiplayer.
- At high speeds, distant coordinate magnitudes, or when parent bodies rotate, preserve frame-consistent timestamps and precision. Never directly mix velocities from two frames without conversion.

Avoid recursively simulating all nested rigid bodies at full fidelity when not observed. A sleeping vehicle can remain welded/constraint-following its moving support until an interaction requires detailed contact; wake/sleep must conserve motion.

# Runtime networks, actors and condition

Lane graph and rail graph drive navigation but are not the collider. Cars use road signs, speed limits, junction priority, traffic lights, curves, available grip, crossings and dynamic obstructions. Trains use track alignment, rolling geometry, switches, signaling, block occupancy, stations and timetable/reservation logic where appropriate; rail gauge, cant and contact must match geometry. NPC and player vehicles may traverse the same created roads/rails. Routing can be hierarchical region→corridor→lane/track, with local high-detail avoidance and deterministic replanning on closures. Damaged roads/rails can produce speed restrictions, temporary closure, derailment risk or structural failure when simulated. Utility/conduit and roadside systems reuse attachment rules, rather than bespoke mesh transforms.

Traffic, rail control and infrastructure state persist independently of whether the region has rendered actors. Use coarse event-based or aggregated updates for far traffic, full vehicle dynamics only for locally relevant actors, and periodic async network updates. Regional AI traffic budgets do not force rendering/updating all planet traffic. Support timewarp through sensible state advancement, not multiplying expensive physics ticks without bound.

# Studio authoring and automation contract

One contextual editor inside Orbit.exe, progressive disclosure:
- Choose road/rail profile; click two nodes, drag Bézier tangents, choose direct/Bézier/routed connection via contextual/radial tools.
- Live editable station/profile gizmos for width, elevation, bank/cant, lane count, track gauge, shoulders, material and construction mode; drag a curve, preview terrain footprint, proposed bridge/tunnel, collisions and invalid constraints before commit.
- Auto-grade/radius/turn easing and railway cant with design-speed input and user overrides, snapping, junction repair and switch authoring, support/foundation preview; avoid multi-panel scavenger hunts.
- Interactive large region planner with deterministic seed and cost sliders, preview multiple alignments, progress/cancel, undo/redo, incremental updates and persistent author intent.
- Inspector tree for networks, nodes/edges, profiles, junctions, support structures, collision and generated children. Resizable views; coherent object selection and camera framing. Diagnostics remain accessible via **Debug**.
- Save/close/reopen must preserve authored graph, overrides, support anchors, condition and references; generated geometry can regenerate. Scene edits and derived revisions are distinguishable.

All meaningful operations should share the same domain commands for UI, CLI/RPC/MCP: create/modify/delete network/node/edge, connect, split/merge, set profile/speed/cant/construction, route preview/commit, query engineering warnings, bake/status/cancel, validate, save/reopen, report profiling. Return stable IDs, input revisions, generation IDs, error codes, dependency invalidations and artifact paths. No agent depends on mouse emulation. Exactly one canonical operation per task; do not fork agent and UI behaviors.

# Debug tests and evidence

Register executable tests under **Orbit Studio → Debug**, categories **Infrastructure**, **Terrain**, **Physics**, **Performance**, and **Regression**. Each test describes fixture, parameters, expected invariants, deterministic seed, owner, failure messages and captured build identity (running binary/assets commit/fingerprint, dirty indicator if relevant). Tests should have both UI and programmable invocation.

Minimum acceptance matrix:
1. Straight, Bézier and routed road across flat/steep terrain; cut/fill and grade warnings.
2. Railway opposing curves with cant transitions, turnout and switch; validate physical rail contact and gauge.
3. Intersection topology vs grade-separated crossing; no accidental lane/rail connection.
4. Bridge over water with supports and culvert; tunnel with actual void and portal; verify no flattened deck-underlay terrain.
5. Edit near a tile seam/cube face: no page discontinuity; CPU/GPU/contact parity.
6. Road damage/pothole/rail wear changes real contact only in explicitly physical detail bands.
7. Ship carrying parked car; ship pitching and rolling; car accelerating/braking/sliding.
8. Car crossing moving shore–ship ramp, then detaching/reattaching; coordinate and momentum invariants.
9. Passenger walking through multi-carriage moving train, including nested movable structures.
10. One corridor across an entire world region; local edit causes bounded invalidation, stable far LOD and no synchronous frame spike.
11. NPC route closure and switch/traffic conflict; save/reopen, streaming and timewarp.
12. Headless MCP command parity, deterministic replay and clean cancellation under rapid edits.
13. Profile on low-end GPU budget and full-capability GPU: CPU job time, GPU upload bandwidth, resident pages, collision cooks, visible LOD and frame variance.

The current Debug physical-page sanity check only verifies presence, revision and finite center height; tests above are **planned** until implemented and run.

# Implementation dependency sequence / gates

**Gate 0: verify the baseline** — build Harmony, confirm real `paths`, `path_geometry`, `studio_session` and physical-page API signatures, test isolated corridor generation. Fix compile/runtime failures first.

**Gate 1: semantic network and alignment** — authoritative graph with stable IDs, profiles, Bézier/transition evaluation, frame-resolved anchors and engineering warnings. Unit tests for curvature, grade, stationing, determinism.

**Gate 2: continuous geometry** — swept cross-section, lane/track references, materials and physical collision generation sharing the same stations; no visual/contact disagreement.

**Gate 3: ground construction** — nondestructive grading, cut/fill and drainage, streaming physical pages, revision-safe async updates. Explicit CPU/GPU/contact parity test.

**Gate 4: structures** — supported decks/bridges, portals/tunnel voids, foundation constraints and support attachments; water crossings without flattening terrain.

**Gate 5: topology** — roadway intersections, merges, rails, switch/turnouts, separated crossing and navigation graph validity.

**Gate 6: dynamic references** — moving deck and nested frames, transfer on/off ship, walking passengers, parent-relative collision and velocity.

**Gate 7: operating systems** — NPC traffic, trains, signals, closures, maintenance, damage and persistence.

**Gate 8: polish and deployment** — planet-scale async cache budgets, multi-resolution LOD, report/profiler captures, RPC/MCP parity, full UI and performance/regression gate.

At every gate: demonstrable native Studio test scene, executable Debug check, automated assertions, failure diagnostics and source-verified implementation state. Do not claim completion from a button existing alone.

## Current implementation increment: unified design preview

The engine now provides `path_geometry::BuildCorridorDesignPreview` (header `CorridorDesignPreview.hpp`) as a read-only adapter for a real `PathCenterline` and authored `PathProfile`. It shares `BuildPathDerived` output with the engineering analyzer and advisory cant/bank solver, and optionally supplies the terrain-offset sampler to the earthworks estimator. `CorridorDesignPreview` returns geometry, station-by-station findings, suggested bank values and optional cut/fill quantities without editing the source network. `Orbit.CorridorDesignPreview` is the integration test target.

**Known boundary:** This is a domain API for a future Studio corridor inspector; the current editor does not yet expose a dedicated road/rail inspector or a terrain sampler resolved from the active planetary frame. The supplied terrain callback must already express heights relative to the designed corridor datum. No claim is made that dragging a road in Studio will automatically update its warnings yet. The test and executable have not been compiled here.

## Live Studio path engineering RPC

`StudioPathNetworkHost::AttachRpc` now registers **`path.engineering`** alongside existing `path.geometry` / route endpoints. Supply `{\"edge\":\"<UUID>\",\"design_speed_mps\":20}`; the response includes `ready`, `valid`, committed derived-product `signature`, station count, per-station engineering findings and bounded suggested banking/cant angles. An edge whose derived geometry is not ready returns `ready:false`, never a fabricated preview. This is read-only and evaluates the same cached stations displayed by the editor.

This is a real Studio integration point for agents and future contextual property panels, **not yet a button or gizmo UI**. Cut/fill remains separate because resolving the active planet's correct terrain-height datum must precede sampling. A native build and RPC smoke test are outstanding.

## Studio Debug road/rail engineering inspector (current integration)

Under **Debug → Terrain / Infrastructure → Road / Railway engineering**, enter a real authored path edge UUID and design speed (m/s), then click **Analyze edge**. The panel calls the *same* `path.engineering` RPC used by agents and displays its returned engineering data or a failure message. It does not create/modify geometry, write a new path, or guess the selection. This is a functional read-only integration surface; the result is currently displayed as serialized structured data rather than a refined inspector table. Planned next: selection-aware edge discovery, station warning drill-down, direct camera framing and editable banking suggestions. Runtime verification remains outstanding.

## Debug edge discovery and warning navigation (next increment)

The **Road / Railway engineering** section now has a **Refresh path edges** action. It enumerates authored path-edge objects from the active scene hierarchy (bounded to 8,192 visited objects / 128 displayed edges), displaying their names as selectable buttons that populate the engineering UUID. **Analyze edge** uses the existing RPC, then presents station count, generation signature, warning count, per-station descriptions and suggested bank/cant sample count. It retains manual UUID input for large or external scenes.

This is intentionally **on-demand** enumeration so an open Debug panel does not repeatedly scan the scene every render frame. Results can become stale after a world switch; refresh to discover the active world's edges. The UI currently lists warning stations rather than navigating the camera or editing geometry. Build and runtime checks remain outstanding.

## Navigable engineering findings increment

`path.engineering` warnings now include `position_frame` and `position_local_meters` from the cached derived path's closest station. These coordinates are **frame-local**, not camera/world coordinates. Studio Debug presents findings as selectable rows, with details for measured value, limit, frame and local XYZ. Selection is reset on reanalysis and stale results clear when changing edges. Camera framing/viewport highlight are **not yet wired**: they require transforming the reported position through the active frame graph and a supported viewport navigation command. Compilation and runtime verification are pending.

## Build → Procedurals workspace (implementation)

A dedicated **Build → Procedurals** panel is now registered in Studio from `StudioProceduralsPanel.hpp`. The first Road/Rail workspace lists actual authored scene path edges on demand, selects by friendly name, accepts a design speed, calls `path.engineering`, and displays geometry revision, station count, a selectable engineering findings list, measured/limit values, reference frame and number of bank/cant recommendations. This is the canonical authoring entry point; **Debug** remains a diagnostic/testing surface. It does not create an alternative path authority or duplicate the analyzer.

**Current limitations:** read-only engineering inspection, no curve manipulation or write-back, no in-viewport warning overlay, no earthworks sampler yet. Refresh is required after modifying the world; the panel doesn't perform a full scene scan per frame. Build/runtime verification remains pending.

## Build → Procedurals: Cities: Skylines-inspired builder (Harmony update)

The existing **Procedurals** panel is now organized as an infrastructure construction workflow: Roads/Railways category choice; numbered selection, construction, alignment, and engineering preview sections; selectable authored segment browser; terrain/bridge/tunnel/deck construction modes; design speed and turn easing; editable Bézier handles; collapsible advanced catalog/version fields; and station-specific engineering findings.

**Apply to authored segment** uses `PathNetworkService::SetInfrastructureIntent` and, for Bézier edges, `SetBezierHandles`, instead of directly mutating scene objects. These existing authoring APIs integrate with Orbit's command/undo architecture. Engineering reports remain non-destructive. This is a **panel builder**, not yet a Cities: Skylines-style point-and-click terrain placement tool: Roads/Railways category selection is currently a workspace choice, not a profile-catalog filter, and bridge/tunnel/deck modes persist intent but do not imply finished structural geometry. Next gates are world-space placement previews, snapping, graphical node manipulation, profile card catalog and active-frame navigation.

Native build and runtime validation are pending.


## Build → Procedurals interactive creation (Harmony implementation)

Studio's existing **Build → Procedurals** panel is registered in `StudioApplication.cpp` and implemented by `apps/editor/src/StudioProceduralsPanel.hpp`. It supports Roads / Railways switching, browsing authored edges, construction mode, design speed, easing, Bézier tangent editing, undoable infrastructure intent edits and the existing engineering-analysis RPC.

The new **Create networks and segments** section additionally calls the *existing* `PathNetworkService` command-authoritative API to create named networks, create frame-point anchor nodes in the newly created network (explicit frame UUID and local coordinates), and connect any two existing nodes belonging to one network. Connections support direct, Bézier (editable endpoint tangent handles) and routed modes. Created edges are selected in the panel and listed after refresh; no parallel path serialization is introduced.

**Current limitations:** the frame UUID must refer to an existing valid frame; this UI does not automatically pick a terrain/geodetic anchor. Node creation currently targets networks created in this panel during the current session. The category switch is a UI workflow distinction rather than automatic road/rail profile assignment; the user still needs to set a suitable authored profile. Bridge/deck/tunnel options persist construction *intent* and do not imply geometry, support simulation or tunnel excavation exists. The tool has not undergone native compilation or runtime validation.
