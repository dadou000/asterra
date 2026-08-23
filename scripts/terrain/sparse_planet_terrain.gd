class_name SparsePlanetTerrain
extends PlanetTerrain
## GPU-only whole-planet terrain backing.
##
## This deliberately does NOT call PlanetTerrain._ready() and never creates a
## QuadNode, ChunkBuilder job, runtime terrain ArrayMesh, stitch rebuild, or visual
## collision body. The whole planet is one immutable grid topology instanced six
## times (one cube-sphere face); the vertex shader reconstructs each face from
## VERTEX_ID and samples the already-baked orbital elevation texture.
##
## The camera-local GroundGeometryClipmap remains the high-resolution surface.
## TerrainCollisionStreamer remains the independent physics path. This class only
## replaces the former CPU visual quadtree.

const GLOBAL_GRID_CELLS: int = 192
const GLOBAL_GRID_VERTS: int = GLOBAL_GRID_CELLS + 1
const GLOBAL_BOUNDS_M: float = 4000000.0

var _planet_batch: MultiMeshInstance3D
var _collision_streamer: TerrainCollisionStreamer
var _last_orbit_texture: Texture2DArray
var _last_orbit_res: int = -1


func _ready() -> void:
	# Intentionally no super._ready(): that was the old CPU terrain pipeline.
	process_priority = 10
	add_to_group("sparse_planet_terrain")

	_ground_mat = ShaderMaterial.new()
	_ground_mat.shader = load("res://shaders/sparse_planet_terrain.gdshader")
	_water_mat = ShaderMaterial.new() # OrbitOcean owns visible water now.

	_build_static_planet_batch()
	_collision_streamer = TerrainCollisionStreamer.new()
	add_child(_collision_streamer)

	Frames.origin_shifted.connect(_on_sparse_origin_shifted)
	Planet.world_ready.connect(_on_sparse_world_ready)
	Planet.coast_profile_changed.connect(_on_sparse_profile_changed)

	if Planet.ready_state and Planet.cfg != null:
		build_roots()


## Compatibility name retained for the existing harness. There are no roots to
## build: this only refreshes immutable GPU resources after a planet rebake.
func build_roots() -> void:
	cfg = Planet.cfg
	roots.clear()
	if cfg == null:
		if _planet_batch != null:
			_planet_batch.visible = false
		return

	# Recreate only the immutable topology. This is cheap (~37k dummy vertices)
	# and lets TerrainDebug turn wireframe generation on after startup without
	# resurrecting any dynamic terrain-building path.
	if _planet_batch != null and _planet_batch.multimesh != null:
		_planet_batch.multimesh.mesh = _build_static_face_mesh()

	_ground_mat.set_shader_parameter("u_planet_radius", cfg.planet_radius)
	_ground_mat.set_shader_parameter("u_atmosphere_height", cfg.atmosphere_height)
	_ground_mat.set_shader_parameter("u_sun_dir", Frames.helion_dir)
	_ground_mat.set_shader_parameter("u_sun_intensity", GraphicsQuality.solar_irradiance())
	_ground_mat.set_shader_parameter("u_global_grid_cells", float(GLOBAL_GRID_CELLS))
	_push_sparse_origin()
	_sync_orbit_texture(true)

	if _collision_streamer != null:
		_collision_streamer.configure(cfg)
	if _planet_batch != null:
		_planet_batch.visible = true

	_stats["chunks"] = 6
	_stats["nodes"] = 6
	_stats["queued"] = 0
	_stats["in_flight"] = 0
	_stats["culled"] = 0
	_stats["handoffs"] = 0


