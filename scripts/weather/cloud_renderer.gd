class_name CloudRenderer
extends MeshInstance3D
## Full-screen spherical volumetric cloud renderer + storm electrical renderer.
##
## Cloud density is planet-centred and weather-driven. Lightning is generated
## procedurally from the *active simulated storm events*; no authored bolt assets
## or temporary reference files are required. Bolt geometry is kept in canonical
## planet coordinates and rebuilt into the current floating render frame.

const LIGHTNING_INTRACLOUD := 0
const LIGHTNING_CLOUD_GROUND := 1
const LIGHTNING_CLOUD_CLOUD := 2
const LIGHTNING_SPIDER := 3
const LIGHTNING_POSITIVE_GIANT := 4
const LIGHTNING_SHEET := 5
const MAX_VISIBLE_BOLTS := 8

var cfg: GenConfig
var weather: WeatherSystem
var cloud_material: ShaderMaterial
var base_noise: NoiseTexture3D
var detail_noise: NoiseTexture3D

var _lightning_instance: MeshInstance3D
var _lightning_mesh: ImmediateMesh
var _lightning_material: ShaderMaterial
var _bolts: Array = []
var _lightning_rng := RandomNumberGenerator.new()

func configure(p_cfg: GenConfig, p_weather: WeatherSystem) -> void:
	cfg = p_cfg
	weather = p_weather
	_setup_mesh()
	_setup_noise()
	_setup_material()
	_setup_lightning()
	refresh_weather()
	set_process(true)

func _setup_mesh() -> void:
	var quad := QuadMesh.new()
	quad.size = Vector2(2.0, 2.0)
	mesh = quad
	cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	# The shader writes clip-space POSITION, so CPU frustum culling of the tiny
	# source quad is meaningless. A large margin keeps the post-effect resident.
	extra_cull_margin = 10000000.0

func _setup_noise() -> void:
	var broad := FastNoiseLite.new()
	broad.seed = cfg.stream_seed("cloud_volume_base")
	broad.noise_type = FastNoiseLite.TYPE_SIMPLEX_SMOOTH
	broad.frequency = 0.032
	broad.fractal_type = FastNoiseLite.FRACTAL_FBM
	broad.fractal_octaves = 5
	broad.fractal_lacunarity = 2.02
	broad.fractal_gain = 0.52
	base_noise = NoiseTexture3D.new()
	base_noise.width = 80
	base_noise.height = 80
	base_noise.depth = 80
	base_noise.seamless = true
	base_noise.normalize = true
	base_noise.noise = broad

	var detail := FastNoiseLite.new()
	detail.seed = cfg.stream_seed("cloud_volume_detail")
	detail.noise_type = FastNoiseLite.TYPE_CELLULAR
	detail.frequency = 0.065
	detail.cellular_distance_function = FastNoiseLite.DISTANCE_EUCLIDEAN
	detail.cellular_return_type = FastNoiseLite.RETURN_DISTANCE2_SUB
	detail.fractal_type = FastNoiseLite.FRACTAL_NONE
	detail_noise = NoiseTexture3D.new()
	detail_noise.width = 48
	detail_noise.height = 48
	detail_noise.depth = 48
	detail_noise.seamless = true
	detail_noise.normalize = true
	detail_noise.noise = detail

func _setup_material() -> void:
	cloud_material = ShaderMaterial.new()
	cloud_material.shader = load("res://shaders/volumetric_clouds.gdshader")
	cloud_material.render_priority = 100
	cloud_material.set_shader_parameter("u_planet_radius", cfg.planet_radius)
	cloud_material.set_shader_parameter("u_atmosphere_radius", cfg.planet_radius + cfg.atmosphere_height)
	cloud_material.set_shader_parameter("u_cloud_outer_radius", cfg.planet_radius + 18500.0)
	cloud_material.set_shader_parameter("u_base_noise", base_noise)
	cloud_material.set_shader_parameter("u_detail_noise", detail_noise)
	material_override = cloud_material

