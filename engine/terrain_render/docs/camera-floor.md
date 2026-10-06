+++
path = "/rendering/terrain/clipmaps/camera-floor"
title = "Camera floor on the drawn ground"
kind = "concept"
status = "stable"
owner_module = "OrbitTerrainRender"
summary = """
Navigation used to clamp the camera to the CPU terrain, but the clipmap draws GPU-generated terrain (plus physical \
pages), which can differ by tens of metres. The renderer now reads back the four vertex elevations around the point \
below the camera from the finest drawn level each frame; StudioRenderViewSet keeps it per view and NavigateTerrain \
floors elevation queries within ~150 m of that point at the highest of the four."""
keywords = ["camera floor", "ground clearance", "under the ground", "rendered ground elevation", "readback", "navigate terrain", "altitude above terrain"]
sources = [
  "engine/terrain_render/src/TerrainPreviewRenderer.cpp",
  "engine/studio_ui/src/StudioRenderViewSet.cpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioRenderViewSet.hpp",
]
symbols = ["NavigateTerrain", "StudioRenderViewSet"]
invariants = [
  "The floor only ever raises the camera; where the drawn ground is below the CPU terrain, the CPU value stays.",
  "The floor uses the highest of the four vertices around the nadir (conservative) and applies to elevation queries within about 150 m of that point.",
  "The readback is a few frames late by design (host-readable buffer); it must never stall the frame.",
  "Reported as clipmap_plan.rendered_ground_elevation_meters and the 'Drawn ground under camera' HUD line, beside the CPU's below_camera.terrain_elevation_meters.",
]
related = ["/rendering/terrain/clipmaps/generator-parity", "/editor/viewport"]
depends_on = ["/rendering/terrain/clipmaps", "/editor/viewport"]
verify = [
  "Measured over land: drawn ground up to 58 m above the CPU terrain, over ocean about 10 m below; ground clearance must follow the drawn surface.",
]
verified = "b0a0de7f"

[[diagnose]]
symptom = "the camera is below, or clips into, the terrain that is drawn"
steps = [
  "Read view.text_diagnostics: compare clipmap_plan.rendered_ground_elevation_meters with below_camera.terrain_elevation_meters.",
  "If the drawn ground is higher than the CPU value the floor should already lift the camera; check the readback is arriving (value changing as you move).",
  "If the two disagree by tens of metres systematically, check generator parity before changing the floor.",
]
docs = ["/rendering/terrain/clipmaps/generator-parity"]
+++

The terrain renderer copies the four vertex elevations around the point below the camera out of the finest
drawn level into a small host-readable buffer each frame and reads them back a few frames later.
`StudioRenderViewSet` keeps the result per view, and `NavigateTerrain` floors every elevation query within about
150 m of that point at it, so ground clearance and altitude-above-terrain follow the drawn surface.
