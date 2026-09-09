extends VolumetricCloudController
## Weather-aware specialization of Asterra's production volumetric cloud renderer.
## WeatherSystem owns cloud placement; this controller binds the canonical density
## field, light volume, surface shadows, quality budgets and volumetric lightning.

const WEATHER_FX_SCRIPT := preload("res://scripts/weather/weather_fx_system.gd")
const CLOUD_NOISE_GENERATOR := preload("res://scripts/rendering/cloud_noise_generator.gd")
const WEATHER_CLOUD_BASE_M := 800.0
const WEATHER_CLOUD_TOP_M := 14500.0
const FALLBACK_CLOUD_COVERAGE := 0.28
const EVE_SHAPE_SCALE := 0.000052
const EVE_DETAIL_SCALE := 0.00042
const EVE_DETAIL_EROSION := 0.50
const EVE_EXTINCTION := 0.0010
const EVE_UPWARD_SPEED_MPS := 5.0

var _weather_fx: WeatherFXSystem


func _ready() -> void:
	super._ready()
	_weather_fx = WEATHER_FX_SCRIPT.new() as WeatherFXSystem
	if _weather_fx != null:
		_weather_fx.name = "WeatherFXSystem"
		add_child(_weather_fx)


func configure(material: ShaderMaterial, world_seed: int, quality: int) -> void:
	super.configure(material, world_seed, quality)
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
	_shape_texture = CLOUD_NOISE_GENERATOR.create_shape_volume(_world_seed)
	_detail_texture = CLOUD_NOISE_GENERATOR.create_detail_volume(_world_seed)


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
	if _weather_fx != null:
		var lightning_texture := _weather_fx.get_lightning_event_texture()
		if lightning_texture != null:
			_depth_effect.set_lightning_texture(lightning_texture)


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


# Production budgets. Empty-air probes are counted separately in the compute
# shader, so these values represent actual occupied-cloud integration work.
func _compositor_steps(quality: int) -> int:
	match GraphicsQuality.sanitize(quality):
		GraphicsQuality.Preset.PERFORMANCE:
			return 20
		GraphicsQuality.Preset.BALANCED:
			return 32
		GraphicsQuality.Preset.ULTRA:
			return 72
		_:
			return 48


func _primary_steps(quality: int) -> int:
	match GraphicsQuality.sanitize(quality):
		GraphicsQuality.Preset.PERFORMANCE:
			return 24
		GraphicsQuality.Preset.BALANCED:
			return 36
		GraphicsQuality.Preset.ULTRA:
			return 64
		_:
			return 48


func _light_steps(quality: int) -> int:
	match GraphicsQuality.sanitize(quality):
		GraphicsQuality.Preset.PERFORMANCE:
			return 3
		GraphicsQuality.Preset.BALANCED:
			return 5
		GraphicsQuality.Preset.ULTRA:
			return 8
		_:
			return 6


func _shadow_steps(quality: int) -> int:
	match GraphicsQuality.sanitize(quality):
		GraphicsQuality.Preset.PERFORMANCE:
			return 4
		GraphicsQuality.Preset.BALANCED:
			return 6
		GraphicsQuality.Preset.ULTRA:
			return 12
		_:
			return 8


func _shadow_disc_samples(quality: int) -> int:
	match GraphicsQuality.sanitize(quality):
		GraphicsQuality.Preset.PERFORMANCE:
			return 1
		GraphicsQuality.Preset.BALANCED:
			return 3
		GraphicsQuality.Preset.ULTRA:
			return 7
		_:
			return 5
