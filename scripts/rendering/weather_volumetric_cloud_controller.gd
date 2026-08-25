extends VolumetricCloudController
## Weather-aware specialization of the existing cloud controller.
## Keeps the established noise volumes, lighting, depth compositing and terrain/ocean
## registration while replacing synthetic cloud placement with WeatherSystem state.

const WEATHER_CLOUD_BASE_M := 800.0
const WEATHER_CLOUD_TOP_M := 14500.0
const FALLBACK_CLOUD_COVERAGE := 0.28


func configure(material: ShaderMaterial, world_seed: int, quality: int) -> void:
	super.configure(material, world_seed, quality)
	# The depth compositor is the normal weather renderer. If it cannot initialize,
	# keep the legacy sky fallback sparse rather than reverting to the old 52%
	# synthetic overcast-like coverage.
	if material != null:
		material.set_shader_parameter("u_cloud_coverage", FALLBACK_CLOUD_COVERAGE)
	_bind_weather_to_depth_effect()
	_sync_all_weather_receivers()


func _install_depth_compositor(world_environment: WorldEnvironment) -> void:
	super._install_depth_compositor(world_environment)
	_bind_weather_to_depth_effect()


func _sync_depth_effect() -> void:
	super._sync_depth_effect()
	_bind_weather_to_depth_effect()


func _sync_shadow_receiver(material: ShaderMaterial) -> void:
	super._sync_shadow_receiver(material)
	if material == null:
		return
	material.set_shader_parameter("u_cloud_shadow_base", WEATHER_CLOUD_BASE_M)
	material.set_shader_parameter("u_cloud_shadow_top", WEATHER_CLOUD_TOP_M)
	if WeatherSystem.global_weather_texture != null:
		material.set_shader_parameter("u_cloud_weather_global", WeatherSystem.global_weather_texture)
	if WeatherSystem.local_weather_texture != null:
		material.set_shader_parameter("u_cloud_weather_local", WeatherSystem.local_weather_texture)
	material.set_shader_parameter("u_cloud_weather_center", WeatherSystem.local_center)
	material.set_shader_parameter("u_cloud_weather_east", WeatherSystem.local_east)
	material.set_shader_parameter("u_cloud_weather_north", WeatherSystem.local_north)
	material.set_shader_parameter("u_cloud_weather_local_span", WeatherSystem.local_span_m)


func _process(delta: float) -> void:
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
