+++
path = "/rendering/volumes/render"
title = "Volume rendering and GPU particles"
kind = "subsystem"
status = "stable"
summary = "UniversalVolumeRenderer ray-marches volumes (with debug modes, runtime settings and diagnostics); the persistent M38 GPU particle renderer keeps particle state, integration and terrain collision GPU resident and presents with weighted OIT and soft depth intersections."
owner_module = "OrbitVolumeRender"
keywords = ["volume render", "particles", "gpu particles", "oit", "soft particles", "collision", "universal volume renderer", "m38", "droplets", "splash"]
sources = [
  "engine/volume_render/include/orbit/volume_render/UniversalVolumeRenderer.hpp",
  "engine/volume_render/include/orbit/volume_render/VolumeParticleGpuBinding.hpp",
  "engine/volume_render/include/orbit/volume_render/VolumeParticleGpuState.hpp",
  "engine/volume_render/include/orbit/volume_render/VolumeParticleRenderer.hpp",
  "engine/volume_render/CMakeLists.txt",
]
symbols = ["VolumeRenderRuntimeSettings", "VolumeParticleGpuSpawn", "VolumeParticleGpuStateRecord", "VolumeParticleRenderer"]
invariants = [
  "Auto representation is the M36 policy wrapper over the unchanged M35 backend; the authored VolumeRepresentationMode stays the explicit force/debug authority.",
  "CPU input to the particle renderer is limited to compact spawn and terrain-page metadata; particle state, integration and collision remain GPU resident between simulation generations.",
  "Transparent particles use weighted OIT, read read-only scene depth for soft intersections and use the same directional and local-light authority as the shared lighting path.",
  "Particle collision refines the cheap reference-ellipsoid test with the resident physical terrain pages; pages outside coverage are skipped, and only compact tile metadata crosses the CPU.",
  "Per-viewport active-index lists and indirect draw arguments are rebuilt at presentation time while the simulation state stays shared and GPU-resident.",
  "The particle centre is rebased with the particle state whenever the floating/presentation origin changes.",
]
related = ["/rendering/volumes", "/rendering/lighting", "/rendering/terrain/gpu-cache", "/legacy/tree-history-research-v007-universal-volumetrics"]
depends_on = ["/authoring/scene", "/foundation/core", "/foundation/math", "/rendering/lighting/radiance-cache", "/rendering/render-graph", "/rendering/render-view", "/rendering/rhi", "/rendering/shader-compiler", "/rendering/volumes/fields", "/rendering/volumes/representation", "/world/world-model"]
used_by = ["/editor/studio-ui"]
verify = [
  "ctest -R Orbit.UniversalVolumeRenderer",
]
verified = "db348ce94035630577b705cffe0c69c6f8a6061f"
+++


