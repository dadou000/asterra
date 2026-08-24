class_name VolumetricCloudController
extends Node
## Runtime owner for Asterra's volumetric cloud field.
##
## Visible clouds are composited after scene geometry with a resolved-depth-aware
## CompositorEffect. The atmosphere sky shader remains the fallback path only if
## the compositor cannot initialize. Terrain/water shadows share the same 3D noise
## volumes and wind coordinate, so the cloud body, its occlusion and its shadow stay
## in one physical coordinate system.

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
var _depth_effect: CloudDepthCompositorEffect
var _shadow_receivers: Array[WeakRef] = []
var _helion_lights: Array[WeakRef] = []
var _wind_offset := Vector3.ZERO
var _wind_accumulator := 0.0
var _world_seed := 0x4153544552524100
var _planet_radius := 1000000.0
var _quality := GraphicsQuality.Preset.HIGH
var _last_helion_angular_radius := -1.0


func _ready() -> void:
	var world_resource := load("res://world.tres")
	if world_resource is GenConfig:
		_world_seed = world_resource.world_seed
		_planet_radius = world_resource.planet_radius
	_quality = GraphicsQuality.sanitize(AppSettings.graphics_quality)
	get_tree().node_added.connect(_on_node_added)
	call_deferred("_scan_existing_environments")
	call_deferred("_register_known_surface_receivers")


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


func _register_known_surface_receivers() -> void:
	# Both are autoload GPU renderers and own one shared ShaderMaterial each. Read
	# them after all autoload _ready() calls so the safe terrain renderer has already
	# swapped in its final compact-UV shader.
	for path: NodePath in [NodePath("/root/GroundGeometryClipmap"), NodePath("/root/OceanSystem")]:
		var node := get_node_or_null(path)
		if node == null:
			continue
		var value: Variant = node.get("_material")
		if value is ShaderMaterial:
			register_shadow_receiver(value as ShaderMaterial)


func _try_bind_environment(world_environment: WorldEnvironment) -> void:
	if world_environment == null or world_environment.environment == null:
		return
	var sky := world_environment.environment.sky
	if sky == null or not (sky.sky_material is ShaderMaterial):
		return
	var material := sky.sky_material as ShaderMaterial
	if material.shader == null or material.shader.resource_path != ATMOSPHERE_SHADER_PATH:
		return

	if material != _material:
		configure(material, _world_seed, AppSettings.graphics_quality)
	_install_depth_compositor(world_environment)
	_register_helion_directional_lights(world_environment)


func configure(material: ShaderMaterial, world_seed: int, quality: int) -> void:
	_material = material
	_world_seed = world_seed
	_quality = GraphicsQuality.sanitize(quality)
	if Planet.ready_state and Planet.cfg != null:
		_planet_radius = Planet.cfg.planet_radius
	_ensure_noise_volumes()
	if _material == null:
		return

	# Keep the sky implementation fully configured as a fallback. Once the depth
	# compositor is installed successfully, _install_depth_compositor disables this
	# visible sky cloud pass to prevent double rendering.
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
	_material.set_shader_parameter("u_helion_angular_radius_rad", Frames.helion_angular_radius_rad())
	_last_helion_angular_radius = Frames.helion_angular_radius_rad()
	_sync_all_shadow_receivers()


func _install_depth_compositor(world_environment: WorldEnvironment) -> void:
	_ensure_noise_volumes()
	if _depth_effect == null:
		_depth_effect = CloudDepthCompositorEffect.new()
	_depth_effect.set_cloud_textures(_shape_texture, _detail_texture)
	_sync_depth_effect()

	# If the RenderingDevice shader cannot initialize, leave the old sky cloud pass
	# enabled. This keeps the project usable on an unsupported renderer instead of
	# silently deleting clouds.
	if not _depth_effect.is_ready():
		if _material != null:
			_material.set_shader_parameter("u_cloud_enabled", 1.0)
		return

	var compositor: Compositor = world_environment.compositor
	if compositor == null:
		compositor = Compositor.new()
	var effects: Array[CompositorEffect] = compositor.compositor_effects
	if not effects.has(_depth_effect):
		effects.append(_depth_effect)
		compositor.compositor_effects = effects
	world_environment.compositor = compositor

	# A Sky shader is always behind scene geometry. Once the depth-aware pass is
	# active it is the sole visible cloud renderer; the sky keeps only atmosphere,
	# deep sky and the stellar disc.
	if _material != null:
		_material.set_shader_parameter("u_cloud_enabled", 0.0)


func _register_helion_directional_lights(world_environment: WorldEnvironment) -> void:
	# The scene currently creates its stellar DirectionalLight3D beside the validated
	# Asterra WorldEnvironment. Keep ordinary Godot shadow penumbrae and volumetric
	# cloud penumbrae on the same physical apparent stellar diameter.
	var parent := world_environment.get_parent()
	if parent == null:
		return
	for child in parent.get_children():
		if not (child is DirectionalLight3D):
			continue
		var light := child as DirectionalLight3D
		if not light.shadow_enabled:
			continue
		var already_registered := false
		for light_ref: WeakRef in _helion_lights:
			if light_ref.get_ref() == light:
				already_registered = true
				break
		if not already_registered:
			_helion_lights.append(weakref(light))
		light.light_angular_distance = Frames.helion_angular_diameter_deg()


