+++
path = "/world/roads-railways/spec-completeness"
title = "Road and railway specification completeness and implementation traceability"
kind = "concept"
status = "experimental"
summary = "Coverage of every core roads/railways requirement against authoritative Orbit code, UI/RPC, tests, and objective acceptance gates; design inclusion is distinguished from working functionality."
keywords = ["roads", "railways", "coverage", "acceptance", "construction", "runtime", "junction", "turnout", "specification", "QA"]
related = ["/world/roads-railways", "/world/roads-railways/alignment", "/world/roads-railways/construction", "/world/roads-railways/runtime-validation", "/world/roads-railways/transport-foundations"]
sources = ["engine/paths/include/orbit/paths/PathNetwork.hpp", "engine/path_geometry/include/orbit/path_geometry/PathDerived.hpp", "engine/path_geometry/include/orbit/path_geometry/TransportJunctions.hpp", "engine/path_geometry/include/orbit/path_geometry/RailInterlocking.hpp", "engine/studio_session/src/StudioSession.cpp", "apps/editor/src/StudioProceduralsPanel.hpp", "apps/editor/src/StudioPathNetworkHost.cpp"]
verify = ["ctest -R Orbit.PathSplit", "ctest -R Orbit.PathSource", "ctest -R Orbit.TransportJunctions", "ctest -R Orbit.JunctionConflicts", "ctest -R Orbit.JunctionSignalPhases", "ctest -R Orbit.RailInterlocking", "python tools/orbit_docs_cli.py check"]
+++

# Audit scope and interpretation

The canonical design is divided among this document's siblings. A requirement is **specified** only if the intended behavior, owning subsystem, acceptance criterion and exclusions are recorded. **Implemented** means the code path exists; **tested** means the relevant test was actually run and passed at the referenced revision; **integrated** means the normal Studio/runtime consumer uses the produced result. No percentage is inferred from source-line count, tests registered, UI buttons or GitHub pushes.

Status legend: **DOMAIN** = functional CPU/domain API present, not necessarily integrated in runtime; **PARTIAL** = one end-to-end subpath or prototype exists; **PLAN** = design/intent recorded without production consumer; **UNVERIFIED** = not run successfully on native hardware in this audit. All evidence below is source-inspection evidence, not an MSVC or GPU test pass.

# Requirement-to-evidence matrix

