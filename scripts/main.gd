extends Node3D
## Phase 1 harness: generate Asterra, keep terrain resident, walk it, dig it, save it.

const AUTOSAVE := "phase1"
const HUD_UPDATE_INTERVAL_S := 0.20

var cfg: GenConfig
var bake: PlanetBake
var terrain: PlanetTerrain
var orbit_ocean: OrbitOcean
var editor: TerrainEditor
var player: AsterraPlayer
var hud: AsterraHUD
var map: PlanetMap
var debug_menu: DebugMenu
var coastline_profile_editor: CoastlineProfileEditor
var terrain_debug: TerrainDebug
var sun: DirectionalLight3D
var sky_mat: ShaderMaterial
var eye_exposure: HumanEyeExposure

var carry := MaterialStock.new()
var brush_radius := 2.5
var dig_depth := 0.45
var elapsed := 0.0
var _hud_update_accum := 0.0
var _aim: Dictionary = {}
var _started := false
var _rebaking := false
var _last_sun_dir := Vector3.ZERO
var _sun_dir_initialized := false
var _last_solar_scale := -1.0

const ORBIT_MATH := preload("res://scripts/world_authoring/model/orbit_math.gd")
const CELESTIAL_SYSTEM_GENERATOR := preload("res://scripts/world_authoring/celestial_system_generator.gd")
const CELESTIAL_PREVIEW := preload("res://scripts/world_authoring/celestial_body_preview_runtime.gd")
const GAS_GIANT_SHELL := preload("res://scripts/rendering/gas_giant_shell.gd")
## Asterra's orbit around Helion in the standalone game. In Planet Studio the
## per-session OrbitalMotionRuntime owns Frames.helion_dir instead.
var _game_orbit: AuthoringOrbitDefinition

## Procedural multi-planet system (M4). `_system` is null in single-planet mode.
## `_baseline_cfg` keeps world.tres's cfg (with `system_seed`) so per-body configs
## can be derived after `cfg` is swapped to the home planet's bake config.
var _system: CelestialSystemDefinition
var _baseline_cfg: GenConfig
var _celestial_preview: Node
var _gas_shells: Dictionary = {}
var _prev_player_world: Vec3D = Vec3D.new()

func _ready() -> void:
	AppSettings.apply_viewport(get_viewport())
	_baseline_cfg = _load_config()
	cfg = _resolve_home_config(_baseline_cfg)
	Planet.configure(cfg)
	# Wire the primary BodyRuntime to the resident terrain stack. Idempotent with
	# Bodies._ready(); called here so the dependency on a configured Planet is
	# explicit and so future milestones have one place to register the body id.
	Bodies.primary()
	Bodies.active_changed.connect(_on_active_body_changed)
	carry.capacity = 2.4

	hud = AsterraHUD.new()
	add_child(hud)
	map = PlanetMap.new()
	add_child(map)
	debug_menu = DebugMenu.new()
	debug_menu.opened.connect(_on_menu_opened)
	debug_menu.closed.connect(_on_menu_closed)
	debug_menu.rebake_requested.connect(_on_rebake_requested)
	debug_menu.coast_profile_requested.connect(_on_coast_profile_requested)
	add_child(debug_menu)
	coastline_profile_editor = CoastlineProfileEditor.new()
	coastline_profile_editor.apply_requested.connect(_on_coast_profile_applied)
	add_child(coastline_profile_editor)
	_setup_environment()

	hud.show_progress("Generating Asterra…", 0.0)
	bake = PlanetBake.new(cfg)
	bake.finished.connect(_on_baked)
	bake.bake_async()

func _load_config() -> GenConfig:
	var path := "res://world.tres"
	if ResourceLoader.exists(path):
		var r := load(path)
		if r is GenConfig:
			return r
	return GenConfig.new()


