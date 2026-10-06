+++
path = "/rendering/terrain/clipmaps/precision-and-pages"
title = "Float precision and physical-page compositing"
kind = "concept"
status = "stable"
owner_module = "OrbitTerrainRender"
summary = """
Hardware sin/cos of offset/planetRadius is good to only ~3e-7 (about 2 m at planet radius), which snapped samples \
onto concentric rings. Every shader that turns a metre offset into a surface direction now uses a series for angles \
under 0.2 rad. Physical-page composites fade from generated terrain to the page over the page's outer 6 texels so no \
height step appears along page bounds."""
keywords = ["precision", "sin", "cos", "float32", "concentric rings", "spiky", "physical page", "composite", "height step", "page bounds", "series"]
sources = [
  "engine/terrain_render/src/ClipmapVertexShader.hpp",
  "engine/terrain_gpu/src/PhysicalPageCompositeCompute.hpp",
  "engine/terrain_gpu/src/GpuPhysicalPageComposite.cpp",
  "engine/terrain_gpu/src/FieldGenerationCompute.hpp",
  "engine/terrain_gpu/src/RegionDeltaCompute.hpp",
]
symbols = ["SurfaceDirectionForOffset", "SurfaceDirectionForOffsetFromBasis"]
invariants = [
  "Every shader that converts a metre offset to a surface direction (clipmap vertex shader and the field-generation, page-composite and region-delta compute shaders) uses the small-angle series below 0.2 rad (relative error < 1e-11) and sin/cos above.",
  "A new shader doing offset/planetRadius -> direction must use the same helper, not raw sin/cos: raw hardware sin/cos snaps samples onto concentric rings at planet scale.",
  "The page composite fades from generated terrain to the physical page over the page's outer 6 texels; it must not replace elevation/water depth outright inside the page.",
  "physical_pages (view.terrain_layers_set) disables the composite for comparison.",
  "Float32 positions lose precision far from a frozen clipmap window (about 1 m at 10,000 km); that is acceptable only for inspection.",
]
related = ["/rendering/terrain/clipmaps/lattice-and-tracking", "/rendering/terrain/clipmaps/generator-parity"]
depends_on = ["/rendering/terrain/clipmaps"]
verify = [
  "Wireframe (clipmap_wireframe) must not show polar-looking concentric structure; terrain must not show spiky ring patterns.",
  "Toggle physical_pages and enable cache_status: no elevation step along the page outline with the composite on.",
]
verified = "b0a0de7f"

[[diagnose]]
symptom = "spiky concentric ring patterns in the terrain or a polar-looking wireframe"
steps = [
  "Suspect float precision in offset -> direction conversion: grep the shader for raw sin(/cos( of offset/planetRadius.",
  "Switch on clipmap_wireframe: rings centred on the planet axis rather than the camera confirm angle quantisation.",
  "Use SurfaceDirectionForOffset (ClipmapVertexShader.hpp) or copy its series branch.",
]
docs = ["/rendering/terrain/clipmaps/debugging"]

[[diagnose]]
symptom = "a height step along the outline of a physical page"
steps = [
  "Enable cache_status (view.terrain_overlays_set) to draw the page outline and confirm the step follows it.",
  "Toggle physical_pages off (view.terrain_layers_set): if the step disappears, the composite edge fade is wrong or missing.",
  "Check the edge fade is the page's outer 6 texels in GpuPhysicalPageComposite / PhysicalPageCompositeCompute.",
]
docs = ["/rendering/terrain/clipmaps/debugging"]
+++

## What went wrong before

1. **Rings.** Hardware `sin`/`cos` of `offset / planetRadius` is accurate to about 3e-7, i.e. about 2 m at planet
   radius, so sample and vertex positions snapped onto concentric rings. Angles under 0.2 rad now use a series
   (error below 1e-11 relative); larger angles use `sin`/`cos`.
2. **Page step.** The page composite used to replace the generated elevation and water depth outright inside a
   physical page, leaving a height step along the page bounds. It now fades from generated terrain to the page over the
   page's outer 6 texels.