| ID | Acceptance intent | Owner / inspected evidence | Status | Missing acceptance gate |
| --- | --- | --- | --- | --- |
| N01 | Unified stable-ID network for road/rail | `engine/paths/PathNetwork` | PARTIAL | Explicit stable junction, turn, track, attachment IDs and persisted revisions |
| N02 | Direct, cubic Bézier and routed authored edges | `PathEvaluation`, `PathSource`, `path_routing` | PARTIAL | Multi-segment spline, arc-length inversion, planetary edge/chunk seam |
| N03 | Split without changing geometry or deleting original identity | `PathNetworkService::SplitEdge`, `Orbit.PathSplit` | DOMAIN | Remap attachments and support mixed-frame, routed and surface-anchor split |
| N04 | Merge, branch, loop, symmetric lane transitions | `PathNetwork`, junction preview | PLAN | Persistent merge operations, reference remap and editable lane transition |
| N05 | Profiles, classifications and custom gauge/width/crossfall | `PathProfile`, `TransportSections` | PARTIAL | Persist per-edge/per-station road and rail section controls in assets |
| N06 | Earth/ship/vehicle/time-resolved anchors | `PathSource` / frames | PARTIAL | Moving support contact and live moving-frame terrain/construction pipeline |
| E01 | Horizontal grade/radius/sight/curvature | `CorridorEngineering` | PARTIAL | Clothoids, visibility occlusion and vertical-radius design |
| E02 | Rail cant vs multiple train speeds, reversals | `RailCantDesign` | DOMAIN | Route-level cant transition constraints, contact parity and special turnout rules |
| E03 | Automatic turn easing with user-overridden constraints | advisory `path.engineering` | PARTIAL | A bounded editable geometry solver, preview alternatives and commit |
| E04 | Collision, lane/reference stations, road crown | `CorridorSweep`, `CorridorDerived` | PARTIAL | Runtime collider insertion, collision/render parity GPU smoke |
| E05 | Physical railhead and true wheel/rail contact | `TransportSections` top-strip, rail frames | PARTIAL | Swept rail profile, frog/flange geometry and bogie contact dynamics |
| E06 | Road curbs, sidewalks, shoulders, markings, utility conduits | road sections (crown/shoulders) | PARTIAL | Materials, sidewalks/medians/drains as semantic layers and physical contact |
| C01 | Terrain grading and cut/fill preview | `CorridorEarthworks`, construction physical pages | PARTIAL | Support slope, material volume, terrain/physics/GPU numerical parity |
| C02 | Non-destructive overlapping construction layers | `CorridorConstructionRegistry` | PARTIAL | Explicit precedence and interaction against buildings/water/soil |
| C03 | Bridges/viaducts/supports/foundations | `constructionMode` authored; grading skipped | PLAN | Structural assembly, collision, support frames and subsoil anchoring |
| C04 | Tunnels and cut-and-cover | `constructionMode=tunnel` authored | PLAN | Actual void, portal, ventilation, surface layers and section constraints |
| C05 | Drainage, culverts, waterways and wet friction | design only | PLAN | Hydrology-authoritative flow interfaces and draining failure test |
| C06 | Road/track condition, rut/pothole/wear/repair | design only | PLAN | Sparse persistent damage with physical contact and repair transitions |
| J01 | Explicit ID junction topology; no spatial auto-connect | `TransportTopology` | DOMAIN | Real turn-object semantics and grade-separated tests in Studio |
| J02 | Junction lane movement curve previews | `TransportJunctions` + read-only RPC | PARTIAL | Actual trim/weld/mesh collision and bidirectional lane rules |
| J03 | Road overlap/merge/conflict graph | `JunctionConflicts` | DOMAIN | Pedestrian conflict, vehicle envelopes and clearance timing |
| J04 | Advisory compatible traffic phase sets | `JunctionSignalPhases` | DOMAIN | Persistent signal layout, actual phase timing and traffic actor enforcement |
| J05 | Turnouts, railway track transitions | rail branch preview | PARTIAL | True switches, blades, frog/guard rails, continuous gauge/contact |
| J06 | Occupancy/lock/release state | `RailInterlocking` (ephemeral) | DOMAIN | Authored switch identity, block controller and train-dependent occupancy |
| J07 | Level crossings with trains and barriers | mixed-node annotation and no cross-mode routing | PARTIAL | Barrier control, conflict policies, closure/signal handshake |
| R01 | Car traffic, priority, turning and parking | lane graph | PLAN | NPC/car runtime driving and graph-based dispatch |
| R02 | Train physical bogies, couplers, stations and timetables | rail contact references | PLAN | Near physical train, far timetable, track occupancy and stations |
| R03 | Ship-car, ramp transfer, nested moving passengers | frames / design | PLAN | Relative contact velocity, momentum and support transfer smoke |
| R04 | Closures, multiuser, streaming/timewarp traffic | design only | PLAN | Stable state under unloaded actor simulation and timewarp |
| P01 | Planetary async chunked path products | `UniverseBoundPathCache`, sampling budget | PARTIAL | End-to-end chunk scheduling, per-page cache ownership, cancellation |
| P02 | Near/mid/orbit representations, sleepers as GPU instances | CPU sleeper placement | PLAN | Actual GPU instancing, LOD thresholds, hysteresis and budget regressions |
| P03 | Local semantic revision invalidation | StudioSession/corridor registry | PARTIAL | Concurrent edits, multiple sections, bounded GPU upload profiling |
| U01 | Studio unified Build > Procedurals controls | `StudioProceduralsPanel` | PARTIAL | Drag/paint, snapped 3D gizmos, turnouts and station section controls |
| U02 | Agents via canonical read/write RPC and MCP | `path.engineering`, `path.junction_preview`, `path.edge_split` | PARTIAL | Complete CRUD, secure agent privileges, cancellation, recordings |
| U03 | Debug tests + build fingerprint and captures | existing Debug center; CPU tests | PARTIAL | Named end-to-end road/rail fixtures and native screenshot/profiler artifacts |
| U04 | Undo/reopen preserves semantic authority | `CommandService`, `Orbit.PathSplit` | PARTIAL | Save/reopen of split, junction edits, switch states and moving supports |

