class_name VolumetricCloudController
extends Node
## Runtime owner for Asterra's sky cloud volumes and their slowly advected weather
## frame. The shader performs the actual spherical raymarch.
##
## The same NoiseTexture3D resources are also bound to terrain/water shadow
## receivers. Visible clouds and their ground shadows therefore sample one physical
## density field instead of two look-alike procedural patterns that can drift apart.

const WIND_UPDATE_INTERVAL := 0.10
const WIND_METRES_PER_SECOND := Vector3(11.0, 0.0, 4.5)
const ATMOSPHERE_SHADER_PATH := "res://shaders/atmosphere_sky.gdshader"

const CLOUD_BASE_M := 1100.0
const CLOUD_TOP_M := 6400.0
const CLOUD_COVERAGE := 0.52
const CLOUD_WEATHER_VARIATION := 0.30
const CLOUD_DENSITY := 1.0
const CLOUD_SHAPE_SCALE := 0.000055
const CLOUD_DETAIL_SCALE := 0.00042
const CLOUD_WEATHER_SCALE := 0.0000085
const CLOUD_DETAIL_STRENGTH := 0.36
const CLOUD_EXTINCTION := 0.00105

var _material: ShaderMaterial
var _shape_texture: NoiseTexture3D
var _detail_texture: NoiseTexture3D
var _shadow_receivers: Array[WeakRef] = []
var _wind_offset := Vector3.ZERO
var _wind_accumulator := 0.0
var _world_seed := 0x4153544552524100
var _quality := GraphicsQuality.Preset.HIGH


func _ready() -> void:
	var world_resource := load("res://world.tres")
	if world_resource is GenConfig:
		_world_seed = world_resource.world_seed
	_quality = GraphicsQuality.sanitize(AppSettings.graphics_quality)
	get_tree().node_added.connect(_on_node_added)
	call_deferred("_scan_existing_environments")


func _on_node_added(node: Node) -> void:
	if node is WorldEnvironment:
		call_deferred("_try_bind_environment", node)


func _scan_existing_environments() -> void:
	# Dynamically created WorldEnvironment nodes are normally caught by node_added.
	# The recursive fallback handles a scene that already contained one before this
	# autoload's first deferred frame.
	_scan_node(get_tree().root)


func _scan_node(node: Node) -> void:
	if node is WorldEnvironment:
		_try_bind_environment(node)
	for child in node.get_children():
		_scan_node(child)


func _try_bind_environment(world_environment: WorldEnvironment) -> void:
	if world_environment == null or world_environment.environment == null:
		return
	var sky := world_environment.environment.sky
	if sky == null or not (sky.sky_material is ShaderMaterial):
		return
	var material := sky.sky_material as ShaderMaterial
	if material.shader == null or material.shader.resource_path != ATMOSPHERE_SHADER_PATH:
		return
	if material == _material:
		return
	configure(material, _world_seed, AppSettings.graphics_quality)


func configure(material: ShaderMaterial, world_seed: int, quality: int) -> void:
	_material = material
	_world_seed = world_seed
	_quality = GraphicsQuality.sanitize(quality)
	_ensure_noise_volumes()
	if _material == null:
		return

	_material.set_shader_parameter("u_cloud_shape_noise", _shape_texture)
	_material.set_shader_parameter("u_cloud_detail_noise", _detail_texture)
	_material.set_shader_parameter("u_cloud_primary_steps", _primary_steps(_quality))
	_material.set_shader_parameter("u_cloud_light_steps", _light_steps(_quality))
	_material.set_shader_parameter("u_cloud_enabled", 1.0)
	_material.set_shader_parameter("u_cloud_base", CLOUD_BASE_M)
	_material.set_shader_parameter("u_cloud_top", CLOUD_TOP_M)
	_material.set_shader_parameter("u_cloud_coverage", CLOUD_COVERAGE)
	_material.set_shader_parameter("u_cloud_weather_variation", CLOUD_WEATHER_VARIATION)
	_material.set_shader_parameter("u_cloud_density", CLOUD_DENSITY)
	_material.set_shader_parameter("u_cloud_shape_scale", CLOUD_SHAPE_SCALE)
	_material.set_shader_parameter("u_cloud_detail_scale", CLOUD_DETAIL_SCALE)
	_material.set_shader_parameter("u_cloud_weather_scale", CLOUD_WEATHER_SCALE)
	_material.set_shader_parameter("u_cloud_detail_strength", CLOUD_DETAIL_STRENGTH)
	_material.set_shader_parameter("u_cloud_extinction", CLOUD_EXTINCTION)
	_material.set_shader_parameter("u_cloud_wind_offset", _wind_offset)
	_sync_all_shadow_receivers()


## Surface renderers call this once after their final ShaderMaterial has been
## installed. Registration is deliberately order-independent: GroundGeometryClipmap
## starts before this autoload in project.godot, while the WorldEnvironment is
## created later by the game scene.
func register_shadow_receiver(material: ShaderMaterial) -> void:
	if material == null:
		return
	for receiver_ref: WeakRef in _shadow_receivers:
		var existing: ShaderMaterial = receiver_ref.get_ref() as ShaderMaterial
		if existing == material:
			_sync_shadow_receiver(material)
			return
	_shadow_receivers.append(weakref(material))
	_ensure_noise_volumes()
	_sync_shadow_receiver(material)


