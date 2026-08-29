extends Node
## Fast visual iteration harness.
##
## Shot.tscn photographs thirty views and takes ten minutes, which is the wrong
## tool for judging a lighting or material change. This takes four, always over
## the same deliberately chosen ground -- high-relief, ice-free, sunlit -- so two
## runs can be compared side by side.
##
## Run: godot --path . res://tests/Look.tscn -- --out=after
##      Writes to user://look/<out>/.

const SETTLE_LIMIT_MS := 45000

var main: Node3D
var _out := "now"
var _frames := 0

func _process(_dt: float) -> void:
	_frames += 1
	if _frames > 60000:
		get_tree().quit(1)

func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			_out = arg.trim_prefix("--out=")
	main = load("res://scenes/Main.tscn").instantiate()
	add_child(main)
	while not main._started:
		await get_tree().process_frame
	main.player.set_mouse_captured(false)
	main.hud.visible = false
	main.map.visible = false
	DirAccess.make_dir_recursive_absolute("user://look/%s" % _out)

	var f: PlanetFields = Planet.fields
	var g: PlanetGrid = Planet.grid
	var land: Vector3 = g.cell_dir(_find_relief(f, g))
	var sea: Vector3 = g.cell_dir(_find_open_sea(f, g))

	# Orbit and high altitude are where relief shading and cast shadows have to
	# do all the work: the mesh carries no shape at either.
	await _shot("1_orbit", land, 900000.0, land, -1.15)
	await _shot("2_high", land, 60000.0, land, -0.80)
	await _shot("3_air", land, 6000.0, land, -0.35)
	# A sea view aimed near the sun, which is where the glint lives.
	await _shot("4_sea", sea, 40000.0, sea, -0.55)

	print("look shots written to %s"
		% ProjectSettings.globalize_path("user://look/%s" % _out))
	get_tree().quit()

## Deliberately off the sub-solar point. Terrain lit head-on has almost no
## shading to show -- the same reason a full moon looks flat -- so relief and cast
## shadows can only be judged with the sun well down the sky.
func _lit(g: PlanetGrid, c: int) -> bool:
	var mu: float = g.cell_dir(c).dot(Frames.helion_dir)
	return mu > 0.16 and mu < 0.52

## The most mountainous sunlit land that is not under ice. Ice reads as a white
## wash and hides exactly the shading this harness exists to judge.
func _find_relief(f: PlanetFields, g: PlanetGrid) -> int:
	var best := -1.0
	var bc := 0
	for c in g.cell_count:
		if f.elev[c] < 150.0 or not _lit(g, c):
			continue
		if f.biome[c] == PlanetFields.Biome.ICE_CAP or f.temp_mean[c] < -2.0:
			continue
		if f.relief[c] > best:
			best = f.relief[c]
			bc = c
	return bc

## Deep water on the lit side, for the specular response.
func _find_open_sea(f: PlanetFields, g: PlanetGrid) -> int:
	var best := 0.0
	var bc := 0
	for c in g.cell_count:
		if f.elev[c] > -1500.0 or not _lit(g, c):
			continue
		var toward_sun: float = g.cell_dir(c).dot(Frames.helion_dir)
		if toward_sun > best:
			best = toward_sun
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
	var deadline := Time.get_ticks_msec() + SETTLE_LIMIT_MS
	while Time.get_ticks_msec() < deadline:
		p.world_pos = Vec3D.new(d.x * r, d.y * r, d.z * r)
		p.vertical_speed = 0.0
		_aim(p, look_dir, pitch)
		p._sync_transform()
		await get_tree().process_frame
		var st: Dictionary = main.terrain.stats()
		var idle: bool = int(st["in_flight"]) == 0 and int(st["handoffs"]) == 0 \
			and int(st["chunks"]) > 0 and int(st["chunks"]) == last_chunks
		last_chunks = int(st["chunks"])
		stable = stable + 1 if idle else 0
		if stable > 30:
			break
	await RenderingServer.frame_post_draw
	var img := get_viewport().get_texture().get_image()
	img.save_png("user://look/%s/%s.png" % [_out, name])
	var hist := {}
	for root in main.terrain.roots:
		_depths(root, hist)
	var keys := hist.keys()
	keys.sort()
	var parts := []
	for k in keys:
		parts.append("%d:%d" % [k, hist[k]])
	print("%s/%s: %d chunks, alt %.0f m, %d fps  depths %s"
		% [_out, name, main.terrain.stats()["chunks"], alt,
		Engine.get_frames_per_second(), " ".join(parts)])

func _depths(node, hist: Dictionary) -> void:
	if node.chunk != null:
		hist[node.depth] = hist.get(node.depth, 0) + 1
	for c in node.children:
		if c != null:
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
