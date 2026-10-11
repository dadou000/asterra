+++
path = "/rendering/unified-transport/core"
title = "Optical transport core (CPU reference library)"
kind = "subsystem"
status = "experimental"
summary = """
OrbitOpticalTransport is a std-only library holding the physics and data structures of the unified-transport proposal: cache stamps and dependency \
invalidation, humidity-driven aerosol growth and moist-air refractivity, finite stellar-disk visibility and transmission-preserving filtering, a sparse \
optical hierarchy, an energy-preserving emitter tree, a lightning discharge graph with charge conservation, and cloud visual-form selection. It is not yet \
wired into any renderer or shader."""
owner_module = "OrbitOpticalTransport"
keywords = ["optical transport", "kappa kohler", "refractivity", "stellar disk", "penumbra", "optical hierarchy", "emitter tree", "many lights", "discharge graph", "cloud lod", "cache stamp"]
sources = [
  "engine/optical_transport/include/orbit/optical_transport/Atmosphere.hpp",
  "engine/optical_transport/include/orbit/optical_transport/CacheStamp.hpp",
  "engine/optical_transport/include/orbit/optical_transport/CloudRepresentation.hpp",
  "engine/optical_transport/include/orbit/optical_transport/DischargeGraph.hpp",
  "engine/optical_transport/include/orbit/optical_transport/EmitterTree.hpp",
  "engine/optical_transport/include/orbit/optical_transport/OpticalHierarchy.hpp",
  "engine/optical_transport/include/orbit/optical_transport/StellarVisibility.hpp",
  "engine/optical_transport/include/orbit/optical_transport/TransmittanceStats.hpp",
  "engine/optical_transport/CMakeLists.txt",
]
symbols = ["CacheStamp", "OpticalHierarchy", "EmitterTree", "DischargeGraph", "HygroscopicGrowthFactor", "DiskTransmission", "ChooseForm", "TransmittanceStats"]
invariants = [
  "Cache validity is decided by planet, region, medium version and source version only; InvalidateForCameraChange returns the empty set, a source change never invalidates the optical hierarchy, a medium change invalidates every downstream product.",
  "Relative humidity is derived from vapour mixing ratio, pressure and temperature; hygroscopic growth follows kappa-Kohler and is clamped below saturation; IsFog is true only at saturation.",
  "Disk transmission averages exp(-tau) over the (limb-darkened) stellar disk; FilterOpticalDepths keeps mean transmission, so EffectiveOpticalDepth <= mean optical depth (Jensen).",
  "OpticalHierarchy keeps min/max/mean per node; a coarse stop yields a conservative [lower, upper] interval, provably empty nodes are skipped, and SetVoxel touches exactly one node per level and bumps Version.",
  "EmitterTree preserves total power and lobe directionality; Evaluate never drops power (powerAccounted is the full total) and refines until the summed Lipschitz-style error bound is within tolerance of the estimate.",
  "DischargeGraph::Transfer conserves charge and refuses to move more than the source holds; EnergyInWindow integrates partial pulses proportionally and counts impulses, and ClusterEmission preserves window energy under any cluster cap.",
  "TransmittanceStats stores mean exp(-K s) at four fixed scales; every channel is a plain average, so Blend (box/linear mip) is exact, and Transmittance(K) is within 0.03 of the sampled truth over K = 0.2..40 where exp(-K mean s) is several times worse.",
  "Cloud form selection is a pure function of region and view; it never changes extent or density and never decides lighting participation. Errors scale with opacity so thin veils may use cheap forms.",
]
related = ["/rendering/unified-transport", "/rendering/unified-transport/plan-and-validation"]
used_by = []
verify = [
  "g++ -std=c++23 -Wall -Wextra -Iengine/optical_transport/include engine/optical_transport/src/*.cpp engine/optical_transport/tests/OpticalTransportTests.cpp && ./a.out (the repo CMake targets Windows only: ctest -R Orbit.OpticalTransport there)",
]
verified = "1d19bb1"

[[diagnose]]
symptom = "a unit test fails after changing a bound or tolerance"
steps = [
  "Hierarchy and emitter bounds must stay conservative: compare against ExactIrradiance or the brute-force depth in the test before loosening a tolerance.",
  "EstimateErrors constants are heuristics, not measurements; re-derive them from a reference render before relying on a LOD switch distance.",
]
docs = ["/rendering/unified-transport"]
+++

## Status

Everything here is a CPU reference implementation with unit tests. Nothing consumes it yet: no shader, renderer pass, RPC method or MCP tool uses it,
so none of the proposal's rendering behaviour exists. It is the numerical core that the GPU implementations are meant to match.

## Pieces

- **CacheStamp**: planet/region/time/version identity of a cached product, and which products each kind of change invalidates.
- **Atmosphere**: saturation vapour pressure (Magnus), humidity from mixing ratio, kappa-Kohler growth for four aerosol classes, Edlen-type air refractivity, vertical refractivity gradient and `BendRay` for dd/ds = (grad n - d(d.grad n))/n.
  The aerosol constants are representative placeholders, not fitted data.
- **StellarVisibility**: angular radius, penumbra width, disk coverage past a straight edge, horizon visibility, disk-averaged transmission, transmission statistics for filtering.
- **OpticalHierarchy**: power-of-two grid pyramid with conservative bounds, interval and exact optical depth along a segment.
- **EmitterTree**: median-split hierarchy; cosine-lobe emitters normalised to preserve power.
- **DischargeGraph**: segments with pulse histories, reactivation of existing channels, bounded clustering, Poisson relaxation for a coarse electric field.
- **TransmittanceStats**: linearly filterable transmittance statistics (see `/rendering/unified-transport/design-review`).
- **CloudRepresentation**: error estimates and hysteretic form choice.

## Not done

Cloud shader morphology and extinction fixes, GPU light volumes, aerosol and refraction integration into the atmosphere passes, glass/caustic restructuring, the
hybrid visibility service, render scheduler, RPC/MCP exposure and the editor UI. The hot-iteration path for this module is native-code only (generation handoff).
