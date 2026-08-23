extends Node
## Per-biome visual inspection harness.
##
## Photographs one named biome at four altitudes -- standing on it, low air,
## regional, and near-orbit -- so a material can be judged at every range it will
## actually be seen from, in one pass, over the same ground.
##
## The sun is placed by solar elevation at the target rather than left at a fixed
## world vector. Lighting angle changes what a surface tells you more than almost
## anything else: head-on light hides all relief, and a low sun exaggerates it,
## so a material that only looks right at one angle is not finished.
##
##   godot --path . res://tests/BiomeLook.tscn -- \
##       --biome=temperate_forest --sun=30 --azimuth=135 --out=run1
##
## Writes to user://biome/<out>/<biome>_<altitude>.png.

const SETTLE_LIMIT_MS := 45000
const ALTITUDES := [2.0, 300.0, 20000.0, 100000.0]
const ALT_NAMES := ["2m", "300m", "20km", "100km"]
## Looking straight down tells you nothing about relief; looking flat tells you
## nothing about the ground. These are picked per altitude to show both.
## At two metres a shallow pitch fills the frame with ground hundreds of metres
## away and shows nothing about the surface underfoot, so the near views look
## down harder than the distant ones.
const PITCHES := [-0.62, -0.34, -0.55, -0.85]

var main: Node3D
var _biome_name := "temperate_forest"
var _out := "now"
var _sun_elevation := 30.0
var _sun_azimuth := 135.0
var _debug_stage := 0
## Restrict to named altitudes. Iterating on one view is far faster than four.
var _only: Array[String] = []
## Hide free water, to tell a lake apart from a shading artifact.
var _hide_water := false
var _overrides := {}
var _frames := 0

func _process(_dt: float) -> void:
	_frames += 1
	if _frames > 90000:
		get_tree().quit(1)

func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--biome="):
			_biome_name = arg.trim_prefix("--biome=")
		elif arg.begins_with("--out="):
			_out = arg.trim_prefix("--out=")
		elif arg.begins_with("--sun="):
			_sun_elevation = float(arg.trim_prefix("--sun="))
		elif arg.begins_with("--azimuth="):
			_sun_azimuth = float(arg.trim_prefix("--azimuth="))
		elif arg.begins_with("--stage="):
			_debug_stage = int(arg.trim_prefix("--stage="))
		elif arg.begins_with("--set="):
			# name:value, applied to the terrain materials. Bisecting a visual
			# defect by switching one term off is far cheaper than editing and
			# recompiling the shader for each guess.
			var kv := arg.trim_prefix("--set=").split(":", false)
			if kv.size() == 2:
				_overrides[kv[0]] = float(kv[1])
		elif arg == "--nowater":
			_hide_water = true
		elif arg.begins_with("--alts="):
			for a in arg.trim_prefix("--alts=").split(",", false):
				_only.append(a)

	main = load("res://scenes/Main.tscn").instantiate()
	add_child(main)
	while not main._started:
		await get_tree().process_frame
	main.player.set_mouse_captured(false)
	main.hud.visible = false
	main.map.visible = false
	DirAccess.make_dir_recursive_absolute("user://biome/%s" % _out)

	var target := _find_biome_site()
	if target == Vector3.ZERO:
		print("no site found for biome '%s'" % _biome_name)
		get_tree().quit(1)
		return

	# Place the sun before anything is photographed. Everything downstream --
	# terrain shading, the sky, aerial perspective, the exposure controller --
	# reads this one vector.
	_place_sun(target)
	_report_site(target)
	if _hide_water:
		main.terrain_debug.hide_water = true
	for mat in main.terrain.debug_materials():
		for k in _overrides:
			mat.set_shader_parameter(k, _overrides[k])
	if _debug_stage != 0:
		for mat in main.terrain.debug_materials():
			mat.set_shader_parameter("u_debug_stage", _debug_stage)

	for i in ALTITUDES.size():
		if not _only.is_empty() and not (ALT_NAMES[i] in _only):
			continue
		await _shot("%s_%s" % [_biome_name, ALT_NAMES[i]], target,
			ALTITUDES[i], PITCHES[i])

	print("biome shots written to %s"
		% ProjectSettings.globalize_path("user://biome/%s" % _out))
	get_tree().quit()

# ------------------------------------------------------------------- siting ---
func _find_biome_site() -> Vector3:
	return BiomeSites.find(_biome_name)

## Put the sun at a chosen elevation and compass bearing as seen from `at`.
func _place_sun(at: Vector3) -> void:
	var up := at.normalized()
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995:
		ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	var elev := deg_to_rad(_sun_elevation)
	var az := deg_to_rad(_sun_azimuth)
	Frames.helion_dir = (up * sin(elev)
		+ (north * cos(az) + east * sin(az)) * cos(elev)).normalized()

func _report_site(d: Vector3) -> void:
	var info: Dictionary = Planet.sample_info(d)
	print("site: %s  elev %.0f m  temp %.1f C  precip %.0f mm  veg %.2f  soil %.2f m  rock %s"
		% [info["biome_name"], info["elevation"], info["temp_mean"], info["precip"],
		info["vegetation"], info["soil_depth"], info["rock_name"]])
	print("sun: %.0f deg elevation, %.0f deg azimuth" % [_sun_elevation, _sun_azimuth])

# -------------------------------------------------------------------- shots ---
func _shot(name: String, at_dir: Vector3, alt: float, pitch: float) -> void:
	var p: AsterraPlayer = main.player
	var d := at_dir.normalized()
	var r: float = Planet.cfg.planet_radius + maxf(Planet.terrain_height(d), 0.0) + alt
	p.world_pos = Vec3D.new(d.x * r, d.y * r, d.z * r)
	Frames.rebase(p.world_pos)
	# Look down-sun rather than into it, so the ground is lit for the camera
	# instead of silhouetted.
	var look := -Frames.helion_dir
	_aim(p, look, pitch)
	p.vertical_speed = 0.0
	p._sync_transform()

	var stable := 0
	var last_chunks := -1
	var deadline := Time.get_ticks_msec() + SETTLE_LIMIT_MS
	while Time.get_ticks_msec() < deadline:
		p.world_pos = Vec3D.new(d.x * r, d.y * r, d.z * r)
		p.vertical_speed = 0.0
		_aim(p, look, pitch)
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
	img.save_png("user://biome/%s/%s.png" % [_out, name])
	print("  %s: %d chunks, %d fps" % [name, main.terrain.stats()["chunks"],
		Engine.get_frames_per_second()])

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
