# Problem list

Running tracker of known issues to recheck later. Newest first. When a problem is
resolved, keep the entry and move it to **Resolved** with the fix + how it was
verified. When something is fixed but not fully verified, leave it **Open** with a
`RECHECK:` line stating exactly what still needs confirming.

Status key: `OPEN` · `FIX-UNVERIFIED` (fix landed, needs confirmation) · `RESOLVED`

---

## Open

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
  reconfirmed with full_bright at the *later*, fixed-clamp altitude. Do not
  reopen this as a general pipeline bug without a fresh, low-churn repro.

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