## If a system seed is set (world.tres field or the ASTERRA_SYSTEM_SEED env var,
## the latter for headless tests), generate the whole celestial system and return
## the home planet's bake config; otherwise the single-planet path is unchanged.
func _resolve_home_config(baseline: GenConfig) -> GenConfig:
	var env_seed := OS.get_environment("ASTERRA_SYSTEM_SEED")
	if env_seed != "" and env_seed.is_valid_int() and env_seed.to_int() != 0:
		baseline.system_seed = env_seed.to_int()
	if baseline.system_seed == 0:
		return baseline
	var minimal := baseline.minimal_system \
		or OS.get_environment("ASTERRA_MINIMAL_SYSTEM") == "1"
	_system = CELESTIAL_SYSTEM_GENERATOR.generate(
		baseline.system_seed, baseline.axial_tilt_deg, 1.0, minimal)
	return CELESTIAL_SYSTEM_GENERATOR.gen_config_for(
		_system, CELESTIAL_SYSTEM_GENERATOR.HOME_BODY_ID, baseline) as GenConfig

func _setup_environment() -> void:
	var we := WorldEnvironment.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	var sky := Sky.new()
	sky_mat = ShaderMaterial.new()
	sky_mat.shader = load("res://shaders/atmosphere_sky.gdshader")
	sky_mat.set_shader_parameter("u_planet_radius", cfg.planet_radius)
	sky_mat.set_shader_parameter("u_atmosphere_radius", cfg.planet_radius + cfg.atmosphere_height)
	sky_mat.set_shader_parameter("u_sun_intensity", GraphicsQuality.solar_irradiance())
	sky.sky_material = sky_mat
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.ambient_light_sky_contribution = 0.25
	AppSettings.apply_world_environment(env)
	we.environment = env
	add_child(we)
	eye_exposure = HumanEyeExposure.new()
	eye_exposure.configure(we)
	add_child(eye_exposure)

	sun = DirectionalLight3D.new()
	sun.light_energy = GraphicsQuality.SUN_LIGHT_ENERGY
	sun.light_angular_distance = 0.7
	sun.shadow_enabled = true
	# At a grazing (near-horizon) sun the shadow ortho frustum is very oblique, so
	# the ridge crest that must shadow a valley sits far along the light direction.
	# 2500 m left that caster outside every cascade, so low-sun terrain cast no
	# shadow at all. Cover the kilometre-scale near/mid band that
	# terrain_sun_occlusion.gdshaderinc deliberately skips (< ~900 m).
	sun.directional_shadow_max_distance = 9000.0
	# Pancaking (default size 20 m) collapses the shadow depth range and flattens
	# any caster taller than ~20 m toward the light, which on dune/mountain relief
	# both drops real shadows and smears a spurious one that tracks the camera.
	sun.directional_shadow_pancake_size = 0.0
	# 0.035 was far too low for the far-split world-space texel size at planet
	# scale; raised to stop grazing-angle self-shadow acne across the near terrain.
	sun.shadow_bias = 0.1
	sun.shadow_normal_bias = 3.0
	AppSettings.apply_sun(sun)
	add_child(sun)
	_setup_game_orbit()
	_sync_sun_direction(true)
	_sync_solar_brightness(true)

func _sync_sun_direction(force: bool = false) -> void:
	if sun == null:
		return
	var next_dir: Vector3 = Frames.helion_dir.normalized()
	if not force and _sun_dir_initialized \
			and next_dir.distance_squared_to(_last_sun_dir) <= 1e-12:
		return
	_last_sun_dir = next_dir
	_sun_dir_initialized = true
	sun.look_at_from_position(Vector3.ZERO, next_dir * -1.0, Vector3(0, 1, 0))

