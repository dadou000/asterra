extends Node
## Temporary: drive the real Main scene to a set of viewpoints and write a PNG of
## each, so terrain and water changes can be looked at instead of guessed at.

var main: Node3D

func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	main = load("res://scenes/Main.tscn").instantiate()
	add_child(main)
	while not main._started:
		await get_tree().process_frame
	main.player.set_mouse_captured(false)
	main.hud.visible = false
	main.map.visible = false
	DirAccess.make_dir_recursive_absolute("user://shots")

	var f := Planet.fields
	var g := Planet.grid
	var coast := _find_coast(f, g)
	var lake := _find_lake(f, g)
	var land := g.cell_dir(_find_land(f, g))

	await _shot("01_coast_air", coast[0], 3500.0, coast[1], -0.28)
	await _shot("02_coast_low", coast[0], 400.0, coast[1], -0.12)
	await _shot("03_shore_ground", coast[1], 2.0, coast[0], 0.02)
	await _shot("04_lake_air", lake[0], 2200.0, lake[1], -0.40)
	await _shot("05_land_air", land, 5000.0, land, -0.30)
	await _shot("06_orbit", land, 900000.0, land, -1.15)
	await _shot("07_high", land, 60000.0, land, -0.85)
	await _shot("08_land_ground", land, 2.0, land, 0.0)
	# Debug views. These are the pictures that answer "is the mapping coherent"
	# and "what is drawing that straight edge", so they are worth keeping.
	var dbg: TerrainDebug = main.terrain_debug
	dbg.hide_water = true
	await _shot("20_orbit_nowater", land, 900000.0, land, -1.15)
	dbg.hide_water = false
	dbg.lod_tint = true
	await _shot("21_orbit_lod", land, 900000.0, land, -1.15)
	await _shot("22_coast_lod", coast[0], 3500.0, coast[1], -0.28)
	dbg.lod_tint = false
	dbg.tile_axes = true
	dbg.uv_glyphs = true
	await _shot("23_axes_air", land, 4000.0, land, -0.55)
	await _shot("24_axes_high", land, 120000.0, land, -0.95)
	dbg.tile_axes = false
	dbg.uv_glyphs = false
	dbg.face_seams = true
	await _shot("25_seams_orbit", land, 900000.0, land, -1.15)
	dbg.face_seams = false

	main.debug_menu.toggle()
	await _shot("26_menu", coast[0], 3500.0, coast[1], -0.28)
	main.debug_menu.toggle()

	dbg.wireframe = true
	await _shot("27_wireframe", land, 3000.0, land, -0.5)
	dbg.wireframe = false

	dbg.force_lod = true
	dbg.force_lod_depth = 4
	dbg.lod_tint = true
	await _shot("28_forced_lod4", land, 120000.0, land, -0.95)
	dbg.lod_tint = false
	dbg.force_lod = false

	dbg.surface_texture = false
	await _shot("29_no_texture", land, 4000.0, land, -0.45)
	dbg.surface_texture = true

	dbg.heightmap = false
	await _shot("30_no_heightmap", land, 60000.0, land, -0.85)
	dbg.heightmap = true
	await _shot("31_restored", land, 60000.0, land, -0.85)

	print("shots written to %s" % ProjectSettings.globalize_path("user://shots"))
	get_tree().quit()

## Only ever look at the lit hemisphere: the sun is fixed, and half the planet is
## night.
func _lit(g: PlanetGrid, c: int) -> bool:
	return g.cell_dir(c).dot(Frames.helion_dir) > 0.45

## [water dir, nearby land dir]
func _find_coast(f: PlanetFields, g: PlanetGrid) -> Array:
	for c in g.cell_count:
		if f.elev[c] > -90.0 or f.elev[c] < -260.0 or not _lit(g, c):
			continue
		for k in 8:
			var nb := g.nbr[c * 8 + k]
			if f.elev[nb] > 120.0 and f.temp_mean[nb] > 9.0 and f.soil_depth[nb] > 0.4:
				return [g.cell_dir(c), g.cell_dir(nb)]
	return [g.cell_dir(0), g.cell_dir(1)]

