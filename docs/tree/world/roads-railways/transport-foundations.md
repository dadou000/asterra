+++
path = "/world/roads-railways/transport-foundations"
title = "Road and rail cross-section and network topology foundation"
kind = "concept"
status = "experimental"
summary = "Road crowns and shoulders, configurable railway railhead strips and contact frames, bounded sleeper instancing, and explicit stable-ID junction/topology derivation."
keywords = ["transport", "road", "railway", "cant", "gauge", "sleepers", "contact", "topology", "junction"]
sources = [
  "engine/path_geometry/include/orbit/path_geometry/TransportSections.hpp",
  "engine/path_geometry/src/TransportSections.cpp",
  "engine/path_geometry/include/orbit/path_geometry/TransportTopology.hpp",
  "engine/path_geometry/src/TransportTopology.cpp",
  "engine/path_geometry/include/orbit/path_geometry/TransportJunctions.hpp",
  "engine/path_geometry/src/TransportJunctions.cpp",
  "engine/path_geometry/include/orbit/path_geometry/RailInterlocking.hpp",
  "engine/path_geometry/src/RailInterlocking.cpp",
  "engine/path_geometry/include/orbit/path_geometry/JunctionConflicts.hpp",
  "engine/path_geometry/src/JunctionConflicts.cpp",
  "engine/path_geometry/include/orbit/path_geometry/JunctionSignalPhases.hpp",
  "engine/path_geometry/src/JunctionSignalPhases.cpp",
  "engine/path_geometry/include/orbit/path_geometry/InfrastructureSectionBuilder.hpp",
  "engine/path_geometry/src/InfrastructureSectionBuilder.cpp",
]
symbols = ["MakeRoadSweep", "MakeRailSweep", "RailContactReferences", "CompileTransportTopology", "PlanInfrastructureSection", "BuildTransportJunction", "RailInterlocking", "AnalyzeJunctionConflicts", "SuggestRoadJunctionPhases"]
invariants = [
  "Network authority remains in engine/paths; road/rail representations are disposable derived products.",
  "Railhead contact reference coordinates match the swept railhead strip station frame, gauge and cant.",
  "Rail and road routing never connect simply because their geometry intersects.",
  "Sleeper instance budgets are bounded and generated from stable station distances.",
]
related = ["/world/roads-railways", "/world/roads-railways/alignment", "/world/roads-railways/construction", "/world/path-geometry"]
verify = ["ctest -R Orbit.TransportSections", "ctest -R Orbit.TransportTopology", "python tools/orbit_docs_cli.py check"]
+++

# Purpose and precise status

Implement mode-specific construction recipes without building a second road/rail network. Both modes consume an authoritative network, resolved source stations and profile inputs. Road and railway products use the existing CorridorSweep renderer/collision derivation rather than disconnected visual and physics meshes.

# Road cross section

MakeRoadSweep emits a three-point crowned pavement with separate shoulder bands. Parameters are lane count, lane width, shoulder width, crown crossfall and shoulder crossfall; numeric limits reject impossible sections. A later version should add asymmetric lane groups, medians, islands, edge curbs, sidewalks, bike tracks, drains, material band labels, profile station keys, variable-width intersections and physical deformation layers.

Crossfall is actual geometry. A lane-following vehicle must eventually consult the same bank and friction/contact product; painted textures alone are not a substitute for surface physics.

# Railway section and operational contract

MakeRailSweep emits track formation and paired top railhead contact strips for one or more tracks, with configurable gauge, centre spacing and head height. RailContactReferences gives the two head points and contact normal at each path station using the same station-local cant transformation. RailSleeperPlacements emits stable instance transforms at equal along-track increments with a hard maximum instance count.

These are not fully modelled rail solids. The existing generic CorridorSweep expects strictly monotonic lateral cross-section coordinates; a real rail web, foot, head overhang, flangeway and frog need a separate true swept rail extrusion. Sleeper instances are not rendered or colliding until a consumer instantiates them. Contact reference points are not a conicity, flange or creep-force model.

For correct railway operation, maintain a shared gauge, section geometry, cant, station transform and timestamp between renderer, collision contacts, bogie solver and train steering. Switches, turnouts, guard rails, crossovers, sleepers on tight curvature, block occupancy and interlocking must remain explicit engineering and operational features.

# Explicit route topology

CompileTransportTopology produces directed travel arcs and mode-separated undirected connectivity components from supplied stable IDs, plus validation findings and junction records. A one-way link has only its declared arc. An intentional shared-node road/rail conflict is marked as a level crossing but never permits a car to turn onto rails or a train to turn onto asphalt. Pure spatial overlap, including an elevated railway crossing a street, never creates a hidden junction.

Turn lanes, rail point/blade connectivity, lane merges and controlled intersection conflict zones are separate descendants of this semantic graph, not inferred from triangle adjacency. No generated topology record becomes scene authority.

# Integration, build and performance boundaries