## Inverse-square solar brightness. The Godot sun energy and the sky/scattering
## u_sun_intensity must scale by the exact same factor as the planet moves toward
## or away from Helion, or the surface and the atmosphere disagree on how bright
## the star is (see GraphicsQuality.solar_irradiance's docstring).
func _sync_solar_brightness(force: bool = false) -> void:
	if sun == null:
		return
	var scale: float = Frames.solar_distance_scale()
	if not force and is_equal_approx(scale, _last_solar_scale):
		return
	_last_solar_scale = scale
	sun.light_energy = GraphicsQuality.SUN_LIGHT_ENERGY * scale
	if sky_mat != null:
		# A sky-material write invalidates the radiance cubemap, so this is gated on
		# a measurable change rather than pushed every tick.
		sky_mat.set_shader_parameter("u_sun_intensity", GraphicsQuality.solar_irradiance())


func _planet_studio_active() -> bool:
	return get_tree().has_meta("launch_mode") \
		and String(get_tree().get_meta("launch_mode")) == "planet_studio"


## Standalone-game orbital setup. Seeds the Frames sim clock's static inputs from
## world.tres and builds the single Asterra orbit consumed by _advance_orbit().
func _setup_game_orbit() -> void:
	if _planet_studio_active():
		return
	Frames.axial_tilt_deg = cfg.axial_tilt_deg
	Frames.day_seconds = maxf(cfg.sidereal_day_seconds, 0.001)
	Frames.year_days = maxf(cfg.year_days, 1.0)
	Frames.anchor_body_id = "asterra"
	# The standalone game runs the clock at real astronomical rate; time of day /
	# season track playtime (`elapsed`) and are restored from the save.
	Frames.playing = true
	Frames.time_scale = 1.0
	_game_orbit = AuthoringOrbitDefinition.new()
	_game_orbit.semi_major_axis_m = cfg.orbit_semi_major_axis_m
	_game_orbit.eccentricity = clampf(cfg.orbit_eccentricity, 0.0, 0.999999)
	_game_orbit.mean_anomaly_at_epoch_deg = cfg.orbit_mean_anomaly_at_epoch_deg
	_game_orbit.argument_periapsis_deg = cfg.orbit_argument_periapsis_deg
	# When the orbit phase is left at its defaults, calibrate it so system_time_s
	# == 0 reproduces the historical fixed sun direction (day-0 look unchanged).
	Frames._rotation_phase0_deg = 0.0
	if is_equal_approx(cfg.orbit_argument_periapsis_deg, 0.0) \
			and is_equal_approx(cfg.orbit_mean_anomaly_at_epoch_deg, 0.0):
		var align: Dictionary = ORBIT_MATH.epoch_alignment_for(
			ORBIT_MATH.LEGACY_SUNWARD, cfg.axial_tilt_deg)
		_game_orbit.argument_periapsis_deg = float(align["argp_deg"])
		Frames._rotation_phase0_deg = float(align["rotation_phase0_deg"])
	Frames.set_orbit_period_s(ORBIT_MATH.period_s(
		cfg.orbit_semi_major_axis_m, cfg.parent_gm_m3_s2))
	_advance_orbit()


## Places Helion for the standalone game from the sim clock: the planet's
## position around Helion gives the sunward direction (star at the origin, so
## sunward = -position) and the distance drives inverse-square brightness.
func _advance_orbit() -> void:
	if _game_orbit == null or _planet_studio_active():
		return
	var pos: Vec3D = ORBIT_MATH.orbit_offset(
		_game_orbit, 1.0, 1.0, cfg.parent_gm_m3_s2, Frames.system_time_s)
	Frames.helion_distance_m = maxf(pos.length(), 1.0)
	Frames.helion_dir = Frames.sun_dir_from_system(pos.mul(-1.0).to_v3().normalized())

