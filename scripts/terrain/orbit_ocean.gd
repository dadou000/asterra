class_name OrbitOcean
extends Node3D
## Global sea-level shell used for distant graphics.
##
## The streamed water mesh is intentionally tied to terrain chunks because it
## needs local lake levels, shore depth and collision-scale detail. That makes it
## the wrong representation for aircraft/orbit views, where a single terrain
## triangle can span kilometres. This shell is geometrically just a smooth
## sphere; its coastline is cut per-fragment from Planet.orbit_elevation_texture.
##
## The shell starts below the visual coastline lock. Its geometry is created once
## with Godot's native SphereMesh implementation; refreshing the coastline now
## only swaps shader parameters. The previous 256x256-per-face GDScript mesh was
## rebuilt whenever the detailed texture completed and could itself cause a large
## main-thread hitch.

const SHELL_OFFSET_M := 1.5
const VISUAL_LOCK_ALTITUDE_M := 12000.0
const RADIAL_SEGMENTS := 256
const RINGS := 128

var _mesh_instance: MeshInstance3D
var _material: ShaderMaterial

func _ready() -> void:
	process_priority = 11
	Frames.origin_shifted.connect(_on_origin_shifted)
	Planet.world_ready.connect(_on_world_ready)
	if Planet.ready_state:
		_refresh()

func _process(_dt: float) -> void:
	if _material != null:
		_material.set_shader_parameter("u_sun_dir", Frames.helion_dir)

func _on_world_ready(_fields: PlanetFields) -> void:
	_refresh()

func refresh_surface() -> void:
	_refresh()

func set_weather_map(texture: Texture2DArray, face_res: float) -> void:
	if _material == null or texture == null:
		return
	_material.set_shader_parameter("u_cloud_weather_map", texture)
	_material.set_shader_parameter("u_cloud_weather_face_res", face_res)

func _refresh() -> void:
	if not Planet.ready_state or Planet.orbit_elevation_texture == null:
		return

	if _mesh_instance == null:
		_mesh_instance = MeshInstance3D.new()
		_mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		add_child(_mesh_instance)

		# The shader samples by planet-space direction, so the distant ocean does
		# not need cube-face UVs or cube topology. SphereMesh is generated in native
		# engine code and is both much cheaper to create and much smaller.
		var radius: float = Planet.cfg.planet_radius + SHELL_OFFSET_M
		var sphere := SphereMesh.new()
		sphere.radius = radius
		sphere.height = radius * 2.0
		sphere.radial_segments = RADIAL_SEGMENTS
		sphere.rings = RINGS
		_mesh_instance.mesh = sphere

	if _material == null:
		_material = ShaderMaterial.new()
		_material.shader = load("res://shaders/orbit_ocean.gdshader")
		_mesh_instance.material_override = _material

	# Texture refreshes update only these values. No geometry allocation or
	# hundreds of thousands of GDScript vertex operations happen here anymore.
	_material.set_shader_parameter("u_planet_radius", Planet.cfg.planet_radius)
	_material.set_shader_parameter("u_atmosphere_height", Planet.cfg.atmosphere_height)
	_material.set_shader_parameter("u_sun_dir", Frames.helion_dir)
	_material.set_shader_parameter("u_orbit_elevation", Planet.orbit_elevation_texture)
	_material.set_shader_parameter("u_orbit_face_res", float(Planet.orbit_texture_face_res))
	_material.set_shader_parameter("u_orbit_start_altitude", VISUAL_LOCK_ALTITUDE_M)
	_sync_origin()

func _on_origin_shifted(_delta: Vector3) -> void:
	_sync_origin()

func _sync_origin() -> void:
	position = Frames.to_render(Vec3D.new(0.0, 0.0, 0.0))
	if _material != null:
		_material.set_shader_parameter("u_origin",
			Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z)))