The new APIs are compiled into OrbitPathGeometry as a small native increment using the current automatic generation fallback. They do not add a file watcher or separate launcher. Add section recipes to the catalog/profile resolver; pass per-edge construction parameters through the existing PathNetworkService/RPC layer. Derive products on workers from immutable snapshots, fence publication against object/source revision, and preserve the previous valid product during a failed build.

Long corridors should be sliced into bounded tiles, keep sleeper transforms as GPU instances and only instantiate contact solids near active vehicles. Nearby physical railway gauge cannot be approximated by distant render LOD. Do not synchronously generate sleepers for a planetary-length line; the current function explicitly rejects excessive instance counts until an incremental chunk consumer is in place.

The same increment now addresses an earlier data-consistency bug in BuildCorridorDerived: after refinement, lane points, lane reference graph, navigation stations and boundary/debug lines are regenerated from the refined station product. Lane points follow the swept primary surface for crown and bank. This aligns sample counts and geometry at the per-edge level. Remaining scope: exact junction connections, independent road-versus-rail navigation semantics, physics contact cooking, and a shared revision-fenced cross-subsystem publication.

# Tests and remaining proof

TransportSectionsTests covers road sweep output, railhead gauge/cant geometry, sleeper spacing, bounded work, invalid profile and invalid station/cant combinations. TransportTopologyTests covers road/rail components, one-way arcs, explicit mixed crossings, ordering stability and rejected invalid links. They are isolated CPU checks; native build and execution, Studio Debug registration, GPU rendering and physics contact parity are still unverified.

Next: bind the profile catalog and Build panel; implement sectional physical rail profiles, wheel-rail dynamics, turnout/junction generators, road marking/signal rules, earthwork and bridge/tunnel construction, and streaming contact/render cache ownership.

## Second increment: refined references

CorridorDerived now regenerates lane positions, graph nodes/edges, navigation samples and debugging lines after adaptive corridor refinement. Primary lane references interpolate the actual swept pavement/formation vertices, so crossfall and banking affect the generated positions rather than just visual mesh vertices. Both collision and the derived references share the same station revision and count. The extended CorridorSweepTests asserts lane count, station parity, graph-node parity and crowned road lane elevations. This is still a CPU derivation test, not an NPC or wheel physics integration test.

## Curvature correctness patch

CorridorEngineering now computes horizontal plan-view curvature by projecting station tangents into the local terrain tangent plane, using horizontal span for the curvature denominator. A purely vertical crest or sag therefore no longer looks like a plan-view corner and does not generate false lateral-acceleration and cant/superelevation recommendations. Invalid vertical/collapsed spans report InvalidInput instead of dividing by zero. The CorridorEngineeringTests include a crest regression.

Further precision work: discrete station-angle curvature is still a sampled approximation, not an analytically integrated clothoid or a complete rail design code; detailed vertical-radius checks and horizontal/vertical sight-distance constraints remain to implement.

## Rail cant deficiency and mixed operating speeds

RailCantDesign (RailCantDesign.hpp/.cpp) adds a railway-specific advisory solver. It converts curvature and speed to physically defined rail-height difference across gauge, and checks the same curve at a minimum (e.g. freight) speed and maximum (e.g. passenger) speed. Limits for maximum installed cant, fast-train cant deficiency, slow-train cant excess and cant runout gradient are independent and user-editable. It performs forward/backward runout limiting, returns recommended cant in metres and radians, and emits warnings where mixed-speed constraints or available runout cannot be satisfied. It does NOT silently reshape authored curves, and it is NOT certified to any particular national railway standard.

The contact reference validator additionally requires normalized, mutually perpendicular, consistently handed station frames so contact points agree with the already-authored section geometry. Any malformed source basis is rejected rather than quietly constructing a different contact plane.

Test targets: Orbit.RailCantDesign, Orbit.TransportSections, Orbit.TransportTopology, Orbit.CorridorSweep and Orbit.CorridorEngineering. Compile and runtime checks still depend on Windows Studio/CI.

## Variable-width travel references

The refined geometry adapter also maps road lane centrelines to the actual usable cross-section width of the first sweep band at every station. Width keys and gradual widening/tapering now move the lane samples and adjust NavSample half-width, rather than leaving navigation lanes at the fixed initial width. The CorridorSweep regression checks narrowing/widening geometry, lane centres, and NavSample width. LaneReference.lateralOffsetMeters is a nominal legacy value; station-specific points are authoritative for dynamic cross-section position. Junction lane-count transitions and true lane merges still require explicit topology products.

## Studio end-to-end integration increment

StudioSession's existing rebuild resolver now invokes PlanInfrastructureSection for the supported rural-road and ballasted-rail catalog IDs instead of independently hard-coded triangle bands. The road recipe derives crowned pavement and real shoulders; railway recipes derive formation, paired railhead strips and optional speed-aware cant from the same station sampling used to build the physical/path mesh. Rail formation width is 4.035 metres by default (configurable through profile integration in subsequent stages).

