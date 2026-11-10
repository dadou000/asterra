+++
path = "/world/roads-railways/vehicle-world"
title = "Transient fixed-step road vehicle world and junction route guidance"
kind = "concept"
status = "experimental"
summary = "Persistent runtime-only vehicles, generation-fenced collision, transactional fixed stepping, explicit authored junction routing, and known physics limitations."
keywords = ["roads", "vehicle", "physics", "runtime", "navigation", "collision", "junction"]
related = ["/world/roads-railways", "/world/roads-railways/pipeline", "/world/roads-railways/spec-completeness"]
sources = ["engine/path_geometry/include/orbit/path_geometry/RoadVehicleWorld.hpp", "engine/path_geometry/src/RoadVehicleWorld.cpp", "engine/studio_session/src/StudioSession.cpp", "apps/editor/src/StudioProceduralsPanel.hpp"]
symbols = ["RoadVehicleRuntimeWorld", "CompileRoadVehicleRoute", "AdvanceRoadVehicleStep"]
verify = ["ctest -R Orbit.RoadVehicleWorld", "ctest -R Orbit.StudioRoadJunctionPipeline", "python tools/orbit_docs_cli.py check"]
+++

# Ownership

The RoadVehicleRuntimeWorld is the first persistent **runtime-only** physics-state container for lightweight vehicles on static road junctions. It is not an authored scene graph, true rigid-body engine, NPC AI controller or player car blueprint. It uses the same four-wheel raycast spring/damper physics as the existing one-shot test fixture, through a shared validated single-step integrator. StudioSession owns the transient instance and clears it on world close/open.

The world binds precisely to a RoadContactSceneOwner's frame, world generation and packet signature. Spawn and Step reject a different contact generation. Rebinding after an authored road edit explicitly discards prior transient actors. Contact generation validity is established through the shared StudioRoadJunctionPipeline eligibility check, so the viewport and vehicle dynamics cannot silently use different road generations.

# Data and execution contracts

| Stage | Owner | Inputs and result |
| --- | --- | --- |
| Bind | RoadVehicleRuntimeWorld | Record active contact frame, world generation and signature; clear actors when it changes |
| Spawn | StudioSession RPC → RoadVehicleRuntimeWorld | Exact authored junction + retained incoming road; place temporary car outside cut at spring equilibrium |
| Controls | RoadVehicleRuntimeWorld | Validate throttle, brake and steer angles without modifying road authority |
| Route | CompileRoadVehicleRoute | Match exact published lane movement from two selected authored edges; build bounded lane-and-connector waypoints |
| Step | RoadVehicleRuntimeWorld | Revalidate contact generation, run 1–120 fixed 120 Hz updates on copied actor states and atomically swap successful states per step |
| Inspect | Studio RPC | Actor position, heading, speed, waypoint index, tick and packet signature |
| Invalidate | StudioRoadJunctionPipeline | Rebuilt packet makes previous vehicle generation ineligible until explicit Bind |
| Remove | Studio RPC | Delete temporary vehicle, or clear all when closing world |

The route compiler does not infer road connectivity from geometric proximity. Each route requires a published JunctionMovement connecting exact authored edge IDs and matching lane indices; both adjacent retained lane reference arrays must terminate at the connector cut positions. It is a local two-edge path rather than a citywide pathfinder.

# Unified Studio control plane

Build → Procedurals → Junction and turnout preview → Temporary vehicle runtime now provides Spawn car, Step 60 ticks, Inspect, Apply controls and Remove. A separate Driving seam regression remains the stateless 480-step check.

Read/write RPC infrastructure.vehicle_world supports these action objects:

- Spawn: action=spawn, node=UUID, optional edge=UUID.
- Control: action=controls, vehicle=UUID, throttle=0..1, brake=0..1, steer_radians=-0.6..0.6.
- Follow route: action=follow, node=UUID, vehicle=UUID, from_edge=UUID, to_edge=UUID.
- Step: action=step, node=UUID, steps=1..120.
- Inspect: action=status, optional vehicle=UUID.
- Remove: action=remove, vehicle=UUID; or action=clear.

IDs are ephemeral; no scene ObjectStore changes, launcher, second editor or file persistence is introduced.

# Validation and limitations

New pure CPU target Orbit.RoadVehicleWorld checks spawn/removal, deterministic fixed-step replay, explicit route construction, invalid controls and nonfinite states, and rejection of changed contact packet generations. Orbit.StudioRoadJunctionPipeline now checks the matching RPC and stale road failure. These are source-level changes until native Windows tests pass.

Critical omissions remain: engine-level 6-DoF rigid bodies, individual wheel rotation/slip, torque/drivetrain, collision with other dynamic actors, true traffic rules and multi-junction pathfinding, moving reference-frame velocity, collision material friction and a physics-safe asynchronous GPU/shape swap. The persistent runtime is **explicitly stepped** through its RPC, not attached to the gameplay scheduler; thus a full playable/NPC road vehicle milestone is not yet achieved.

Next: verify compiler/CTest on Windows; connect deterministic time-accumulator scheduling to SimulationClock with bounded catch-up; implement real multi-junction lane routing, traffic signals and native rigid-body components.
