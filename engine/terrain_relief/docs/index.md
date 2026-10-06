+++
path = "/rendering/terrain/relief"
title = "Base relief and derivative procedural fields (M06)"
kind = "subsystem"
status = "stable"
summary = "BaseReliefField is the process-independent procedural relief layer downstream of macro geology: continental base band, ridge and valley bands, analytic tangent derivatives and physical-footprint spectral filtering, evaluated directly in canonical body space."
owner_module = "OrbitTerrainRelief"
keywords = ["relief", "base relief", "noise", "ridge", "valley", "derivative", "slope", "footprint", "spectral", "procedural height"]
sources = [
  "engine/terrain_relief/include/orbit/terrain_relief/BaseReliefField.hpp",
  "engine/terrain_relief/CMakeLists.txt"]
symbols = ["BaseReliefField", "BaseReliefDesc", "BaseReliefSample", "ReliefDerivative"]
invariants = [
  "Relief is derived procedural state: individual cheap noise octaves are evaluated directly and are never cached as authority.",
  "Every band samples from the canonical unit direction (noisePosition = unitDirection * planetRadius / wavelength): no cube-face UV, clipmap ring, cache slot or camera coordinate participates in phase.",
  "Changing the physical footprint changes only a band's spectral weight, never its coordinate, seed or phase.",
  "Derivatives are analytic (the quintic value-noise interpolation is differentiated and converted to physical east/north slope with the sphere chain rule), not finite differences.",
  "The M05 uplift channel is deliberately NOT added to heightMeters: drainage and stream-power erosion turn uplift, resistance and drainage into the large-scale landform."]
related = ["/rendering/terrain/macro-geology", "/rendering/terrain/clipmaps/generator-parity"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/terrain/contracts", "/rendering/terrain/macro-geology", "/world/planet-coordinates"]
verify = [
  "ctest -R Orbit.TerrainRelief"]
verified = "b0a0de7f"
+++


