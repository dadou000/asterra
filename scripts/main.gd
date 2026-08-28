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

func _ready() -> void:
	AppSettings.apply_viewport(get_viewport())
	cfg = _load_config()
	Planet.configure(cfg)
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
	sun.directional_shadow_max_distance = 2500.0
	sun.shadow_bias = 0.035
	sun.shadow_normal_bias = 3.0
	AppSettings.apply_sun(sun)
	add_child(sun)
	_sync_sun_direction(true)

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

	if SaveGame.list_saves().has(AUTOSAVE):
		hud.notify("Save '%s' found — press F9 to load it" % AUTOSAVE)
	_started = true

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
	_sync_sun_direction()
	_aim = player.aim()
	map.set_player_dir(player.up_dir())
	sky_mat.set_shader_parameter("u_up", player.up_dir())
	sky_mat.set_shader_parameter("u_camera_height", player.altitude())
	if _hud_update_accum >= HUD_UPDATE_INTERVAL_S:
		_hud_update_accum = fmod(_hud_update_accum, HUD_UPDATE_INTERVAL_S)
		hud.update_info(player, terrain, carry, brush_radius, _aim)

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
	var err := SaveGame.save(AUTOSAVE, cfg, _player_state(), editor, elapsed)
	hud.notify("Saved '%s'" % AUTOSAVE if err == OK else "Save failed (%d)" % err)

func _load() -> void:
	var data := SaveGame.load_into(AUTOSAVE, cfg, editor)
	if data.is_empty():
		hud.notify("No save to load")
		return
	player.restore(data["player"])
	carry.deserialize(data["player"].get("carry", []))
	elapsed = data.get("elapsed", 0.0)
	terrain.build_roots()
	_push_orbit_surface_textures()
	hud.notify("Loaded '%s' — %d delta tiles restored" % [AUTOSAVE, Deltas.edited_tile_count()])

func _player_state() -> Dictionary:
	var s := player.state()
	s["carry"] = carry.serialize()
	return s