func _setup_lightning() -> void:
	_lightning_rng.seed = cfg.stream_seed("lightning_visual")
	_bolts.clear()
	if _lightning_instance == null:
		_lightning_instance = MeshInstance3D.new()
		_lightning_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		_lightning_instance.extra_cull_margin = 10000000.0
		add_child(_lightning_instance)
	_lightning_mesh = ImmediateMesh.new()
	_lightning_instance.mesh = _lightning_mesh
	_lightning_material = ShaderMaterial.new()
	_lightning_material.shader = load("res://shaders/lightning.gdshader")
	# Draw the physical channel immediately before the volumetric cloud pass. The
	# cloud then naturally veils channels that are deep inside a cumulonimbus.
	_lightning_material.render_priority = 99

func refresh_weather() -> void:
	if cloud_material == null or weather == null:
		return
	if weather.weather_map == null or weather.wind_map == null:
		return
	cloud_material.set_shader_parameter("u_weather_map", weather.weather_map)
	cloud_material.set_shader_parameter("u_wind_map", weather.wind_map)
	cloud_material.set_shader_parameter("u_weather_face_res", float(weather.face_res))

func _process(dt: float) -> void:
	if cloud_material == null:
		return
	var cam := get_viewport().get_camera_3d()
	if cam != null:
		var camera_world: Vec3D = Frames.to_world(cam.global_position)
		cloud_material.set_shader_parameter("u_camera_world", Vector3(
			float(camera_world.x), float(camera_world.y), float(camera_world.z)))
	cloud_material.set_shader_parameter("u_sun_dir", Frames.helion_dir.normalized())
	_update_lightning(dt, cam)

# -------------------------------------------------------------- lightning ---
func _update_lightning(dt: float, cam: Camera3D) -> void:
	# Existing channels live for fractions of a second and often restrike. Their
	# broad in-cloud illumination is fed back to the volumetric shader below.
	for i in range(_bolts.size() - 1, -1, -1):
		var bolt: Dictionary = _bolts[i]
		bolt["age"] = float(bolt["age"]) + dt
		if float(bolt["age"]) >= float(bolt["life"]):
			_bolts.remove_at(i)
		else:
			_bolts[i] = bolt

	if weather != null and _bolts.size() < MAX_VISIBLE_BOLTS:
		var events: Array = weather.active_events()
		for item in events:
			if _bolts.size() >= MAX_VISIBLE_BOLTS:
				break
			var ev: Dictionary = item
			var intensity: float = clampf(float(ev.get("intensity", 0.0)), 0.0, 1.0)
			# Rate is real-time so accelerated weather does not turn every storm into
			# a solid white strobe. Squall lines are a little more electrically active.
			var rate: float = 0.10 + intensity * intensity * 1.05
			if int(ev.get("type", 0)) == WeatherSystem.EVENT_SQUALL_LINE:
				rate *= 1.35
			if _lightning_rng.randf() < rate * dt:
				_spawn_lightning(ev)

	_push_lightning_lights()
	_rebuild_lightning_mesh(cam)

