class_name WeatherFXSystem
extends Node
## Near-camera weather effects driven by the same local WeatherSystem texture used
## by volumetric clouds. No EVE art/audio is shipped: all particles and droplets are
## procedural and all state comes from Asterra's native meteorology.

const SAMPLE_INTERVAL := 0.20
const RAIN_FALL_SPEED := 12.0
const SNOW_FALL_SPEED := 0.75
const THUNDER_MAX_HZ := 0.05
const WET_ACCUM_RATE := 0.085
const WET_DRY_RATE := 0.0045
const DROPLET_SHADER := preload("res://shaders/weather_droplets.gdshader")

var _observer: AsterraPlayer
var _fx_root: Node3D
var _rain: GPUParticles3D
var _snow: GPUParticles3D
var _lightning: OmniLight3D
var _canvas: CanvasLayer
var _droplet_rect: ColorRect
var _droplet_material: ShaderMaterial
var _rng := RandomNumberGenerator.new()
var _sample_accum := 999.0
var _clock := 0.0
var _cloud := 0.0
var _storm := 0.0
var _precip := 0.0
var _pressure := 0.5
var _wetness := 0.0
var _flash := 0.0
var _observer_speed_mps := 0.0
var _last_observer_planet_pos := Vector3.ZERO
var _have_observer_motion_sample := false


func _ready() -> void:
	var world := load("res://world.tres")
	_rng.seed = int(world.world_seed) ^ 0x4C494748544E494E if world is GenConfig else 0x41535445525241
	set_process(true)


func _process(delta: float) -> void:
	_clock += delta
	_sample_accum += delta
	_try_bind_observer()
	if _observer == null or not is_instance_valid(_observer):
		return
	_ensure_fx_nodes()
	_update_observer_motion(delta)
	if _sample_accum >= SAMPLE_INTERVAL:
		_sample_accum = fmod(_sample_accum, SAMPLE_INTERVAL)
		_sample_weather_center()

	var planet_pos := _planet_position()
	var altitude := maxf(planet_pos.length() - _planet_radius(), 0.0)
	var polar := clampf((absf(WeatherSystem.local_center.y) - 0.63) / 0.25, 0.0, 1.0)
	var mountain := clampf((altitude - 1800.0) / 2600.0, 0.0, 1.0)
	var snow_fraction := clampf(maxf(polar, mountain), 0.0, 1.0)
	var fx_precip := smoothstep(0.035, 0.55, _precip)

	_update_precipitation(fx_precip, snow_fraction)
	_update_wetness(delta, fx_precip)
	_update_lightning(delta, altitude)
	_update_droplets()
	_sync_surface_wetness()


func _try_bind_observer() -> void:
	if _observer != null and is_instance_valid(_observer):
		return
	var scene := get_tree().current_scene
	if scene == null:
		return
	_observer = _find_player(scene)
	if _observer != null:
		_have_observer_motion_sample = false
		_observer_speed_mps = 0.0
		_rebuild_fx_root(scene)


func _find_player(node: Node) -> AsterraPlayer:
	if node is AsterraPlayer:
		return node as AsterraPlayer
	for child: Node in node.get_children():
		var found := _find_player(child)
		if found != null:
			return found
	return null


func _rebuild_fx_root(scene: Node) -> void:
	if _fx_root != null and is_instance_valid(_fx_root):
		_fx_root.queue_free()
	_fx_root = Node3D.new()
	_fx_root.name = "WeatherFXRuntime"
	scene.add_child(_fx_root)
	_rain = _make_precip(false)
	_snow = _make_precip(true)
	_fx_root.add_child(_rain)
	_fx_root.add_child(_snow)
	_lightning = OmniLight3D.new()
	_lightning.name = "WeatherLightningFlash"
	_lightning.light_energy = 0.0
	_lightning.omni_range = 1800.0
	_lightning.shadow_enabled = false
	_fx_root.add_child(_lightning)
	_create_droplet_overlay(scene)


func _ensure_fx_nodes() -> void:
	if _fx_root == null or not is_instance_valid(_fx_root):
		var scene := get_tree().current_scene
		if scene != null:
			_rebuild_fx_root(scene)
	if _fx_root != null:
		_fx_root.global_position = _observer.global_position


func _update_observer_motion(delta: float) -> void:
	var current := _planet_position()
	if not _have_observer_motion_sample:
		_last_observer_planet_pos = current
		_have_observer_motion_sample = true
		_observer_speed_mps = 0.0
		return
	var dt := maxf(delta, 1.0e-4)
	var instantaneous := clampf((current - _last_observer_planet_pos).length() / dt, 0.0, 500.0)
	var smoothing := 1.0 - exp(-dt * 10.0)
	_observer_speed_mps = lerpf(_observer_speed_mps, instantaneous, smoothing)
	_last_observer_planet_pos = current


