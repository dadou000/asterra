extends Node
## Visual regression harness for biome-specific displacement.
## Writes representative settled Forward+ views to user://biome_terrain_shots/.

var main: Node3D
var _shot_filter: Array[String] = []

func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	main = load("res://scenes/Main.tscn").instantiate()
	add_child(main)
	while not main._started:
		await get_tree().process_frame
	main.player.set_mouse_captured(false)
	main.hud.visible = false
	main.map.visible = false
	# Make the visual harness independent of any debug-menu state.
	main.terrain_debug.set_surface_texture(true)
	DirAccess.make_dir_recursive_absolute("user://biome_terrain_shots")
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--shots="):
			for requested in arg.trim_prefix("--shots=").split(",", false):
				_shot_filter.append(requested)

	var targets := _find_targets()
	if _wants("mountain_air"):
		await _shot("mountain_air", targets["mountain"], 9000.0, -0.78)
	if _wants("mountain_close"):
		await _shot("mountain_close", targets["mountain"], 900.0, -0.48)
	if _wants("desert"):
		await _shot("desert", targets["desert"], 4800.0, -0.70)
	if _wants("wetland"):
		await _shot("wetland", targets["wetland"], 4800.0, -0.70)
	if _wants("forest"):
		await _shot("forest", targets["forest"], 3600.0, -0.66)
	if _wants("forest_ground"):
		await _shot("forest_ground", targets["forest_ground"], 1.7, -0.82)
	if _wants("forest_consistency_ground"):
		await _shot("forest_consistency_ground", targets["forest_consistency"], 1.7, -0.82)
	if _wants("forest_consistency_200m"):
		await _shot("forest_consistency_200m", targets["forest_consistency"], 206.0, -0.82)
	if _wants("forest_consistency_600m"):
		await _shot("forest_consistency_600m", targets["forest_consistency"], 580.0, -0.82)
	if _wants("forest_consistency_1200m"):
		await _shot("forest_consistency_1200m", targets["forest_consistency"], 1210.0, -0.82)
	if _wants("forest_patch_2550m"):
		await _shot("forest_patch_2550m", targets["forest_patch"], 2550.0, -0.82)
	if _wants("forest_patch_6790m"):
		await _shot("forest_patch_6790m", targets["forest_patch_far"], 6790.0, -0.82)
	if _wants("ice"):
		await _shot("ice", targets["ice"], 6000.0, -0.72)
	if _wants("shelf"):
		await _shot("shelf", targets["shelf"], 1200.0, -0.55)
	if _wants("shelf_bed"):
		await _shot("shelf_bed", targets["shelf"], 1200.0, -0.55)
	if _wants("coastland"):
		await _shot("coastland", targets["coastland"], 1200.0, -0.55)
	print("biome terrain shots written to %s" % ProjectSettings.globalize_path("user://biome_terrain_shots"))
	get_tree().quit()

func _wants(name: String) -> bool:
	return _shot_filter.is_empty() or name in _shot_filter