## [lake dir, shore dir]
func _find_lake(f: PlanetFields, g: PlanetGrid) -> Array:
	for c in g.cell_count:
		if f.lake_level[c] < -1e8 or not _lit(g, c):
			continue
		var wet := 0
		for k in 8:
			if f.lake_level[g.nbr[c * 8 + k]] > -1e8:
				wet += 1
		if wet >= 5:
			return [g.cell_dir(c), g.cell_dir(g.nbr[c * 8 + 4])]
	return [g.cell_dir(0), g.cell_dir(1)]

func _find_land(f: PlanetFields, g: PlanetGrid) -> int:
	var best := -1.0
	var bc := 0
	for c in g.cell_count:
		if f.elev[c] < 200.0 or f.elev[c] > 2200.0 or not _lit(g, c):
			continue
		if f.temp_mean[c] < 6.0:
			continue
		if f.relief[c] > best:
			best = f.relief[c]
			bc = c
	return bc

func _shot(name: String, at_dir: Vector3, alt: float, look_dir: Vector3, pitch: float) -> void:
	var p: AsterraPlayer = main.player
	var d := at_dir.normalized()
	var r: float = Planet.cfg.planet_radius + maxf(Planet.terrain_height(d), 0.0) + alt
	p.world_pos = Vec3D.new(d.x * r, d.y * r, d.z * r)
	Frames.rebase(p.world_pos)
	_aim(p, look_dir, pitch)
	p.vertical_speed = 0.0
	p._sync_transform()

	var stable := 0
	var last_chunks := -1
	# Bounded in seconds, not frames: a cold start has hundreds of chunks to mesh
	# on the worker pool, and at 250 fps a frame budget runs out long before the
	# meshing does -- which is how this harness used to photograph half-built
	# terrain and call it settled.
	var deadline := Time.get_ticks_msec() + 40000
	while Time.get_ticks_msec() < deadline:
		# Hold the observer still: the flight controller would otherwise drift it.
		p.world_pos = Vec3D.new(d.x * r, d.y * r, d.z * r)
		p.vertical_speed = 0.0
		_aim(p, look_dir, pitch)
		p._sync_transform()
		await get_tree().process_frame
		var st: Dictionary = main.terrain.stats()
		# Settled means nothing is building *and* nothing is mid-dissolve; a
		# hand-off has no builds in flight, so in_flight alone reads as settled
		# half way down a descent.
		# Settled means nothing building, nothing mid-hand-over, and the chunk
		# count holding still -- a descent passes through plenty of momentary
		# lulls that satisfy the first two on their own.
		var idle := int(st["in_flight"]) == 0 and int(st["handoffs"]) == 0 			and int(st["chunks"]) > 0 and int(st["chunks"]) == last_chunks
		last_chunks = int(st["chunks"])
		stable = stable + 1 if idle else 0
		if stable > 40:
			break
	await RenderingServer.frame_post_draw
	var img := get_viewport().get_texture().get_image()
	img.save_png("user://shots/%s.png" % name)
	var hist := {}
	for root in main.terrain.roots:
		_depths(root, hist)
	var keys := hist.keys()
	keys.sort()
	var parts := []
	for k in keys:
		parts.append("%d:%d" % [k, hist[k]])
	var st2: Dictionary = main.terrain.stats()
	print("%s: %d chunks, alt %.0f m, %d fps  depths %s"
		% [name, st2["chunks"], alt, Engine.get_frames_per_second(), " ".join(parts)])

func _depths(node, hist: Dictionary) -> void:
	if node.chunk != null:
		hist[node.depth] = hist.get(node.depth, 0) + 1
	for c in node.children:
		_depths(c, hist)

func _aim(p: AsterraPlayer, target_dir: Vector3, pitch: float) -> void:
	var up := p.up_dir()
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995:
		ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	var t := target_dir - up * target_dir.dot(up)
	if t.length() > 1e-6:
		t = t.normalized()
		p.yaw = atan2(t.dot(east), t.dot(north))
	p.pitch = pitch
