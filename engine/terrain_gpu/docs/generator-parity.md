+++
path = "/rendering/terrain/clipmaps/generator-parity"
title = "GPU generator parity with the CPU source"
kind = "concept"
status = "stable"
owner_module = "OrbitTerrainGpu"
summary = """
The clipmap's GPU field generator (FieldGenerationCompute.hpp) is a float32 port of AnalyticTerrainSource. \
rendered_ground_elevation_meters (GPU, four vertices under the camera) next to rendered_ground_cpu_elevation_meters \
(CPU evaluated at exactly those vertices) is a direct parity check; flying a line they must agree. When changing the \
generator, bisect by stage outputs against the CPU, not by eye."""
keywords = ["parity", "gpu generator", "cpu", "analytic terrain source", "field generation", "hash", "mix64", "crater", "bisect", "elevation mismatch"]
sources = [
  "engine/terrain_gpu/src/FieldGenerationCompute.hpp",
  "engine/terrain_gpu/src/GpuFieldGenerator.cpp",
  "engine/terrain/include/orbit/terrain/AnalyticTerrainSource.hpp",
]
symbols = ["GpuFieldGenerator", "AnalyticTerrainSource"]
invariants = [
  "The GPU field generator must stay numerically equivalent to AnalyticTerrainSource within float32 tolerance (42 samples from sea level to 2 km mountains at three altitudes agreed to 0.07 m or better).",
  "The vector-noise corner hash is Mix64(seed ^ x ^ y ^ z) split into three 21-bit channels (mask 0x1FFFFF); a different mask or an un-mixed hash changes the mountain domain warp and therefore the relief.",
  "A crater's bounding test must not round to exactly 1.0 for small craters (dot() < boundingCosine culled them almost everywhere).",
  "Changes to the generator are validated per stage (global coarse, macro, mountains, craters, detail) against the CPU, never visually.",
]
related = ["/rendering/terrain/clipmaps/camera-floor"]
depends_on = ["/rendering/terrain"]
verify = [
  "Fly a line and compare view.text_diagnostics clipmap_plan.rendered_ground_elevation_meters with rendered_ground_cpu_elevation_meters: they must agree.",
  "ctest -R Gpu (engine/terrain_gpu/tests) for the shader parity tests.",
]
verified = "b0a0de7f"

[[diagnose]]
symptom = "GPU terrain height differs from CPU terrain height (camera floor or altitude readout disagrees with what is drawn)"
steps = [
  "Read view.text_diagnostics: compare clipmap_plan.rendered_ground_elevation_meters with rendered_ground_cpu_elevation_meters and below_camera.terrain_elevation_meters.",
  "Fly a line and plot both; a constant offset suggests physical pages (toggle physical_pages), a position-dependent one suggests a port bug.",
  "Bisect with stage outputs against the CPU: global coarse, macro, mountains, craters, detail. Check the three historical bugs first (corner hash, 21-bit mask, crater bounding test).",
]
docs = ["/rendering/terrain/clipmaps/precision-and-pages", "/rendering/terrain/clipmaps/camera-floor"]
+++

## History (so the bugs are recognised if they return)

The two readings disagreed by -11 to +58 m until three port bugs were fixed:

1. the vector-noise corner hash was not `Mix64`-ed (the CPU hashes each corner with `Mix64(seed ^ x ^ y ^ z)` and
   splits it into three channels);
2. its 21-bit channel mask was `0x1F000FFFFF` instead of `0x1FFFFF` (each channel kept 20 bits, biased into [-1, 0]);
   together with (1) the mountain domain warp differed, so the mountain relief did;
3. the crater bounding test `dot() < boundingCosine` rounded to exactly 1.0 for small craters and culled them almost
   everywhere.

After the fixes, 42 samples from sea level to 2 km mountains at three altitudes agree to 0.07 m or better.