func _on_baked(fields: PlanetFields) -> void:
	if _rebaking and terrain != null:
		var keep_dir := player.up_dir() if player != null else Vector3(1, 0, 0)
		Planet.adopt(fields)
		map.invalidate()
		if map.visible:
			map.refresh()
		terrain.build_roots()
		_push_orbit_surface_textures()
		if orbit_ocean != null:
			orbit_ocean.refresh_surface()
		if editor != null:
			editor.refresh()
		if player != null:
			player.spawn_at(keep_dir, 60.0)
			player.input_enabled = not debug_menu.visible
		debug_menu.set_rebake_busy(false)
		hud.hide_progress()
		hud.notify("Planet rebaked — resident global height refreshed")
		_rebaking = false
		_started = true
		return

	Planet.adopt(fields)
	hud.hide_progress()

	terrain = FastPlanetTerrain.new()
	add_child(terrain)
	terrain.build_roots()
	_push_orbit_surface_textures()

	orbit_ocean = OrbitOcean.new()
	add_child(orbit_ocean)

	terrain_debug = TerrainDebug.new()
	terrain_debug.terrain = terrain
	add_child(terrain_debug)
	debug_menu.debug = terrain_debug

	editor = TerrainEditor.new()
	add_child(editor)
	editor.pile_parent = self
	editor.refresh()

	player = AsterraPlayer.new()
	add_child(player)
	var spawn := find_spawn()
	player.spawn_at(spawn, 60.0)
	player.set_mouse_captured(true)
	eye_exposure.observe(player)

	_activate_system()

	if SaveGame.list_saves().has(AUTOSAVE):
		hud.notify("Save '%s' found — press F9 to load it" % AUTOSAVE)
	_started = true


## After the home planet is resident: register every other solid body with the
## Bodies pool (so the M3 approach state machine can travel to them) and spin up
## the far-LOD celestial preview so they render as lit spheres in the sky. No-op
## in single-planet mode.
func _activate_system() -> void:
	if _system == null or _baseline_cfg == null:
		return
	var count := CELESTIAL_SYSTEM_GENERATOR.populate_pool(
		_system, _baseline_cfg, Bodies, Frames, Frames.system_time_s)
	Bodies.concurrent_parent = self   # live second clipmaps parent under Main (M5b)
	if player != null:
		player.set_atmosphere_system(_system)          # GG4: gas-giant drag / buoyancy
		if not player.crush_zone.is_connected(_on_player_crush_zone):
			player.crush_zone.connect(_on_player_crush_zone)
	var home_center: Vec3D = ORBIT_MATH.system_position(
		_system, CELESTIAL_SYSTEM_GENERATOR.HOME_BODY_ID, Frames.system_time_s)
	Bodies.primary().center_system = home_center
	_celestial_preview = CELESTIAL_PREVIEW.new()
	_celestial_preview.name = "CelestialPreview"
	add_child(_celestial_preview)
	_celestial_preview.call("show_system", _system,
		CELESTIAL_SYSTEM_GENERATOR.HOME_BODY_ID, CELESTIAL_SYSTEM_GENERATOR.HOME_BODY_ID)
	print("[system] %s (seed %d): %d bodies, %d travelable from %s"
		% [_system.display_name, _baseline_cfg.system_seed, _system.bodies.size(),
			count, CELESTIAL_SYSTEM_GENERATOR.HOME_BODY_ID])
	hud.notify("System %s — %d other worlds you can fly to" % [_system.display_name, count])


var _crush_warn_accum: float = 999.0

## GG4: the player is below a gas giant's crush/heat boundary -- warn on the HUD
## (rate-limited; the player controller already shoves them back up).
func _on_player_crush_zone(depth_m: float) -> void:
	_crush_warn_accum += get_process_delta_time()
	if _crush_warn_accum < 3.0:
		return
	_crush_warn_accum = 0.0
	if hud != null:
		hud.notify("CRUSHING PRESSURE — %.0f km below the safe limit, ascend" % (depth_m / 1000.0))

