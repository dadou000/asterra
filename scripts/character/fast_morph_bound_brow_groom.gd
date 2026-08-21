extends "res://scripts/character/morph_bound_brow_groom.gd"

## Performance layer for exact surface-bound brow morphs.
##
## Keeps the barycentric transfer used by morph_bound_brow_groom, but removes
## the expensive parts that made Character Studio appear to hang at load:
## - the source triangle cache is restricted to a broad forehead/temple band;
## - repeated procedural vertices share one skin binding;
## - source blend-shape vertex deltas are cached;
## - LOD2 stays as the cheap far-distance plate (at >=15 m its millimetre-scale
##   expression deformation is below useful screen resolution).
##
## LOD0 and LOD1 still receive the actual imported facial deformation field.

const BIND_QUANTIZATION := 0.00018 # 0.18 mm: below the strand diameter.
const SOURCE_CACHE_Y_HALF_RANGE := 0.085
const SOURCE_CACHE_X_MARGIN := 0.030
const SOURCE_CACHE_FRONT_LIMIT := -0.12

var _fast_binding_cache: Dictionary = {}
var _source_delta_cache: Dictionary = {}
var _fast_unique_bindings := 0
var _fast_binding_reuses := 0

func rebuild_brows() -> void:
	# Generated strand positions can change with creator sliders, so bindings are
	# invalidated. Source morph data/topology itself is stable and remains cached.
	_fast_binding_cache.clear()
	_fast_unique_bindings = 0
	_fast_binding_reuses = 0
	super.rebuild_brows()

func set_front_sign(sign_value: float) -> void:
	_clear_fast_morph_source_cache()
	super.set_front_sign(sign_value)

func diagnostics() -> String:
	var base: String = super.diagnostics()
	return "%s • fast bind %d unique/%d reused" % [base, _fast_unique_bindings, _fast_binding_reuses]

func _clear_fast_morph_source_cache() -> void:
	_morph_triangles.clear()
	_morph_grid.clear()
	_morph_surface_arrays.clear()
	_morph_surface_blends.clear()
	_source_delta_cache.clear()
	_fast_binding_cache.clear()

func _ensure_morph_source_cache() -> bool:
	if not _morph_triangles.is_empty():
		return true
	if _body_mesh == null or _body_mesh.mesh == null or _mount == null or _head.is_empty():
		_morph_status = "no body mesh"
		return false

	_source_shape_count = _body_mesh.mesh.get_blend_shape_count()
	if _source_shape_count <= 0:
		_morph_status = "source has no blend shapes"
		return false

	var array_mesh: ArrayMesh = _body_mesh.mesh as ArrayMesh
	_source_blend_mode = array_mesh.blend_shape_mode if array_mesh != null else Mesh.BLEND_SHAPE_MODE_RELATIVE
	_body_to_mount_basis = _mount.global_transform.basis.inverse() * _body_mesh.global_transform.basis
	# Smaller cells significantly reduce candidate triangles for each strand.
	_morph_cell_size = maxf(_character_height * 0.010, 0.012)

	var center: Vector3 = _head["center"]
	var rx: float = float(_head["rx"])
	var ry: float = float(_head["ry"])
	var broad_brow_y: float = center.y + ry * 0.37
	var y_margin: float = maxf(SOURCE_CACHE_Y_HALF_RANGE, _character_height * 0.045)
	var min_y: float = broad_brow_y - y_margin
	var max_y: float = broad_brow_y + y_margin
	var x_half: float = rx * 1.05 + SOURCE_CACHE_X_MARGIN
	var min_x: float = center.x - x_half
	var max_x: float = center.x + x_half
	var front_axis := Vector3(0.0, 0.0, _front_sign)

	for surface in _body_mesh.mesh.get_surface_count():
		var arrays: Array = _body_mesh.mesh.surface_get_arrays(surface)
		if arrays.size() <= Mesh.ARRAY_VERTEX or arrays[Mesh.ARRAY_VERTEX] == null:
			continue
		var blend_sets: Array = _body_mesh.mesh.surface_get_blend_shape_arrays(surface)
		if blend_sets.size() != _source_shape_count:
			continue

		_morph_surface_arrays[surface] = arrays
		_morph_surface_blends[surface] = blend_sets

		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var indices := PackedInt32Array()
		if arrays.size() > Mesh.ARRAY_INDEX and arrays[Mesh.ARRAY_INDEX] != null:
			indices = PackedInt32Array(arrays[Mesh.ARRAY_INDEX])

		if not indices.is_empty():
			var triangle_count: int = indices.size() / 3
			for tri_i in triangle_count:
				var ia: int = indices[tri_i * 3]
				var ib: int = indices[tri_i * 3 + 1]
				var ic: int = indices[tri_i * 3 + 2]
				if ia < 0 or ib < 0 or ic < 0 or ia >= vertices.size() or ib >= vertices.size() or ic >= vertices.size():
					continue
				_append_fast_triangle(surface, ia, ib, ic, vertices, center, front_axis, min_x, max_x, min_y, max_y)
		else:
			var triangle_count: int = vertices.size() / 3
			for tri_i in triangle_count:
				var base: int = tri_i * 3
				_append_fast_triangle(surface, base, base + 1, base + 2, vertices, center, front_axis, min_x, max_x, min_y, max_y)

	if _morph_triangles.is_empty():
		_morph_status = "no morphable forehead triangles"
		return false

	_morph_status = "fast barycentric face binding"
	return true

