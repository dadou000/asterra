class_name VolumetricCloudController
extends Node
## Runtime owner for Asterra's sky cloud volumes and their slowly advected weather
## frame. The shader performs the actual spherical raymarch.
##
## This is autoloaded so the dynamic Phase-1 WorldEnvironment does not need cloud-
## specific setup code. It only binds environments that use atmosphere_sky.gdshader;
## character/editor environments are left untouched.

const WIND_UPDATE_INTERVAL := 0.10
const WIND_METRES_PER_SECOND := Vector3(11.0, 0.0, 4.5)
const ATMOSPHERE_SHADER_PATH := "res://shaders/atmosphere_sky.gdshader"

var _material: ShaderMaterial
var _wind_offset := Vector3.ZERO
var _wind_accumulator := 0.0
var _world_seed := 0x4153544552524100


func _ready() -> void:
	var world_resource := load("res://world.tres")
	if world_resource is GenConfig:
		_world_seed = world_resource.world_seed
	get_tree().node_added.connect(_on_node_added)
	call_deferred("_scan_existing_environments")


func _on_node_added(node: Node) -> void:
	if node is WorldEnvironment:
		call_deferred("_try_bind_environment", node)


func _scan_existing_environments() -> void:
	for node in get_tree().get_nodes_in_group("__asterra_cloud_scan"):
		_try_bind_environment(node)
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
	if _material == null:
		return

	# NoiseTexture3D generates its volume on Godot worker threads. Binding the
	# resource here is non-blocking; the texture becomes populated when generation
	# completes, avoiding a world-start hitch.
	var shape_noise := FastNoiseLite.new()
	shape_noise.seed = _seed32(world_seed, 0x43A51)
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

	var shape_texture := NoiseTexture3D.new()
	shape_texture.width = 96
	shape_texture.height = 96
	shape_texture.depth = 96
	shape_texture.seamless = true
	shape_texture.seamless_blend_skirt = 0.12
	shape_texture.normalize = true
	shape_texture.noise = shape_noise

	var detail_noise := FastNoiseLite.new()
	detail_noise.seed = _seed32(world_seed, 0x7D19B)
	detail_noise.noise_type = FastNoiseLite.TYPE_SIMPLEX_SMOOTH
	detail_noise.frequency = 0.085
	detail_noise.fractal_type = FastNoiseLite.FRACTAL_RIDGED
	detail_noise.fractal_octaves = 4
	detail_noise.fractal_gain = 0.53
	detail_noise.fractal_lacunarity = 2.11

	var detail_texture := NoiseTexture3D.new()
	detail_texture.width = 64
	detail_texture.height = 64
	detail_texture.depth = 64
	detail_texture.seamless = true
	detail_texture.seamless_blend_skirt = 0.14
	detail_texture.normalize = true
	detail_texture.noise = detail_noise

	_material.set_shader_parameter("u_cloud_shape_noise", shape_texture)
	_material.set_shader_parameter("u_cloud_detail_noise", detail_texture)
	_material.set_shader_parameter("u_cloud_primary_steps", _primary_steps(quality))
	_material.set_shader_parameter("u_cloud_light_steps", _light_steps(quality))
	_material.set_shader_parameter("u_cloud_enabled", 1.0)
	_material.set_shader_parameter("u_cloud_wind_offset", _wind_offset)


func _process(delta: float) -> void:
	if _material == null:
		return
	_wind_offset += WIND_METRES_PER_SECOND * delta
	_wind_accumulator += delta
	if _wind_accumulator < WIND_UPDATE_INTERVAL:
		return
	_wind_accumulator = fmod(_wind_accumulator, WIND_UPDATE_INTERVAL)
	# A sky-uniform write invalidates the radiance map. Ten updates per second is
	# visually continuous at ~12 m/s (about one metre of advection per update), but
	# avoids pointlessly invalidating it at 120/144/240 Hz display rates.
	_material.set_shader_parameter("u_cloud_wind_offset", _wind_offset)


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


func _seed32(world_seed: int, salt: int) -> int:
	# FastNoiseLite stores a signed 32-bit seed. Fold Asterra's deterministic
	# 64-bit world seed into that range without introducing global RNG state.
	var mixed := world_seed ^ (salt * 0x45D9F3B)
	mixed = mixed ^ (mixed >> 16)
	return int(mixed & 0x7FFFFFFF)
