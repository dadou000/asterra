+++
path = "/world/roads-railways/alignment"
title = "Road and rail network, alignment and engineering rules"
kind = "design"
status = "planned"
summary = "Authored path network and profiles, Bézier and routed alignments, road and railway constraints, lane and track topology, nodes and junction generation."
keywords = ["road profile", "rail profile", "spline", "bezier", "curvature", "cant", "superelevation", "junction", "routing", "speed"]
related = ["/world/roads-railways", "/world/paths", "/world/path-routing", "/world/path-geometry"]
+++

# Network semantics

Use stable identifiers for network, node, edge, cross-section, support segment, junction, lane, track and attachment. Nodes may be world/body surface anchors, reference-frame points or entity/socket attachments. Edges carry mode (road/rail/shared service/path), design speed, allowed traffic, continuity policy, profile references, construction choice, ownership and generated-state revision. Split/merge must preserve attachments and remap persistent references deterministically. Network connectivity is independent of mesh adjacency.

An authored edge supports straight, cubic Bézier, editable multi-segment spline, transition curves and terrain-aware routed mode. Endpoints and tangents are user-editable. Smooth stationing uses arc-length parameterization (adaptive error-bounded inversion) so materials, sleeper spacing, roadside assets, simulation and multiplayer reproduce the same placement. Interpolate in a stable local tangent frame (not naïve planet XYZ) with curvature/grade calculation in the correct reference frame. Handle seams, tangent reversal, vertical curves, cross-face planet coordinates, floating origin, exact endpoints, zero-length edges and loop closure. Compute evaluation at explicit simulation time for moving anchors.

## Engineering dimensions / constraints

Parameters are **data**, not permanent baked vertices:
- Horizontal: curve radius, curvature gradient, transitions/easements, design speed, lateral acceleration and sight lines.
- Vertical: grade, vertical radius, crest/sag transition, drainage, clearance and maximum earthworks.
- Cross-section: carriageway width/lane count and direction, shoulders, kerbs, sidewalks, verges, medians, sidewalks/cycle tracks, road crown and banking; ballast/formation/track centres, gauge, rail profile, sleepers, cant, turnout envelope and clearance.
- Construction: ground conforming vs cut/fill vs retained vs deck/bridge vs tunnel, support attachment, foundation and water interfaces.
- Surface: material layers, friction/wetness, road markings/signage, condition/damage and repair state.

Defaults and validators are preset/rule-driven by road/rail classification, speed, environment and user overrides. Avoid hard-coded universal engineering thresholds: track gauge and standards are configurable, and warnings distinguish unsafe geometry from a permitted fantasy/sandbox override. Report the offending station and proposed remedy. **Automatic radius solver and turn easing** should satisfy grade/curvature/jerk and footprint constraints where feasible; present cost/space tradeoffs. Railway cant solves from speed and curvature with bounded cant deficiency/excess and cant ramp, including reversals, stations, tight turnouts and low-speed branches.

## Junction / connected infrastructure

A junction is an explicit network object, not merely crossing triangle strips. Detect candidate same-frame/intersecting edges with grade-separated exceptions; preserve bridges above roads and tunnels below ground without false junctions. Generate approach/exit and surface patches, tapers, lane links, turning paths, signal and conflict zones and pedestrian links from topological intent. Support intersections, T/cross/Y, roundabouts, ramps, merges/diverges, parking entrances, level crossings, depot/yards, points/switches, turnouts, crossovers and platforms. Allow manual geometry and priority override, including nonuniform and asymmetrical profiles. Stable junction identity matters for downstream NPC routes and traffic rules.

**Rail specifics:** continuous tangent, grade and cant transition; consistent gauge and flangeway; turnout frogs/guard rails; structural clearance, platform offset and rail contact geometry. Track occupancy, switch lock state and block/signalling are runtime outputs consuming the authored track graph, not ad hoc mesh topology.

## Procedural planning and generation

Routing can use hierarchical terrain-aware costs: slope, curvature, soil/rock, hydrology, flooding, cliffs, legal/protected land, existing structures and network connectivity. Distinguish cost of cutting/filling, bridging, tunneling, retaining, upkeep and travel time. Generate multiple editable alternatives with previews; committed route remains authored intent, not a fragile raster search result. Large-area planners may combine organic, grid, radial, arterial and terrain-following styles for settlements; preserve deterministic seeds and explicit user edits. Roads precede derived blocks/parcels/zoning/buildings in procedural city generation, with selective downstream invalidation and no destructive regenerate-all.

## Acceptance cases

- Build a straight, eased and tightly curved road; change design speed and observe proposed radius/bank changes without mutating an overridden profile.
- Place rail through opposing curves; ensure cant reverses smoothly and contact geometry remains continuous.
- Merge lanes and construct grade-separated crossing; topology reports connected/isolated correctly.
- Route around water and a protected slope; compare cut/fill vs bridge/tunnel alternatives and deterministic rebuild.
- Move a socket-mounted endpoint; recompute affected edges without breaking graph IDs.
