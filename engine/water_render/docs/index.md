+++
path = "/rendering/water"
title = "Water rendering (rivers and lakes)"
kind = "subsystem"
status = "stable"
summary = "RiverWaterRenderer draws river segments and lake cells from regional terrain caches. The detached OceanRenderer and the OceanMesh generator it used were removed in 0.0.9: nothing in the apps linked them."
owner_module = "OrbitWaterRender"
keywords = ["water", "ocean", "river", "lake", "water render", "region cache"]
sources = [
  "engine/water_render/include/orbit/water_render/RiverWaterRenderer.hpp",
  "engine/water_render/CMakeLists.txt",
]
symbols = ["RiverWaterCamera"]
invariants = [
  "Legacy cell overlays are opt-in: standing water normally belongs to the terrain surface, and enabling both draws duplicate lake surfaces.",
  "With a finer fine-region cache, wherever it has ready coverage the finer segment or lake cell is drawn instead of the coarse one, never both (which would z-fight or double up water at slightly different elevations).",
]
related = ["/rendering/terrain", "/legacy/standing-water-rendering"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/rhi", "/rendering/shader-compiler", "/rendering/terrain/regions", "/rendering/terrain/water", "/world/planet-coordinates"]
used_by = ["/apps/sandbox"]
verify = [
  "ctest -R Orbit.RiverWater",
]
verified = "b0a0de7f"
+++


