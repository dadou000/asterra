+++
path = "/rendering"
title = "Rendering"
kind = "section"
status = "stable"
summary = """
Everything that turns the planet into pixels: the terrain stack (generation, clipmaps, persistent GPU \
caches, water), celestial rendering (atmosphere, clouds, globe, rings, stars), lighting (radiance cache, \
proxy sun shadow, eye adaptation), shading, post-process and performance. Terrain/clipmaps, lighting, clouds and the atmosphere are \
documented as blocks; other areas are reachable through the section routes below."""
keywords = ["rendering", "gpu", "vulkan", "terrain", "atmosphere", "clouds", "lighting", "shading", "performance", "post process"]
related = ["/editor/viewport"]

[routes]
"terrain generation, clipmaps, water, GPU terrain cache" = "terrain"
"terrain looks wrong: holes, cracks, rings, popping, wrong detail" = "terrain/clipmaps/debugging"
"hitch or multi-second freeze when altitude changes" = "terrain/clipmaps/rebuild-hitches"
"lighting, direct/indirect light, reflections, bypass flags" = "lighting"
"proxy sun shadow, visible proxies" = "lighting/proxy-sun-shadow"
"sky-only radiance cache, sky fill, enclosed spaces black" = "lighting/sky-cache-fill"
"eye adaptation, highlight protection, exposure" = "lighting/eye-adaptation"
"tone mapping, color LUT, histogram, display resolve" = "post-process"
"clouds, cloud shadows, god rays, cloud cost" = "clouds"
"atmosphere, sky, scattering, limb, sunset, atmosphere LUTs" = "atmosphere"
"author an atmosphere from pressure/composition, presets, locked values" = "atmosphere/authoring-solver"
"GPU API, Vulkan, command lists, barriers, ray query capability" = "rhi"
"render graph, passes, GPU pass timing" = "render-graph"
"render views, screenshots, captures" = "render-view"
"free camera, altitude-scaled navigation" = "free-camera"
"shader compile, DXC, SPIR-V" = "shader-compiler"
"shader contract, shading tab, material preview" = "shading"
"volumes, fog, particles, volumetrics" = "volumes"
"frame cost, idle pacing, far terrain, clouds in clipmap view" = "/legacy/orbit-performance"
"CPU profiler, GPU timing, Perfetto trace" = "/legacy/orbit-profiler"
"standing water, near-field water pass" = "/legacy/standing-water-rendering"
"planets seen from orbit, rings, stars, orbits" = "/celestial"
+++

Rendering module families (each is `engine/<name>/`):

- **terrain stack:** `terrain` and the `terrain_*` modules (see `/rendering/terrain`).
- **celestial:** `celestial_atmosphere` (`/rendering/atmosphere`), `celestial_clouds` (`/rendering/clouds`); the other `celestial_*` modules are under `/celestial`.
- **lighting / shading / post:** `lighting`, `shading`, `post_process`, `shader`, `render_graph`.
- **device layer:** `rhi` (interface) and `rhi/vulkan` backend; world state never holds GPU handles
  (`/rules/architecture`).
- **volumes / water:** `volume_fields`, `volume_render`, `volume_solver`, `water_render`.

Performance findings and measured numbers live in `docs/ORBIT_PERFORMANCE.md` (per-section under
`/legacy/orbit-performance`).
