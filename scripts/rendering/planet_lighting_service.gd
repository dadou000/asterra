extends Node
## Shared planet-scale lighting state and solar transmittance LUT.
##
## The LUT stores atmospheric transmittance from a sample point to the top of
## the atmosphere as a function of altitude and solar zenith cosine. Sky and
## aerial-perspective shaders sample it instead of ray-marching the solar path
## for every primary sample.

const LUT_WIDTH := 256
const LUT_HEIGHT := 64
const INTEGRATION_STEPS := 40

const RAYLEIGH := Vector3(5.5e-6, 13.0e-6, 22.4e-6)
const MIE := 21.0e-6
const RAYLEIGH_SCALE_HEIGHT := 8000.0
const MIE_SCALE_HEIGHT := 1200.0
const OZONE := Vector3(0.650e-6, 1.881e-6, 0.085e-6)
const OZONE_CENTRE := 25000.0
const OZONE_WIDTH := 15000.0

const LUT_NAME := &"asterra_transmittance_lut"
const SUN_DIR_NAME := &"asterra_sun_dir"

var _lut: ImageTexture
var _last_radius := -1.0
var _last_atmosphere_height := -1.0
var _last_sun_dir := Vector3.ZERO


func _ready() -> void:
	_register_globals()
	_sync_sun_direction(true)


func _process(_delta: float) -> void:
	_sync_sun_direction(false)
	_sync_lut_if_needed()


func _register_globals() -> void:
	var names := RenderingServer.global_shader_parameter_get_list()
	if not names.has(LUT_NAME):
		var placeholder := Image.create(1, 1, false, Image.FORMAT_RGBAH)
		placeholder.set_pixel(0, 0, Color(1.0, 1.0, 1.0, 1.0))
		_lut = ImageTexture.create_from_image(placeholder)
		RenderingServer.global_shader_parameter_add(
			LUT_NAME,
			RenderingServer.GLOBAL_VAR_TYPE_SAMPLER2D,
			_lut)
	if not names.has(SUN_DIR_NAME):
		RenderingServer.global_shader_parameter_add(
			SUN_DIR_NAME,
			RenderingServer.GLOBAL_VAR_TYPE_VEC3,
			Vector3(1.0, 0.0, 0.0))


func _sync_sun_direction(force: bool) -> void:
	var d := Frames.helion_dir
	if d.length_squared() < 1.0e-12:
		return
	d = d.normalized()
	if not force and d.distance_squared_to(_last_sun_dir) <= 1.0e-12:
		return
	_last_sun_dir = d
	RenderingServer.global_shader_parameter_set(SUN_DIR_NAME, d)


func _sync_lut_if_needed() -> void:
	if not Planet.ready_state or Planet.cfg == null:
		return
	var radius: float = Planet.cfg.planet_radius
	var atmosphere_height: float = Planet.cfg.atmosphere_height
	if is_equal_approx(radius, _last_radius) \
			and is_equal_approx(atmosphere_height, _last_atmosphere_height):
		return
	_last_radius = radius
	_last_atmosphere_height = atmosphere_height
	_lut = _build_transmittance_lut(radius, atmosphere_height)
	RenderingServer.global_shader_parameter_set(LUT_NAME, _lut)


func _build_transmittance_lut(planet_radius: float, atmosphere_height: float) -> ImageTexture:
	var image := Image.create(LUT_WIDTH, LUT_HEIGHT, false, Image.FORMAT_RGBAH)
	var top := planet_radius + atmosphere_height
	for y in range(LUT_HEIGHT):
		var altitude := atmosphere_height * (float(y) + 0.5) / float(LUT_HEIGHT)
		var radius := planet_radius + altitude
		var origin := Vector3(0.0, radius, 0.0)
		for x in range(LUT_WIDTH):
			var mu := -1.0 + 2.0 * (float(x) + 0.5) / float(LUT_WIDTH)
			var tangent := sqrt(maxf(1.0 - mu * mu, 0.0))
			var direction := Vector3(tangent, mu, 0.0)
			if _ray_hits_planet(origin, direction, planet_radius):
				image.set_pixel(x, y, Color(0.0, 0.0, 0.0, 1.0))
				continue
			var path_length := _ray_exit_distance(origin, direction, top)
			if path_length <= 0.0:
				image.set_pixel(x, y, Color(1.0, 1.0, 1.0, 1.0))
				continue
			var step_len := path_length / float(INTEGRATION_STEPS)
			var od_r := 0.0
			var od_m := 0.0
			var od_o := 0.0
			for i in range(INTEGRATION_STEPS):
				var p := origin + direction * ((float(i) + 0.5) * step_len)
				var h := maxf(p.length() - planet_radius, 0.0)
				od_r += exp(-h / RAYLEIGH_SCALE_HEIGHT) * step_len
				od_m += exp(-h / MIE_SCALE_HEIGHT) * step_len
				od_o += _ozone_density(h) * step_len
			var tau := RAYLEIGH * od_r + Vector3.ONE * (MIE * od_m) + OZONE * od_o
			var tr := Vector3(exp(-tau.x), exp(-tau.y), exp(-tau.z))
			image.set_pixel(x, y, Color(tr.x, tr.y, tr.z, 1.0))
	return ImageTexture.create_from_image(image)


func _ozone_density(height: float) -> float:
	return maxf(1.0 - absf(height - OZONE_CENTRE) / OZONE_WIDTH, 0.0)


func _ray_hits_planet(origin: Vector3, direction: Vector3, radius: float) -> bool:
	var b := origin.dot(direction)
	var c := origin.length_squared() - radius * radius
	var disc := b * b - c
	if disc <= 0.0:
		return false
	var root := sqrt(disc)
	return -b - root > 1.0e-5 or -b + root > 1.0e-5


func _ray_exit_distance(origin: Vector3, direction: Vector3, radius: float) -> float:
	var b := origin.dot(direction)
	var c := origin.length_squared() - radius * radius
	var disc := b * b - c
	if disc <= 0.0:
		return 0.0
	return maxf(-b + sqrt(disc), 0.0)