func _find_targets() -> Dictionary:
	var f: PlanetFields = Planet.fields
	var g: PlanetGrid = Planet.grid
	var result := {}
	var reported_shelf := CubeSphere.latlon_to_dir(deg_to_rad(36.0), deg_to_rad(-34.75))
	result["shelf"] = g.dir_to_index(reported_shelf)
	var reported_forest := CubeSphere.latlon_to_dir(deg_to_rad(29.07), deg_to_rad(32.67))
	result["forest_ground"] = g.dir_to_index(reported_forest)
	var consistency_forest := CubeSphere.latlon_to_dir(deg_to_rad(27.56), deg_to_rad(-37.48))
	result["forest_consistency"] = g.dir_to_index(consistency_forest)
	var reported_patch := CubeSphere.latlon_to_dir(deg_to_rad(39.30), deg_to_rad(-0.86))
	result["forest_patch"] = g.dir_to_index(reported_patch)
	var reported_patch_far := CubeSphere.latlon_to_dir(deg_to_rad(39.35), deg_to_rad(-0.59))
	result["forest_patch_far"] = g.dir_to_index(reported_patch_far)
	var coastland_score := -INF
	var scores := {"mountain": -INF, "desert": -INF, "wetland": -INF,
		"forest": -INF, "ice": -INF}
	for c in g.cell_count:
		var d := g.cell_dir(c)
		var runtime_h := Planet.macro_height(d)
		if runtime_h > 12.0 and runtime_h < 140.0 and d.dot(Frames.helion_dir) > 0.12:
			var candidate_score := d.dot(reported_shelf) * 100000.0 - absf(runtime_h - 55.0)
			if candidate_score > coastland_score:
				coastland_score = candidate_score
				result["coastland"] = c
		if d.dot(Frames.helion_dir) < 0.28:
			continue
		# Exclude known macro-cache spikes so this harness judges displacement,
		# not a pathological erosion cell.
		if f.elev[c] <= 20.0 or f.elev[c] > Planet.cfg.max_uplift + 1400.0 \
				or f.relief[c] > 5200.0 or not _is_interior_land(c, f, g):
			continue
		var b: int = f.biome[c]
		var runtime_relief := _runtime_neighbour_relief(c, g)
		var score: float
		if b in [PlanetFields.Biome.ALPINE, PlanetFields.Biome.BARE_ROCK]:
			score = runtime_relief * 2.0 + Planet.macro_height(d) * 0.35 + f.fault[c] * 800.0
			_take("mountain", c, score, result, scores)
		if b in [PlanetFields.Biome.HOT_DESERT, PlanetFields.Biome.COLD_DESERT]:
			score = f.soil_sand[c] * 900.0 + runtime_relief * 0.7 + Planet.macro_height(d) * 0.08
			_take("desert", c, score, result, scores)
		if b == PlanetFields.Biome.WETLAND:
			score = f.wetland[c] * 1000.0 + f.discharge[c] * 0.05
			_take("wetland", c, score, result, scores)
		if b in [PlanetFields.Biome.TEMPERATE_FOREST, PlanetFields.Biome.TAIGA,
				PlanetFields.Biome.TEMPERATE_RAINFOREST, PlanetFields.Biome.TROPICAL_RAINFOREST]:
			score = runtime_relief + f.vegetation[c] * 600.0
			_take("forest", c, score, result, scores)
		if b == PlanetFields.Biome.ICE_CAP:
			score = runtime_relief + maxf(Planet.macro_height(d), 0.0) * 0.18
			_take("ice", c, score, result, scores)
	for key in scores:
		if not result.has(key):
			result[key] = 0
		else:
			var c: int = result[key]
			print("%s cell %d biome %s elev %.0f relief raw %.0f runtime %.0f geomorph %s" % [key, c,
				PlanetFields.BIOME_NAMES[f.biome[c]], f.elev[c], f.relief[c],
				_runtime_neighbour_relief(c, g), Planet.geomorph_composition(g.cell_dir(c))])
	return result

func _runtime_neighbour_relief(cell: int, g: PlanetGrid) -> float:
	var h := Planet.macro_height(g.cell_dir(cell))
	var lo := h
	var hi := h
	for k in 8:
		var neighbour_h := Planet.macro_height(g.cell_dir(g.nbr[cell * 8 + k]))
		lo = minf(lo, neighbour_h)
		hi = maxf(hi, neighbour_h)
	return hi - lo

func _is_interior_land(cell: int, f: PlanetFields, g: PlanetGrid) -> bool:
	for k in 8:
		var neighbour: int = g.nbr[cell * 8 + k]
		if f.elev[neighbour] <= 0.0:
			return false
		for k2 in 8:
			if f.elev[g.nbr[neighbour * 8 + k2]] <= 0.0:
				return false
	return true

func _take(key: String, cell: int, score: float, result: Dictionary, scores: Dictionary) -> void:
	if score > float(scores[key]):
		scores[key] = score
		result[key] = cell