The authoring service accepts terrain, supported, bridge, deck and tunnel construction intents. Ground grading is only published for mode terrain; changing to bridge/deck/tunnel/supported removes the continuous grading influence and waits for the appropriate future foundation/excavation runtime rather than incorrectly flattening the surface. The canonical Studio Build → Procedurals panel assigns a supported catalog to new segments and shows the real cache's derived vertex/contact counts, pending state and errors.

The Studio path.geometry and path.engineering reports should consume this same derived construction cache when available; integration verification must ensure there is no duplicate generic mesh masking a newer construction result.

## Build → Procedurals construction cache coherence

The editor's StudioPathNetworkHost now prefers the committed construction product from StudioSession.PathProducts when one exists. A late cache publication causes RPC derived geometry to refresh even if authoring revisions have not changed; the path.geometry and path.engineering RPCs thereby describe the same production geometry as the terrain construction path. The engineering response exposes construction readiness, dirty state, and rail-specific cant samples/warnings for the ballasted-rail catalog. Both generic and rail-specific bank recommendations remain advisory; turn easing controls application of generated rail cant in the cache, not persistent modification of control points.

The panel's category selection sets actual catalog intent for newly connected edges, and selecting an existing edge restores its category. The panel shows derived mesh/collision counts, revision, blocked builds and pending work; new frame point nodes can use the active celestial body's explicit reference frame without manually copying its UUID. Existing workflows with manually supplied frame IDs remain supported.

User-created bridges, supported decks and tunnels store their construction intent and can be inspected, but do not yet generate structural supports or tunnel voids. No continuous terrain cut/fill is published for those modes. A native end-to-end visual/simulation smoke test is still required.

## Junctions and rail-switch reservation (Harmony large pass)

BuildTransportJunction takes exact authored node identity, explicit endpoint orientation, a common frame, and the existing derived corridor station/lane products. It generates stable same-mode Hermite road-turn and rail-branch alignment previews, a bounded road footprint hull and typed findings for invalid geometry, distant endpoints, mixed frames or excess workload. The footprint is **NOT yet collidable junction pavement**, and all road/rail alignments are candidates, never authority. Rail and road route movements do not interchange; an overpass cannot create a junction by spatial overlap alone.

RailInterlocking is a pure ephemeral switch reservation model requiring an explicit stem, straight and diverging edge. It blocks conflicting reservations, locks alignment during reservations, rejects unauthorized releases and disallows throwing a switch while occupied. It is not a persistent rail signal system or safety-certified control.

StudioPathNetworkHost registers read-only path.junction_preview(node UUID, optional include_samples bool) returning frame, connection candidates, preview hull dimensions and typed findings. Build → Procedurals exposes the same RPC through a node browser, with no independent topology store. Only bounded geometry is emitted. Tests: Orbit.TransportJunctions and Orbit.RailInterlocking. The actual runtime build, switch geometry, continuous junction collision and GPU/physics smoke test remain outstanding.

## Intersection conflict pairs

AnalyzeJunctionConflicts consumes the exact frame-local movement polylines produced by BuildTransportJunction and computes deterministic pairs which share a vehicle entry/exit lane or physically overlap within configurable road/rail vehicle sweep envelopes. It uses true double-precision 3D segment distance, so elevated crossings clear the underlying road when there is sufficient vertical separation. Pair counts, segment comparisons and returned conflicts are separately capped; truncation is explicit, never described as a complete traffic phase safety result. Studio's path.junction_preview and Build → Procedurals surface the conflict count and truncation indicator. This supports future phase grouping/interlocking but does not yet control traffic lights, signal aspects, player vehicle behaviour or any collider.

## Advisory road conflict-free phase groups

SuggestRoadJunctionPhases deterministically groups the complete road candidate movement graph into independent sets with no known route conflict inside any group. It refuses truncated/invalid conflict reports, excessive candidate counts and mixed rail traffic (which requires separately authored rail interlocking). Its result is solely a planning suggestion: no signal timing, priority, pedestrian clearance, yellow/all-red phases or automatic control. path.junction_preview includes grouping status and movement indices; Build → Procedurals exposes the phase count. Tests cover determinism, safe grouping, capacity rejection, truncated conflicts and rail interlocking exclusion.

The railway connection preview now samples paired swept railhead vertices for the supported single-track 7-vertex-per-station formation/rail geometry instead of misreporting the formation lane datum as track height. Unsupported or generic unconstructed rail meshes cannot claim validated track connection geometry. Duplicate incident edges are rejected to avoid phantom repeated movements.

## Geometry and signalling guard corrections

Road footprint previews triangulate from the boundary of their convex hull rather than an assumed interior semantic-node location. This prevents invalid fan triangles for asymmetrical junctions. Rail presence is tracked at the approach level, not only through successfully generated rail movement samples; advisory road phases are unavailable whenever a rail approach is connected to the same semantic node, including unsupported rail geometry. This cannot be bypassed by missing track contact data.
