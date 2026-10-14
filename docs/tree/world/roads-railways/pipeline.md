+++
path = "/world/roads-railways/pipeline"
title = "Roads and railways production pipeline: authority to GPU, contact and simulation"
kind = "concept"
status = "experimental"
summary = "Execution stages, data contracts, ownership, invalidation, atomic CPU publication, GPU and physics handoffs, multiframe safety and measurable acceptance for Orbit roads and railways."
keywords = ["roads", "railways", "architecture", "pipeline", "junction", "GPU", "collision", "render", "navigation", "streaming"]
related = ["/world/roads-railways", "/world/roads-railways/road-junction-construction", "/world/roads-railways/spec-completeness", "/world/roads-railways/runtime-validation"]
sources = [
  "engine/paths/include/orbit/paths/PathNetwork.hpp",
  "engine/path_geometry/include/orbit/path_geometry/PathDerived.hpp",
  "engine/path_geometry/include/orbit/path_geometry/RoadJunctionConstruction.hpp",
  "engine/path_geometry/include/orbit/path_geometry/RoadJunctionPublication.hpp",
  "engine/studio_session/include/orbit/studio_session/UniverseBoundPathCache.hpp",
  "engine/studio_session/include/orbit/studio_session/StudioRoadJunctionPipeline.hpp",
  "engine/studio_session/src/StudioSession.cpp",
  "apps/editor/src/StudioPathNetworkHost.cpp"
]
symbols = ["PathNetworkService", "BuildPathDerived", "BuildCorridorDerived", "BuildRoadJunctionConstruction", "AssembleRoadJunctionScene", "StudioRoadJunctionPipeline"]
invariants = [
  "One authoritative editable network is the sole source for every derived geometry and transport graph.",
  "No junction patch can coexist with its untrimmed incident corridor mesh or collider within a published consumer generation.",
  "Render, contact and lane consumers must observe one matching junction/edge/frame/dependency generation.",
  "A stale source revision or dirty edge invalidates publication eligibility; failure never publishes a partial patch.",
  "Coordinates are explicitly scoped to a frame and simulation time; proximity never implies network connectivity."
]
verify = ["ctest -R Orbit.RoadJunctionPublication", "ctest -R Orbit.RoadJunctionConstruction", "python tools/orbit_docs_cli.py check"]
+++

# Production architecture

**Authority → parameterized design → scoped derived products → junction replacement assembly → atomic CPU generation → GPU/physics/navigation handoffs → vehicle/train runtime.** The code now reaches the **atomic CPU generation and read-only inspector** stage for simple roads. The actual Vulkan draw submission, cooked physics shapes and traversing car must not be marked complete.

