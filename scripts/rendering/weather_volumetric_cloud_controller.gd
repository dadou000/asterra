extends VolumetricCloudController
## Weather-aware specialization of the existing cloud controller.
##
## The normal visible path is the resolved-depth compositor. WeatherSystem owns
## horizontal cloud placement; this class supplies the Kerbin/EVE-inspired
## procedural Worley volumes and keeps cloud, surface-shadow and fallback-sky
## coordinates synchronized.

const WEATHER_CLOUD_BASE_M := 800.0
const WEATHER_CLOUD_TOP_M := 14500.0
const FALLBACK_CLOUD_COVERAGE := 0.28

# Adapted from the Kerbin base-layer tuning. Horizontal dimensions are enlarged
# by Asterra/Kerbin radius (1000/600) while vertical heights remain terrestrial.
const EVE_SHAPE_SCALE := 0.000052
const EVE_DETAIL_SCALE := 0.00042
const EVE_DETAIL_EROSION := 0.65
const EVE_EXTINCTION := 0.0010
const EVE_UPWARD_SPEED_MPS := 5.0


func configure(material: ShaderMaterial, world_seed: int, quality: int) -> void:
	super.configure(material, world_seed, quality)
	# The depth compositor is the normal weather renderer. If it cannot initialize,
	# keep an EVE-tuned procedural fallback rather than the older synthetic preset.
	if material != null:
		material.set_shader_parameter("u_cloud_base", WEATHER_CLOUD_BASE_M)
		material.set_shader_parameter("u_cloud_top", WEATHER_CLOUD_TOP_M)
		material.set_shader_parameter("u_cloud_coverage", FALLBACK_CLOUD_COVERAGE)
		material.set_shader_parameter("u_cloud_density", 1.0)
		material.set_shader_parameter("u_cloud_shape_scale", EVE_SHAPE_SCALE)
		material.set_shader_parameter("u_cloud_detail_scale", EVE_DETAIL_SCALE)
		material.set_shader_parameter("u_cloud_detail_strength", EVE_DETAIL_EROSION)
		material.set_shader_parameter("u_cloud_extinction", EVE_EXTINCTION)
	_bind_weather_to_depth_effect()
	_sync_all_weather_receivers()


func _install_depth_compositor(world_environment: WorldEnvironment) -> void:
	super._install_depth_compositor(world_environment)
	_bind_weather_to_depth_effect()


func _sync_depth_effect() -> void:
	super._sync_depth_effect()
	_bind_weather_to_depth_effect()


func _ensure_noise_volumes() -> void:
	if _shape_texture != null and _detail_texture != null:
		return

	# EVE's base mass is spherical Worley fBm. Godot generates one seamless
	# cellular-distance primitive here; the shaders invert and combine two octaves
	# explicitly with Kerbin's 0.57 persistence. This keeps the resource generic and
	# deterministic rather than shipping any reference-pack texture.
	var shape_noise := FastNoiseLite.new()
	shape_noise.seed = _seed32(_world_seed, 0x43A51)
	shape_noise.noise_type = FastNoiseLite.TYPE_CELLULAR
	shape_noise.frequency = 0.055
	shape_noise.fractal_type = FastNoiseLite.FRACTAL_NONE
	shape_noise.cellular_distance_function = FastNoiseLite.DISTANCE_EUCLIDEAN
	shape_noise.cellular_return_type = FastNoiseLite.RETURN_DISTANCE
	shape_noise.cellular_jitter = 1.0
	shape_noise.domain_warp_enabled = false

	_shape_texture = NoiseTexture3D.new()
	_shape_texture.width = 96
	_shape_texture.height = 96
	_shape_texture.depth = 96
	_shape_texture.seamless = true
	_shape_texture.seamless_blend_skirt = 0.12
	_shape_texture.normalize = true
	_shape_texture.noise = shape_noise

	# The high-frequency resource is only an erosion primitive. The cloud shaders
	# apply the Kerbin erosionDepth=0.65 at the density boundary, so dense interiors
	# remain solid while only fringes become ragged.
	var detail_noise := FastNoiseLite.new()
	detail_noise.seed = _seed32(_world_seed, 0x7D19B)
	detail_noise.noise_type = FastNoiseLite.TYPE_SIMPLEX_SMOOTH
	detail_noise.frequency = 0.085
	detail_noise.fractal_type = FastNoiseLite.FRACTAL_RIDGED
	detail_noise.fractal_octaves = 4
	detail_noise.fractal_gain = 0.53
	detail_noise.fractal_lacunarity = 2.11

	_detail_texture = NoiseTexture3D.new()
	_detail_texture.width = 64
	_detail_texture.height = 64
	_detail_texture.depth = 64
	_detail_texture.seamless = true
	_detail_texture.seamless_blend_skirt = 0.14
	_detail_texture.normalize = true
	_detail_texture.noise = detail_noise