## A body with a live concurrent clipmap (M5b) is rendering its real terrain, so
## suppress its far-LOD preview sphere; restore it when the concurrent clipmap is
## torn down.
func _sync_concurrent_preview() -> void:
	if _celestial_preview == null:
		return
	for rt: BodyRuntime in Bodies.slots:
		if rt.is_primary:
			continue
		var gg: Object = _gas_shells.get(String(rt.id))
		var gg_hides: bool = gg != null and is_instance_valid(gg) \
			and bool(gg.call("suppresses_far_lod"))
		_celestial_preview.call("set_body_render_hidden", String(rt.id),
			rt.is_concurrent() or gg_hides)


## GG2: give a gas giant that is the resident body (or a concurrently-rendered
## one) its raymarched volumetric envelope; drop the shell when it is no longer
## close. The far-LOD preview sphere is hidden for a body that has a live shell
## (see _sync_concurrent_preview).
func _sync_gas_giant_shells() -> void:
	if _system == null:
		for id: String in _gas_shells.keys():
			_drop_gas_shell(id)
		return
	var want: Dictionary = {}
	for rt: BodyRuntime in Bodies.slots:
		if not (rt == Bodies.active or rt.is_concurrent()):
			continue
		var body: Resource = _system.call("find_body", String(rt.id))
		if body == null or not bool(body.call("is_gas_giant")):
			continue
		want[String(rt.id)] = true
		var shell: GasGiantShell = _gas_shells.get(String(rt.id))
		if shell == null or not is_instance_valid(shell):
			var model: Object = CELESTIAL_SYSTEM_GENERATOR.gas_giant_model_for(body)
			if model == null:
				continue
			shell = GAS_GIANT_SHELL.new()
			shell.name = "GasGiantShell_%s" % rt.id
			add_child(shell)
			shell.bind(model, _gas_haze_color(body),
				float(body.get(&"sidereal_rotation_period_s")))
			_gas_shells[String(rt.id)] = shell
		var canon: Vec3D = Vec3D.new() if rt == Bodies.active \
			else Frames.body_center_canonical(rt.id)
		# The observer position drives which envelope bands raymarch this frame.
		var observer_render: Vector3 = Frames.to_render(player.world_pos) \
			if player != null else Vector3.ZERO
		shell.sync(Frames.to_render(canon), Frames.helion_dir.normalized(),
			observer_render, float(Frames.system_time_s))
	for id: String in _gas_shells.keys():
		if not want.has(id):
			_drop_gas_shell(id)


func _drop_gas_shell(id: String) -> void:
	var shell: Object = _gas_shells.get(id)
	if shell != null and is_instance_valid(shell):
		shell.queue_free()
	_gas_shells.erase(id)


## Per-gas-giant haze tint, seeded from the body id: mostly Jool-green, some tan /
## gold, the occasional pale blue.
func _gas_haze_color(body: Resource) -> Color:
	var h: int = String(body.get(&"body_id")).hash()
	var rng := RandomNumberGenerator.new()
	rng.seed = h
	var roll: float = rng.randf()
	var hue: float
	if roll < 0.55:
		hue = 0.24 + rng.randf_range(-0.05, 0.06)   # green
	elif roll < 0.85:
		hue = 0.10 + rng.randf_range(-0.03, 0.04)   # tan / gold
	else:
		hue = 0.56 + rng.randf_range(-0.04, 0.05)   # pale blue
	return Color.from_hsv(fposmod(hue, 1.0), rng.randf_range(0.35, 0.6), rng.randf_range(0.55, 0.78))