func _ensure_noise_volumes() -> void:
	if _shape_texture != null and _detail_texture != null:
		return

	# NoiseTexture3D generates its volume on Godot worker threads. Binding the
	# resources immediately is non-blocking; they populate as synthesis completes.
	var shape_noise := FastNoiseLite.new()
	shape_noise.seed = _seed32(_world_seed, 0x43A51)
	shape_noise.noise_type = FastNoiseLite.TYPE_SIMPLEX_SMOOTH
	shape_noise.frequency = 0.034
	shape_noise.fractal_type = FastNoiseLite.FRACTAL_FBM
	shape_noise.fractal_octaves = 5
	shape_noise.fractal_gain = 0.52
	shape_noise.fractal_lacunarity = 2.03
	shape_noise.domain_warp_enabled = true
	shape_noise.domain_warp_amplitude = 9.0
	shape_noise.domain_warp_frequency = 0.022
	shape_noise.domain_warp_fractal_octaves = 3

	_shape_texture = NoiseTexture3D.new()
	_shape_texture.width = 96
	_shape_texture.height = 96
	_shape_texture.depth = 96
	_shape_texture.seamless = true
	_shape_texture.seamless_blend_skirt = 0.12
	_shape_texture.normalize = true
	_shape_texture.noise = shape_noise

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
	if material == null or _shape_texture == null:
		return
	material.set_shader_parameter("u_cloud_shadow_shape_noise", _shape_texture)
	material.set_shader_parameter("u_cloud_shadow_enabled", 1.0)
	material.set_shader_parameter("u_cloud_shadow_base", CLOUD_BASE_M)
	material.set_shader_parameter("u_cloud_shadow_top", CLOUD_TOP_M)
	material.set_shader_parameter("u_cloud_shadow_coverage", CLOUD_COVERAGE)
	material.set_shader_parameter("u_cloud_shadow_weather_variation", CLOUD_WEATHER_VARIATION)
	material.set_shader_parameter("u_cloud_shadow_density", CLOUD_DENSITY)
	material.set_shader_parameter("u_cloud_shadow_shape_scale", CLOUD_SHAPE_SCALE)
	material.set_shader_parameter("u_cloud_shadow_weather_scale", CLOUD_WEATHER_SCALE)
	material.set_shader_parameter("u_cloud_shadow_extinction", CLOUD_EXTINCTION)
	material.set_shader_parameter("u_cloud_shadow_steps", _shadow_steps(_quality))
	material.set_shader_parameter("u_cloud_shadow_wind_offset", _wind_offset)


func _sync_all_shadow_receivers() -> void:
	for i in range(_shadow_receivers.size() - 1, -1, -1):
		var material: ShaderMaterial = _shadow_receivers[i].get_ref() as ShaderMaterial
		if material == null:
			_shadow_receivers.remove_at(i)
			continue
		_sync_shadow_receiver(material)


func _process(delta: float) -> void:
	if _material == null and _shadow_receivers.is_empty():
		return
	_wind_offset += WIND_METRES_PER_SECOND * delta
	_wind_accumulator += delta
	if _wind_accumulator < WIND_UPDATE_INTERVAL:
		return
	_wind_accumulator = fmod(_wind_accumulator, WIND_UPDATE_INTERVAL)

	# Ten updates per second is visually continuous at ~12 m/s (about one metre of
	# advection per update), while avoiding per-frame sky-radiance invalidation.
	if _material != null:
		_material.set_shader_parameter("u_cloud_wind_offset", _wind_offset)
	for i in range(_shadow_receivers.size() - 1, -1, -1):
		var material: ShaderMaterial = _shadow_receivers[i].get_ref() as ShaderMaterial
		if material == null:
			_shadow_receivers.remove_at(i)
			continue
		material.set_shader_parameter("u_cloud_shadow_wind_offset", _wind_offset)


func _primary_steps(quality: int) -> int:
	match GraphicsQuality.sanitize(quality):
		GraphicsQuality.Preset.PERFORMANCE:
			return 18
		GraphicsQuality.Preset.BALANCED:
			return 28
		GraphicsQuality.Preset.ULTRA:
			return 56
		_:
			return 40


func _light_steps(quality: int) -> int:
	match GraphicsQuality.sanitize(quality):
		GraphicsQuality.Preset.PERFORMANCE:
			return 2
		GraphicsQuality.Preset.BALANCED:
			return 3
		GraphicsQuality.Preset.ULTRA:
			return 6
		_:
			return 4


func _shadow_steps(quality: int) -> int:
	match GraphicsQuality.sanitize(quality):
		GraphicsQuality.Preset.PERFORMANCE:
			return 3
		GraphicsQuality.Preset.BALANCED:
			return 4
		GraphicsQuality.Preset.ULTRA:
			return 10
		_:
			return 6


func _seed32(world_seed: int, salt: int) -> int:
	# FastNoiseLite stores a signed 32-bit seed. Fold Asterra's deterministic
	# 64-bit world seed into that range without introducing global RNG state.
	var mixed := world_seed ^ (salt * 0x45D9F3B)
	mixed = mixed ^ (mixed >> 16)
	return int(mixed & 0x7FFFFFFF)