func _append_fast_triangle(
	surface: int,
	ia: int,
	ib: int,
	ic: int,
	vertices: PackedVector3Array,
	center: Vector3,
	front_axis: Vector3,
	min_x: float,
	max_x: float,
	min_y: float,
	max_y: float
) -> void:
	var a: Vector3 = _body_mesh.to_global(vertices[ia])
	var b: Vector3 = _body_mesh.to_global(vertices[ib])
	var c: Vector3 = _body_mesh.to_global(vertices[ic])

	var tri_min_x: float = minf(a.x, minf(b.x, c.x))
	var tri_max_x: float = maxf(a.x, maxf(b.x, c.x))
	var tri_min_y: float = minf(a.y, minf(b.y, c.y))
	var tri_max_y: float = maxf(a.y, maxf(b.y, c.y))
	if tri_max_x < min_x or tri_min_x > max_x or tri_max_y < min_y or tri_min_y > max_y:
		return

	var centroid: Vector3 = (a + b + c) / 3.0
	var radial: Vector3 = centroid - center
	if radial.length_squared() > 0.000001 and radial.normalized().dot(front_axis) < SOURCE_CACHE_FRONT_LIMIT:
		return

	var a_local: Vector3 = _mount.to_local(a)
	var b_local: Vector3 = _mount.to_local(b)
	var c_local: Vector3 = _mount.to_local(c)
	var neutral_tangent: Vector3 = b_local - a_local
	var neutral_normal: Vector3 = neutral_tangent.cross(c_local - a_local)
	var neutral_bitangent := Vector3.ZERO
	if neutral_tangent.length_squared() > 0.00000001 and neutral_normal.length_squared() > 0.00000001:
		neutral_tangent = neutral_tangent.normalized()
		neutral_normal = neutral_normal.normalized()
		neutral_bitangent = neutral_normal.cross(neutral_tangent).normalized()

	var tri_index: int = _morph_triangles.size()
	_morph_triangles.append({
		"surface": surface,
		"ia": ia,
		"ib": ib,
		"ic": ic,
		"a": a,
		"b": b,
		"c": c,
		"al": a_local,
		"bl": b_local,
		"cl": c_local,
		"nt": neutral_tangent,
		"nn": neutral_normal,
		"nb": neutral_bitangent
	})

	var min_cell: Vector2i = _morph_grid_cell(Vector3(tri_min_x, tri_min_y, 0.0))
	var max_cell: Vector2i = _morph_grid_cell(Vector3(tri_max_x, tri_max_y, 0.0))
	for gx in range(min_cell.x, max_cell.x + 1):
		for gy in range(min_cell.y, max_cell.y + 1):
			var key := Vector2i(gx, gy)
			var bucket: Array = _morph_grid.get(key, [])
			bucket.append(tri_index)
			_morph_grid[key] = bucket

func _fast_vertex_key(vertex_local: Vector3) -> Vector3i:
	return Vector3i(
		int(round(vertex_local.x / BIND_QUANTIZATION)),
		int(round(vertex_local.y / BIND_QUANTIZATION)),
		int(round(vertex_local.z / BIND_QUANTIZATION))
	)

func _bind_brow_vertex(vertex_local: Vector3) -> Dictionary:
	var key: Vector3i = _fast_vertex_key(vertex_local)
	if _fast_binding_cache.has(key):
		_fast_binding_reuses += 1
		var cached: Dictionary = _fast_binding_cache[key]
		return cached.duplicate()

	var binding: Dictionary = super._bind_brow_vertex(vertex_local)
	_fast_unique_bindings += 1
	_fast_binding_cache[key] = binding.duplicate()
	return binding

func _create_morph_bound_mesh(neutral_mesh: ArrayMesh, lod: int) -> ArrayMesh:
	# LOD2 starts at 15 m. Keep the already-good transparent plate cheap rather
	# than allocating a full facial morph stack that cannot be resolved there.
	if lod >= 2:
		return neutral_mesh
	return super._create_morph_bound_mesh(neutral_mesh, lod)