func _shot(name: String, cell: int, altitude: float, pitch: float) -> void:
	var g: PlanetGrid = Planet.grid
	var f: PlanetFields = Planet.fields
	var d := g.cell_dir(cell)
	# Look down the steepest local fall line, not arbitrarily across a plateau.
	var heading_cell := g.nbr[cell * 8]
	for k in 8:
		var candidate: int = g.nbr[cell * 8 + k]
		if f.elev[candidate] < f.elev[heading_cell]:
			heading_cell = candidate
	var heading := g.cell_dir(heading_cell)
	var p: AsterraPlayer = main.player
	# Observe expressive landforms from their lower neighbour instead of hovering
	# over the summit/plateau. The latter is a useful stream stress test but hides
	# the very silhouette this visual regression exists to inspect.
	var use_foothill := name not in ["wetland", "forest_ground", "forest_consistency_ground",
		"forest_consistency_200m", "forest_consistency_600m", "forest_consistency_1200m",
		"forest_patch_2550m", "forest_patch_6790m", "shelf", "shelf_bed", "coastland"]
	var expose_seabed := name == "shelf_bed"
	main.terrain_debug.set_hide_water(expose_seabed)
	var camera_dir := heading if use_foothill else d
	var r: float = Planet.cfg.planet_radius + maxf(Planet.terrain_height(camera_dir), 0.0) + altitude
	p.world_pos = Vec3D.new(camera_dir.x * r, camera_dir.y * r, camera_dir.z * r)
	Frames.rebase(p.world_pos)
	if use_foothill:
		_aim_at_surface(p, d)
	else:
		_aim(p, heading, pitch)
	p.vertical_speed = 0.0
	p._sync_transform()

	var stable := 0
	var last_chunks := -1
	var deadline := Time.get_ticks_msec() + 35000
	while Time.get_ticks_msec() < deadline:
		p.world_pos = Vec3D.new(camera_dir.x * r, camera_dir.y * r, camera_dir.z * r)
		p.vertical_speed = 0.0
		if use_foothill:
			_aim_at_surface(p, d)
		else:
			_aim(p, heading, pitch)
		p._sync_transform()
		await get_tree().process_frame
		var stats: Dictionary = main.terrain.stats()
		var idle := int(stats["in_flight"]) == 0 and int(stats["handoffs"]) == 0 \
			and int(stats["chunks"]) > 0 and int(stats["chunks"]) == last_chunks
		last_chunks = int(stats["chunks"])
		stable = stable + 1 if idle else 0
		if stable > 30:
			break
	# Let asynchronous orbit/material caches relinquish their worker cores before
	# sampling FPS or the final image. Terrain convergence alone can finish on the
	# same frame as those global caches and would measure startup contention.
	for _frame in 120:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	image.save_png("user://biome_terrain_shots/%s.png" % name)
	var stats: Dictionary = main.terrain.stats()
	print("%s: %s alt %.0f m chunks %d fps %d" % [name,
		PlanetFields.BIOME_NAMES[f.biome[cell]], altitude, stats["chunks"],
		Engine.get_frames_per_second()])
	if expose_seabed:
		main.terrain_debug.set_hide_water(false)

func _aim(p: AsterraPlayer, target_dir: Vector3, pitch: float) -> void:
	var up := p.up_dir()
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995:
		ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	var target := target_dir - up * target_dir.dot(up)
	if target.length() > 1e-6:
		target = target.normalized()
		p.yaw = atan2(target.dot(east), target.dot(north))
	p.pitch = pitch

func _aim_at_surface(p: AsterraPlayer, target_dir: Vector3) -> void:
	var target_r := Planet.cfg.planet_radius + Planet.terrain_height(target_dir)
	var target := target_dir * target_r
	var camera := p.world_pos.to_v3()
	var look := (target - camera).normalized()
	var up := p.up_dir()
	var flat := look - up * look.dot(up)
	_aim(p, look, atan2(look.dot(up), maxf(flat.length(), 1.0e-6)))