func _spawn_lightning(ev: Dictionary) -> void:
	var lat := deg_to_rad(float(ev.get("latitude", 0.0)))
	var lon := deg_to_rad(float(ev.get("longitude", 0.0)))
	var event_center := CubeSphere.latlon_to_dir(lat, lon)
	var intensity: float = clampf(float(ev.get("intensity", 0.5)), 0.0, 1.0)
	var event_type: int = int(ev.get("type", 0))
	var frame: Array = CubeSphere.tangent_basis(event_center)
	var tangent: Vector3 = (frame[0] as Vector3) * cos(_lightning_rng.randf_range(0.0, TAU)) \
		+ (frame[1] as Vector3) * sin(_lightning_rng.randf_range(0.0, TAU))
	# Put individual cells around the storm, not exactly at the abstract event
	# centre. Hurricanes use a wider electrical envelope than squall-line cells.
	var spread: float = lerpf(45000.0, 180000.0, intensity)
	if event_type == WeatherSystem.EVENT_TROPICAL_CYCLONE:
		spread = lerpf(90000.0, 280000.0, intensity)
	var cell_dir := _move_on_sphere(event_center, tangent.normalized(),
		sqrt(_lightning_rng.randf()) * spread)

	var kind := _choose_lightning_kind(event_type, intensity)
	var main_length := 10000.0
	var start_alt := 8500.0
	var end_alt := 7000.0
	var width := 14.0
	var life := 0.18
	var energy := 4.0
	var branch_count := 3

	match kind:
		LIGHTNING_INTRACLOUD:
			main_length = _lightning_rng.randf_range(4000.0, 18000.0)
			start_alt = _lightning_rng.randf_range(5500.0, 12500.0)
			end_alt = clampf(start_alt + _lightning_rng.randf_range(-2200.0, 2200.0), 3500.0, 15000.0)
			width = 10.0
			life = _lightning_rng.randf_range(0.12, 0.24)
			energy = _lightning_rng.randf_range(2.8, 5.5)
			branch_count = _lightning_rng.randi_range(2, 5)
		LIGHTNING_CLOUD_GROUND:
			main_length = _lightning_rng.randf_range(3000.0, 18000.0)
			start_alt = _lightning_rng.randf_range(6500.0, 14500.0)
			end_alt = -1.0 # terrain/sea surface below
			width = 16.0
			life = _lightning_rng.randf_range(0.18, 0.34)
			energy = _lightning_rng.randf_range(5.0, 8.5)
			branch_count = _lightning_rng.randi_range(4, 8)
		LIGHTNING_CLOUD_CLOUD:
			main_length = _lightning_rng.randf_range(18000.0, 52000.0)
			start_alt = _lightning_rng.randf_range(6500.0, 13500.0)
			end_alt = _lightning_rng.randf_range(6000.0, 14000.0)
			width = 13.0
			life = _lightning_rng.randf_range(0.18, 0.34)
			energy = _lightning_rng.randf_range(4.0, 7.0)
			branch_count = _lightning_rng.randi_range(4, 8)
		LIGHTNING_SPIDER:
			main_length = _lightning_rng.randf_range(60000.0, 120000.0)
			start_alt = _lightning_rng.randf_range(9000.0, 15000.0)
			end_alt = clampf(start_alt + _lightning_rng.randf_range(-1800.0, 1800.0), 7500.0, 16500.0)
			width = 12.0
			life = _lightning_rng.randf_range(0.30, 0.58)
			energy = _lightning_rng.randf_range(5.5, 9.0)
			branch_count = _lightning_rng.randi_range(8, 15)
		LIGHTNING_POSITIVE_GIANT:
			main_length = _lightning_rng.randf_range(12000.0, 42000.0)
			start_alt = _lightning_rng.randf_range(12000.0, 17500.0)
			end_alt = -1.0
			width = 24.0
			life = _lightning_rng.randf_range(0.28, 0.46)
			energy = _lightning_rng.randf_range(8.0, 13.0)
			branch_count = _lightning_rng.randi_range(3, 7)
		LIGHTNING_SHEET:
			main_length = _lightning_rng.randf_range(5000.0, 14000.0)
			start_alt = _lightning_rng.randf_range(6500.0, 12000.0)
			end_alt = start_alt
			width = 8.0
			life = _lightning_rng.randf_range(0.08, 0.16)
			energy = _lightning_rng.randf_range(6.0, 11.0)
			branch_count = 0

	var direction_angle := _lightning_rng.randf_range(0.0, TAU)
	var local_frame: Array = CubeSphere.tangent_basis(cell_dir)
	var travel: Vector3 = ((local_frame[0] as Vector3) * cos(direction_angle)
		+ (local_frame[1] as Vector3) * sin(direction_angle)).normalized()
	var end_dir := _move_on_sphere(cell_dir, travel, main_length)
	if end_alt < 0.0:
		end_alt = maxf(Planet.terrain_height(end_dir), 0.0) + 12.0

	var point_count := clampi(int(main_length / 2600.0) + 8, 9, 52)
	if kind == LIGHTNING_CLOUD_GROUND or kind == LIGHTNING_POSITIVE_GIANT:
		point_count = clampi(int(start_alt / 650.0) + 10, 14, 38)
	var jitter := clampf(main_length * 0.018, 180.0, 2100.0)
	var main_points := _channel_points(cell_dir, end_dir, start_alt, end_alt, point_count, jitter)
	var segments: Array = []
	_append_channel(main_points, 1.0, segments)

	for _b in branch_count:
		if main_points.size() < 5:
			break
		var bi := _lightning_rng.randi_range(2, main_points.size() - 3)
		var bp: Vector3 = main_points[bi]
		var bd := bp.normalized()
		var balt := bp.length() - cfg.planet_radius
		var bf: Array = CubeSphere.tangent_basis(bd)
		var ba := _lightning_rng.randf_range(0.0, TAU)
		var bt: Vector3 = ((bf[0] as Vector3) * cos(ba) + (bf[1] as Vector3) * sin(ba)).normalized()
		var branch_len := main_length * _lightning_rng.randf_range(0.08, 0.30)
		if kind == LIGHTNING_SPIDER:
			branch_len = main_length * _lightning_rng.randf_range(0.12, 0.42)
		var bend := _move_on_sphere(bd, bt, branch_len)
		var bend_alt := clampf(balt + _lightning_rng.randf_range(-1800.0, 1200.0), 2500.0, 17000.0)
		var bpoints := _channel_points(bd, bend, balt, bend_alt,
			clampi(int(branch_len / 2600.0) + 4, 4, 22), jitter * 0.72)
		_append_channel(bpoints, _lightning_rng.randf_range(0.32, 0.70), segments)

	var flash_point: Vector3 = main_points[main_points.size() / 2]
	_bolts.append({
		"kind": kind,
		"segments": segments,
		"age": 0.0,
		"life": life,
		"width": width,
		"energy": energy * lerpf(0.72, 1.18, intensity),
		"flash": flash_point,
		"seed": _lightning_rng.randf_range(0.0, 1000.0),
	})