func _source_vertex_delta(surface: int, shape_index: int, vertex_index: int) -> Vector3:
	var key := Vector3i(surface, shape_index, vertex_index)
	if _source_delta_cache.has(key):
		return Vector3(_source_delta_cache[key])

	var base_arrays: Array = _morph_surface_arrays.get(surface, [])
	var blend_sets: Array = _morph_surface_blends.get(surface, [])
	if base_arrays.is_empty() or blend_sets.is_empty() or shape_index < 0 or shape_index >= blend_sets.size():
		return Vector3.ZERO
	if base_arrays.size() <= Mesh.ARRAY_VERTEX or base_arrays[Mesh.ARRAY_VERTEX] == null:
		return Vector3.ZERO

	var base_vertices: PackedVector3Array = base_arrays[Mesh.ARRAY_VERTEX]
	if vertex_index < 0 or vertex_index >= base_vertices.size():
		return Vector3.ZERO
	var shape_arrays: Array = blend_sets[shape_index]
	if shape_arrays.size() <= Mesh.ARRAY_VERTEX or shape_arrays[Mesh.ARRAY_VERTEX] == null:
		return Vector3.ZERO
	var shape_vertices: PackedVector3Array = shape_arrays[Mesh.ARRAY_VERTEX]
	if vertex_index >= shape_vertices.size():
		return Vector3.ZERO

	var delta_local: Vector3
	if _source_blend_mode == Mesh.BLEND_SHAPE_MODE_NORMALIZED:
		delta_local = shape_vertices[vertex_index] - base_vertices[vertex_index]
	else:
		delta_local = shape_vertices[vertex_index]
	var delta_mount: Vector3 = _body_to_mount_basis * delta_local
	_source_delta_cache[key] = delta_mount
	return delta_mount

func _binding_shape_delta(binding: Dictionary, shape_index: int) -> Vector3:
	var surface: int = int(binding["surface"])
	var weights: Vector3 = binding["weights"]
	var da: Vector3 = _source_vertex_delta(surface, shape_index, int(binding["ia"]))
	var db: Vector3 = _source_vertex_delta(surface, shape_index, int(binding["ib"]))
	var dc: Vector3 = _source_vertex_delta(surface, shape_index, int(binding["ic"]))
	var skin_delta: Vector3 = da * weights.x + db * weights.y + dc * weights.z
	if not binding.has("tri") or not binding.has("neutral_vertex"):
		return skin_delta

	var tri_index: int = int(binding["tri"])
	if tri_index < 0 or tri_index >= _morph_triangles.size():
		return skin_delta
	var tri: Dictionary = _morph_triangles[tri_index]
	var a_local: Vector3 = tri["al"]
	var b_local: Vector3 = tri["bl"]
	var c_local: Vector3 = tri["cl"]
	var neutral_tangent: Vector3 = tri["nt"]
	var neutral_normal: Vector3 = tri["nn"]
	var neutral_bitangent: Vector3 = tri["nb"]
	if neutral_tangent.length_squared() < 0.00000001 or neutral_normal.length_squared() < 0.00000001:
		return skin_delta

	var neutral_skin: Vector3 = a_local * weights.x + b_local * weights.y + c_local * weights.z
	var neutral_vertex: Vector3 = binding["neutral_vertex"]
	var neutral_offset: Vector3 = neutral_vertex - neutral_skin
	var shaped_a: Vector3 = a_local + da
	var shaped_b: Vector3 = b_local + db
	var shaped_c: Vector3 = c_local + dc
	var shaped_skin: Vector3 = shaped_a * weights.x + shaped_b * weights.y + shaped_c * weights.z

	var shaped_tangent: Vector3 = shaped_b - shaped_a
	var shaped_normal: Vector3 = shaped_tangent.cross(shaped_c - shaped_a)
	if shaped_tangent.length_squared() < 0.00000001 or shaped_normal.length_squared() < 0.00000001:
		return skin_delta
	shaped_tangent = shaped_tangent.normalized()
	shaped_normal = shaped_normal.normalized()
	if neutral_normal.dot(shaped_normal) < 0.0:
		shaped_normal = -shaped_normal
	var shaped_bitangent: Vector3 = shaped_normal.cross(shaped_tangent).normalized()

	var rotated_offset: Vector3 = shaped_tangent * neutral_offset.dot(neutral_tangent)
	rotated_offset += shaped_bitangent * neutral_offset.dot(neutral_bitangent)
	rotated_offset += shaped_normal * neutral_offset.dot(neutral_normal)
	return shaped_skin + rotated_offset - neutral_vertex
