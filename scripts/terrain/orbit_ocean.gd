class_name OrbitOcean
extends Node3D
## Global sea-level shell used for distant graphics.
##
## The local OceanGeometryClipmap owns displaced waves and surf from the ground
## through regional/aircraft altitude. Above the handoff this smooth shell takes
## over and evaluates the same procedural GPU terrain field for its shoreline.

const SHELL_OFFSET_M := 1.5
const VISUAL_LOCK_ALTITUDE_M := 120000.0
const RADIAL_SEGMENTS := 256
const RINGS := 128
const TARGET_FINE_DEPTH: int = 16
const PROCEDURAL_DETAIL_STRENGTH: float = 1.0

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
		_material.set_shader_parameter("u_sun_intensity", GraphicsQuality.solar_irradiance())

func _on_world_ready(_fields: PlanetFields) -> void:
	_refresh()

func material() -> ShaderMaterial:
	return _material

func refresh_surface() -> void:
	_refresh()

func _refresh() -> void:
	if not Planet.ready_state or Planet.orbit_elevation_texture == null:
		return

	if _mesh_instance == null:
		_mesh_instance = MeshInstance3D.new()
		_mesh_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		add_child(_mesh_instance)

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

	var base_spacing: float = PI * 0.5 * Planet.cfg.planet_radius \
		/ (float(Planet.cfg.chunk_grid) * pow(2.0, float(TARGET_FINE_DEPTH)))
	var detail_seed: int = Planet.cfg.stream_seed("gpu_visual_detail") & 0x00ffffff

	_material.set_shader_parameter("u_planet_radius", Planet.cfg.planet_radius)
	_material.set_shader_parameter("u_atmosphere_height", Planet.cfg.atmosphere_height)
	_material.set_shader_parameter("u_sun_dir", Frames.helion_dir)
	_material.set_shader_parameter("u_macro_elevation", Planet.orbit_elevation_texture)
	_material.set_shader_parameter("u_macro_face_res", float(Planet.orbit_texture_face_res))
	_material.set_shader_parameter("u_macro_ready", 1.0)
	_material.set_shader_parameter("u_base_spacing", base_spacing)
	_material.set_shader_parameter("u_detail_seed", maxi(detail_seed, 1))
	_material.set_shader_parameter("u_detail_strength", PROCEDURAL_DETAIL_STRENGTH
		* maxf(0.05, Planet.cfg.detail_amplitude / 260.0))
	_material.set_shader_parameter("u_orbit_start_altitude", VISUAL_LOCK_ALTITUDE_M)
	_sync_origin()

func _on_origin_shifted(_delta: Vector3) -> void:
	_sync_origin()

func _sync_origin() -> void:
	position = Frames.to_render(Vec3D.new(0.0, 0.0, 0.0))
	if _material != null:
		_material.set_shader_parameter("u_origin",
			Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z)))