func _choose_lightning_kind(event_type: int, intensity: float) -> int:
	var r := _lightning_rng.randf()
	# Weak storms are mostly small intracloud discharges. Mature squall lines and
	# strong tropical convection unlock long crawlers and rare positive giants.
	if intensity < 0.42:
		return LIGHTNING_INTRACLOUD if r < 0.82 else LIGHTNING_CLOUD_CLOUD
	if event_type == WeatherSystem.EVENT_SQUALL_LINE:
		if r < 0.42: return LIGHTNING_INTRACLOUD
		if r < 0.66: return LIGHTNING_CLOUD_GROUND
		if r < 0.80: return LIGHTNING_CLOUD_CLOUD
		if r < 0.93: return LIGHTNING_SPIDER
		if r < 0.975: return LIGHTNING_POSITIVE_GIANT
		return LIGHTNING_SHEET
	if r < 0.52: return LIGHTNING_INTRACLOUD
	if r < 0.68: return LIGHTNING_CLOUD_GROUND
	if r < 0.82: return LIGHTNING_CLOUD_CLOUD
	if r < 0.93: return LIGHTNING_SPIDER
	if r < 0.97: return LIGHTNING_POSITIVE_GIANT
	return LIGHTNING_SHEET

func _move_on_sphere(d: Vector3, tangent: Vector3, metres: float) -> Vector3:
	var t := tangent - d * d.dot(tangent)
	if t.length_squared() < 1e-10:
		return d
	t = t.normalized()
	var angle := metres / maxf(cfg.planet_radius, 1.0)
	return (d * cos(angle) + t * sin(angle)).normalized()

func _channel_points(a_dir: Vector3, b_dir: Vector3, a_alt: float, b_alt: float,
		count: int, jitter_m: float) -> Array:
	var out: Array = []
	for i in count:
		var t := float(i) / float(maxi(count - 1, 1))
		# Nlerp is stable for the <=120 km arcs used here and follows the sphere once
		# renormalised. Endpoints are kept exact so ground strikes really terminate.
		var d := (a_dir * (1.0 - t) + b_dir * t).normalized()
		var alt := lerpf(a_alt, b_alt, t)
		if i > 0 and i < count - 1:
			var frame: Array = CubeSphere.tangent_basis(d)
			var envelope := sin(PI * t)
			var jx := _lightning_rng.randf_range(-jitter_m, jitter_m) * envelope
			var jy := _lightning_rng.randf_range(-jitter_m, jitter_m) * envelope
			d = (d + (frame[0] as Vector3) * (jx / cfg.planet_radius)
				+ (frame[1] as Vector3) * (jy / cfg.planet_radius)).normalized()
			alt += _lightning_rng.randf_range(-jitter_m * 0.38, jitter_m * 0.38) * envelope
		out.append(d * (cfg.planet_radius + alt))
	return out

