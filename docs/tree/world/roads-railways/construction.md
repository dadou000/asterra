+++
path = "/world/roads-railways/construction"
title = "Corridor construction, physical contact, structures and LOD"
kind = "design"
status = "planned"
summary = "Nondestructive earthworks, supported decks and tunnels, shared collision and rendering derivation, material condition, hydrology, caching and planet-scale streaming."
keywords = ["cut fill", "grading", "bridge", "viaduct", "tunnel", "collision", "LOD", "material column", "corridor", "rail sleeper"]
related = ["/world/roads-railways", "/world/roads-railways/alignment", "/world/roads-railways/runtime-validation", "/rendering/terrain/invalidation"]
+++

# Corridor footprint and physical construction

Resolve a swept, station-indexed corridor envelope from alignment plus cross-section. Keep these distinct layers:
1. Surveyed/natural terrain and geology, hydrology and authored protection constraints.
2. Planned excavation/cut, fill, embankment and retaining volumes; substrate and foundations.
3. Constructed trackbed/pavement/deck or tunnel shell, shoulders/sidewalks, utilities and drainage.
4. Damage, erosion, deformation, repair, accumulated dirt, wetness and temporary effects.
5. Derived render and physical-contact products.

Compose edits deterministically and non-destructively by revision/priority, with an auditable conflict rule when infrastructure overlaps terrain edits, bodies of water, buildings or other corridors. Expose displaced material volumes and hard constraints for reasonable road grades, slopes and foundation support. Protect no-cut regions. Tunnels require explicit void/solid representation or sparse local volume fields: a displaced *heightfield alone cannot produce a real tunnel*. Bridges and decks must NOT flatten underlying terrain indiscriminately; supports can penetrate terrain independently from the elevated deck. Cars/rails on a supported platform query the deck as the contact authority, not a terrain sample.

Support road embankments, retaining walls, culverts, drains, rock cuts, piers, beams, expansion joints, abutments, tunnels, portals, cut-and-cover, viaducts, stations and elevated urban roads. A span is a meaningful engineering support assembly with optional structural integrity and damage states, not just decoration.

## Continuous physical/visual consistency

One derived geometric representation should provide correlated: visual mesh, surface derivatives, solid bounds, contact triangles/SDF/convex collision, operating lane/track geometry, decals and high-resolution local damage. Do not let high-frequency normals invent traction bumps that collision never sees; use clearly differentiated cosmetic vs physical frequency bands. Bank, camber, road crown, ballast shoulders, rail height, platform gap and surface damage must remain coherent at profile transitions. Slip/traction and wet friction depend on physical surface and weather; deterministic stationing aligns tire contact and rail/wheel path.

Road markings, textures, sidewalks, safety barriers, catenary poles, signals, signs, lights, ballast, rails, sleepers, switches, conduits and culverts use a catalogue/rule system with seeded spacing and contextual exclusion. Attachment positions derive from station/profile frame and survive regeneration. Road and rail materials can age, crack, rut, pothole, deform, break, be repaired or be replaced. Model damage with sparse local fields or per-segment state, retaining physical consequences only where relevant/nearby; avoid planet-scale per-vertex state.

## Water and weather

Bridges and culverts must preserve flow paths and clearance; embankments can obstruct or redirect drainage. Roads need crossfall, drains and pooling. Rail beds have ballast drainage. Material friction/visibility are weather-dependent (rain, wetness, ice where implemented), and washouts/foundation failure can damage geometry or suspend connectivity. Global water and terrain solvers remain authorities; corridor evaluation feeds constraints and flow alterations rather than standing up an incompatible water simulation.

## Async construction and streaming

Large corridors cross thousands of spatial pages; edits invalidate **only dependent tiles, support regions, lane/route edges and GPU resources**. Separate priority queues for visible observer pages, near traffic/contact pages and distant cached/prefetch pages. Rebuild asynchronously with cancellation, ownership-safe handles and version fingerprint checks. Commit new generations atomically when all required products are ready; preserve previous valid render/contact while in flight.

Cache keys should include network/profile/terrain/support revision, simulation-dependent condition state where relevant, representation resolution and procedural seed. Visible/safe fallback remains during transient missing products. Enforce explicit budgets for GPU uploads, CPU tasks, mesh memory, collision cooking and resident pages, with instrumentation and backpressure. Avoid a global synchronous job, unnecessary per-frame reconstruction or copying massive surface grids. Shared near/far representation:
- Near: full track/contact fidelity, geometric deformation, markings and infrastructure objects.
- Mid: simplify meshes and contact detail, merge draw calls/instances, keep lane/track and structural silhouette.
- Far/orbit: coarse alignment/appearance contribution, roads and rail as thin stable linear features, dominant bridges/cuts when projected size justifies them; no fine sleepers/bolts.
- Unloaded: persistent compact network, semantic state and optional baked snapshots; no active individual meshes/physics actors unless needed.

Transitions use geometric error, screen-space footprint, hysteresis, cross-fade/geomorph where valid, and coherent physical proxies. Support render-time floating-origin, moving reference frames and planet-to-ground continuity.

## Verification

Compare authoritative CPU terrain/corridor sample vs GPU rendered physical page vs collision/contact at repeatable stations; enforce finite heights, revision consistency, no seams/cracks, stable support boundaries and water drainage. Exercise crossing pages, cube faces, negative elevations, rapid editing, undo/redo, streaming-in/out and device loss. Run captures at near/mid/orbit, with profiler traces to expose shader/upload/CPU spikes. Existing simple physical-page-sanity test is **not** a complete acceptance test.