func _sync_shadow_receiver(material: ShaderMaterial) -> void:
	super._sync_shadow_receiver(material)
	if material == null:
		return
	material.set_shader_parameter("u_cloud_shadow_base", WEATHER_CLOUD_BASE_M)
	material.set_shader_parameter("u_cloud_shadow_top", WEATHER_CLOUD_TOP_M)
	material.set_shader_parameter("u_cloud_shadow_density", 1.0)
	material.set_shader_parameter("u_cloud_shadow_shape_scale", EVE_SHAPE_SCALE)
	material.set_shader_parameter("u_cloud_shadow_extinction", EVE_EXTINCTION)
	if WeatherSystem.global_weather_texture != null:
		material.set_shader_parameter("u_cloud_weather_global", WeatherSystem.global_weather_texture)
	if WeatherSystem.local_weather_texture != null:
		material.set_shader_parameter("u_cloud_weather_local", WeatherSystem.local_weather_texture)
	material.set_shader_parameter("u_cloud_weather_center", WeatherSystem.local_center)
	material.set_shader_parameter("u_cloud_weather_east", WeatherSystem.local_east)
	material.set_shader_parameter("u_cloud_weather_north", WeatherSystem.local_north)
	material.set_shader_parameter("u_cloud_weather_local_span", WeatherSystem.local_span_m)


func _process(delta: float) -> void:
	# Kerbin base-layer upwardsCloudSpeed = 5 m/s. Add that development coordinate
	# before the parent synchronizes compositor and shadow uniforms this frame.
	_wind_offset.y += EVE_UPWARD_SPEED_MPS * delta
	super._process(delta)
	_bind_weather_to_depth_effect()
	_sync_all_weather_receivers()


func _bind_weather_to_depth_effect() -> void:
	if _depth_effect == null:
		return
	if WeatherSystem.global_weather_texture == null or WeatherSystem.local_weather_texture == null:
		return
	_depth_effect.set_weather_textures(
		WeatherSystem.global_weather_texture,
		WeatherSystem.local_weather_texture)
	_depth_effect.set_weather_basis(
		WeatherSystem.local_center,
		WeatherSystem.local_east,
		WeatherSystem.local_north,
		WeatherSystem.local_span_m)


func _sync_all_weather_receivers() -> void:
	for i in range(_shadow_receivers.size() - 1, -1, -1):
		var material: ShaderMaterial = _shadow_receivers[i].get_ref() as ShaderMaterial
		if material == null:
			_shadow_receivers.remove_at(i)
			continue
		material.set_shader_parameter("u_cloud_weather_center", WeatherSystem.local_center)
		material.set_shader_parameter("u_cloud_weather_east", WeatherSystem.local_east)
		material.set_shader_parameter("u_cloud_weather_north", WeatherSystem.local_north)
		material.set_shader_parameter("u_cloud_weather_local_span", WeatherSystem.local_span_m)
		if WeatherSystem.global_weather_texture != null:
			material.set_shader_parameter("u_cloud_weather_global", WeatherSystem.global_weather_texture)
		if WeatherSystem.local_weather_texture != null:
			material.set_shader_parameter("u_cloud_weather_local", WeatherSystem.local_weather_texture)