func _append_channel(points: Array, strength: float, segments: Array) -> void:
	for i in range(points.size() - 1):
		segments.append({"a": points[i], "b": points[i + 1], "strength": strength})

func _bolt_pulse(bolt: Dictionary) -> float:
	var age: float = float(bolt["age"])
	var life: float = maxf(float(bolt["life"]), 0.001)
	var fade := clampf(1.0 - age / life, 0.0, 1.0)
	# Fast leader flash + two weaker return strokes.
	var p0 := exp(-age * 42.0)
	var p1 := exp(-absf(age - 0.055) * 72.0) * 0.72
	var p2 := exp(-absf(age - 0.115) * 85.0) * 0.42
	var flicker := 0.76 + 0.24 * absf(sin(age * 115.0 + float(bolt["seed"])))
	return maxf(p0, maxf(p1, p2)) * fade * flicker

func _push_lightning_lights() -> void:
	var active: Array = []
	for item in _bolts:
		var bolt: Dictionary = item
		var pulse := _bolt_pulse(bolt)
		if pulse > 0.002:
			active.append({"bolt": bolt, "score": pulse * float(bolt["energy"])})
	active.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return float(a["score"]) > float(b["score"]))
	for i in 4:
		var value := Vector4.ZERO
		if i < active.size():
			var bolt: Dictionary = active[i]["bolt"]
			var p: Vector3 = bolt["flash"]
			value = Vector4(p.x, p.y, p.z, float(active[i]["score"]))
		cloud_material.set_shader_parameter("u_lightning%d" % i, value)

func _rebuild_lightning_mesh(cam: Camera3D) -> void:
	if _lightning_mesh == null:
		return
	_lightning_mesh.clear_surfaces()
	if _bolts.is_empty() or cam == null:
		return
	_lightning_mesh.surface_begin(Mesh.PRIMITIVE_TRIANGLES, _lightning_material)
	var camera_pos := cam.global_position
	for item in _bolts:
		var bolt: Dictionary = item
		var pulse := _bolt_pulse(bolt)
		if pulse < 0.002:
			continue
		var base_width: float = float(bolt["width"])
		for seg_item in bolt["segments"]:
			var seg: Dictionary = seg_item
			var a_world: Vector3 = seg["a"]
			var b_world: Vector3 = seg["b"]
			var a := Frames.to_render(Vec3D.from_v3(a_world))
			var b := Frames.to_render(Vec3D.from_v3(b_world))
			var strength: float = float(seg["strength"])
			# Wide faint halo then hot core. The deliberately exaggerated metre width
			# is optical glow, not the physical plasma-channel diameter.
			_emit_ribbon(a, b, camera_pos, base_width * 4.5,
				Color(0.22, 0.42, 1.0, 0.13 * pulse * strength))
			_emit_ribbon(a, b, camera_pos, base_width,
				Color(0.78, 0.90, 1.0, 0.92 * pulse * strength))
	_lightning_mesh.surface_end()

func _emit_ribbon(a: Vector3, b: Vector3, camera_pos: Vector3, half_width: float,
		color: Color) -> void:
	var axis := b - a
	if axis.length_squared() < 1e-8:
		return
	var mid := (a + b) * 0.5
	var to_cam := camera_pos - mid
	var side := axis.cross(to_cam)
	if side.length_squared() < 1e-8:
		side = axis.cross(Vector3.UP)
	if side.length_squared() < 1e-8:
		side = axis.cross(Vector3.RIGHT)
	side = side.normalized() * half_width
	var a0 := a - side
	var a1 := a + side
	var b0 := b - side
	var b1 := b + side
	_lightning_mesh.surface_set_color(color)
	_lightning_mesh.surface_add_vertex(a0)
	_lightning_mesh.surface_add_vertex(b0)
	_lightning_mesh.surface_add_vertex(b1)
	_lightning_mesh.surface_add_vertex(a0)
	_lightning_mesh.surface_add_vertex(b1)
	_lightning_mesh.surface_add_vertex(a1)
