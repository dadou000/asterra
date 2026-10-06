+++
path = "/rendering"
title = "Rendering"
kind = "section"
status = "stable"
summary = """
Everything that turns the planet into pixels: the terrain stack (generation, clipmaps, persistent GPU \
caches, water), celestial rendering (atmosphere, clouds, globe, rings, stars), lighting (radiance cache, \
proxy sun shadow, eye adaptation), shading, post-process and performance. Terrain/clipmaps is fully \
documented as blocks; other areas are reachable through the section routes below."""
keywords = ["rendering", "gpu", "vulkan", "terrain", "atmosphere", "clouds", "lighting", "shading", "performance", "post process"]
related = ["/editor/viewport"]

[routes]
"terrain generation, clipmaps, water, GPU terrain cache" = "terrain"
"terrain looks wrong: holes, cracks, rings, popping, wrong detail" = "terrain/clipmaps/debugging"
"hitch or multi-second freeze when altitude changes" = "terrain/clipmaps/rebuild-hitches"
"proxy sun shadow, visible proxies" = "/legacy/orbit-mcp/proxy-sun-shadow-and-visible-proxies"
"sky-only radiance cache, final gather, indirect lighting" = "/legacy/orbit-mcp/sky-only-radiance-cache-channel"
"eye adaptation, highlight protection, exposure" = "/legacy/orbit-mcp/eye-adaptation-highlight-protection-and-boost-limit"
"shader contract, shading assets, material shaders" = "/legacy/orbit-shading"
"frame cost, idle pacing, far terrain, clouds in clipmap view" = "/legacy/orbit-performance"
"CPU profiler, GPU timing, Perfetto trace" = "/legacy/orbit-profiler"
"standing water, near-field water pass" = "/legacy/standing-water-rendering"
"atmosphere, clouds, rings, stars, orbits, celestial research" = "/legacy/research-v006-physical-atmosphere"
+++

Rendering module families (each is `engine/<name>/`):

- **terrain stack:** `terrain` and the `terrain_*` modules (see `/rendering/terrain`).
- **celestial:** `celestial_atmosphere`, `celestial_clouds`, `celestial_globe`, `celestial_far_render`,
  `celestial_rings`, `celestial_stellar`, `celestial_ocean`, `celestial_radiometry`, ... (specs and research
  under `/legacy`, starting from `docs/V0.0.6_SPEC.md`).
- **lighting / shading / post:** `lighting`, `shading`, `post_process`, `shader`, `render_graph`.
- **device layer:** `rhi` (interface) and `rhi/vulkan` backend; world state never holds GPU handles
  (`/rules/architecture`).
- **volumes / water:** `volume_fields`, `volume_render`, `volume_solver`, `water_render`.

Performance findings and measured numbers live in `docs/ORBIT_PERFORMANCE.md` (per-section under
`/legacy/orbit-performance`).
