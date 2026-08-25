extends Node3D
## GPU-driven near-field terrain scatter.
##
## CPU responsibility is deliberately limited to:
##   - constructing three static candidate MultiMeshes once,
##   - keeping a snapped tangent-space candidate window near the camera,
##   - binding immutable planet-context textures and world/origin uniforms.
##
## Candidate acceptance, position jitter, terrain height, biome/material
## suitability, visual scale and orientation are all evaluated in the vertex
## shaders from INSTANCE_ID + absolute tangent cell + world seeds.

const REANCHOR_M: float = 8192.0
const MAX_RADIAL_ALTITUDE_M: float = 12000.0
const MIN_RADIAL_ALTITUDE_M: float = -1000.0
const SCATTER_BOUNDS_M: float = 24000.0

const GRASS_GRID: int = 96
const GRASS_SPACING_M: float = 1.25
const GRASS_DENSITY: float = 0.95

const GEO_STONE_GRID: int = 64
const GEO_STONE_SPACING_M: float = 2.50
const GEO_STONE_DENSITY: float = 0.68

const RIVER_STONE_GRID: int = 64
const RIVER_STONE_SPACING_M: float = 2.00
const RIVER_STONE_DENSITY: float = 0.82

var _grass_batch: MultiMeshInstance3D
var _geo_stone_batch: MultiMeshInstance3D
var _river_stone_batch: MultiMeshInstance3D
var _grass_material: ShaderMaterial
var _geo_stone_material: ShaderMaterial
var _river_stone_material: ShaderMaterial
var _materials: Array[ShaderMaterial] = []

var _anchor_dir := Vector3(1.0, 0.0, 0.0)
var _anchor_right := Vector3(0.0, 0.0, -1.0)
var _anchor_up := Vector3(0.0, 1.0, 0.0)
var _have_anchor := false
var _debug_enabled := true
var _bound_context_generation: int = -1
var _bound_macro: Texture2DArray
var _bound_macro_res: int = -1


func _ready() -> void:
	process_priority = 10
	_build_batches()
	_set_visible(false)
	if Planet.has_signal("world_ready"):
		Planet.world_ready.connect(_on_world_ready)
	if Frames.has_signal("origin_shifted"):
		Frames.origin_shifted.connect(_on_origin_shifted)
	if Planet.ready_state and Planet.cfg != null:
		_configure_world()


func _process(_dt: float) -> void:
	if not _debug_enabled or not Planet.ready_state or Planet.cfg == null:
		_set_visible(false)
		return

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		_set_visible(false)
		return

	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var planet_pos: Vector3 = camera.global_position + origin
	if planet_pos.length_squared() <= 1.0:
		_set_visible(false)
		return

	var radius: float = Planet.cfg.planet_radius
	var observer_radius: float = planet_pos.length()
	var radial_altitude: float = observer_radius - radius
	if radial_altitude > MAX_RADIAL_ALTITUDE_M or radial_altitude < MIN_RADIAL_ALTITUDE_M:
		_set_visible(false)
		return

	var observer_dir: Vector3 = planet_pos / observer_radius
	if not _have_anchor:
		_reset_anchor(observer_dir)

	var observer_surface: Vector3 = observer_dir * radius
	var anchor_surface: Vector3 = _anchor_dir * radius
	var rel: Vector3 = observer_surface - anchor_surface
	var px: float = rel.dot(_anchor_right)
	var py: float = rel.dot(_anchor_up)
	if absf(px) > REANCHOR_M or absf(py) > REANCHOR_M:
		_reset_anchor(observer_dir)
		px = 0.0
		py = 0.0

	_bind_gpu_resources(false)
	_sync_material_window(_grass_material, GRASS_GRID, GRASS_SPACING_M, GRASS_DENSITY,
		Vector2(round(px / GRASS_SPACING_M), round(py / GRASS_SPACING_M)), origin)
	_sync_material_window(_geo_stone_material, GEO_STONE_GRID, GEO_STONE_SPACING_M, GEO_STONE_DENSITY,
		Vector2(round(px / GEO_STONE_SPACING_M), round(py / GEO_STONE_SPACING_M)), origin)
	_sync_material_window(_river_stone_material, RIVER_STONE_GRID, RIVER_STONE_SPACING_M, RIVER_STONE_DENSITY,
		Vector2(round(px / RIVER_STONE_SPACING_M), round(py / RIVER_STONE_SPACING_M)), origin)

	var context: Node = get_node_or_null("/root/PlanetContext")
	var context_ready: bool = context != null and bool(context.get("ready_state"))
	_set_visible(context_ready and _bound_macro != null)


func _configure_world() -> void:
	_have_anchor = false
	_bound_context_generation = -1
	_bound_macro = null
	_bound_macro_res = -1
	_bind_gpu_resources(true)