func _make_precip(snow: bool) -> GPUParticles3D:
	var particles := GPUParticles3D.new()
	particles.name = "Snow" if snow else "Rain"
	particles.amount = 9000 if snow else 12000
	particles.amount_ratio = 0.0
	particles.lifetime = 7.0 if snow else 3.2
	particles.local_coords = false
	particles.visibility_aabb = AABB(Vector3(-55.0, -45.0, -55.0), Vector3(110.0, 90.0, 110.0))
	particles.emitting = true

	var pm := ParticleProcessMaterial.new()
	pm.emission_shape = ParticleProcessMaterial.EMISSION_SHAPE_BOX
	pm.emission_box_extents = Vector3(38.0, 22.0, 38.0)
	pm.direction = Vector3(0.0, -1.0, 0.0)
	pm.spread = 18.0 if snow else 5.0
	pm.initial_velocity_min = SNOW_FALL_SPEED if snow else RAIN_FALL_SPEED
	pm.initial_velocity_max = 1.35 if snow else 15.0
	pm.gravity = Vector3(0.0, -0.35, 0.0) if snow else Vector3.ZERO
	pm.scale_min = 0.55 if snow else 0.75
	pm.scale_max = 1.25
	particles.process_material = pm

	var quad := QuadMesh.new()
	quad.size = Vector2(0.085, 0.085) if snow else Vector2(0.028, 1.25)
	var mat := StandardMaterial3D.new()
	mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	mat.albedo_color = Color(0.92, 0.96, 1.0, 0.72) if snow else Color(0.72, 0.82, 0.94, 0.48)
	quad.material = mat
	particles.draw_pass_1 = quad
	return particles


func _create_droplet_overlay(scene: Node) -> void:
	if _canvas != null and is_instance_valid(_canvas):
		_canvas.queue_free()
	_canvas = CanvasLayer.new()
	_canvas.name = "WeatherDroplets"
	_canvas.layer = 90
	scene.add_child(_canvas)
	_droplet_rect = ColorRect.new()
	_droplet_rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_droplet_rect.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_droplet_material = ShaderMaterial.new()
	_droplet_material.shader = DROPLET_SHADER
	_droplet_rect.material = _droplet_material
	_canvas.add_child(_droplet_rect)


func _sample_weather_center() -> void:
	if WeatherSystem.local_weather_texture == null:
		return
	var image := WeatherSystem.local_weather_texture.get_image()
	if image == null or image.is_empty():
		return
	var c := image.get_pixel(image.get_width() / 2, image.get_height() / 2)
	_cloud = clampf(c.r, 0.0, 1.0)
	_storm = clampf(c.g, 0.0, 1.0)
	_precip = clampf(c.b, 0.0, 1.0)
	_pressure = clampf(c.a, 0.0, 1.0)


func _update_precipitation(intensity: float, snow_fraction: float) -> void:
	if _rain == null or _snow == null:
		return
	_rain.amount_ratio = intensity * (1.0 - snow_fraction)
	_snow.amount_ratio = intensity * snow_fraction
	var up := WeatherSystem.local_center.normalized()
	var wind_hint := Vector3(11.0, 0.0, 4.5)
	_rain.position = up * 18.0 - wind_hint * 0.20
	_snow.position = up * 14.0 - wind_hint * 0.45


func _update_wetness(delta: float, rain_intensity: float) -> void:
	var cloud_gate := smoothstep(0.12, 0.55, _cloud)
	var target_rain := rain_intensity * cloud_gate
	if target_rain > 0.001:
		_wetness = minf(1.0, _wetness + target_rain * WET_ACCUM_RATE * delta)
	else:
		_wetness = maxf(0.0, _wetness - WET_DRY_RATE * delta)


func _update_lightning(delta: float, altitude: float) -> void:
	_flash = maxf(0.0, _flash - delta * 6.5)
	if altitude < 18000.0 and _storm > 0.58 and _precip > 0.08:
		var rate := THUNDER_MAX_HZ * pow(_storm, 2.4) * smoothstep(0.08, 0.55, _precip)
		if _rng.randf() < rate * delta:
			_flash = 1.0
			if _lightning != null:
				var horizontal := Vector3(_rng.randf_range(-1.0, 1.0), 0.0,
					_rng.randf_range(-1.0, 1.0)).normalized() * _rng.randf_range(120.0, 700.0)
				_lightning.global_position = _observer.global_position \
					+ WeatherSystem.local_center.normalized() * _rng.randf_range(250.0, 900.0) + horizontal
	if _lightning != null:
		_lightning.light_energy = _flash * 18.0


func _update_droplets() -> void:
	if _droplet_material == null:
		return
	_droplet_material.set_shader_parameter("u_rain", _precip)
	_droplet_material.set_shader_parameter("u_wetness", _wetness)
	_droplet_material.set_shader_parameter("u_speed", _observer_speed_mps)
	_droplet_material.set_shader_parameter("u_lightning_flash", _flash)
	_droplet_material.set_shader_parameter("u_time", _clock)
	if _droplet_rect != null:
		_droplet_rect.visible = _wetness > 0.004 or _flash > 0.001


func _sync_surface_wetness() -> void:
	var ground := get_node_or_null("/root/GroundGeometryClipmap")
	if ground == null:
		return
	var value: Variant = ground.get("_material")
	if value is ShaderMaterial:
		(value as ShaderMaterial).set_shader_parameter("u_weather_surface_wetness", _wetness)


func _planet_position() -> Vector3:
	var o := Frames.origin
	return _observer.global_position + Vector3(float(o.x), float(o.y), float(o.z))


func _planet_radius() -> float:
	if Planet.ready_state and Planet.cfg != null:
		return Planet.cfg.planet_radius
	return 1000000.0


static func smoothstep(a: float, b: float, x: float) -> float:
	var t := clampf((x - a) / maxf(b - a, 1.0e-6), 0.0, 1.0)
	return t * t * (3.0 - 2.0 * t)
