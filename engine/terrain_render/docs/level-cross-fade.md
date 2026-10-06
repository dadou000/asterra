+++
path = "/rendering/terrain/clipmaps/level-cross-fade"
title = "Level cross-fade"
kind = "concept"
status = "stable"
owner_module = "OrbitTerrainRender"
summary = """
A level the plan adds fades in, and one it drops fades out, over clipmap_fade_seconds (default 0.4 s) using a \
per-pixel screen-door dither in the clipmap pixel shader. The fading level keeps a pixel where interleaved-gradient \
noise is below its coverage and the level around it takes the rest, so the two always add up to one surface. Content \
is generated once and never morphed in time."""
keywords = ["cross fade", "fade", "dither", "screen door", "pop", "popping", "level transition", "hole", "hand-off", "clipmap_fade_seconds"]
sources = [
  "engine/terrain_render/src/TerrainPreviewRenderer.cpp",
  "engine/terrain_render/src/ClipmapVertexShader.hpp",
  "engine/terrain_render/src/TerrainSurfaceShader.hpp",
]
symbols = ["AdvanceLevelFades", "selfFade", "finerFade"]
invariants = [
  "A pixel is owned by exactly one level during a fade: fading level keeps it where noise < coverage, the surrounding level takes the remaining pixels (they always sum to one surface).",
  "Levels still fading stay in the layout (generated and drawn), so the HUD's active range can briefly be wider than the planner's.",
  "While the finer level is mid-fade the hole in the coarser level is not cut; it is dithered, and becomes a hard cut once the finer level is fully drawn.",
  "Content does not change during a fade; it is generated once when the level is added. Only dissolved, never morphed in time.",
  "The outermost level dissolves into the sky. The water pass is not dithered (flat at sea level, so the levels coincide).",
  "clipmap_fade_seconds = 0 gives the old instant swap, for comparison only.",
  "Banded (experimental) levels do not use time fades; selfFade/finerFade carry band edges in metres instead.",
]
related = ["/rendering/terrain/clipmaps/level-planning"]
depends_on = ["/rendering/terrain/clipmaps"]
verify = [
  "Sweep altitude with clipmap_fade_seconds = 0 and = 0.4 and compare frames at the plan boundary: the 0.4 s run must show no hard pop.",
  "Freeze clipmaps (clipmap_freeze): a dissolve already under way still finishes.",
]
verified = "b0a0de7f"

[[diagnose]]
symptom = "terrain pops or a visible seam appears when altitude changes"
steps = [
  "Check clipmap_fade_seconds is not 0 (view.terrain_layers_get).",
  "Tint by level (clipmap_levels) and record the boundary: if the seam is exactly at a level edge, enable clipmap_hole_view to see why vertices there are culled.",
  "Check planner churn: if first/last level change every few frames, raise ClipmapPlannerConfig::hysteresisOctaves or fix the input that oscillates (ground elevation hint, FOV) rather than lengthening the fade.",
  "A pop under 1% of pixels at a one-octave step was measured acceptable; sinking/morphing handoffs were deliberately skipped - revisit only if a real pop is seen while flying.",
]
docs = ["/rendering/terrain/clipmaps/debugging", "/rendering/terrain/clipmaps/level-planning"]
+++

## Where it lives

`TerrainPreviewRenderer::Draw` advances the fades each frame (`AdvanceLevelFades`) and passes per-level
`selfFade` / `finerFade` to the clipmap shaders (`ClipmapVertexShader.hpp`, `TerrainSurfaceShader.hpp`). A time
fade is used when the ladder is active (negative mode flag); for distance bands the same two values are the
band's inner/outer edge.

## Design notes

Sinking transitions (pushing geometry below the reference surface near level boundaries) were measured and
skipped: forcing the finest level to jump two octaves at a fixed pose changed under 1% of pixels by more than a
small threshold, so a time-based handoff would add shader work for no visible gain. The debug overlays show
where handoffs fall if a pop is seen later.
