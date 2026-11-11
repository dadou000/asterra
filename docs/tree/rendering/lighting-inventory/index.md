+++
path = "/rendering/lighting-inventory"
title = "Lighting, atmosphere, cloud and weather: system inventory"
kind = "meta"
status = "draft"
summary = """
An inventory of every system that produces, transports or consumes light in Orbit (star, atmosphere, weather, clouds, \
terrain visibility, direct lighting, radiance cache, reflections, water, exposure): what each is, where its data lives \
(CPU, GPU or built on one and used on the other), how big it is, how often it changes, and what feeds what. Written to \
plan a unified, cloud-aware ambient lighting system; graph.json beside it is the same inventory as nodes and edges for a \
planning canvas."""
keywords = ["inventory", "lighting", "atmosphere", "clouds", "weather", "residency", "cpu gpu", "architecture", "canvas", "graph"]
sources = [
  "engine/studio_ui/src/PlanningUi.cpp",
  "engine/lighting/src/DirectLighting.cpp",
  "engine/celestial_clouds/src/CloudRenderer.cpp",
  "engine/celestial_atmosphere/include/orbit/celestial_atmosphere/Atmosphere.hpp",
]
related = ["/rendering/lighting", "/rendering/atmosphere", "/rendering/clouds", "/rendering/unified-transport"]
depends_on = ["/rendering/lighting", "/rendering/atmosphere", "/rendering/clouds"]
verified = "d639bc09"
+++

## How to read this

The inventory is split by where light comes from and where it ends up:

- `/rendering/lighting-inventory/sources-and-medium`: star, orbits, atmosphere, weather, clouds, optical transport,
  planetary appearance (everything that decides how much light exists and what it passes through).
- `/rendering/lighting-inventory/surface-and-lighting`: terrain data and visibility, direct lighting, the radiance
  cache, the new terrain ambient channel, reflections, water and exposure (everything that turns light into pixels).
- `/rendering/lighting-inventory/flows-and-gaps`: the CPU/GPU transfer ledger, duplicated mechanisms, gaps and
  candidate residency moves.

`graph.json` (same folder) holds the same facts as 41 nodes and 59 edges. Every node has `residency`, `size`,
`update_rate`, a doc path and source paths; every edge has a `transport` (shader code, CPU to GPU constants, GPU only,
GPU to CPU readback) so a canvas can colour the CPU/GPU boundary. `confidence` is `verified` when the fact was read in
code or docs, `inferred` otherwise.

## Residency vocabulary

| Value | Meaning |
|---|---|
| cpu | Authoritative and consumed on the CPU. |
| gpu | Produced and consumed on the GPU; never crosses the bus. |
| cpu->gpu | Built on the CPU and uploaded; consumed by shaders. |
| cpu+gpu | A CPU control path and a GPU data path (for example residency plus cells). |

## Principles used to judge placement

1. Compute where the data already lives and end up where it is consumed; shaders are the consumers of light.
2. CPU to GPU is cheap for small constants, tables and dirty-region deltas; GPU to CPU is avoided in the render path
   (tiny async diagnostics only, with the frames-in-flight delay the ground-height readback already uses).
3. Arrays larger than about 64 KB do not cross the bus at runtime.
4. Data-parallel per-texel or per-ray work belongs on the GPU; serial, branchy or cache-once work (solvers, tables,
   keys) belongs on the CPU.
5. Sun, cloud and weather change slowly: latency is acceptable, so work can be lazy, amortised and cross-faded.

## Scale of the inventory

41 systems across 8 layers (lighting 8, medium 7, cloud 7, terrain 6, weather 5, control 4, source 3, output 1); 59
data or control edges. Of the nodes, 17 are GPU-resident, 13 CPU-only, 8 built on the CPU and uploaded, 3 split. 16
edges cross CPU to GPU at runtime and 2 cross GPU to CPU: a 16-byte ground-height readback every frame and storm
checkpoints rarely. See flows-and-gaps for the sizes and rates.