func _reset_anchor(observer_dir: Vector3) -> void:
	_anchor_dir = observer_dir.normalized()
	var tangent: Array = CubeSphere.tangent_basis(_anchor_dir)
	_anchor_right = tangent[0]
	_anchor_up = tangent[1]
	_have_anchor = true


func _build_batches() -> void:
	_grass_material = ShaderMaterial.new()
	_grass_material.shader = load("res://shaders/terrain_scatter_grass.gdshader")

	_geo_stone_material = ShaderMaterial.new()
	_geo_stone_material.shader = load("res://shaders/terrain_scatter_stone.gdshader")
	_geo_stone_material.set_shader_parameter("u_stone_kind", 0)

	_river_stone_material = ShaderMaterial.new()
	_river_stone_material.shader = load("res://shaders/terrain_scatter_stone.gdshader")
	_river_stone_material.set_shader_parameter("u_stone_kind", 1)

	_materials = [_grass_material, _geo_stone_material, _river_stone_material]

	var grass_mesh: ArrayMesh = _build_grass_clump_mesh()
	var stone_mesh: ArrayMesh = _build_stone_mesh()
	_grass_batch = _make_batch("TerrainScatterGrass", grass_mesh, _grass_material,
		GRASS_GRID * GRASS_GRID, false)
	_geo_stone_batch = _make_batch("TerrainScatterGeologicStone", stone_mesh, _geo_stone_material,
		GEO_STONE_GRID * GEO_STONE_GRID, false)
	_river_stone_batch = _make_batch("TerrainScatterRiverStone", stone_mesh, _river_stone_material,
		RIVER_STONE_GRID * RIVER_STONE_GRID, false)
	add_child(_grass_batch)
	add_child(_geo_stone_batch)
	add_child(_river_stone_batch)


func _make_batch(node_name: String, mesh: ArrayMesh, material: ShaderMaterial,
		count: int, cast_shadows: bool) -> MultiMeshInstance3D:
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.mesh = mesh
	mm.instance_count = count
	mm.visible_instance_count = count
	mm.custom_aabb = AABB(
		Vector3(-SCATTER_BOUNDS_M, -SCATTER_BOUNDS_M, -SCATTER_BOUNDS_M),
		Vector3(SCATTER_BOUNDS_M * 2.0, SCATTER_BOUNDS_M * 2.0, SCATTER_BOUNDS_M * 2.0))
	for i: int in count:
		mm.set_instance_transform(i, Transform3D.IDENTITY)

	var batch := MultiMeshInstance3D.new()
	batch.name = node_name
	batch.multimesh = mm
	batch.material_override = material
	batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON \
		if cast_shadows else GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	batch.visible = false
	return batch


func _bind_gpu_resources(force: bool) -> void:
	if Planet.cfg == null:
		return
	var macro: Texture2DArray = Planet.orbit_elevation_texture
	var macro_res: int = Planet.orbit_texture_face_res
	if force or macro != _bound_macro or macro_res != _bound_macro_res:
		_bound_macro = macro
		_bound_macro_res = macro_res
		for material: ShaderMaterial in _materials:
			material.set_shader_parameter("u_scatter_macro_elevation", macro)
			material.set_shader_parameter("u_scatter_macro_face_res", float(macro_res))
			material.set_shader_parameter("u_scatter_macro_ready", 1.0 if macro != null else 0.0)

	var context: Node = get_node_or_null("/root/PlanetContext")
	if context == null or not bool(context.get("ready_state")):
		for material: ShaderMaterial in _materials:
			material.set_shader_parameter("u_ctx_ready", 0.0)
		return

	var generation: int = int(context.get("generation"))
	if force or generation != _bound_context_generation:
		_bound_context_generation = generation
		for material: ShaderMaterial in _materials:
			material.set_shader_parameter("u_ctx_soil", context.get("soil_texture"))
			material.set_shader_parameter("u_ctx_surface", context.get("surface_texture"))
			material.set_shader_parameter("u_ctx_geology", context.get("geology_texture"))
			material.set_shader_parameter("u_ctx_structure", context.get("structure_texture"))
			material.set_shader_parameter("u_ctx_climate", context.get("climate_texture"))
			material.set_shader_parameter("u_ctx_hydrology", context.get("hydrology_texture"))
			material.set_shader_parameter("u_ctx_rock", context.get("rock_texture"))
			material.set_shader_parameter("u_ctx_biome", context.get("biome_texture"))
			material.set_shader_parameter("u_ctx_face_res", float(context.get("face_res")))
			material.set_shader_parameter("u_ctx_ready", 1.0)

	var scatter_seed: int = Planet.cfg.stream_seed("gpu_scatter") & 0x00ffffff
	var detail_seed: int = Planet.cfg.stream_seed("gpu_visual_detail") & 0x00ffffff
	for material: ShaderMaterial in _materials:
		material.set_shader_parameter("u_scatter_seed", maxi(scatter_seed, 1))
		material.set_shader_parameter("u_scatter_detail_seed", maxi(detail_seed, 1))
		material.set_shader_parameter("u_scatter_geomorph_spacing", 0.75)


