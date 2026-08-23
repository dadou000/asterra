extends "res://scripts/terrain/ground_geometry_clipmap.gd"
## Minimal clipmap topology used by the terrain-performance branch.
##
## The height shader reconstructs logical grid coordinates from VERTEX_ID, so the
## CPU mesh only needs enough zero-valued vertices to define the legal index range.
## Normals and UVs are deliberately omitted. Rows are emitted as triangle strips
## and disconnected strip segments are joined with degenerate indices.
##
## This keeps the existing MeshInstance3D/spatial-shader integration (shadows,
## material pipeline, custom AABB) while substantially reducing static vertex and
## index bandwidth before the later RenderingDevice procedural-draw migration.


func _build_ring_nodes() -> void:
	var full: ArrayMesh = _build_strip_mesh(false)
	var ring: ArrayMesh = _build_strip_mesh(true)
	for level: int in RENDER_LEVELS:
		var mi := MeshInstance3D.new()
		mi.name = "GroundClipmapL%d" % level
		mi.mesh = full if level == 0 else ring
		mi.material_override = _material
		mi.set_instance_shader_parameter("clip_level", float(level))
		mi.set_instance_shader_parameter("clip_outer",
			1.0 if level == RENDER_LEVELS - 1 else 0.0)
		# All submitted vertex positions are zero because the shader reconstructs
		# the real planet-space position. Keep explicit displacement bounds so CPU
		# frustum culling never rejects a valid ring from its dummy source AABB.
		mi.custom_aabb = AABB(
			Vector3(-100000.0, -100000.0, -100000.0),
			Vector3(200000.0, 200000.0, 200000.0))
		add_child(mi)
		_rings.append(mi)


static func _build_strip_mesh(with_hole: bool) -> ArrayMesh:
	var vertex_count: int = GRID_VERTS * GRID_VERTS
	var vertices := PackedVector3Array()
	vertices.resize(vertex_count)
	# PackedVector3Array.resize() zero-initialises the dummy stream. Do not fill it:
	# VERTEX itself is ignored and VERTEX_ID is the authoritative grid address.

	var indices := PackedInt32Array()
	if with_hole:
		_build_ring_strip_indices(indices)
	else:
		_build_full_strip_indices(indices)

	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_INDEX] = indices

	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLE_STRIP, arrays)
	return mesh


static func _build_full_strip_indices(indices: PackedInt32Array) -> void:
	for y: int in GRID_CELLS:
		_append_row_segment(indices, y, 0, GRID_CELLS)


static func _build_ring_strip_indices(indices: PackedInt32Array) -> void:
	# The inner hole is exactly half the grid width, matching the previous
	# triangle-list topology: cells [16,48) x [16,48) for a 64-cell grid.
	var inner_min: int = GRID_CELLS >> 2
	var inner_max: int = GRID_CELLS - inner_min
	for y: int in GRID_CELLS:
		if y < inner_min or y >= inner_max:
			_append_row_segment(indices, y, 0, GRID_CELLS)
		else:
			_append_row_segment(indices, y, 0, inner_min)
			_append_row_segment(indices, y, inner_max, GRID_CELLS)


## Add one horizontal strip covering cells [x0, x1) between rows y and y+1.
## Separate strips are connected by duplicate end/start vertices. Every triangle
## spanning the connector is therefore degenerate and cannot bridge the ring hole
## or two unrelated rows.
static func _append_row_segment(indices: PackedInt32Array,
		y: int, x0: int, x1: int) -> void:
	if x1 <= x0:
		return
	var first: int = y * GRID_VERTS + x0
	if not indices.is_empty():
		var previous_last: int = indices[indices.size() - 1]
		indices.append(previous_last)
		indices.append(first)

	for x: int in range(x0, x1 + 1):
		indices.append(y * GRID_VERTS + x)
		indices.append((y + 1) * GRID_VERTS + x)
