class_name WeatherFXSystem
extends Node
## Near-camera weather effects and authoritative transient lightning events.
## Cloud placement remains owned by WeatherSystem. Lightning events are exported
## as a tiny HDR texture so the volumetric cloud compositor can scatter them from
## inside the same 3D density field instead of faking a screen-space flash.

const SAMPLE_INTERVAL := 0.20
const RAIN_FALL_SPEED := 12.0
const SNOW_FALL_SPEED := 0.75
const THUNDER_MAX_HZ := 0.12
const WET_ACCUM_RATE := 0.085
const WET_DRY_RATE := 0.0045
const MAX_LIGHTNING_EVENTS := 4
const LIGHTNING_CANDIDATES := 16
const LIGHTNING_MIN_SCORE := 0.14
const DROPLET_SHADER := preload("res://shaders/weather_droplets.gdshader")

class LightningEvent:
	var position_planet := Vector3.ZERO
	var age := 0.0
	var duration := 0.50
	var peak := 1.0
	var secondary_time := 0.18

	func strength() -> float:
		var primary := exp(-pow((age - 0.025) / 0.018, 2.0))
		var secondary := 0.68 * exp(-pow((age - secondary_time) / 0.040, 2.0))
		var tail := 0.16 * exp(-age * 6.5)
		return clampf((maxf(primary, secondary) + tail) * peak, 0.0, 1.0)


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
var _storm_activity := 0.0
var _wetness := 0.0
var _flash := 0.0
var _observer_speed_mps := 0.0
var _last_observer_planet_pos := Vector3.ZERO
var _have_observer_motion_sample := false
var _weather_image: Image
var _lightning_events: Array[LightningEvent] = []
var _lightning_event_image: Image
var _lightning_event_texture: ImageTexture


func _ready() -> void:
	var world := load("res://world.tres")
	_rng.seed = int(world.world_seed) ^ 0x4C494748544E494E if world is GenConfig else 0x41535445525241
	_create_lightning_event_texture()
	set_process(true)


func _process(delta: float) -> void:
	_clock += delta
	_sample_accum += delta
	_try_bind_observer()
	if _observer == null or not is_instance_valid(_observer):
		_update_lightning_events(delta)
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
	_update_lightning_events(delta)
	_update_droplets()
	_sync_surface_wetness()


func get_lightning_event_texture() -> Texture2D:
	return _lightning_event_texture


func _create_lightning_event_texture() -> void:
	_lightning_event_image = Image.create(MAX_LIGHTNING_EVENTS, 1, false, Image.FORMAT_RGBAF)
	_lightning_event_image.fill(Color(0.0, 0.0, 0.0, 0.0))
	_lightning_event_texture = ImageTexture.create_from_image(_lightning_event_image)


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
	_lightning.omni_range = 6000.0
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
	_weather_image = image
	var c := image.get_pixel(image.get_width() / 2, image.get_height() / 2)
	_cloud = clampf(c.r, 0.0, 1.0)
	_storm = clampf(c.g, 0.0, 1.0)
	_precip = clampf(c.b, 0.0, 1.0)
	_pressure = clampf(c.a, 0.0, 1.0)

	_storm_activity = 0.0
	var sx := maxi(image.get_width() / 12, 1)
	var sy := maxi(image.get_height() / 12, 1)
	for y in range(0, image.get_height(), sy):
		for x in range(0, image.get_width(), sx):
			var wx := image.get_pixel(x, y)
			var score := pow(clampf(wx.g, 0.0, 1.0), 1.7) \
				* smoothstep(0.035, 0.50, clampf(wx.b, 0.0, 1.0))
			_storm_activity = maxf(_storm_activity, score)


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


func _update_lightning_events(delta: float) -> void:
	for i in range(_lightning_events.size() - 1, -1, -1):
		var event := _lightning_events[i]
		event.age += delta
		if event.age >= event.duration:
			_lightning_events.remove_at(i)

	if _weather_image != null and not _weather_image.is_empty() \
			and _lightning_events.size() < MAX_LIGHTNING_EVENTS:
		var activity := maxf(_storm_activity,
			pow(_storm, 1.7) * smoothstep(0.035, 0.50, _precip))
		var rate := THUNDER_MAX_HZ * activity * activity
		if _rng.randf() < rate * delta:
			_spawn_lightning_event()

	_flash = 0.0
	for event in _lightning_events:
		_flash = maxf(_flash, event.strength())
	_update_lightning_event_texture()
	_update_local_lightning_omni()


