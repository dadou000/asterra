extends Node3D
## Phase 1 harness: generate Asterra, stream it, walk it, dig it, save it.

const AUTOSAVE := "phase1"

var cfg: GenConfig
var bake: PlanetBake
var terrain: PlanetTerrain
var orbit_ocean: OrbitOcean
var editor: TerrainEditor
var player: AsterraPlayer
var hud: AsterraHUD
var map: PlanetMap
var debug_menu: DebugMenu
var terrain_debug: TerrainDebug
var sun: DirectionalLight3D
var sky_mat: ShaderMaterial

var carry := MaterialStock.new()
var brush_radius := 2.5
var dig_depth := 0.45
var elapsed := 0.0
var _aim: Dictionary = {}
var _started := false
var _rebaking := false

func _ready() -> void:
	cfg = _load_config()
	Planet.configure(cfg)
	carry.capacity = 2.4                       ## a hand-carried bucket, not a truck

	hud = AsterraHUD.new()
	add_child(hud)
	map = PlanetMap.new()
	add_child(map)
	debug_menu = DebugMenu.new()
	debug_menu.opened.connect(_on_menu_opened)
	debug_menu.closed.connect(_on_menu_closed)
	debug_menu.rebake_requested.connect(_on_rebake_requested)
	add_child(debug_menu)
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
	sky.sky_material = sky_mat
	env.sky = sky
	env.ambient_light_source = Environment.AMBIENT_SOURCE_SKY
	env.ambient_light_sky_contribution = 0.85
	env.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	env.ssao_enabled = false
	we.environment = env
	add_child(we)

	sun = DirectionalLight3D.new()
	sun.light_energy = 1.15
	sun.light_angular_distance = 0.7
	sun.shadow_enabled = true
	sun.directional_shadow_max_distance = 320.0
	sun.directional_shadow_mode = DirectionalLight3D.SHADOW_ORTHOGONAL
	add_child(sun)

func _on_baked(fields: PlanetFields) -> void:
	# During a manual debug rebake the existing world stays alive while the new
	# fields are generated. Swap them atomically here, then rebuild only the
	# systems that depend on baked data instead of duplicating the whole scene.
	if _rebaking and terrain != null:
		var keep_dir := player.up_dir() if player != null else Vector3(1, 0, 0)
		Planet.adopt(fields)
		map.invalidate()
		if map.visible:
			map.refresh()
		terrain.build_roots()
		_push_orbit_surface_textures()
		if editor != null:
			editor.refresh()
		if player != null:
			# Elevation may have changed substantially; keep the geographic
			# location but put the player safely above the newly baked surface.
			player.spawn_at(keep_dir, 60.0)
			player.input_enabled = not debug_menu.visible
		debug_menu.set_rebake_busy(false)
		hud.hide_progress()
		hud.notify("Planet rebaked from seed — cache ignored")
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

## Pick the most convincing place to start: a well-drained, buildable site on a
## natural transport corridor beside a real river. This is the same score the
## roadmap will later use to justify where Sterling grew.
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
	terrain.set_observer(player.world_pos)
	sun.rotation = Vector3.ZERO
	sun.look_at_from_position(Vector3.ZERO, Frames.helion_dir * -1.0, Vector3(0, 1, 0))
	_aim = player.aim()
	map.set_player_dir(player.up_dir())
	sky_mat.set_shader_parameter("u_up", player.up_dir())
	sky_mat.set_shader_parameter("u_camera_height", player.altitude())
	hud.update_info(player, terrain, carry, brush_radius, _aim)

func _on_menu_opened() -> void:
	if player == null:
		return
	player.set_mouse_captured(false)
	player.input_enabled = false

func _on_menu_closed() -> void:
	if player == null:
		return
	player.set_mouse_captured(true)
	player.input_enabled = true

func _on_rebake_requested() -> void:
	# Do not start a second generator while the initial bake or another manual
	# rebake is already running. The menu button is re-enabled if the request was
	# made too early.
	if not _started or _rebaking:
		debug_menu.set_rebake_busy(false)
		return
	_rebaking = true
	_started = false
	_aim.clear()
	hud.show_progress("Rebaking Asterra from seed…", 0.0)
	bake = PlanetBake.new(cfg)
	bake.finished.connect(_on_baked)
	# Explicit false bypasses user://.../*.bake without deleting it. The fresh
	# result is still saved by PlanetBake when generation finishes.
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
		moved += carry.add(e["material_id"], e["rock_family"], e["volume_loose"], e["props"])
		e["volume_loose"] -= moved
	# Anything the bucket could not take is left on the ground as a heap.
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
	# Every chunk must be rebuilt: the deltas it was meshed with have changed.
	terrain.build_roots()
	_push_orbit_surface_textures()
	hud.notify("Loaded '%s' — %d delta tiles restored" % [AUTOSAVE, Deltas.edited_tile_count()])

func _player_state() -> Dictionary:
	var s := player.state()
	s["carry"] = carry.serialize()
	return s