## Seamless active-body swap arrived (Bodies.set_active). Planet.adopt(target
## fields) + Frames radius/rebase + the target body's deltas have already been
## applied by BodyRuntime.activate_singleton; this refreshes the resident
## render/query stack around the new body and re-seats the player without the
## forced spawn_at teleport the rebake path uses.
func _on_active_body_changed(runtime: Object, player_world_pos: Vec3D) -> void:
	if terrain == null:
		return
	map.invalidate()
	if map.visible:
		map.refresh()
	terrain.build_roots()
	_push_orbit_surface_textures()
	if orbit_ocean != null:
		orbit_ocean.refresh_surface()
	if editor != null:
		editor.refresh()
	# Planet Studio owns the preview camera (world_authoring_runtime_host drives
	# _preview_player + camera framing on a body switch); do not fight it by
	# reseating the player or re-enabling gameplay input.
	if player != null and not _planet_studio_active():
		player.reseat_to_active_body(player_world_pos)
		player.input_enabled = not debug_menu.visible
	# Resize the sky/atmosphere shell to the new resident body. A full per-body
	# atmosphere-profile swap (rayleigh/mie/ozone, clouds) is M6's AtmosphereBinder.
	if sky_mat != null and Planet.cfg != null:
		sky_mat.set_shader_parameter("u_planet_radius", Planet.cfg.planet_radius)
		sky_mat.set_shader_parameter("u_atmosphere_radius",
			Planet.cfg.planet_radius + Planet.cfg.atmosphere_height)
	# Re-anchor the far-LOD preview on the new resident body so its own lightweight
	# duplicate is hidden and the body we just left is drawn as a far sphere again.
	if _celestial_preview != null and _system != null:
		var new_id := String(runtime.id)
		_celestial_preview.call("show_system", _system, new_id, new_id)
	if hud != null:
		hud.notify("Arrived at %s" % String(runtime.id))


func _push_orbit_surface_textures() -> void:
	if terrain == null or Planet.orbit_elevation_texture == null:
		return
	var mats := terrain.debug_materials()
	if mats.is_empty():
		return
	var ground: ShaderMaterial = mats[0]
	ground.set_shader_parameter("u_orbit_elevation", Planet.orbit_elevation_texture)
	ground.set_shader_parameter("u_orbit_face_res", float(Planet.orbit_texture_face_res))
	ground.set_shader_parameter("u_relief_ready", 1.0)

func find_spawn() -> Vector3:
	var f := Planet.fields
	var g := Planet.grid
	var best := -1.0
	var best_c := 0
	for c in g.cell_count:
		if f.elev[c] <= 5.0 or f.elev[c] > 1400.0:
			continue
		if f.temp_mean[c] < 2.0 or f.temp_mean[c] > 24.0:
			continue
		var river := clampf(f.discharge[c] / 260.0, 0.0, 1.0)
		var score: float = f.corridor[c] * f.suitability[c] * (0.35 + river)
		score *= 1.0 - f.wetland[c] * 0.5
		if score > best:
			best = score
			best_c = c
	return g.cell_dir(best_c)

func _process(dt: float) -> void:
	if not _started:
		var st := bake.status()
		hud.show_progress("%s" % st["stage"], st["fraction"])
		return
	elapsed += dt
	_hud_update_accum += dt
	terrain.set_observer(player.world_pos)
	# Seamless multi-planet approach state machine. A no-op until a second body is
	# registered (M4 wires the game's CelestialSystemDefinition into Bodies).
	# Skipped in Planet Studio: there the resident detailed body is chosen
	# explicitly from the celestial map (world_authoring_runtime_host routes the
	# swap through the pool), so proximity must never warm/bake bodies on its own.
	if Bodies.slots.size() > 1 and not _planet_studio_active():
		var vel := Vec3D.new()
		if dt > 1e-5:
			vel = player.world_pos.sub(_prev_player_world).mul(1.0 / dt)
		Bodies.update(player.world_pos, vel)
		_sync_concurrent_preview()
	_sync_gas_giant_shells()
	_prev_player_world = player.world_pos
	_advance_orbit()
	_sync_sun_direction()
	_sync_solar_brightness()
	_aim = player.aim()
	map.set_player_dir(player.up_dir())
	sky_mat.set_shader_parameter("u_up", player.up_dir())
	sky_mat.set_shader_parameter("u_camera_height", player.altitude())
	if _hud_update_accum >= HUD_UPDATE_INTERVAL_S:
		_hud_update_accum = fmod(_hud_update_accum, HUD_UPDATE_INTERVAL_S)
		hud.update_info(player, terrain, carry, brush_radius, _aim)
		_log_body_budget()

