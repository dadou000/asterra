+++
path = "/rendering/water"
title = "Water rendering (ocean mesh and rivers)"
kind = "subsystem"
status = "stable"
summary = "OceanRenderer/OceanMesh draw the ocean surface and RiverWaterRenderer draws river segments and lake cells from regional terrain caches."
owner_module = "OrbitWaterRender"
keywords = ["water", "ocean", "river", "lake", "water render", "ocean mesh", "region cache"]
sources = [
  "engine/water_render/include/orbit/water_render/OceanMesh.hpp",
  "engine/water_render/include/orbit/water_render/OceanRenderer.hpp",
  "engine/water_render/include/orbit/water_render/RiverWaterRenderer.hpp",
  "engine/water_render/CMakeLists.txt",
]
symbols = ["OceanMeshConfig", "OceanCamera", "RiverWaterCamera"]
invariants = [
  "Legacy cell overlays are opt-in: standing water normally belongs to the terrain surface, and enabling both draws duplicate lake surfaces.",
  "With a finer fine-region cache, wherever it has ready coverage the finer segment or lake cell is drawn instead of the coarse one, never both (which would z-fight or double up water at slightly different elevations).",
]
related = ["/rendering/terrain", "/legacy/standing-water-rendering"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/rhi", "/world/planet-coordinates"]
verify = [
  "ctest -R Orbit.OceanMesh",
]
verified = "b0a0de7f"
+++