func set_observer(world_pos: Vec3D) -> void:
	observer = world_pos
	if cfg == null:
		return
	var radius: float = cfg.planet_radius
	var r_obs: float = maxf(observer.length(), radius + 1.0)
	_obs_dir = observer.normalized().to_v3()
	_horizon_angle = acos(clampf(radius / r_obs, -1.0, 1.0)) \
		+ acos(clampf(radius / (radius + MAX_TERRAIN_HEIGHT), -1.0, 1.0))
	_stats["horizon_deg"] = rad_to_deg(_horizon_angle)
	_stats["horizon_km"] = _horizon_angle * radius / 1000.0

	# Physics is still streamed independently, but its activation test is cheap:
	# macro height is enough to decide whether the observer is near the surface.
	if _collision_streamer != null:
		var agl: float = maxf(observer.length() - radius - Planet.macro_height(_obs_dir), 0.0)
		_collision_streamer.set_observer(observer, agl <= 320.0)


func _process(_dt: float) -> void:
	if not Planet.ready_state or cfg == null:
		return
	_push_sparse_origin()
	_sync_orbit_texture(false)
	_ground_mat.set_shader_parameter("u_sun_dir", Frames.helion_dir)


func stats() -> Dictionary:
	return _stats.duplicate()


func debug_materials() -> Array:
	return [_ground_mat]


func _on_sparse_world_ready(_fields: PlanetFields) -> void:
	build_roots()


func _on_sparse_profile_changed() -> void:
	build_roots()


func _on_sparse_origin_shifted(_delta_render: Vector3) -> void:
	_push_sparse_origin()


func _push_sparse_origin() -> void:
	if _ground_mat == null:
		return
	_ground_mat.set_shader_parameter("u_origin", Vector3(
		float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z)))


func _sync_orbit_texture(force: bool) -> void:
	if _ground_mat == null:
		return
	var tex: Texture2DArray = Planet.orbit_elevation_texture
	var res: int = Planet.orbit_texture_face_res
	if force or tex != _last_orbit_texture:
		_last_orbit_texture = tex
		_ground_mat.set_shader_parameter("u_orbit_elevation", tex)
	if force or res != _last_orbit_res:
		_last_orbit_res = res
		_ground_mat.set_shader_parameter("u_orbit_face_res", float(res))
	_ground_mat.set_shader_parameter("u_orbit_ready", 1.0 if tex != null else 0.0)


func _build_static_planet_batch() -> void:
	var mesh: ArrayMesh = _build_static_face_mesh()
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_custom_data = true
	mm.mesh = mesh
	mm.instance_count = 6
	mm.visible_instance_count = 6
	mm.custom_aabb = AABB(
		Vector3(-GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M),
		Vector3(GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0))
	for face: int in 6:
		mm.set_instance_transform(face, Transform3D.IDENTITY)
		mm.set_instance_custom_data(face, Color(float(face), 0.0, 0.0, 0.0))

	_planet_batch = MultiMeshInstance3D.new()
	_planet_batch.name = "SparsePlanetSurface"
	_planet_batch.multimesh = mm
	_planet_batch.material_override = _ground_mat
	# The local clipmap provides near-field terrain shadows. Casting a single
	# planet-scale mesh into Godot's local cascades wastes fill and causes acne.
	_planet_batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_planet_batch)


static func _build_static_face_mesh() -> ArrayMesh:
	# Vertex values are intentionally zero: VERTEX_ID is the topology coordinate.
	var vertices := PackedVector3Array()
	vertices.resize(GLOBAL_GRID_VERTS * GLOBAL_GRID_VERTS)
	var indices := PackedInt32Array()
	for y: int in GLOBAL_GRID_CELLS:
		var first: int = y * GLOBAL_GRID_VERTS
		if not indices.is_empty():
			var previous_last: int = indices[indices.size() - 1]
			indices.append(previous_last)
			indices.append(first)
		for x: int in range(GLOBAL_GRID_CELLS + 1):
			indices.append(y * GLOBAL_GRID_VERTS + x)
			indices.append((y + 1) * GLOBAL_GRID_VERTS + x)

	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLE_STRIP, arrays)
	return mesh