## Surface renderers call this once after their final ShaderMaterial has been
## installed. Registration is deliberately order-independent.
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
	material.set_shader_parameter("u_cloud_shadow_disc_samples", _shadow_disc_samples(_quality))
	material.set_shader_parameter("u_cloud_shadow_helion_radius_m", Frames.helion_radius_m)
	material.set_shader_parameter("u_cloud_shadow_helion_distance_m", Frames.helion_distance_m)
	material.set_shader_parameter("u_helion_angular_radius_rad", Frames.helion_angular_radius_rad())
	material.set_shader_parameter("u_cloud_shadow_wind_offset", _wind_offset)
	material.set_shader_parameter("u_cloud_shadow_sun_dir", Frames.helion_dir)


func _sync_all_shadow_receivers() -> void:
	for i in range(_shadow_receivers.size() - 1, -1, -1):
		var material: ShaderMaterial = _shadow_receivers[i].get_ref() as ShaderMaterial
		if material == null:
			_shadow_receivers.remove_at(i)
			continue
		_sync_shadow_receiver(material)


func _sync_depth_effect() -> void:
	if _depth_effect == null:
		return
	if Planet.ready_state and Planet.cfg != null:
		_planet_radius = Planet.cfg.planet_radius
	var frame_origin := Frames.origin
	var origin_v3 := Vector3(float(frame_origin.x), float(frame_origin.y), float(frame_origin.z))
	_depth_effect.set_runtime_state(
		origin_v3,
		_planet_radius,
		Frames.helion_dir,
		GraphicsQuality.solar_irradiance(),
		_wind_offset,
		_compositor_steps(_quality),
		Frames.helion_angular_radius_rad())


func _process(delta: float) -> void:
	if _material == null and _shadow_receivers.is_empty() and _depth_effect == null:
		return

	# The depth-aware visible cloud pass can take current wind/camera coordinates
	# every frame without invalidating a Sky radiance cubemap, so keep it continuous.
	_wind_offset += WIND_METRES_PER_SECOND * delta
	_sync_depth_effect()

	_wind_accumulator += delta
	if _wind_accumulator < WIND_UPDATE_INTERVAL:
		return
	_wind_accumulator = fmod(_wind_accumulator, WIND_UPDATE_INTERVAL)

	# The fallback sky and the surface shadows keep their lower-frequency uniform
	# updates. At ~12 m/s this is only about one metre of displacement per update.
	if _material != null and (_depth_effect == null or not _depth_effect.is_ready()):
		_material.set_shader_parameter("u_cloud_wind_offset", _wind_offset)
	for i in range(_shadow_receivers.size() - 1, -1, -1):
		var material: ShaderMaterial = _shadow_receivers[i].get_ref() as ShaderMaterial
		if material == null:
			_shadow_receivers.remove_at(i)
			continue
		material.set_shader_parameter("u_cloud_shadow_wind_offset", _wind_offset)
		material.set_shader_parameter("u_cloud_shadow_sun_dir", Frames.helion_dir)
		# These are intentionally refreshed too. If Helion's system geometry changes
		# later (season/orbit simulation or debug controls), penumbra width follows it
		# without rebuilding materials.
		material.set_shader_parameter("u_cloud_shadow_helion_radius_m", Frames.helion_radius_m)
		material.set_shader_parameter("u_cloud_shadow_helion_distance_m", Frames.helion_distance_m)
		material.set_shader_parameter("u_helion_angular_radius_rad", Frames.helion_angular_radius_rad())

	var angular_radius := Frames.helion_angular_radius_rad()
	if _material != null and not is_equal_approx(angular_radius, _last_helion_angular_radius):
		# Only touch the sky uniform when the physical stellar geometry actually
		# changes; setting sky uniforms every tick would invalidate its radiance map.
		_material.set_shader_parameter("u_helion_angular_radius_rad", angular_radius)
		_last_helion_angular_radius = angular_radius

	var angular_diameter_deg := rad_to_deg(angular_radius * 2.0)
	for i in range(_helion_lights.size() - 1, -1, -1):
		var light := _helion_lights[i].get_ref() as DirectionalLight3D
		if light == null:
			_helion_lights.remove_at(i)
			continue
		light.light_angular_distance = angular_diameter_deg


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


func _compositor_steps(quality: int) -> int:
	# This pass runs at internal render resolution (before FSR upscaling), not the
	# former half-res sky target, so use a tighter step budget while retaining the
	# same density field and optical integration.
	match GraphicsQuality.sanitize(quality):
		GraphicsQuality.Preset.PERFORMANCE:
			return 10
		GraphicsQuality.Preset.BALANCED:
			return 14
		GraphicsQuality.Preset.ULTRA:
			return 24
		_:
			return 18


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


func _shadow_disc_samples(quality: int) -> int:
	# Number of equal-area points sampled across Helion's apparent disc. One sample
	# is the point-source fallback; higher presets converge toward the finite-source
	# Beer-Lambert integral without changing its physical angular radius.
	match GraphicsQuality.sanitize(quality):
		GraphicsQuality.Preset.PERFORMANCE:
			return 1
		GraphicsQuality.Preset.BALANCED:
			return 3
		GraphicsQuality.Preset.ULTRA:
			return 7
		_:
			return 5


func _seed32(world_seed: int, salt: int) -> int:
	# FastNoiseLite stores a signed 32-bit seed. Fold Asterra's deterministic
	# 64-bit world seed into that range without introducing global RNG state.
	var mixed := world_seed ^ (salt * 0x45D9F3B)
	mixed = mixed ^ (mixed >> 16)
	return int(mixed & 0x7FFFFFFF)
