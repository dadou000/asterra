# Problem list

Running tracker of known issues to recheck later. Newest first. When a problem is
resolved, keep the entry and move it to **Resolved** with the fix + how it was
verified. When something is fixed but not fully verified, leave it **Open** with a
`RECHECK:` line stating exactly what still needs confirming.

Status key: `OPEN` · `FIX-UNVERIFIED` (fix landed, needs confirmation) · `RESOLVED`

---

## Open

### P-012 — Center L0 quadtree ring offset causes bind-camera-culling terrain disappearance; scatter placement offset in the air; scatter casts no shadows; scatter lit side not aligned with light source
- **Status:** OPEN, partially fixed. Symptom 3 (no shadows) fixed and verified.
  Symptom 2 (floating) root-caused to a specific mechanism, not yet fixed.
  Symptoms 1 and 4 still not investigated. The four symptoms do **not** appear
  to share one root cause after this pass — see below.
- **Found:** 2026-09-11, reported by user, not yet investigated at filing time.
- **Symptom 3 (no shadows) — FIXED, verified 2026-09-11:** `scripts/terrain/
  gpu_terrain_scatter.gd::_build_batches()` passed a hardcoded `false` for
  `cast_shadows` to all three `_make_batch()` calls (grass, geologic stone,
  river stone) regardless of any setting — shadow casting was unconditionally
  disabled for every scatter instance in the game, not a subtle bug. Changed
  all three to `true`. Verified: `--headless --import` clean (no parse
  errors), fresh windowed 4K launch, live in `Main` (not Planet Studio).
  Not yet re-screenshotted with a visible cast shadow on the ground because
  the same live session hit symptom 2 at the test site (floating instances
  have nothing under them to shadow) — RECHECK at a site where scatter sits
  on solid, fully-streamed ground.
