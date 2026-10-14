+++
path = "/world/path-geometry"
title = "Path geometry (derived centrelines and meshes)"
kind = "subsystem"
status = "stable"
summary = "Derives PathCenterline samples, stations, lane references and PathMesh vertices from an authored network (PathSourceRequest, PathBuildOptions)."
owner_module = "OrbitPathGeometry"
keywords = ["path geometry", "centerline", "mesh", "lane", "station", "derived", "road mesh"]
sources = [
  "engine/path_geometry/include/orbit/path_geometry/PathDerived.hpp",
  "engine/path_geometry/include/orbit/path_geometry/PathSource.hpp",
  "engine/path_geometry/CMakeLists.txt",
]
symbols = ["PathCenterlineSample", "PathSourceRequest"]
invariants = [
  "Generated path geometry is a disposable cache; the semantic path network stays the source of truth (/rules/architecture, rule 10).",
]
related = ["/world/paths", "/rules/architecture"]
depends_on = ["/authoring/scene", "/foundation/core", "/foundation/frames", "/foundation/math", "/foundation/time", "/world/path-routing", "/world/paths", "/world/universe"]
used_by = ["/apps/studio", "/editor/studio-session", "/editor/ui-toolkit"]
verify = [
  "ctest -R Orbit.PathGeometry",
  "ctest -R Orbit.PathSource",
]
verified = "b0a0de7f"
+++



## Canonical road / rail specification

The complete road, railway and supported-construction design contract lives at **`/world/roads-railways`** (source: `docs/tree/world/roads-railways.md`), including alignment, earthworks, bridge/tunnel support, vehicles aboard moving ships, Studio authoring, traffic and validation. This module implements only a portion of the complete contract; do not infer feature completion from source presence.

## Corridor engineering validation (Harmony)

`CorridorEngineering.hpp/.cpp` now supplies a **pure, deterministic and non-mutating** analysis of derived `PathStation` sequences against caller-provided design limits. The result includes per-station grade, curvature/radius, lateral acceleration and recommended bank/cant, plus station-local findings for excessive grade, tight radius, excessive lateral acceleration, excessive bank and abrupt cant transition. Invalid/non-finite geometry or limits produce `InvalidInput` findings. The companion `SuggestCorridorBankProfile` generates a bounded advisory profile with forward/backward slope limiting; applying it remains an explicit authoring action.

This is the **engineering foundation**, not yet a full rail standards solver (turnout envelope, cant deficiency and exact clothoids), geometry auto-fixer, traffic simulator or an integrated Studio editor panel. `ctest -R Orbit.CorridorEngineering` is the dedicated test target. Compiling and running the test still requires local/CI verification.

## Road/rail derived construction and topology (Harmony)

The pure TransportSections API now builds road crowns and shoulders, railway formation and railhead strips, cant-consistent rail contact reference points, and deterministic bounded sleeper placements. The pure TransportTopology API compiles explicit node/link IDs into directed mode-aware route arcs, connected components and intentional junction/crossing annotations. Both are derived data, not new authoring authorities. See /world/roads-railways/transport-foundations for API limitations and integration steps.

Validation: ctest -R Orbit.TransportSections and ctest -R Orbit.TransportTopology. Native build execution is still pending.

### Refined corridor navigation parity

CorridorDerived now regenerates lane/graph/navigation/debug references after geometric refinement, and road lane positions sample the swept primary surface including crown and bank instead of retaining stale coarse path references. Verify via Orbit.CorridorSweep; Studio runtime validation is still pending.

### Railway cant as a height-difference constraint

RailCantDesign computes advisory railway cant in metres and signed radians, including multiple operating speeds, deficiency/excess warnings, and a bounded runout gradient. The output is non-mutating. Verify via Orbit.RailCantDesign. Station frame validity now requires orthonormal handedness for physical railhead contact consistency.

### Native infrastructure recipe integration

InfrastructureSectionBuilder supplies deterministic road/rail sections from one source centreline, path profile, sampling spacing and requested operating speed. It delegates to the existing section and rail cant solvers and does not add semantic authority. StudioSession's infrastructure path cache now consumes the recipe directly and publishes swept geometry/collision for supported catalog IDs. The CMake test Orbit.InfrastructureSectionBuilder checks road and railway production and invalid dimensions. Actual Windows compilation and GPU rendering need verification.

## Defensive planetary-scale allocation limits

PathSourceRequest.maximumSamples bounds Bezier/routed centreline expansion. PathBuildOptions.maximumStations bounds the next Resample stage before it allocates station arrays, reference graphs or vertex/collision products. Both defaults are 16384 samples and reject excess work; spatial tiling and streaming are not yet integrated. Orbit.PathSource and Orbit.PathGeometry tests exercise these cases.
