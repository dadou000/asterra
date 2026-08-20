class_name CloudRenderer
extends MeshInstance3D
## Full-screen spherical volumetric cloud renderer.
##
## The volume is planet-centred, not camera-centred, so the same cloud field can
## be flown through from the surface and inspected from orbit without a handoff.
## The broad cloud state comes from WeatherSystem; two generated 3D noise volumes
## only provide sub-grid billowing/erosion. No assets or shader code are taken from
## the temporary visual reference package.

var cfg: GenConfig
var weather: WeatherSystem
var cloud_material: ShaderMaterial
var base_noise: NoiseTexture3D
var detail_noise: NoiseTexture3D

func configure(p_cfg: GenConfig, p_weather: WeatherSystem) -> void:
	cfg = p_cfg
	weather = p_weather
	_setup_mesh()
	_setup_noise()
	_setup_material()
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
	# Broad fBm volume: low frequency structure/towers.
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

	# Higher-frequency cellular texture erodes the outer edges. This is generated
	# by Godot/FastNoiseLite at runtime and is not related to the reference assets.
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
	cloud_material.set_shader_parameter("u_cloud_outer_radius", cfg.planet_radius + 14500.0)
	cloud_material.set_shader_parameter("u_base_noise", base_noise)
	cloud_material.set_shader_parameter("u_detail_noise", detail_noise)
	material_override = cloud_material

func refresh_weather() -> void:
	if cloud_material == null or weather == null:
		return
	if weather.weather_map == null or weather.wind_map == null:
		return
	cloud_material.set_shader_parameter("u_weather_map", weather.weather_map)
	cloud_material.set_shader_parameter("u_wind_map", weather.wind_map)
	cloud_material.set_shader_parameter("u_weather_face_res", float(weather.face_res))

func _process(_dt: float) -> void:
	if cloud_material == null:
		return
	# Pass the actual active camera position in Asterra's canonical planet frame.
	# Do not rebuild it in the shader from CAMERA_POSITION_WORLD + floating-origin
	# state: those values belong to different coordinate representations and can be
	# observed on opposite sides of an origin rebase for a frame. A single canonical
	# camera position keeps the cloud shell rigidly attached to the planet.
	var cam := get_viewport().get_camera_3d()
	if cam != null:
		var camera_world: Vec3D = Frames.to_world(cam.global_position)
		cloud_material.set_shader_parameter("u_camera_world", Vector3(
			float(camera_world.x), float(camera_world.y), float(camera_world.z)))
	cloud_material.set_shader_parameter("u_sun_dir", Frames.helion_dir.normalized())