func _sync_material_window(material: ShaderMaterial, grid: int, spacing: float,
		density: float, center_cell: Vector2, origin: Vector3) -> void:
	material.set_shader_parameter("u_scatter_enabled", 1.0 if _debug_enabled else 0.0)
	material.set_shader_parameter("u_scatter_density", density)
	material.set_shader_parameter("u_scatter_spacing", spacing)
	material.set_shader_parameter("u_scatter_grid", grid)
	material.set_shader_parameter("u_scatter_center_cell", center_cell)
	material.set_shader_parameter("u_scatter_anchor_dir", _anchor_dir)
	material.set_shader_parameter("u_scatter_anchor_right", _anchor_right)
	material.set_shader_parameter("u_scatter_anchor_up", _anchor_up)
	material.set_shader_parameter("u_scatter_planet_radius", Planet.cfg.planet_radius)
	material.set_shader_parameter("u_scatter_origin", origin)


static func _build_grass_clump_mesh() -> ArrayMesh:
	var vertices := PackedVector3Array()
	var normals := PackedVector3Array()
	var indices := PackedInt32Array()
	for blade: int in 3:
		var angle: float = float(blade) * PI / 3.0
		var side := Vector3(cos(angle), 0.0, sin(angle))
		var normal := Vector3(-sin(angle), 0.0, cos(angle)).normalized()
		var base_index: int = vertices.size()
		vertices.append(-side * 0.50)
		vertices.append(side * 0.50)
		vertices.append(side * 0.10 + Vector3.UP)
		vertices.append(-side * 0.10 + Vector3.UP)
		for _i: int in 4:
			normals.append(normal)
		indices.append(base_index + 0)
		indices.append(base_index + 1)
		indices.append(base_index + 2)
		indices.append(base_index + 0)
		indices.append(base_index + 2)
		indices.append(base_index + 3)

	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


static func _build_stone_mesh() -> ArrayMesh:
	var top := Vector3(0.0, 1.0, 0.0)
	var bottom := Vector3(0.0, 0.0, 0.0)
	var ring: Array[Vector3] = [
		Vector3(1.0, 0.28, 0.0),
		Vector3(0.0, 0.34, 0.86),
		Vector3(-0.82, 0.24, 0.0),
		Vector3(0.0, 0.30, -1.0),
	]
	var vertices: Array[Vector3] = []
	var normals: Array[Vector3] = []
	for i: int in 4:
		var next: int = (i + 1) % 4
		_append_flat_triangle(vertices, normals, top, ring[i], ring[next])
		_append_flat_triangle(vertices, normals, bottom, ring[next], ring[i])

	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array(vertices)
	arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array(normals)
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


static func _append_flat_triangle(vertices: Array[Vector3], normals: Array[Vector3],
		a: Vector3, b: Vector3, c: Vector3) -> void:
	var n: Vector3 = (b - a).cross(c - a).normalized()
	vertices.append(a)
	vertices.append(b)
	vertices.append(c)
	normals.append(n)
	normals.append(n)
	normals.append(n)


func _set_visible(value: bool) -> void:
	if _grass_batch != null:
		_grass_batch.visible = value
	if _geo_stone_batch != null:
		_geo_stone_batch.visible = value
	if _river_stone_batch != null:
		_river_stone_batch.visible = value


func set_debug_enabled(value: bool) -> void:
	_debug_enabled = value
	if not value:
		_set_visible(false)
	for material: ShaderMaterial in _materials:
		material.set_shader_parameter("u_scatter_enabled", 1.0 if value else 0.0)


func debug_enabled() -> bool:
	return _debug_enabled


func scatter_stats() -> Dictionary:
	return {
		"gpu_driven": true,
		"enabled": _debug_enabled,
		"grass_candidates": GRASS_GRID * GRASS_GRID,
		"geologic_stone_candidates": GEO_STONE_GRID * GEO_STONE_GRID,
		"river_stone_candidates": RIVER_STONE_GRID * RIVER_STONE_GRID,
		"grass_radius_m": GRASS_GRID * GRASS_SPACING_M * 0.5,
		"geologic_stone_radius_m": GEO_STONE_GRID * GEO_STONE_SPACING_M * 0.5,
		"river_stone_radius_m": RIVER_STONE_GRID * RIVER_STONE_SPACING_M * 0.5,
		"context_generation": _bound_context_generation,
	}


func _on_world_ready(_fields: PlanetFields) -> void:
	_configure_world()


func _on_origin_shifted(_delta: Vector3) -> void:
	# Candidate positions are reconstructed from planet coordinates every frame.
	pass