## Console instrumentation for the multi-body pool budget (M9). Only prints when
## the resident/warm/concurrent composition actually changes, so a steady cruise
## is quiet.
var _last_body_budget := ""
func _log_body_budget() -> void:
	if Bodies.slots.size() <= 1:
		return
	var s: Dictionary = Bodies.budget_stats()
	var line := "[bodies] slots=%d hot=%d (conc %d) warm=%d far=%d active=%s contact=%s swaps=%d" % [
		s["slots"], s["hot"], s["concurrent"], s["warm"], s["far"],
		s["active"], s["contact"], s["swaps"]]
	if line != _last_body_budget:
		_last_body_budget = line
		print(line)

func _on_menu_opened() -> void:
	if player == null:
		return
	player.set_mouse_captured(false)
	player.input_enabled = false

func _on_menu_closed() -> void:
	if coastline_profile_editor != null:
		coastline_profile_editor.close()
	if player == null:
		return
	player.set_mouse_captured(true)
	player.input_enabled = true

func _on_coast_profile_requested() -> void:
	if coastline_profile_editor != null and Planet.ready_state:
		coastline_profile_editor.open()

func _on_coast_profile_applied(points: PackedVector2Array) -> void:
	if not _started or terrain == null:
		return
	var keep_dir := player.up_dir() if player != null else Vector3(1, 0, 0)
	Planet.set_coast_profile_points(points)
	if Planet.has_method("rebuild_global_height"):
		Planet.rebuild_global_height(false)
	terrain.build_roots()
	_push_orbit_surface_textures()
	map.invalidate()
	if map.visible:
		map.refresh()
	if orbit_ocean != null:
		orbit_ocean.refresh_surface()
	if editor != null:
		editor.refresh()
	if player != null:
		player.spawn_at(keep_dir, 60.0)
	if hud != null:
		hud.notify("Coastline profile applied — global height refreshed")

func _on_rebake_requested() -> void:
	if not _started or _rebaking:
		debug_menu.set_rebake_busy(false)
		return
	_rebaking = true
	_started = false
	_aim.clear()
	hud.show_progress("Rebaking Asterra from seed…", 0.0)
	bake = PlanetBake.new(cfg)
	bake.finished.connect(_on_baked)
	bake.bake_async(false)

func _unhandled_input(event: InputEvent) -> void:
	if not _started or not (event is InputEventKey or event is InputEventMouseButton):
		return
	if event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_M: map.toggle()
			KEY_COMMA: map.cycle(-1)
			KEY_PERIOD: map.cycle(1)
			KEY_BRACKETLEFT: brush_radius = clampf(brush_radius * 0.75, 0.6, 24.0)
			KEY_BRACKETRIGHT: brush_radius = clampf(brush_radius * 1.33, 0.6, 24.0)
			KEY_Q: _drop()
			KEY_E: _collect()
			KEY_G: _grade()
			KEY_T: _teleport()
			KEY_F5: _save()
			KEY_F9: _load()
	if event is InputEventMouseButton and event.pressed:
		if event.button_index == MOUSE_BUTTON_LEFT:
			_dig()
		elif event.button_index == MOUSE_BUTTON_RIGHT:
			_fill()

func _dig() -> void:
	if _aim.is_empty():
		return
	var removed := editor.dig(_aim["dir"], brush_radius, dig_depth)
	var moved := 0.0
	for k in removed.entries.keys():
		var e: Dictionary = removed.entries[k]
		var accepted := carry.add(e["material_id"], e["rock_family"], e["volume_loose"], e["props"])
		moved += accepted
		e["volume_loose"] -= accepted
	var leftover := MaterialStock.new()
	for k in removed.entries.keys():
		var e: Dictionary = removed.entries[k]
		if e["volume_loose"] > 1e-6:
			leftover.add(e["material_id"], e["rock_family"], e["volume_loose"], e["props"])
	if leftover.total_volume() > 1e-6:
		editor.drop_pile(leftover, _aim["dir"])
		hud.notify("Bucket full — %.2f m³ heaped on site" % leftover.total_volume())

