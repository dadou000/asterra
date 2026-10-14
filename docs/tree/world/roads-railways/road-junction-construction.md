+++
path = "/world/roads-railways/road-junction-construction"
title = "Road junction patch and trimmed corridor construction"
kind = "concept"
status = "experimental"
summary = "Derived T/cross road junction pavement patch with exact cut cross-section seams, cropped shoulder/pavement meshes and matching contact triangles."
keywords = ["road", "intersection", "junction", "trim", "collision", "surface", "seam", "weld"]
sources = [
  "engine/path_geometry/include/orbit/path_geometry/RoadJunctionConstruction.hpp",
  "engine/path_geometry/src/RoadJunctionConstruction.cpp",
  "engine/path_geometry/tests/RoadJunctionConstructionTests.cpp",
  "apps/editor/src/StudioPathNetworkHost.cpp",
  "apps/editor/src/StudioProceduralsPanel.hpp",
]
symbols = ["BuildRoadJunctionConstruction"]
invariants = [
  "All cut positions and generated connectors remain in the same explicitly identified reference frame.",
  "No junction patch is overlaid on untrimmed source corridor triangles.",
  "Pavement crown, both pavement edges and both shoulder outer edges share exact geometric positions between cut and patch.",
  "Every retained and junction collision triangle is generated from the same vertex/index arrays used for visualization.",
  "The construction is derived and read-only; it never mutates semantic networks.",
  "The generated geometry must be atomically substituted for old corridor geometry before use by physics or rendering.",
]
related = ["/world/roads-railways", "/world/roads-railways/transport-foundations", "/world/roads-railways/spec-completeness"]
verify = ["ctest -R Orbit.RoadJunctionConstruction", "python tools/orbit_docs_cli.py check"]
+++

# Purpose

Until this increment, BuildTransportJunction and its roadFootprint only described connection candidates and a convex hull. Adding that hull as another visual/collision object would overlap the original road meshes. The constructor now generates a **complete CPU replacement bundle**: retained fragments of all affected corridors, one shared junction pavement patch, derived collision triangles and cut-anchored lane connectors. It does not silently publish the bundle into the live world.

# Explicit supported first milestone

- Exactly 2–8 authored incident road edges, same junction node ID and one resolved frame. Their inputs must be complete 3+2+2 rural-road section meshes (crowned pavement plus two shoulders), and stable per-edge station/lane arrays.
- One positive, user-controlled cut distance per incident approach (default 8 m), with at least two retained segments beyond the cut. A short approach returns a typed TooShort failure; it is never reduced to an empty strip or extended beyond its source.
- Each source band is compacted to stations on the outside of its cut. Only triangles whose three vertices survive are retained. All generated collision triangles use exactly those retained vertices and triangle indices.
- Junction perimeter uses the exact **five unique** vertices across each source cut: left outer shoulder, left pavement edge, crown, right pavement edge and right outer shoulder. Cyclic angular ordering is deterministic by geometry and stable edge ID.
- A bounded center-fan patch closes the trimmed cross-sections with the same cut vertex positions. Each patch collision triangle is produced from its actual mesh. It supports the initial star-shaped planar/small-grade T and cross case only.
- The lane connector graph is resampled from the existing candidate turns, with endpoint positions anchored to the retained **cut station lane reference**, rather than the now-deleted lane reference near the semantic node. One-way and mode filtering remain in the upstream junction planner.
- The builder checks incidence, source validity, local-up agreement, grade bounds, angular overlap/self-intersection hints, total source and output vertex budgets and deterministic dependency signatures.
- Stable construction output is a source-revision/product-signature-fenced disposable plan, not persistent scene authority.

# Explicitly blocked cases

Mixed rail/road nodes, true level crossings, multi-profile/custom material topology, significant grade/normal discontinuity, sharply overlapping and non-star-shaped road approaches, too-short edges, and unready/stale source products are **not** silently constructed. The diagnostics identify why the CPU product cannot be built.