func _spawn_lightning_event() -> void:
	if _weather_image == null or _weather_image.is_empty():
		return
	var best_score := 0.0
	var best_uv := Vector2(0.5, 0.5)
	var best_weather := Color(_cloud, _storm, _precip, _pressure)
	var w := _weather_image.get_width()
	var h := _weather_image.get_height()
	for _i in range(LIGHTNING_CANDIDATES):
		var uv := Vector2(_rng.randf_range(0.06, 0.94), _rng.randf_range(0.06, 0.94))
		var px := clampi(int(uv.x * float(w - 1)), 0, w - 1)
		var py := clampi(int(uv.y * float(h - 1)), 0, h - 1)
		var wx := _weather_image.get_pixel(px, py)
		var score := pow(clampf(wx.g, 0.0, 1.0), 1.7) \
			* smoothstep(0.035, 0.50, clampf(wx.b, 0.0, 1.0))
		if score > best_score:
			best_score = score
			best_uv = uv
			best_weather = wx
	if best_score < LIGHTNING_MIN_SCORE:
		return

	var radius := _planet_radius()
	var span := maxf(WeatherSystem.local_span_m, 1000.0)
	var center := WeatherSystem.local_center.normalized()
	var east := WeatherSystem.local_east.normalized()
	var north := WeatherSystem.local_north.normalized()
	var direction := (center
		+ east * ((best_uv.x - 0.5) * span / radius)
		+ north * ((best_uv.y - 0.5) * span / radius)).normalized()

	var coverage := clampf(best_weather.r, 0.0, 1.0)
	var storm := clampf(best_weather.g, 0.0, 1.0)
	var precip := clampf(best_weather.b, 0.0, 1.0)
	var low_pressure := clampf((0.5 - best_weather.a) * 3.0, 0.0, 1.0)
	var convection := smoothstep(0.12, 0.86,
		maxf(storm, precip * 0.76 + low_pressure * 0.20))
	var base_alt := lerpf(1450.0, 720.0, convection) - low_pressure * 90.0
	var fair_top := lerpf(3500.0, 6200.0, smoothstep(0.16, 0.82, coverage))
	var top_alt := lerpf(fair_top, 14500.0, pow(convection, 0.68))
	var lower := base_alt + 350.0
	var upper := maxf(lower + 400.0, lerpf(base_alt, top_alt, 0.72))
	var event_alt := lerpf(lower, upper, _rng.randf_range(0.30, 0.72))

	var event := LightningEvent.new()
	event.position_planet = direction * (radius + event_alt)
	event.duration = _rng.randf_range(0.42, 0.68)
	event.peak = _rng.randf_range(0.72, 1.0) * smoothstep(0.45, 0.95, storm)
	event.secondary_time = _rng.randf_range(0.13, 0.27)
	_lightning_events.append(event)


func _update_lightning_event_texture() -> void:
	if _lightning_event_image == null or _lightning_event_texture == null:
		return
	for i in range(MAX_LIGHTNING_EVENTS):
		if i < _lightning_events.size():
			var event := _lightning_events[i]
			var p := event.position_planet
			_lightning_event_image.set_pixel(i, 0, Color(p.x, p.y, p.z, event.strength()))
		else:
			_lightning_event_image.set_pixel(i, 0, Color(0.0, 0.0, 0.0, 0.0))
	_lightning_event_texture.update(_lightning_event_image)


func _update_local_lightning_omni() -> void:
	if _lightning == null or _observer == null:
		return
	var observer_planet := _planet_position()
	var best_event: LightningEvent
	var best_score := 0.0
	for event in _lightning_events:
		var strength := event.strength()
		var distance := (event.position_planet - observer_planet).length()
		if distance > 18000.0:
			continue
		var score := strength / (1.0 + distance / 3500.0)
		if score > best_score:
			best_score = score
			best_event = event
	if best_event == null:
		_lightning.light_energy = 0.0
		return
	var strength := best_event.strength()
	var origin := Frames.origin
	var origin_v3 := Vector3(float(origin.x), float(origin.y), float(origin.z))
	_lightning.global_position = best_event.position_planet - origin_v3
	_lightning.light_energy = strength * 22.0
	_lightning.omni_range = lerpf(2600.0, 8200.0, sqrt(strength))


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