func _fill() -> void:
	if _aim.is_empty():
		return
	if carry.total_volume() <= 1e-5:
		hud.notify("Nothing to fill with")
		return
	var placed := editor.fill(_aim["dir"], brush_radius, dig_depth, carry)
	hud.notify("Placed %.2f m³ in place" % placed)

func _grade() -> void:
	if _aim.is_empty():
		return
	var removed := editor.grade(_aim["dir"], brush_radius * 1.4, _aim["height"], carry)
	for k in removed.entries.keys():
		var e: Dictionary = removed.entries[k]
		carry.add(e["material_id"], e["rock_family"], e["volume_loose"], e["props"])
	hud.notify("Graded to %.1f m" % _aim["height"])

func _drop() -> void:
	if carry.total_volume() <= 1e-5:
		return
	var d: Vector3 = _aim["dir"] if not _aim.is_empty() else player.up_dir()
	var moved := MaterialStock.new()
	for k in carry.entries.keys():
		var e: Dictionary = carry.entries[k]
		moved.add(e["material_id"], e["rock_family"], e["volume_loose"], e["props"])
	carry.entries.clear()
	editor.drop_pile(moved, d)
	hud.notify("Dropped %.2f m³" % moved.total_volume())

func _collect() -> void:
	var d: Vector3 = _aim["dir"] if not _aim.is_empty() else player.up_dir()
	var got := editor.collect_pile_near(d, 4.0, carry)
	hud.notify("Picked up %.2f m³" % got if got > 0.0 else "No pile in reach")

func _teleport() -> void:
	player.spawn_at(find_spawn(), 40.0)
	hud.notify("Moved to the best transport corridor site")

func _save() -> void:
	var err := SaveGame.save(AUTOSAVE, cfg, _player_state(), editor, elapsed,
		_baseline_cfg.system_seed if _baseline_cfg != null else 0,
		String(Bodies.active.id), _collect_body_deltas())
	hud.notify("Saved '%s'" % AUTOSAVE if err == OK else "Save failed (%d)" % err)

## Every visited body's terrain-edit blob: the active body's live edits + each
## other body's captured `_delta_blob`.
func _collect_body_deltas() -> Dictionary:
	var out: Dictionary = {}
	for rt: BodyRuntime in Bodies.slots:
		if rt == Bodies.active:
			out[String(rt.id)] = Deltas.serialize()
		elif not rt.delta_blob().is_empty():
			out[String(rt.id)] = rt.delta_blob()
	return out

func _load() -> void:
	var data := SaveGame.load_into(AUTOSAVE, cfg, editor)
	if data.is_empty():
		hud.notify("No save to load")
		return
	# Restore per-body edit blobs + re-activate the body the player was standing on
	# (a no-op for a single-planet / v2 save).
	if Bodies.slots.size() > 1:
		SaveGame.restore_multibody(data, Bodies, Frames)
	player.restore(data["player"])
	carry.deserialize(data["player"].get("carry", []))
	elapsed = data.get("elapsed", 0.0)
	Frames.system_time_s = float(data["player"].get("sim_time_s", Frames.system_time_s))
	_advance_orbit()
	terrain.build_roots()
	_push_orbit_surface_textures()
	hud.notify("Loaded '%s' on %s — %d delta tiles restored" % [
		AUTOSAVE, Bodies.active.id, Deltas.edited_tile_count()])

func _player_state() -> Dictionary:
	var s := player.state()
	s["carry"] = carry.serialize()
	s["sim_time_s"] = Frames.system_time_s
	s["active_body_id"] = String(Bodies.active.id) if Bodies.active != null else "asterra"
	return s