# Completeness findings

**The specification describes the intended scope but is NOT implementation-complete.** Crucial additions required for an actionable engineering contract:

1. **Persistent semantic schema**: road lane directions, rail-track IDs/gauge, turnout object, signed movements, junction priority, signal object, support assembly, surface material slots, damage state, and station-key overrides must have versioned serialized IDs and explicit migration/undo behavior.
2. **Derived-product transaction**: same source revision/frame/timestamp for mesh, contact, graph and construction footprint. A build must publish atomically or retain the last good complete generation. No mixing stale collision with new visuals or rail gauges.
3. **Geometry ownership at intersections**: define trim stations on incoming corridors, cut-off collision triangles, patch boundary welds, normals, material seams, lane continuity and allowable overlap. A convex preview hull is not a real intersection mesh.
4. **Railway turnout acceptance**: allow actual selected branches only; gauge at both heads, flangeways, frog, guard rails, switch blade throw travel, taper/cant limits and locking must agree with bogie contact and signal state.
5. **Construction conditions**: support classes ground/bridge/deck/tunnel/moving deck need distinct terrain, water, structural and collision authorities; no false flattening under deck or invisible solid terrain through tunnel.
6. **Multi-frame dynamic contract**: transforms, linear/angular velocities, contact normals and support changes evaluated at one fixed simulation time; tested with moving ship/car and ship-to-shore ramp.
7. **Planetary streaming contract**: stable chunk/station IDs, explicit bounded work and memory, per-representation LOD/residency, exception/backpressure, revisions, queue priorities and deterministic timewarp; a single global mesh is unacceptable.
8. **Playable operation and automation**: NPC path following, traffic signals, trains/occupancy, maintenance and rail switch authoring must be integrated via the existing commands/RPC/Debug, with no second hidden simulation graph.

# Required release gates

| Gate | Pass condition | Status |
| --- | --- | --- |
| A: domain/build | Windows build + all Orbit.Path*/Transport*/Junction*/Rail* CTests green | UNVERIFIED |
| B: authoring/replay | Undo/redo and save/reopen with split, junction, one profile override; byte-stable derived products | OPEN |
| C: physical road | 3-way/T/cross intersection trims and welds including collision, continuous driving and lane routing | OPEN |
| D: physical rail | Three-way turnout with real railhead/flange contact, locking and occupied rejection | OPEN |
| E: world construction | Cut/fill and bridge/tunnel/culvert correctness across terrain tile boundaries | OPEN |
| F: moving frames | Car on pitched ship and crossing a ramp without impulse/velocity resets | OPEN |
| G: planet scale | >100 km line streamed in spatial chunks; local edit bounded cost; near/mid/orbit capture | OPEN |
| H: gameplay | Player/NPC car and train route traversals, signal conflicts, persisted operations | OPEN |
| I: automation | All authoring operations through one RPC/MCP domain service, native Debug fixtures and captures | OPEN |

**Completion policy:** no gate may be marked passed merely because a UI button exists or a unit test target is registered. Store a reproducible test revision, world fixture, running binary fingerprint and profiler capture.

# Implementation order

1. Verify compiler and source regression for new transactional edge splits and bounded geometry sampling.
2. Finish persistent network node/edge/turnout/lane profile schema and editor controls.
3. Corridor seam/junction patch trimming + collision/nav, then true rail turnout with locked branch.
4. Ship/vehicle moving support-frame collision contract and real bridge/tunnel contact.
5. Streaming/LOD, hydrology/material condition, traffic/train simulation and full Debug/MCP parity.