The current output only produces top-surface pavement and shoulder triangles. It is not a closed volumetric solid, excavated kerb system, full roundabout or road foundation, nor is it a physics actor. The patch is not yet shaded with independently persisted pavement/shoulder material bands. It remains to implement an on-world atomic publication path which replaces all source corridor visual/collision products and associated lane graphs as one source-signature-checked generation. Publishing only the patch would create overlapping road collision and is forbidden.

# Studio, RPC and debug observability

Read-only path.junction_preview accepts include_construction=true (plus existing node and include_samples options). Its response includes construction_ready, construction_sources_dirty, construction_dependency_signature, construction_committed_to_world=false, typed construction_findings, patch vertices/collision counts, trimmed per-edge cut station and retained collision counts and rerouted lane candidates. Optional include_samples exposes only bounded frame-local patch vertices and triangle indices. A successfully built preview is **not** an installed world collision mesh.

Build → Procedurals → Junction and turnout preview requests this product, renders counts and exact cut stations, and shows failures. Its language must not imply playable physical completion. The normal editor corridor renderer remains owned by the existing path cache; there is no second engine or launcher.

# Required acceptance and subsequent publication design

1. Native CPU unit test: T-shaped three-way junction, deterministic input permutation/signature, exact five-point seam equality, no retained triangles under cut, collision/render parity, reanchored lane movement endpoints and rejected invalid/mixed/budget cases.
2. Physical publication transaction: consume only when **all** incident source revisions match the construction dependency fingerprint; atomically replace edge fragments, junction patch, contact triangles and connected lane references. If any part fails, keep old coherent world generation.
3. Studio Debug fixture: create a reference-frame-local T junction, run construction, show actual viewport mesh (near and mid), drive a simulated car over the stitched seam, inspect wheel/contact penetration and profiler capture. The test must check original-body and GPU footprint consistency.
4. Extend to asymmetric road widths and lane splits, genuine road material bands, sidewalks/curbs, crossing controls, intersections with grade, non-star-shaped merge geometries and ray-stable planetary tangent-plane operation.

The CPU builder and test target are in source, but the native compilation/execution and real-world publication gates have not been marked passed.

## Additional material and failure invariants

The junction seam preserves the constructed cut boundary vertex *normals*, not only positions. Connector Hermite tangents follow the surviving cut station (not the removed semantic-node station) so a curved road approaches the patch in the correct local direction. A built-in road section must carry the complete expected 3+2+2 surface and indexed triangle topology; coincident vertex counts from an unrelated profile are not sufficient. Work budgets have hard maxima independent of caller configuration.

If a connector, index or source check fails after any temporary geometry has been staged, the builder discards the staged patch, all fragments, contact triangles, lane links and dependency fingerprint while preserving typed findings. Only Ready() output can be handed to a downstream publisher. Regression cases cover post-staging invalidation, curve/source-dependent fingerprints, cut shoulder seam normals and unsupported acute joins.

## Frame-local production CPU publication (new)

The formerly preview-only CPU product now feeds `RoadJunctionPublication`, which assembles replacement strips, patches, contact triangles and lane links into a revision-fenced frame packet. It supports one road shared by junctions at both ends through a single doubly cropped product, never two overlapping source meshes. `StudioRoadJunctionPipeline` is bound to StudioSession's authoritative world and raw cache, and `path.geometry` can inspect published edge products. Studio provides `infrastructure.junction_pipeline_status` and `path.junction_preview.construction_published_cpu`.

**The GPU and physics consumers are not connected.** There is no native roadway rendered/cooked collider assertion until the S9 handoff described in [pipeline.md](pipeline.md). The source-level publication is an important boundary but must not be described as a drivable completed intersection.

## Live viewport handoff versus physics readiness

The primary Studio Vulkan Paths pass now uses published road-junction replacement surfaces when their source signatures remain current. Junction patches are persistent synthetic `PathDerivedProduct` rendering proxies tied to authored junction node IDs and frame-local coordinates; drawing occurs through the existing PathPreviewRenderer, not a second overlay. The edited/raw caches are never mutated, and invalid packets fall back as a whole to raw geometry rather than leaving half a patch.

The CPU contact probe reads the same packet collision triangles but is only a diagnostic. A cooked physics collider and drivable car are still outstanding, as is native GPU verification. See pipeline.md for the handoff contract.