- **Symptom 2 (floating) — root-caused, not fixed:** Confirmed live via
  `studio_screenshot` (F9-loaded save `phase1`, lat 38.27 lon 61.17): several
  geologic-stone instances render clearly detached above the ground, and the
  debug HUD reads `GPU scatter global 2048²×6 STABLE FALLBACK` continuously
  while `spherical L0-L10 ... sectors 5/12` never advances past 5/12. Waited
  8 s and re-screenshotted at the same camera pose — **pixel-identical**
  output (same rock positions, same `STABLE FALLBACK`, same `sectors 5/12`),
  ruling out "still streaming in, will settle" as the explanation. This is a
  persistent stall at this location, not a load transient.
  `shaders/gpu_scatter_common.gdshaderinc::sg_terrain_sample()` has two paths:
  the primary one reads the actual rendered L0 height cache
  (`sg_rendered_l0_sample`, exact match to what's drawn) and a fallback that
  reconstructs height from `sg_macro_height() + gm_geomorph_height(...)` when
  the cache read fails — the HUD's "STABLE FALLBACK" label corresponds to this
  fallback branch being active. The fallback's reconstruction evidently does
  not exactly match the real rendered terrain surface, which is the floating.
  **Not yet found:** why `sg_rendered_l0_sample` fails persistently at this
  site (vs. clearing after a frame or two elsewhere) — needs tracing into why
  `sectors 5/12` itself stalls (terrain streaming, not just the scatter cache)
  in `spherical_geometry_clipmap_*.gd` / the GPU height page atlas.
  **Correction: "STABLE FALLBACK" is not a live signal at all.**
  `gpu_terrain_scatter_global.gd:12` — `const STABLE_FALLBACK_ONLY := true` is
  a hardcoded, permanent flag (the GPU-compute classification path is force-
  disabled project-wide); the HUD always shows "STABLE FALLBACK" regardless of
  per-frame cache state. My first-pass reasoning that it indicated a stalled
  cache was wrong. Got a working ground-facing camera this time (see method
  note below) and re-tested at the `T` good site (lat 37.93 lon 39.37,
  `sectors 12/12`, fully streamed, non-coastal Temperate forest): floating
  rocks/trees still clearly visible, **and** a new HUD field I added
  (`scripts/ui/hud.gd`, next to the scatter line) reads
  `terrain cache BOUND gen 5` — the authoritative rendered-terrain cache
  scatter reads from is bound and ready. So it is not "cache never
  populates" either.
- **Root cause found, confirmed by direct code inspection (not yet fixed):**
  `shaders/terrain_clipmap_cache.glsl:91` — the compute shader that fills the
  cache scatter reads (`sg_rendered_l0_sample` in `gpu_scatter_common.
  gdshaderinc`) computes `final_h = macro_h + geomorph(dir, spacing, macro_h)
  * coast_guard * ...` — i.e. macro elevation plus the multi-octave procedural
  landform shape only (`geomorph()`'s finest band is 24 m wavelength noise).
  It does **not** call anything from `shaders/gpu_material_microrelief.
  gdshaderinc`. That include *is* used by the actual terrain surface shaders
  (`spherical_geometry_clipmap_cached_surface.gdshader`,
  `spherical_geometry_clipmap_global_surface.gdshader`) to add a dense,
  near-camera-only displacement layer on top ("Near-field geometric
  microrelief" in the debug menu, ON by default). So: the cache scatter
  places itself against is the terrain height **before** microrelief: the
  visually rendered ground (with microrelief) and the height scatter reads
  are two different surfaces near the camera, and the gap between them is the
  floating.
- **Why not fixed yet:** the correct fix (evaluate microrelief inside
  `terrain_clipmap_cache.glsl` too) means porting a near-field-only, camera-
  proximity-gated displacement into a background cache-population compute
  pass that currently has no notion of camera distance and runs over a much
  wider area — a real architecture change to a shared, perf-sensitive shader,
  not a narrow fix. A cheap mitigation exists (`u_scatter_surface_bias`,
  already plumbed into both scatter shaders) but it is one constant and
  microrelief is spatially varying, so it would reduce but not eliminate the
  float, and could equally make some instances sink instead. Left for a
  deliberate follow-up rather than guessed at with the budget remaining this
  session — matches this file's own precedent on P-011 (fix scoped narrowly,
  not the shared generation pipeline, when blast radius is high).
- **Method note for next time:** getting a ground-facing screenshot needs
  `studio_input` mouse `motion` events with `_mouse_captured` already true
  (true by default once spawned in `Main`) — **positive** `relative.y` pitches
  down (`pitch = pitch - relative.y * MOUSE_SENS`, `player.gd:164`), negative
  pitches up. A single huge-delta event did not register reliably; several
  smaller incremental motion events (e.g. 8-13x `relative.y: ±60-80`) did.
- **Attempted the real fix 2026-09-11: landed, verified safe, did NOT resolve
  the visible floating.** Ported `gpu_material_microrelief.gdshaderinc`'s pure
  math (`mmr_height` + its `spat_periodic_value` noise dependency from
  `gpu_surface_antitile.gdshaderinc`) directly into `shaders/
  terrain_clipmap_cache.glsl`, applied for `level == 0` cache cells only,
  matching the real surface shaders' own gate. Could not `#include` the
  original files as-is: their `uniform float u_microrelief_*` declarations
  are Godot ShaderMaterial-style bindings with no meaning in a raw
  `#[compute]` RDShaderFile, so the math was duplicated with every
  `u_microrelief_*_scale` baked to its 1.0 default. Deliberately used the
  already-local `offset_m`/`final_h` as the noise-domain position instead of
  the planet-absolute `dir*radius` `geomorph()` uses a few lines above:
  microrelief's finest noise cell is 1 m, and `dir*radius` is planet-radius
  magnitude (millions of metres) — at that scale float32 does not have enough
  precision left for 1 m cells (the same class of bug as the
  [[authored-displacement-cpu-gpu-split]] memory note's "float32 `fract()`
  spikes"), whereas `geomorph`'s finest band (24 m) tolerates it. `rock_id`
  and `biome` are approximated as `0.0`/unused (matches
  `gpu_material_microrelief.gdshaderinc`'s own note that `biome` is accepted
  but never read; `rock_id` only reshapes the rock sub-pattern, not the
  overall material weighting). `--headless --import` compiled clean both
  times. **Live result:** re-tested at the identical `T` good site, ground-
  facing pose — the screenshot is **pixel-identical** to the pre-fix one,
  same rocks floating in the same positions. Reading the ported math's
  coefficients (e.g. `mmr_rock_height`'s dominant terms are ~0.12/0.055/0.022
  m magnitude before the ~1.0-1.2x rock-type multiplier), the real
  contribution here is sub-metre — too small to account for what looks like a
  1-3 m gap in the screenshots. **Conclusion: the microrelief omission was
  real and is now fixed (kept, it's a genuine correctness improvement and
  measured safe), but it is not the dominant cause of the visible floating.**
  Something larger-magnitude is still unaccounted for. Candidates not yet
  checked: whether `pc.context_detail.z` (this compute shader's geomorph
  amplitude multiplier) actually equals whatever the real surface shaders
  bind `u_geomorph_biome_terrain_variation` to (both default-looked
  consistent by inspection, not verified by value); the "stable displacement
  lattice"'s per-LOD vertex snapping (debug menu confirms it's ON) doing
  something to rendered height beyond the cache's plain
  macro+geomorph(+now-microrelief); or a bias/units mismatch in
  `u_scatter_surface_bias` / `sg_terrain_sample`'s edit-delta addition
  specific to this exact spot.
- **Stable-displacement-lattice snapping candidate — traced, likely ruled
  out.** Read `stable_surface_offset()` (`spherical_geometry_clipmap_cached_
  surface.gdshader:123`): it is a numerically-stable reformulation of
  `dir*(radius+altitude)` for float32 precision near a planet-scale sphere,
  not an approximation that moves the sampled point. The actual "snapping"
  is `level_center_m = round(u_lattice_center_plane / spacing) * spacing`
  (`:344`) -- since `cell_offset_m` is also an exact multiple of `spacing`,
  every rendered vertex sits on a fixed `spacing`-aligned grid, which is
  expected clipmap behaviour, not drift. Compared against the cache-writing
  side: `gpu_terrain_clipmap_cache.gd::_update_level_window` computes
  `int(round(_center_plane.x/spacing))` -- the **same** `round()` convention,
  same `spacing`, same `_center_plane` source that gets passed into
  `update_cache()` from `spherical_geometry_clipmap_cached.gd::
  _update_terrain_caches()`. The two grids line up structurally: same
  anchor, same snapping, same spacing per level. Did not verify they are
  never one frame apart in practice (`_center_plane` could in principle lag
  between the cache dispatch and the render bind within the same frame), but
  found no static mismatch. **This mechanism is probably not the cause of
  the multi-metre floating** -- deprioritize it below the other two
  candidates (`context_detail.z` amplitude-value mismatch,
  `u_scatter_surface_bias`/edit-delta) unless something else rules those out
  first.
- **`context_detail.z` amplitude mismatch — traced, strongest lead so far,
  not yet fixed.** The cache's geomorph strength and the actual render's
  geomorph strength come from two independent systems with no wiring between
  them: `gpu_terrain_clipmap_cache.gd:548` sets `context_detail.z = maxf(0.05,
  Planet.cfg.detail_amplitude / 260.0)` (a world-gen-time config baked once
  at world creation, `gen_config.gd:98`, default `260.0` so default
  `detail_strength = 1.0`); the actual rendered terrain's `u_detail_strength`
  is instead set by `scripts/world_authoring/
  terrain_displacement_runtime_phase32.gd:39-40` from
  `GEOMORPH_CONTRACT.normalized_controls(_production_controls).
  get("detail_strength", 1.0)` -- a **live, Planet-Studio-authored** control
  (`terrain_production_geomorph_schema.gd`), unrelated to `Planet.cfg.
  detail_amplitude`. Both default to `1.0`, so an untouched world shows no
  symptom -- consistent with this bug being invisible until someone actually
  authors terrain (this branch has a lot of uncommitted terrain-authoring
  WIP). At the 24 m geomorph band alone (~4.5 m amplitude in `geomorph()`),
  a strength mismatch is easily large enough to explain a multi-metre float.
  **Not yet confirmed live** -- did not verify the *actual current* value of
  `_production_controls.detail_strength` in this session/world (would need
  Planet Studio open, which the live test session was not in; the live-read
  path is `studio_surface_settings` with the node_type `_phase47_surface_
  node_id` resolves for the geomorph production node -- exact node_type
  string not yet looked up) or of `Planet.cfg.detail_amplitude`.
  **RECHECK / next fix:** (1) confirm live whether the two values actually
  differ right now before touching code; (2) if so, the correct fix is almost
  certainly making `gpu_terrain_clipmap_cache.gd` read the same
  `_production_controls`-derived `detail_strength` the render shader uses
  (passed in alongside the other terrain args already threaded into
  `update_cache()`/`_update_terrain_caches()`) instead of independently
  recomputing it from `Planet.cfg.detail_amplitude` -- not the other way
  around, since the production controls are the live-authored source of
  truth and `detail_amplitude` is a generation-time seed value.
- **CORRECTION, 2026-09-11: the last two entries (stable-displacement-lattice
  ruling-out, and the `context_detail.z` mismatch) and the microrelief fix
  earlier were all investigated/applied against `shaders/
  terrain_clipmap_cache.glsl` / `scripts/terrain/gpu_terrain_clipmap_cache.gd`
  -- which is NOT the shader `GroundGeometryClipmap` actually uses.** Traced
  the real inheritance chain (`spherical_geometry_clipmap_phase31.gd` ->
  ... -> `_phase29.gd` -> `_cache_contract_phase42.gd` -> ... ->
  `_cached.gd`): `_cached.gd`'s `_ready()` first creates the base
  `GPUTerrainClipmapCacheScript` (the file I'd been editing), then
  `_cache_contract_phase42.gd`'s `_ready()` immediately calls
  `_replace_initial_active_cache()` and swaps `_terrain_cache_active` to a
  `Phase42TerrainClipmapCacheScript` (`gpu_terrain_clipmap_cache_phase42_
  final.gd` -> `..._phase42_active.gd`, which loads a **separate** shader,
  `shaders/terrain_clipmap_cache_phase42.glsl`). The base class/shader is
  created and discarded within the same startup and never touches anything
  scatter or the renderer actually read from. This explains why the
  microrelief fix produced a pixel-identical screenshot (never ran) --
  **not** because the effect was too small, though it likely also was;
  separately explains an intermittent (non-deterministic: absent in one
  launch, present in two later launches of the identical, unmodified code)
  `ERROR: GPU terrain clipmap cache shader is invalid.` at boot from the base
  class's shader load -- almost certainly environmental (this session killed
  and relaunched the process many times back-to-back; plausibly leftover GPU
  resource contention from the previous process rather than a real bug), and
  harmless either way since the class is discarded regardless. Left the dead
  edit in place rather than spend remaining budget reverting it -- it affects
  nothing live.
  **The `context_detail.z` lead is very likely also moot for the same
  reason:** the real `terrain_clipmap_cache_phase42.glsl` does not use a
  loose push-constant float for detail strength at all -- it reads
  `GC_DETAIL_STRENGTH` (`gc.value[0].x`) from a dedicated `std430` storage
  buffer (`GeomorphControls`, binding 8, "ABI v2") that the shader's own
  comment states is "shared by every warm-cache dispatch; the visible
  analytic shader receives the same normalized dictionary through ordinary
  material uniforms" -- i.e. this was deliberately engineered as a single
  shared source of truth precisely to prevent a cache/render amplitude
  mismatch. Did not verify the GDScript binding actually keeps this promise,
  but the design intent argues against this being the bug.
- **New, much more significant finding from reading the real
  `terrain_clipmap_cache_phase42.glsl`'s actual `main()`:** it does not call
  geomorph OR microrelief AT ALL. Line 500, verbatim: `float final_h=macro_h;`
  with the shader's own comment above it: "Terrain height is the coarse
  elevation map alone... All finer terrain shape is composed on top per
  biome by the Biome Terrain authoring path. The synthesised geomorph bands
  no longer contribute height." **The cache scatter reads from contains
  nothing but raw macro/continental elevation** -- not geomorph, not
  microrelief, not whatever the Biome Terrain authoring system (the newer
  graph-based per-biome displacement system mentioned in this project's own
  memory notes on the authored-displacement CPU/GPU split) adds on top,
  which is almost certainly several metres at many biomes given the terrain
  debug menu's own description ("Alpine/Bare rock ridged and tall"). This is
  a far more complete explanation for a 1-3 m float than anything
  investigated so far, and a materially bigger fix: it would mean threading
  the Biome Terrain authoring displacement (not yet located in this
  investigation -- likely `terrain_biome_profile.gdshaderinc` per existing
  project memory) into this compute shader, not one self-contained function
  like microrelief was. **Not attempted this session** -- out of budget for
  a change of this scope with adequate verification. This supersedes the
  microrelief-in-cache work above as the priority next step; redoing that
  fix in the *correct* file is still worth doing but is now expected to be a
  minor contribution next to Biome Terrain authoring, not the main fix.
- **Symptoms 1 and 4 — still not investigated** this pass. Original
  hypothesis (shared root cause across all four) is weakened: symptom 3 had
  its own, unrelated, purely-CPU-side cause (a hardcoded literal), unconnected
  to height sampling or culling.
- **Symptoms:**
  1. The center L0 ring gets offset from its expected position, and terrain
     disappears because bind-camera culling then treats visible terrain as
     out of frustum/range.
  2. Terrain scatter placement is offset — instances appear floating in the
     air rather than sitting on the terrain surface.
  3. Scatter instances do not cast shadows onto the ground.
  4. Scatter's lit side does not match the actual light/sun direction.
- **Suspect areas:** `scripts/terrain/spherical_geometry_clipmap_blankaware.gd`,
  `scripts/terrain/spherical_geometry_clipmap_occlusion.gd`,
  `scripts/terrain/gpu_terrain_scatter_global.gd`,
  `scripts/terrain/gpu_terrain_scatter_authoring_empty.gd` — check whether
  scatter transform sampling uses the same rebased/offset coordinate frame as
  the L0 ring culling, and whether the scatter shader receives the current
  light direction and is registered in the shadow pass.
- **RECHECK:** Reproduce in-game with wireframe + camera frustum visualization
  at a rebase boundary to confirm the L0 offset and correlate its timing with
  the scatter floating; separately verify light-direction wiring and
  shadow-caster flags on scatter mesh/MultiMesh instances.
- **2026-09-11 session — ported scatter shadow/LOD, wind shader, and
  volumetric fog from the reference repo `GamesNotDeveloped/godot-forest-demo`
  (user request: keep our planet-scale terrain generation, adopt his scatter
  LOD/shadow technique, wind shader, and volumetric fog).** His repo's exact
  mechanism (CPU-baked per-chunk `MultiMeshInstance3D`s with native
  `visibility_range` billboard crossfade) does not transplant onto our
  100%-GPU-vertex-shader-driven, one-`MultiMeshInstance3D`-per-asset
  architecture, so the *effects* were ported instead, adapted to our system:
  - **Symptom 3 (no shadows), finished:** the earlier fix only touched the
    base procedural fallback batches (grass/geologic-stone/river-stone).
    `gpu_terrain_scatter_global.gd:400`'s `_ecology_definitions()` still
    defaulted `"shadows"` to `tier in ["major", "canopy"]` — every real-asset
    grass/fern/shrub/moss ecology tier cast no shadow by default. Flipped to
    default `true` for every tier. Also fixed a dead-code hardcoded
    `SHADOW_CASTING_SETTING_OFF` in `gpu_terrain_scatter_compact.gd:158`
    (currently-disabled `STABLE_FALLBACK_ONLY` indirect-compute path) for
    consistency.
  - **Wind shader upgraded**, `shaders/terrain_scatter_ecology.gdshader` and
    `terrain_scatter_grass.gdshader`/`terrain_scatter_compact_grass.gdshader`:
    replaced the old single-octave, texture-free `tip*tip`-gated sway (which
    pushed only along each instance's own randomly-yawed local X, i.e. not a
    real shared wind direction at all) with a 3-octave sway (primary + a
    perpendicular secondary + a slow gust, ported from the reference repo's
    `materials/tree_wind.gdshader`) applied along a genuine shared world
    tangent-plane direction (`sg_wind_offset()` in the new shared
    `gpu_scatter_common.gdshaderinc` helpers), with a smoothstep height gate
    and a small trunk floor so trunks aren't perfectly rigid. Added
    `global uniform u_global_wind_direction/speed/strength` (registered in
    `project.godot`'s new `[shader_globals]` section) driven by a new
    autoload `scripts/rendering/global_wind_driver.gd`, defaulting to the
    same `(11, 0, 4.5)` m/s direction already used for cosmetic cloud/rain
    drift (`VolumetricCloudController.WIND_METRES_PER_SECOND`,
    `weather_fx_system.gd:249`) for visual consistency, at a hand-picked
    oscillation speed (not that vector's literal m/s magnitude, which would
    flicker far too fast as a sway rate).
  - **Distance-based scatter thinning added** (the GPU-driven equivalent of
    the reference repo's per-chunk `visible_instance_count` density LOD):
    `sg_distance_lod_weight()` in `gpu_scatter_common.gdshaderinc` fades
    acceptance probability out smoothly between `u_scatter_lod_start_m`/
    `u_scatter_lod_end_m`, sourced per ecology asset from the catalog's
    existing (previously-unused for this purpose) `max_distance_m` field
    (65%-to-100% taper). `u_scatter_camera_pos` is pushed explicitly per
    frame from `camera.global_position` rather than relying on Godot's
    built-in `CAMERA_POSITION_WORLD`, since these shaders hand-build `VERTEX`
    as an origin-relative position, not a normal object-local one. Full
    billboard/impostor tier explicitly **deferred** — no texture-bake
    pipeline exists, and a flat billboard would make symptom 2's floating
    *more* visible, not less.
  - **Volumetric fog re-enabled** (`scripts/rendering/graphics_quality.gd`
    `configure_world_environment()`), with the reference repo's tuned
    parameters (density 0.015, anisotropy 0.35, length 6.23, etc.). This had
    been deliberately forced `false` with a comment warning that Godot's
    local volumetric fog sees the sun through the planet body and lights the
    night side gray. **Live-tested at true local night** (lat 38.27° lon
    61.17°, hour set to well past sunset): screen stayed appropriately dark,
    only a faint plausible ambient/starlight-level glow on nearby
    geometry, sky stayed black with no gray haze/wash — the described
    failure mode did **not** reproduce in this pass. Kept enabled. (Not an
    exhaustive test — only one lat/lon and one "well past sunset" hour was
    checked, not a full sweep across the terminator or other latitudes.)
  - **Verification done:** `--headless --import` clean after all script/
    shader changes. Live in `Main` (F9-loaded `phase1` save, lat 38.27°
    lon 61.17°, Temperate forest, `sectors 5/12`): game boots and renders
    scatter (trees, rocks, ferns/shrubs) normally with no new shader-compile
    or script errors in the console beyond the pre-existing harmless
    `GPU terrain clipmap cache shader is invalid` boot artifact (see the
    P-011 entry below — a discarded base-class shader, unrelated).
  - **Not conclusively verified visually:** wind sway and shadow contact.
    Two screenshots ~10s apart of the same conifer tree showed no visible
    branch-tip movement — plausibly just sub-pixel amplitude at that zoom for
    a canopy-tier asset's small authored wind value, not necessarily a sign
    the code path isn't running, but not proven either way. Shadow contact
    under scatter was not clearly visible against the site's flat khaki
    ground texture at the tested sun angles. **RECHECK:** re-verify wind on a
    grass patch (much higher authored amplitude than trees) at close range,
    and verify shadow contact at a site with light-colored ground and a
    clearly grazing sun angle, ideally using the `pssm_splits` or
    `directional_shadow_atlas` debug view (`studio_view` tool) at high zoom
    rather than the default lit view.
  - **Symptoms 1 and 4 (L0 ring offset; lit-side mismatch) — still not
    fixed, but symptom 4's leading hypothesis changed.** Reading
    `spherical_geometry_clipmap_cached_surface.gdshader`'s `void light()`
    closely: the actual N·L shading term uses Godot's built-in `NORMAL`/
    `LIGHT`, the same real `DirectionalLight3D` scatter's un-overridden
    automatic lighting reads — `u_cloud_shadow_sun_dir` (pushed only to
    ground+ocean materials by `volumetric_cloud_controller.gd`) only feeds a
    self-shadow/cloud-shadow *occlusion* multiplier and sky-irradiance
    emission, not the lit-side direction itself. Both ultimately derive from
    the same `Frames.helion_dir`. This weakens "scatter and terrain disagree
    on light direction" as the cause. Stronger candidate: terrain gets a
    self-shadow/cloud-shadow occlusion term scatter never receives at all
    (scatter shaders have no `light()` override) — at a grazing sun, ground
    near a tree could visibly darken while the tree doesn't, which would look
    exactly like a lighting mismatch without being a direction bug. Not yet
    live-verified either way — needs a side-by-side screenshot at a grazing
    sun angle comparing terrain's shaded boundary to a tree/shrub's lit face,
    which this session did not conclusively capture (viewing angles used were
    mostly backlit silhouettes, not a clean side-by-side).

### P-009 — Weather WorkerThreadPool jobs degrade frame time ~25-30x; the "fps" HUD grossly overstates it as "1"
- **Status:** OPEN — real, modest degradation confirmed; earlier "fps collapses
  to 1 / hangs" framing in this entry was **wrong**, corrected below with
  instrumented proof. The backlog-cap / simulation_speed fixes are still good
  changes but were not fixing what I originally thought they fixed.
- **Found:** 2026-09-11, during a full MCP-toolchain stress pass, then
  reproduced/investigated directly via `studio_time {"action":"rate",...}`.
- **Where:** `scripts/weather/weather_system.gd` (`_process`,
  `_global_sim_accum`/`_local_sim_accum`, `_schedule_weather_worker`,
  `_run_weather_worker` on `WorkerThreadPool`), `scripts/world_authoring/mcp/
  planet_studio_vision.gd::time_command`.
- **Correction (read this first):** repeated screenshots during
  `studio_time rate=10000` showed the in-game HUD's `fps` line reading "1" for
  15+ seconds straight, which read as a genuine freeze/hang. Direct
  instrumentation (temporary prints of the real per-frame `delta` and of
  `Engine.get_frames_per_second()` side by side, since removed) proved this
  wrong: **actual per-frame `delta` stayed a steady 0.1-0.15 s (~7-10 fps) the
  entire time** — never close to literally 1 fps. The same misleading "fps 1"
  reading also appears during the **completely normal, unrelated 160 s weather
  pre-spin every game boot already does** (`PRESPIN_GLOBAL_STEPS`), which has
  nothing to do with timewarp at all — proving the "fps" readout is unreliable
  (a slow/laggy smoothed average, or otherwise miscomputed) for the whole
  duration of *any* long-running `WorkerThreadPool` weather task, not a signal
  of an actual stall. Do not trust the HUD `fps` line as evidence during a
  weather job; measure real `_process` `delta` instead.
- **The real, smaller bug:** sustained high timewarp (or just the normal boot
  pre-spin) still measurably degrades frame time by ~25-30x (from a ~230+ fps /
  ~4 ms baseline to ~7-10 fps / ~100-150 ms) for as long as a weather
  `WorkerThreadPool` job cycle is active. Root cause not yet found — the
  degradation is real but modest, not a hang, so it's lower priority than
  originally logged. `_publish_global_weather`'s native call measured only
  ~15-20 ms (not the cause). Disabling clouds/shadows/aerial/ocean/scatter via
  `studio_view render_toggle` made no difference, ruling out rendering cost.
- **Fixes applied (worth keeping regardless of the corrected severity):**
  `WeatherSystem.notify_time_jump()` (called from `time_command` after
  `seek`/`advance`/`date`) resets both sim accumulators on an explicit clock
  jump; `MAX_GLOBAL_SIM_BACKLOG_S`/`MAX_LOCAL_SIM_BACKLOG_S` hard-cap them every
  frame (~2.5 simulated hours max instead of unbounded); `studio_time`'s `rate`
  action now also mirrors into `WeatherSystem.simulation_speed` via
  `_sync_weather_simulation_speed()` so the existing job-limit scaling (more
  native steps per background job at higher declared warp) actually engages for
  MCP-driven rate changes, which it never did before.
- **RECHECK / next step:** if this is worth chasing further, profile with a
  real Godot profiler (not print instrumentation) around a `WorkerThreadPool`
  job cycle to find the ~100 ms/frame source — candidates: `WorkerThreadPool`
  task submission/teardown briefly contending a lock with the main thread,
  GC/allocation pressure from the large `PackedFloat32Array` reductions, or
  something outside `weather_system.gd` entirely reacting per-frame to
  `Frames.system_time_s` changing quickly. Low priority given it's a ~10x-not-
  ~200x effect and self-recovers.

### P-008 — Planet Studio renders no detail terrain below ~orbit distance
- **Status:** INVALIDATED — see note below. Kept only as a pointer to the
  unexplained residue (wireframe-black at one specific test spot).
- **Found:** 2026-09-11
- **Where:** `TerrainHeightQuery` / `GroundHeightPageAtlas` / `GroundGeometryClipmap`
  in the Planet Studio (`world_authoring`) runtime.
- **What:** From orbit the planet renders (far LOD via the orbit-elevation
  texture). Descending below ~80 km — by MCP teleport **or** by manually flying
  with `studio_input` W — the view goes fully black; `TERRAIN DEBUG WIREFRAME`
  shows *no* geometry at all, `chunks 0 / nodes 0`, `GPU height PENDING async`
  never clears. The clipmap LOD math itself is fine (`LOD 6.23 m/px`,
  `sample distance 4.2 km`, `sectors 12/12`, `fps` healthy) — there is just no
  detail surface.
- **Lead:** HUD `GPU height … async` = `TerrainHeightQuery.stats()` stuck
  `ready=false / latest_valid=false` while `supported=true, failed=false` — its
  GPU compute readback never returns a valid sample, so no height pages upload,
  so `GroundHeightPageAtlas.ready_for_shader()` stays false. ("streaming OFF" in
  the HUD is a hard-coded label, not a live flag — red herring.)
- **Suspects:** the branch's uncommitted WIP (`M shaders/…`, `M scripts/water/…`,
  `gpu_surface_classifier`, occlusion); or Planet Studio not pumping
  `TerrainHeightQuery` per-frame; or apply-without-PlanetBake leaving the height
  field unbuilt.
- **RECHECK / next step:** repro on a clean `git stash` of the WIP to see if it is
  a regression; check whether the normal game (not Planet Studio) streams detail
  terrain on this branch; trace `TerrainHeightQuery` readiness in Planet Studio.
- **Invalidated 2026-09-11:** the user, live in the same session, flew to
  lat -28.35 lon 51.51 (a location I never touched) and confirmed the ground
  renders fully — wireframe mesh, working `AIM`/dig contact, real biome. So
  Planet Studio's detail-terrain pipeline is NOT broken. My repro was confined to
  one specific spot (lat -44.37 lon 153.38) after ~150-400 rapid MCP
  teleports/rebases in a few seconds — most likely that location was simply on
  the **night side** (the user's own read the first time: "you were on the
  nightside") compounded by something in that test spot's state never being
  reconfirmed with full_bright at the *later*, fixed-clamp altitude.
- **Re-confirmed 2026-09-11 (second session), decisively this time:** reproduced the
  identical black screen at the exact same spot on a **fresh boot**, a **single** `T`
  "teleport to a good site" press, `chunks 0 / GPU height PENDING async` as always —
  including with `full_bright` enabled, which is what made it look like new evidence
  against the night-side explanation (logged as a reopen, since retracted below). The
  user's suggestion to test with time-of-day/timewarp settled it: `studio_time advance
  seconds:40000` (then `+10000` more) turned the screen from solid black to a lit,
  visibly-grained ground with a normal sky gradient at the horizon, with `chunks`
  still reading `0` the whole time. Enabling `studio_view terrain_debug wireframe`
  at the same spot, in daylight, showed a real, dense wireframe mesh with actual
  triangle detail (plus the sink-radius circle from `sink_scale`) -- definitive proof
  real geometry was rendering. **`chunks 0` is confirmed a stale/misleading counter,
  not a live signal of missing geometry** -- do not trust it, even alongside
  `full_bright`, as proof terrain isn't rendering. **`full_bright` is NOT a reliable
  way to rule out night-side blackness either** -- it stayed black under full_bright
  at this spot until the clock was actually advanced, meaning something in the
  night-side render path (likely aerial/atmospheric compositing or a sun-visibility
  term) still zeroes the image even in the "unshaded" debug view. Do not reopen this
  as a pipeline bug from a `full_bright`-still-black observation alone -- advance the
  clock and/or check wireframe first.

### P-007 — Biome terrain composition needs per-biome visual verification
- **Status:** FIX-UNVERIFIED
- **Found:** 2026-09-11
- **Where:** Planet Studio → TERRAIN → BIOME TERRAIN; preset
  `user://world_authoring/presets/all-biomes-terrain.tres`
- **What:** Composed one displacement layer for each of the 15 land biomes via the
  MCP bridge (15/16 shared layer budget). `apply` succeeded, diagnostic panel
  showed non-zero CPU displacement per biome.
- **RECHECK:** Fly the live view over each biome and confirm the relief reads as
  intended (Alpine/Bare rock ridged and tall; Wetland/rainforest carved-in;
  deserts subtle). Confirm the 15/16 budget did not silently drop a layer.
- **Note:** A pre-existing `simple-biome-terrain-7` slot (Temperate forest) showed
  `rev=8` while "layers in use" read `0/16` before we started — odd stale state,
  worth understanding.

### P-006 — `studio_camera` teleport/slew "black screen"
- **Status:** FIX-UNVERIFIED (was largely a test-methodology artifact)
- **Found:** 2026-09-11
- **Where:** `scripts/world_authoring/mcp/planet_studio_vision.gd`
- **What actually happened:** the black screenshots were (a) the **night side** of
  the planet, and (b) teleport targets set *below* the local terrain height
  (600–700 m radial offset where terrain there is 300–865 m tall) — a sub-terrain
  observer sends the studio host into a per-frame "push camera back above ground"
  correction that collapses fps to 1 and drops `sectors 0/12`. The normal
  fly-in approach path never hits this. `chunks 0 / GPU height PENDING async` is a
  red herring — the same lines show from orbit where rendering is fine (the shell
  renders via the orbit-elevation texture, `_bound_orbit`).
- **Fixes applied (need a clean-session check):**
  - `_clear_local_ground()` clamps teleport/slew targets to
    `planet_radius + broad ground height + 120 m` so a teleport can't land
    underground.
  - `_await_terrain_ready()` — teleport/slew now waits ≤8 s for
    `GroundGeometryClipmap.gpu_stream_stats().coverage_ready` before returning.
  - Slew interpolation steps use coalesced `maintain_origin` (no per-step hard
    rebase); an immediate `Frames.rebase()` on teleport was tried and **reverted**
    — it amplifies the underground correction storm.
- **RECHECK:** fresh game + Planet Studio, `studio_camera slew` to a daylit
  surface point, confirm terrain renders and fps stays healthy; confirm a
  deliberately-too-low `position_m` gets clamped above ground.
- **Note:** a long test session accreted ~400 `rebases` and left fps at 1 — a
  Planet Studio restart clears it.

### P-004 — Stale native-DLL temp files + GDExtension copy errors
- **Status:** OPEN
- **Found:** 2026-09-11
- **Where:** `bin/` — `asterra_weather.dll` plus leftover
  `~asterra_weather.dll~RF*.TMP` files; `--headless --import` logs
  `Failed to open '.../bin/~asterra_weather.dll'` / `Can't open GDExtension
  dynamic library`.
- **What:** Only happens when a second Godot instance (the editor) holds the DLL
  locked while another process tries the hot-reload copy. Not a runtime error in a
  single-instance run (weather native loads fine: "native pre-spin: 220 global
  steps"). See memory `stale-weather-native-dll`.
- **RECHECK:** Delete the `~*.TMP` files with the editor closed; decide whether the
  build/import step should tolerate the locked copy quietly.

### P-003 — RID leak warnings on shutdown
- **Status:** OPEN (low priority)
- **Found:** 2026-09-11
- **What:** Clean game exit still prints `WARNING: N RID of type "Compute" /
  "StorageBuffer" / "Shader" / "Sampler" / "Texture" was leaked` (1 each) from
  `rendering_device.cpp` finalize. Benign but indicates a compute resource freed
  after the device, likely one of the water/occlusion GPU passes.
- **RECHECK:** Track which subsystem owns those RIDs; free them in `_exit_tree` /
  `NOTIFICATION_PREDELETE` before the RenderingDevice goes away.

---

## Resolved

### P-011 — Scatter never rendered on Asterra land; root cause was a `find_spawn()`/renderer height desync, not the scatter suitability code
- **Status:** RESOLVED
- **Found:** 2026-09-11, while adding biome/texture-mask/global placement
  gating to the three procedural scatter families (this session's new
  `PRODUCTION_SCATTER_*_SETTINGS` controls / `sg_gate_weight` in
  `shaders/gpu_scatter_common.gdshaderinc`).
- **Original (wrong) hypothesis:** `sg_grass_suitability`/
  `sg_geologic_stone_suitability`/`sg_river_stone_suitability`'s "land" gate
  (`smoothstep` windows centred within ~2 m of `terrain_h = 0`) looked
  miscalibrated, since every test site read `TERRAIN` around -196 to -220 m
  despite plausible land-like climate/soil/geology stats.
- **Investigated properly instead of just recalibrating the thresholds** (the
  user's call, since patching the smoothstep windows would have papered over
  whatever the real cause was): confirmed via `scripts/gen/pass_macro.gd`'s
  hypsometric remap, `scripts/gen/pass_erosion.gd`'s 0-referenced boundary
  conditions, and `scripts/gen/planet_sampler.gd`'s ocean surface (literally
  `0.0`) that **`terrain_h = 0` is the CORRECT sea-level datum by design** --
  recalibrating the scatter thresholds would have been the wrong fix.
- **Real root cause:** `scripts/gen/pass_biome.gd` (`PassBiome`) and
  `scripts/main.gd`'s `find_spawn()` both correctly read the same canonical
  baked field, `Planet.fields.elev` (post-erosion, pre-render) -- so a cell
  `find_spawn()` approves as "> 5 m of land" really is a land biome by that
  field. But the actual RENDERED terrain (and therefore the water mask,
  contact height, and everything scatter/visual/physical) comes from a
  SEPARATE, DERIVED field: `PlanetSampler._build_smoothed_macro_elevation()`
  duplicates `fields.elev` once at world load and runs it through two
  Gaussian smoothing passes (0.5 strength) before clamping -- never
  reconciled back into `fields.elev` or biome classification. Near almost any
  coastline, that smoothing can drag a barely-positive land cell's RENDERED
  height (`Planet.macro_height(dir)`) far negative by averaging it with much
  deeper ocean/shelf neighbours, so a cell that is correctly "land" by
  `fields.elev` and biome can still render/behave as if 100-200+ m
  underwater -- exactly what every test site hit, because `find_spawn()`
  never checked the rendered height, only the canonical one.
- **Fix:** `scripts/main.gd`'s `find_spawn()` now also requires
  `Planet.macro_height(dir) > 5.0` (the same rendered-height quantity scatter,
  water and contact all already agree on) alongside the existing
  `f.elev[c] > 5.0` check, so a fresh spawn/teleport site is guaranteed to
  actually render as land, not just be classified as land. Scoped narrowly to
  spawn-site selection rather than touching the generation/smoothing pipeline
  itself (which shapes every coastline on the planet and carries much higher
  blast radius for a fix aimed at "stop teleporting me underwater").
- **Verified live:** fresh boot, `T` teleport landed on a genuinely different,
  positively-elevated site (`TERRAIN 149.9 m`, real river `Strahler 6`,
  `Temperate forest`, `vegetation 0.34`) instead of the old -200 m coastal
  site. Grass (dark blade billboards) and geologic stone (light diamond
  billboards) both visibly rendered close to the ground for the first time.
  Confirmed the new gating feature itself works end-to-end: set grass's gate
  to Biome + a biome that did NOT match the current site -> grass disappeared
  completely (stone, ungated, stayed); set the biome back to the actual
  current biome -> grass reappeared. Reset grass back to Global afterward.
- **Not fixed / out of scope:** the underlying `fields.elev` vs
  `_macro_elev` desync itself (why coastal cells can diverge this much) is
  still there for any OTHER code path that reads `fields.elev` expecting it to
  match the renderer -- this fix only covers `find_spawn()`. The "ecology"
  10-real-asset scatter layer is still suppressed (`TerrainScatterEmpty`)
  pending real asset verification, separate from the two procedural families
  fixed here.

### P-010 — Micro/macro terrain texture blend: shader done + live, editor/MCP UI not wired to the live game
- **Status:** RESOLVED
- **Found / built:** 2026-09-11, following up on "PBR texture so small we can't
  see it" at 20 m AGL over Temperate forest.
- **Where:** `shaders/gpu_surface_pbr.gdshaderinc` (shader), `scripts/world_authoring/
  terrain_material_runtime_phase32.gd` (GDScript->shader binding), `scripts/
  world_authoring/model/terrain_beginner_surface_catalog.gd` (the pre-existing,
  previously-orphaned control catalog), `scripts/world_authoring/
  world_authoring_editor_live_phase46.gd` (the fix -- new UI wiring).
- **Shader change (done earlier, verified live):** each of the 4 built-in materials
  (ground/grass/mud/forest) is sampled twice against the SAME texture -- small
  "micro" tile size and a much larger "macro" tile size -- cross-faded by camera
  distance. See the shader/binding details this entry originally logged; unchanged
  by this fix.
- **Original problem:** the new fields were registered in
  `terrain_beginner_surface_catalog.gd`, but that catalog's only consumer was the
  *retired* shader-graph composer (`terrain_graph_editor_phase44.gd`), not the live
  editor (`world_authoring_editor_live_phase46.gd`). The live "GLOBAL TERRAIN" tab
  showed only 3 coarse-elevation sliders -- no texture/material section existed at
  all, so neither a human nor MCP could reach the macro/micro blend, or the surface
  classifier's rock/soil/vegetation/sand/snow balance, albedo chroma/contrast, key
  colours or slope/temperature thresholds (all of which had the same orphaned-catalog
  problem, confirmed by the same investigation).
- **Fix:** added `_phase47_build_surface_detail_controls()` to
  `world_authoring_editor_live_phase46.gd`, called from `_build_terrain_page()`'s
  GLOBAL TERRAIN branch. It reads `TerrainBeginnerSurfaceCatalog.controls_for_mode
  (MODE_DETAILED)` and renders one panel per category (Surface Detail, Material
  Balance, Visual Character, Key Colors, Classification Rules) with number/toggle/
  color widgets as appropriate, get/set through a new `(node_type, key)`-addressed
  helper pair (`_phase47_surface_value`/`_phase47_set_surface_*`) against the
  MATERIAL-domain `production-terrain-surface` graph that `_phase29_ensure_production_
  graphs` already unconditionally provisions with every needed node type
  (`create_production_stage_graph`'s MATERIAL branch) -- no data-model or shader
  changes needed, this was purely a missing UI registration. Mirrors Global Terrain's
  existing no-rebuild-on-edit pattern (unlike Biome Terrain/Texture) so control ids
  stay valid across edits.
- **Verified live** via a fresh windowed launch with `ASTERRA_MCP_ENABLED=1`: opened
  TERRAIN -> GLOBAL TERRAIN through `studio_input`/`studio_category`, confirmed all
  5 new category panels and all ~47 expected controls appear in `studio_ui` with
  correct default values; round-tripped a `SpinBox` (`micro_macro_near_m`, 4.0 -> 5.0)
  and a `ColorPickerButton` (`rock_granite`) through `studio_control`, re-read via
  `studio_ui` and confirmed the new value stuck with the control's `id` unchanged
  (no forced rebuild).
- **Also delivered in the same pass -- new `studio_texture_stack` MCP tool:** reads/
  replaces a biome's entire BIOME TEXTURE band stack as one structured JSON object
  (the same layer-dict shape `_phase47_texture_stack`/`_phase47_stage_biome_texture`
  already use internally) instead of requiring a raw `studio_inspect` graph-node/link
  traversal or one `studio_control` call per field. Added to `scripts/world_authoring/
  mcp/planet_studio_bridge.gd` (`_texture_stack`/`_texture_stack_get`/
  `_texture_stack_set`) and declared in `tools/planet_studio_mcp/server.py`; documented
  in `docs/mcp/tools.md`. Verified live over the raw bridge socket protocol (the
  running Python MCP wrapper process predates the edit and needs a reconnect to see
  the new tool in `tools/list`): `get` on an empty biome returned `{layers:[]}`;
  `set` with a 2-band payload (colors as `[r,g,b]`, a `gradient_curve` point list,
  relative height) returned `{ok:true, band_count:2}`; a follow-up `get` round-tripped
  every field exactly; `studio_ui` on the BIOME TEXTURE tab confirmed two real band
  cards ("Grass", "Ground") rendered from the MCP-authored graph, matching what the
  UI itself would have produced one field at a time. Cleared back to empty afterward.
- **RECHECK:** none outstanding for the UI wiring or the new tool. A live client
  still needs to reconnect the `asterra-planet-studio` MCP server once to see
  `studio_texture_stack` in its tool list (it's a long-running subprocess that read
  `server.py` at launch).

### P-005 — `studio_*` MCP tools not registering / bridge only alive inside Planet Studio
- **Status:** RESOLVED
- **Found / fixed:** 2026-09-11
- **Where:** `.mcp.json` (repo root, gitignored), `scripts/world_authoring/mcp/
  planet_studio_bridge.gd`, `scripts/world_authoring/mcp/planet_studio_vision.gd`,
  `scripts/world_authoring/world_authoring_editor.gd`, `project.godot`.
- **Original finding:** the tools weren't showing up in this Claude Code
  session even though `.mcp.json` and the stdio server were valid — resolved
  itself on a later session reload (config/trust-prompt timing, not a code
  bug).
- **Follow-up capability request:** "make it so the mcp tools are alive also
  on main menu for automation" — the bridge was a child node of the Planet
  Studio editor Control, created only once a human opened Planet Studio, so
  automation had no way to reach the start menu at all. Moved the bridge to a
  **boot autoload** (`PlanetStudioMCP` in `project.godot`) that listens from
  the first frame regardless of scene; `_editor` is now resolved dynamically
  each dispatch (duck-typed search, works for both the standalone
  `PlanetStudio.tscn` test scene and the live in-game overlay) instead of
  assumed from `get_parent()`. `studio_status`, `studio_input` and
  `studio_screenshot` were made null-editor-safe (`studio_status.editor_open`
  reports state instead of crashing; `studio_input`'s viewport check no longer
  needs the editor). `studio_category`/`studio_control`/`studio_session`/
  `studio_inspect` now return a clean error instead of crashing when Planet
  Studio isn't open yet.
- **Verified:** fresh boot → `studio_status` at the **StartMenu** returned
  `editor_open:false, scene:"StartMenu"` (no crash) → `studio_screenshot`
  captured the start menu cleanly → `studio_input` clicked the "PLANET STUDIO"
  button by pixel coordinate → `studio_status` polled through the scene
  transition (`scene:"Main"`, `editor_open:false`) to the editor finishing
  attach (`editor_open:true, live_world:true`) → `studio_category` on TERRAIN
  worked normally. Full cold-boot-to-editing loop with zero manual clicks.

### P-002 — Rebase "flicker": far terrain rings pop every ~km of travel
- **Status:** FIX-UNVERIFIED → needs one live look
- **Found / fixed:** 2026-09-11
- **Where:** `scripts/terrain/spherical_geometry_clipmap_occlusion.gd`
  `_update_occlusion_candidates()`
- **Root cause:** A bare LOD level-centre re-snap (every ~km of travel, co-incident
  with the 4 km floating-origin rebase) was treated like a real topology change —
  it bumped `_occlusion_generation`, cleared occlusion history and restored full
  ring prefixes, so every occlusion-culled far-terrain ring popped back to visible
  for a few frames then re-culled. Read as a one-frame view lurch. The camera
  transform itself is provably continuous across rebases (measured to 4 decimals
  over 7 rebases).
- **Fix:** Split the trigger into `topology_changed` (active level / anchor /
  level-set-size change → still bump generation + clear history + restore) vs
  `centers_shifted` (only `level_center` moved ≤1 cell → keep history, just refresh
  candidate spheres at the same generation).
- **Verified:** Post-fix probe shows `occ_gen` flat across 6 rebases (was climbing
  every frame). See memory `occlusion-history-clear-per-km-flicker`.
- **RECHECK:** One real session flying low over terrain to confirm the visible
  flicker is gone (headless can't exercise occlusion culling).

### P-001 — Parse errors on startup (water test scripts)
- **Status:** RESOLVED
- **Found / fixed:** 2026-09-11
- **Where:** `tests/water/test_sparse_hydro_scheduler.gd:19`,
  `tests/water/test_sparse_hydro_volume_diagnostics.gd:31`
- **Cause:** `:=` type inference off a value indexed from an untyped array literal
  (`1 << level` where `level` came from an untyped `for` array;
  `[0.5, 99.0, 1.25][slot]`).
- **Fix:** Explicit annotations — `var side: int = ...`, `var depth: float = ...`.
- **Verified:** Full `--headless --import` re-run, no parse errors remain.