\`\`\`text
[Project / authored PathNetwork / CommandService]  S0 semantic authority
         |
         +-- world generation / object revision / node anchors / profile ID
         v
[Frame and time resolver, routed path builder]     S1 alignment snapshot
         |         (FrameGraph + terrain/world inputs; no frame guessing)
         v
[Engineering + section recipe + sweep/cant]        S2 candidate design
         |
         v
[PathDerived + CorridorDerived]                     S3 raw corridor cache
         |         geometry, collision, lanes, revision, build signature
         |
         +-------------------------------+
         |                               |
         v                               v
[Terrain construction footprint]  [Exact-node incident-edge index] S4 topology
         |                               |
         v                               v
[Terrain page service]            [TransportJunctions + conflicts] S5 movements
                                         |
                                         v
                                  [RoadJunctionConstruction] S6
                                         |
                              cut strips + complete patch
                              collision + cut lane links
                                         |
                                         v
                             [RoadJunctionPublication] S7
                          verify every source revision/frame,
                        apply BOTH endpoint cuts, rebuild nav,
                         atomically stage packet per frame
                                         |
                                         v
                             [StudioRoadJunctionPipeline] S8
                           bounded semantic discovery and
                         cache/revision-driven publication
                                         |
                +------------------------+------------------------+
                |                        |                        |
                v                        v                        v
        [Vulkan mesh/cache]        [contact/physics]      [road/rail routing] S9
             OPEN                      OPEN                  partial
                |                        |                        |
                +------------------------+------------------------+
                                         v
                           [NPC cars / player / trains] S10
                                       OPEN
                                         |
                                         v
                          [Debug fixture, profiler, capture] S11
                                     OPEN
\`\`\`

# Exact pipeline contracts

| Stage | Inputs | Owner and current status | Outputs and invalidation |
| --- | --- | --- | --- |
| S0 semantic | PathNetwork node/edge records, stable IDs, profile/intent and permissions | \`engine/paths\`, implemented for direct/Bézier/routed and split | Versioned command history. Never store generated triangles as authority. |
| S1 alignment | Authored endpoints, frame IDs, sampled route, fixed simulation time | \`PathSource\`, partial | Frame-local centerline + source revision; reject unsupported anchors and unbounded work. |
| S2 engineered design | Centerline, operating speed, road/rail catalog, profile and constraints | \`CorridorEngineering\`, \`RailCantDesign\`, \`TransportSections\`, partial | Section design + engineering findings. Do not silently change authored curves. |
| S3 raw products | Alignment, section, refinement budget | \`UniverseBoundPathCache\`, implemented slice | \`PathDerivedProduct\`: stations, visualMesh, contact triangle list, lane/reference graph, signature. Old valid generation may exist while replacement is pending but must not be presented as current after a semantic edit. |
| S4 topology | Shared stable node IDs and incident road/rail edges | \`TransportTopology\`, Studio semantic discovery, partial | Explicit incidence by node; never connect bridge and road merely because positions overlap. |
| S5 junction candidates | Raw edge products, node/frame incidence, lane direction | \`BuildTransportJunction\`, \`JunctionConflicts\`, advisory phase grouping | Deterministic movement candidates and typed failures. Railway switch identity remains unimplemented. |
| S6 physical road assembly | Same-frame 3+2+2 rural-road mesh strips plus S5 | \`BuildRoadJunctionConstruction\`, CPU slice | Exactly trimmed pavement/shoulder strips, patch with cut normals, collision parity, cut-anchored lane connectors and source signatures. Unsupported geometry fails closed. |
| S7 composition | All relevant S6 junctions, current raw edge products and dirty edge set | \`AssembleRoadJunctionScene\`, new | One immutable \`RoadJunctionScenePacket\` per frame: unique trimmed edges, patches, contact triangles, routing links, dependency fingerprint, world generation. Cuts at both ends of the same edge are merged into a single strip. |
| S8 owner/publication | S0 revision + cache signatures + S7 packet | \`StudioRoadJunctionPipeline\` in \`StudioSession::Tick\`, new | One atomic CPU packet swap per frame; bounded scene walk and 32-junction budget. Missing/dirty inputs suppress that junction. Inspection via read-only RPC. |
| S9 Vulkan | S8 packet mesh vertices and indices, frame-to-view transform, LOD, materials | NOT CONNECTED | A future GPU upload record keyed by packet and body generation, with a fence before replacing previous raw corridor drawables. |
| S9 collision | S8 trimmed road and patch triangle lists, frame/shape owner and physic tick | NOT CONNECTED | Physically cooked road collider set in the same generation; no original full-edge triangles may remain within the junction. |
| S9 navigation | S8 retained lane graph and junction movement links | CPU packet partial; no actor dispatcher | A connected typed lane graph, with signal/priority enforcement and deterministic route selection. |
| S10 vehicles/trains | Full S9 outputs, signals, train blocks and timewarp | NOT CONNECTED | Car/wheel and bogie contact, traffic and train operations. Rail turnout requires a separate real switch construction path. |
| S11 validation | S8 generation, S9 GPU and physics handles, camera version fingerprint | tests partially specified | CTest, Debug fixture, frame capture + profiler capture and stored provenance. |

# Ownership, cache keys, and publisher handoff

- **Authoritative IDs:** path edges and nodes remain in \`PathNetworkService\`; junction patches use the existing node ID. \`RoadJunctionScenePacket\` is disposable, not a second persistent road network.
- **Cache layering:** \`UniverseBoundPathCache\` owns **raw** corridor products. \`StudioRoadJunctionPipeline\` owns **published replacement** products. Never mutate or replace the raw source cache with already-trimmed data, or a subsequent build will trim the same edge a second time.
- **World generation:** each packet records a composed-frame ID, the current \`UniverseGeneration\` and deterministic hash of participating raw products and junction cuts. Close/open/recompose invalidates the old published owner.
- **Dirty data:** a pending or failed authored edge must not produce a "ready" publication. Existing last-good per-edge cache remains an internal source; consumer visibility is fenced by publication revision. Missing source products never become geometry at the node by proximity.
- **Double-end trim:** a road may participate in two junctions. The publication layer gathers both cut station indices and crops the original road **once** to the retained range; overlapping cuts are rejected. Multiple junctions claiming the same end are rejected.
- **Atomicity:** stage all frame-local patch, strip, collision and lane products before replacing the immutable CPU generation. On failure discard the candidate and preserve/clear the previous generation according to its world/semantic currency. CPU packet atomicity does *not* automatically confer GPU or physics atomicity.
- **Consumer contract:** \`PublishedRoadJunctionPipeline::FindEdge\` provides replacement mesh/contact/nav for affected edges; unaffected edges continue using the raw path cache. \`FindPatch\` provides the shared mesh/contact. External renderer/physics must consume the same packet generation, not separately query an arbitrary stale raw product or silently overlay the old corridor.
- **Frame-local presentation:** renderer needs explicit frame→camera transform and observer-relative origin; physics also needs time-matched frame velocities for moving supports. Current road publication is for frame-local/static terrain infrastructure only.

# Rendering handoff design (next implementation)

1. Add \`InfrastructureSceneUploadRequest\` carrying \`frame, packetBuildSignature, worldGeneration, retainedMeshes, patchMeshes, materialSlots\`; coalesce repeated requests by frame and signature.
2. Upload immutable per-chunk vertex/index buffers (split near/mid/far batches), release replaced buffers only after Vulkan GPU fence completes; never stall Studio's frame to wait on staging.
3. Publish a *generation-keyed draw packet*: affected raw edge drawables are disabled **in the same swap** that enables the trimmed edge and patch. The previous complete draw packet may be retained until the replacement is fully resident.
4. Material interface: first road primary pavement vs two shoulders, road marking overlay, wetness/friction and generated UV continuity. Do not bake these semantic slots into every triangle ID.
5. LOD streaming: shared station IDs and exact border vertices at chunk seams; light far-distance corridor strokes, GPU-instanced road furniture, and independent city/planet page residency. World/timewarp budgets need explicit counters.

# Physics and traffic handoff design (next implementation)

1. Cook static collision shapes from **exact** patch and retained triangles, frame-local double positions→physics local origin transform. Collision revisions match draw packet generations.
2. Stage and commit the full set of per-edge and patch contact shapes in one physics-safe simulation checkpoint, not in the middle of an integration substep. Remove old untrimmed collider in the same transaction.
3. Replace raw affected lane graph fragments with cropped lane samples and explicit cut-station movement links. Preserve full stable semantic edge IDs; regenerate derived routing indices. No routing point may refer to the deleted portion of a corridor.
4. Drive a vehicle through a T junction and assert bounded seam penetration, normals, friction continuity, waypoint progression and zero duplicate contacts. For two connected junctions on one edge, test the doubly cropped intermediate road.
5. Build separate railway turnout publication (frog, flangeway, rolling contact, switch lock and signal-block state); do not treat rail Hermite branch candidates as physical turnouts.

# Asynchrony and performance controls

At present S8 re-evaluates on scene revision/source-signature change, not every renderer frame, and uses bounded discovery and 32-junction candidate caps. The **next** owner should schedule per-node dirty sets and frame-local immutable packets on worker queues. Distinguish source geometry task, junction assembly task, GPU upload, collision cooking, nav compile and publication fence; each has its own cancelable dependency token, budget and failure reason.

Backpressure: if an edit invalidates generation \`G\`, cancel pending uploads/cooks for \`G\`, retain a coherent last-good generation until a current complete replacement exists, and never patch half the physical scene. The high-level task must not busy-loop on missing dependency states. Build latency, queue size, CPU allocations, vertex counts, GPU bytes, contact cook time and packet swap time belong in Debug profiler.

# RPC and source truth for agents

- \`path.junction_preview(node, include_samples?, include_construction?)\`: bounded diagnostic, with raw S5 candidates plus S6 construction, and S8 \`construction_published_cpu\` flag. A preview remains read-only.
- \`infrastructure.junction_pipeline_status\`: read-only S8 snapshots by frame, world generation, signatures, patch/edge/link counts and last error; GPU/physics committed fields are **false** until those consumers actually validate their commits.
- \`path.geometry(edge)\`: use the S8 published replacement corridor when eligible; raw S3 otherwise. Never hide which pipeline generation was used in future capture metadata.
- Build → Procedurals remains the one human authoring and testing entry point. New Debug road/rail fixtures should be registered in the existing Debug hierarchy; no parallel project/launcher/editor.

# Validation gates

| Gate | Objective | Status |
| --- | --- | --- |
| P0 | CPU stage builds real T junction, exact five-point seams, collision/render parity and cut lane graph | Source implemented, native CTest unverified |
| P1 | Single road used by junctions at both ends becomes one nonoverlapping cropped product | Source/test added, native CTest unverified |
| P2 | CPU publication rejects dirty/stale/foreign-frame/oversized/duplicate-end products without partial replacement | Source/test added, native CTest unverified |
| P3 | StudioSession lifecycle publishes and invalidates by world/scene revision; RPC and panel show exact packet state | Source wired, Windows smoke unverified |
| P4 | Vulkan Paths pass consumes replacement strips and unique junction patch instead of raw roads | Source wired into existing render graph; native GPU capture and material/overdraw verification pending |
| P5 | Physics-driven vehicle traverses patch and retained strips without contact duplication or gaps | NOT DONE |
| P6 | Respects road materials, drainage, bridges and moving support frames | NOT DONE |
| P7 | Planetary asynchronous incremental rebuild/capture benchmarks | NOT DONE |

Do not mark the road network "fully implemented" until P4–P7 have demonstrated runtime evidence.

## Vulkan presentation bridge and CPU contact diagnostic (Oct 2026 increment)

`StudioViewportRenderer::ComposeBase` now obtains its path draw list from `StudioRoadJunctionPipeline::RenderProducts(rawCache)` rather than drawing all raw corridors. For each frame packet with current universe generation and matching, non-dirty source build signatures, the draw list **replaces affected raw road edges with cropped replacement products** and appends one persistent patch proxy product per authored junction. Unaffected roads and railways keep their raw products. A failed signature check suppresses that entire packet in the draw list, never just one side of a junction. The existing `PathPreviewRenderer` GPU pipeline, depth, MRT surface outputs and camera-relative transforms are reused; no extra draw overlay is introduced.

`ProbeRoadJunctionContact(packet, probe)` reads exactly those published clipped-edge and patch collision triangles using a bounded, double-precision ray/triangle test in one explicit frame. The `infrastructure.junction_contact_probe` read-only RPC takes node, origin_frame_local_meters, direction_frame_local and optional maximum_distance_meters, returning nearest hit, normal, owner ID, patch attribution, packet signature and triangle budget. This tests CPU contact parity against the published geometry but is **not** the engine's vehicle dynamics, native physics broad phase, shape cooker or scene shape registration.

Pending before P4 can pass: run Windows Studio, inspect a T junction against terrain, capture Vulkan frame with exact build fingerprint, verify no duplicate raw-corridor drawcalls and correct normal/material/depth. Pending before P5: physics-world shape commit at fixed-step boundary, physical traction/wheel contact and drive-through fixture, while reusing the same publication token for Vulkan/physics/nav.


## Static road-contact scene owner and joint publication gate

\`RoadContactScene\` is the first physics-facing **CPU** contact ownership layer. It is deliberately independent of a future rigid-body solver. \`PrepareRoadContactScene\` receives the same \`RoadJunctionScenePacket\` as the Vulkan draw list, checks that its clipped road and patch triangles match their visual index buffers, enforces unique owner IDs, frame consistency and triangle/shape budgets, and stages immutable shape bounds plus contact triangles. Any invalid shape invalidates the **whole** candidate.

\`RoadContactSceneOwner::Commit\` activates a completed contact scene as one move at the StudioSession update boundary; a rejected candidate cannot evict an already committed valid scene. \`StudioRoadJunctionPipeline\` commits the contact owner and CPU geometry publication as one coherent lifecycle step. \`PacketCurrent\` is the single common eligibility check for \`RenderProducts\` and \`CurrentContact\`: packet world generation, frame, contact packet signature and every raw source signature must match, and no source may be dirty. A mismatch suppresses **both** publication consumers until the next current generation.

\`RoadContactSceneOwner::Probe\` uses conservative shape AABBs as a broadphase, followed by double-precision triangle contact. The \`infrastructure.junction_contact_probe\` RPC is routed to this active owner, rather than scanning a stale packet. It returns \`contact_scene_active\` and the matching packet signature, with \`physics_committed=false\`. The \`infrastructure.junction_pipeline_status\` readout exposes the count of staged contact scenes.

**Important distinction:** these shapes are CPU contact data, not registered rigid bodies or a wheel solver. Commit currently occurs during the StudioSession update, **not** within a dedicated physics fixed-step checkpoint. The fixed-step integration, engine collider ownership, vehicle contact response, friction and dynamic-frame velocity handoff remain open. A Vulkan GPU buffer fence and the native render/collision capture are also not proven. Tests: \`Orbit.RoadContactScene\`, \`Orbit.StudioRoadJunctionPipeline\`. The P5 drivable vehicle acceptance gate remains **OPEN**.

## Fixed-step four-wheel CPU seam-driving fixture

`RoadVehicleFixture` introduces the first deterministic **fixed-step vehicle-contact experiment** using the active `RoadContactSceneOwner`. A planar rigid-body approximation tracks longitudinal travel/yaw, body heave, pitch and roll; four independent spring/damper ray wheels sample the actual committed trimmed-edge and junction patch triangles. Traction and braking forces are limited by total contact load; gravity, aerodynamic and rolling resistance are included. Every step uses a fixed 1/120 s by default, independent of viewport FPS, simulation time warp and global clock rate.

`infrastructure.road_vehicle_fixture(node, steps?)` in StudioSession uses the canonical authored node and incident derived road: it chooses a built road approach, spawns four metres outside the cut, points toward the node and runs 1..600 bounded steps without altering the scene. It returns a contact packet signature, completed steps, all-four-contact duration, pavement/patch contact counts, owner transitions, bottom-out, acceleration and the final body position/speed. **Build → Procedurals → Junction and turnout preview → Driving seam regression** invokes the same read-only RPC.

The dedicated `Orbit.RoadVehicleFixture` test uses adjacent road/patch/road surfaces with exact seams, asserts deterministic four-wheel contact transitions and suspension stability, then deliberately introduces a physical gap and verifies the regression detects incomplete four-wheel contact. The Studio junction test invokes the production RPC. This proves nothing until the Windows CTests execute successfully.

**Scope boundary:** this is not a registered 6-DoF rigid-body/vehicle actor, lateral slip/weight-transfer tire model, sound, moving support-frame contact, or a simulation-clock-driven gameplay component. Gravity assumes local +Y up, steering is a bicycle approximation, and pitch/roll are small-angle suspension estimates. It cannot replace the future full tire/vehicle dynamics engine. The playable driving gate remains open, and physics-world fixed-step publication/synchronization remains unimplemented.

## Transient vehicle world following the CPU seam fixture

RoadVehicleRuntimeWorld now keeps independent vehicle states across Studio RPC calls rather than restarting a fixture for every request. AdvanceRoadVehicleStep reuses the original validated four-wheel spring/damper solver. The vehicle world's fixed 120 Hz Step checks the active contact packet/frame/world identity; on failure the current actor states remain intact for that step. Bind to a newer packet explicitly clears actors. CompileRoadVehicleRoute requires a published JunctionMovement between exact authored edge and lane IDs, and concatenates the surviving lane samples and cut-to-cut curve without proximity connections. The runtime is **explicitly stepped**, with bounded 64 actors and 120 steps per request, not yet connected to the gameplay simulation clock or a proper 6-DoF rigid-body solver. See vehicle-world.md.
