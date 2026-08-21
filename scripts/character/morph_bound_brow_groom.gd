extends "res://scripts/character/natural_brow_groom.gd"

## Surface-bound facial deformation for procedural brows.
##
## The previous animation pass approximated five ARKit brow channels in a
## shader. That cannot reproduce expressions whose forehead/temple deformation
## comes from other face blend shapes. This implementation instead transfers
## the imported face morph field onto the procedural brow meshes themselves.
##
## Every generated brow vertex is bound to a triangle on the neutral forehead.
## The binding stores the source triangle plus barycentric weights. For every
## imported blend shape that actually moves that patch of skin by a visible
## amount, an equivalent relative blend shape is generated on LOD0/1/2 by
## barycentrically interpolating the three source vertex deltas. The tiny hair
## offset from the skin is also rotated with the triangle's local deformation
## frame, so strong expressions follow both skin translation and local tilt.
## Runtime then only mirrors blend-shape weights from the face to the brows.
##
## Result: browInnerUp/browDown/etc. still work, but so do eye/cheek/expression
## shapes whenever those shapes genuinely deform the skin under the eyebrow.
## There is no hand-authored expression approximation in the deformation path.

const MORPH_SYNC_INTERVAL := 1.0 / 60.0
const MORPH_WEIGHT_EPSILON := 0.0001
# Ignore source-shape motion below 0.05 mm at the actual brow attachment patch.
# This avoids carrying mouth/viseme shapes whose tiny numerical noise would add
# memory without producing a visible brow deformation.
const MORPH_RELEVANCE_EPSILON_METERS := 0.00005

var _morph_triangles: Array[Dictionary] = []
var _morph_grid: Dictionary = {}
var _morph_surface_arrays: Dictionary = {}
var _morph_surface_blends: Dictionary = {}
var _morph_cell_size := 0.03
var _body_to_mount_basis := Basis.IDENTITY
var _source_blend_mode := Mesh.BLEND_SHAPE_MODE_RELATIVE

var _bound_shape_names: Array[StringName] = []
var _bound_source_indices: PackedInt32Array = PackedInt32Array()
var _last_bound_weights: PackedFloat32Array = PackedFloat32Array()
var _morph_sync_elapsed := 0.0
var _morph_bind_failures := 0
var _source_shape_count := 0
var _morph_status := "not initialized"

func rebuild_brows() -> void:
	# LOD0 is built first by advanced_surface_groom. Clearing here makes LOD0
	# rediscover the set of face shapes relevant to the newly generated brow
	# layout; LOD1/2 then receive the exact same shape list/order.
	_bound_shape_names.clear()
	_bound_source_indices = PackedInt32Array()
	_morph_bind_failures = 0
	super.rebuild_brows()
	_reset_weight_cache()
	_sync_bound_weights(true)

func _build_brow_mesh(lod: int) -> ArrayMesh:
	var neutral_mesh: ArrayMesh = super._build_brow_mesh(lod)
	if neutral_mesh == null:
		return neutral_mesh
	if not _ensure_morph_source_cache():
		return neutral_mesh
	return _create_morph_bound_mesh(neutral_mesh, lod)

func _process(delta: float) -> void:
	super._process(delta)
	_morph_sync_elapsed += delta
	if _morph_sync_elapsed < MORPH_SYNC_INTERVAL:
		return
	_morph_sync_elapsed = 0.0
	_sync_bound_weights(false)

func diagnostics() -> String:
	var base: String = super.diagnostics()
	return "%s • brow surface morphs %d/%d (%s, %d unbound verts)" % [
		base,
		_bound_shape_names.size(),
		_source_shape_count,
		_morph_status,
		_morph_bind_failures
	]

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
	if array_mesh != null:
		_source_blend_mode = array_mesh.blend_shape_mode
	else:
		_source_blend_mode = Mesh.BLEND_SHAPE_MODE_RELATIVE

	_body_to_mount_basis = _mount.global_transform.basis.inverse() * _body_mesh.global_transform.basis
	_morph_cell_size = maxf(_character_height * 0.020, 0.022)

	var head_top: float = float(_head.get("top", _character_bottom + _character_height))
	var min_face_y: float = head_top - _character_height * 0.19

	for surface in _body_mesh.mesh.get_surface_count():
		var arrays: Array = _body_mesh.mesh.surface_get_arrays(surface)
		if arrays.size() <= Mesh.ARRAY_VERTEX or arrays[Mesh.ARRAY_VERTEX] == null:
			continue

		var blend_sets: Array = _body_mesh.mesh.surface_get_blend_shape_arrays(surface)
		if blend_sets.size() != _source_shape_count:
			# A surface without the full imported morph set cannot be used as the
			# deformation source, but another body surface may still be valid.
			continue

		_morph_surface_arrays[surface] = arrays
		_morph_surface_blends[surface] = blend_sets

		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var indices := PackedInt32Array()
		if arrays.size() > Mesh.ARRAY_INDEX and arrays[Mesh.ARRAY_INDEX] != null:
			indices = PackedInt32Array(arrays[Mesh.ARRAY_INDEX])

		if not indices.is_empty():
			var indexed_triangle_count: int = indices.size() / 3
			for tri_i in indexed_triangle_count:
				var ia: int = indices[tri_i * 3]
				var ib: int = indices[tri_i * 3 + 1]
				var ic: int = indices[tri_i * 3 + 2]
				if ia < 0 or ib < 0 or ic < 0 or ia >= vertices.size() or ib >= vertices.size() or ic >= vertices.size():
					continue
				_append_morph_triangle(surface, ia, ib, ic, vertices, min_face_y)
		else:
			var plain_triangle_count: int = vertices.size() / 3
			for tri_i in plain_triangle_count:
				var base: int = tri_i * 3
				_append_morph_triangle(surface, base, base + 1, base + 2, vertices, min_face_y)

	if _morph_triangles.is_empty():
		_morph_status = "no morphable forehead triangles"
		return false

	_morph_status = "barycentric face binding"
	return true

func _append_morph_triangle(
	surface: int,
	ia: int,
	ib: int,
	ic: int,
	vertices: PackedVector3Array,
	min_face_y: float
) -> void:
	var a: Vector3 = _body_mesh.to_global(vertices[ia])
	var b: Vector3 = _body_mesh.to_global(vertices[ib])
	var c: Vector3 = _body_mesh.to_global(vertices[ic])
	if maxf(a.y, maxf(b.y, c.y)) < min_face_y:
		return

	var tri_index: int = _morph_triangles.size()
	_morph_triangles.append({
		"surface": surface,
		"ia": ia,
		"ib": ib,
		"ic": ic,
		"a": a,
		"b": b,
		"c": c
	})

	var min_x: float = minf(a.x, minf(b.x, c.x))
	var max_x: float = maxf(a.x, maxf(b.x, c.x))
	var min_y: float = minf(a.y, minf(b.y, c.y))
	var max_y: float = maxf(a.y, maxf(b.y, c.y))
	var min_cell: Vector2i = _morph_grid_cell(Vector3(min_x, min_y, 0.0))
	var max_cell: Vector2i = _morph_grid_cell(Vector3(max_x, max_y, 0.0))

	for gx in range(min_cell.x, max_cell.x + 1):
		for gy in range(min_cell.y, max_cell.y + 1):
			var key := Vector2i(gx, gy)
			var bucket: Array = _morph_grid.get(key, [])
			bucket.append(tri_index)
			_morph_grid[key] = bucket

func _morph_grid_cell(world_point: Vector3) -> Vector2i:
	return Vector2i(
		int(floor(world_point.x / _morph_cell_size)),
		int(floor(world_point.y / _morph_cell_size))
	)

func _candidate_triangles(world_point: Vector3, radius: int = 1) -> Array[int]:
	var result: Array[int] = []
	var seen: Dictionary = {}
	var center_cell: Vector2i = _morph_grid_cell(world_point)
	for ox in range(-radius, radius + 1):
		for oy in range(-radius, radius + 1):
			var key := center_cell + Vector2i(ox, oy)
			var bucket: Array = _morph_grid.get(key, [])
			for tri_variant in bucket:
				var tri_index := int(tri_variant)
				if seen.has(tri_index):
					continue
				seen[tri_index] = true
				result.append(tri_index)
	return result

func _create_morph_bound_mesh(neutral_mesh: ArrayMesh, lod: int) -> ArrayMesh:
	if neutral_mesh.get_surface_count() <= 0:
		return neutral_mesh

	var base_arrays: Array = neutral_mesh.surface_get_arrays(0)
	if base_arrays.size() <= Mesh.ARRAY_VERTEX or base_arrays[Mesh.ARRAY_VERTEX] == null:
		return neutral_mesh
	var vertices: PackedVector3Array = base_arrays[Mesh.ARRAY_VERTEX]
	if vertices.is_empty():
		return neutral_mesh

	var bindings: Array[Dictionary] = []
	bindings.resize(vertices.size())
	var used_source_vertices: Dictionary = {}

	for vertex_i in vertices.size():
		var binding: Dictionary = _bind_brow_vertex(vertices[vertex_i])
		if not binding.is_empty():
			binding["neutral_vertex"] = vertices[vertex_i]
		bindings[vertex_i] = binding
		if binding.is_empty():
			_morph_bind_failures += 1
			continue
		var surface := int(binding["surface"])
		for source_index in [int(binding["ia"]), int(binding["ib"]), int(binding["ic"])]:
			var key := "%d:%d" % [surface, source_index]
			used_source_vertices[key] = Vector2i(surface, source_index)

	if lod == 0:
		_select_relevant_shapes(used_source_vertices)
	if _bound_shape_names.is_empty():
		return neutral_mesh

	var output := ArrayMesh.new()
	output.blend_shape_mode = Mesh.BLEND_SHAPE_MODE_RELATIVE
	for shape_name in _bound_shape_names:
		output.add_blend_shape(shape_name)

	var blend_arrays: Array = []
	var has_normals: bool = base_arrays.size() > Mesh.ARRAY_NORMAL and base_arrays[Mesh.ARRAY_NORMAL] != null
	var has_tangents: bool = base_arrays.size() > Mesh.ARRAY_TANGENT and base_arrays[Mesh.ARRAY_TANGENT] != null

	for bound_i in _bound_source_indices.size():
		var source_shape_index: int = _bound_source_indices[bound_i]
		var delta_vertices := PackedVector3Array()
		delta_vertices.resize(vertices.size())

		for vertex_i in vertices.size():
			var binding: Dictionary = bindings[vertex_i]
			if binding.is_empty():
				delta_vertices[vertex_i] = Vector3.ZERO
			else:
				delta_vertices[vertex_i] = _binding_shape_delta(binding, source_shape_index)

		var shape_arrays: Array = []
		shape_arrays.resize(Mesh.ARRAY_MAX)
		shape_arrays[Mesh.ARRAY_VERTEX] = delta_vertices

		# Godot requires Vertex/Normal/Tangent presence to match the base surface.
		# Relative zero normal/tangent deltas preserve the strand's own generated
		# shading while the geometry itself follows the face morph precisely.
		if has_normals:
			var zero_normals := PackedVector3Array()
			zero_normals.resize(vertices.size())
			shape_arrays[Mesh.ARRAY_NORMAL] = zero_normals
		if has_tangents:
			var zero_tangents := PackedFloat32Array()
			zero_tangents.resize(vertices.size() * 4)
			shape_arrays[Mesh.ARRAY_TANGENT] = zero_tangents
		blend_arrays.append(shape_arrays)

	var primitive: int = neutral_mesh.surface_get_primitive_type(0)
	output.add_surface_from_arrays(primitive, base_arrays, blend_arrays)
	# Morph movement is only a few millimetres, but the small margin prevents a
	# close-up brow from being culled at an expression extreme.
	output.custom_aabb = neutral_mesh.get_aabb().grow(_character_height * 0.012)
	return output

func _select_relevant_shapes(used_source_vertices: Dictionary) -> void:
	_bound_shape_names.clear()
	_bound_source_indices = PackedInt32Array()
	if _body_mesh == null or _body_mesh.mesh == null:
		return

	for shape_index in _source_shape_count:
		var max_motion := 0.0
		for vertex_variant in used_source_vertices.values():
			var source_vertex: Vector2i = vertex_variant
			var delta: Vector3 = _source_vertex_delta(source_vertex.x, shape_index, source_vertex.y)
			max_motion = maxf(max_motion, delta.length())
			if max_motion >= MORPH_RELEVANCE_EPSILON_METERS:
				break

		if max_motion < MORPH_RELEVANCE_EPSILON_METERS:
			continue
		_bound_source_indices.append(shape_index)
		_bound_shape_names.append(_body_mesh.mesh.get_blend_shape_name(shape_index))

	if _bound_shape_names.is_empty():
		_morph_status = "no relevant face morphs"
	else:
		_morph_status = "%d exact transferred morphs" % _bound_shape_names.size()

func _bind_brow_vertex(vertex_local: Vector3) -> Dictionary:
	var world_vertex: Vector3 = _mount.to_global(vertex_local)
	var center: Vector3 = _head["center"]
	var front := Vector3(0.0, 0.0, _front_sign)
	var outward: Vector3 = world_vertex - center
	if outward.length_squared() < 0.000001:
		outward = front
	else:
		outward = outward.normalized()
	if outward.dot(front) < 0.12:
		outward = (outward * 0.55 + front * 0.45).normalized()

	var ray_origin: Vector3 = world_vertex + outward * maxf(_character_height * 0.018, 0.020)
	var ray_direction: Vector3 = -outward
	var candidates: Array[int] = _candidate_triangles(world_vertex, 1)
	var binding: Dictionary = _best_ray_binding(ray_origin, ray_direction, candidates)
	if not binding.is_empty():
		return binding

	# Temple geometry can move farther in X/Y along the radial cast. Try a wider
	# grid neighborhood before falling back to a closest-point search.
	candidates = _candidate_triangles(world_vertex, 2)
	binding = _best_ray_binding(ray_origin, ray_direction, candidates)
	if not binding.is_empty():
		return binding

	return _closest_triangle_binding(world_vertex, candidates)

func _best_ray_binding(origin: Vector3, direction: Vector3, candidates: Array[int]) -> Dictionary:
	var best_t := INF
	var best_binding: Dictionary = {}
	for tri_index in candidates:
		if tri_index < 0 or tri_index >= _morph_triangles.size():
			continue
		var tri: Dictionary = _morph_triangles[tri_index]
		var a: Vector3 = tri["a"]
		var b: Vector3 = tri["b"]
		var c: Vector3 = tri["c"]
		var hit_distance: float = _ray_triangle_distance(origin, direction, a, b, c)
		if hit_distance < 0.0 or hit_distance >= best_t:
			continue
		var hit: Vector3 = origin + direction * hit_distance
		var weights: Vector3 = _triangle_barycentric(hit, a, b, c)
		if weights.x < -0.001 or weights.y < -0.001 or weights.z < -0.001:
			continue
		best_t = hit_distance
		best_binding = {
			"tri": tri_index,
			"surface": int(tri["surface"]),
			"ia": int(tri["ia"]),
			"ib": int(tri["ib"]),
			"ic": int(tri["ic"]),
			"weights": weights
		}
	return best_binding

func _closest_triangle_binding(world_vertex: Vector3, candidates: Array[int]) -> Dictionary:
	var search_candidates: Array[int] = candidates
	if search_candidates.is_empty():
		search_candidates.resize(_morph_triangles.size())
		for candidate_i in _morph_triangles.size():
			search_candidates[candidate_i] = candidate_i

	var best_distance_sq := INF
	var best_binding: Dictionary = {}
	for tri_index in search_candidates:
		if tri_index < 0 or tri_index >= _morph_triangles.size():
			continue
		var tri: Dictionary = _morph_triangles[tri_index]
		var a: Vector3 = tri["a"]
		var b: Vector3 = tri["b"]
		var c: Vector3 = tri["c"]
		var weights: Vector3 = _closest_triangle_barycentric(world_vertex, a, b, c)
		var closest: Vector3 = a * weights.x + b * weights.y + c * weights.z
		var distance_sq: float = world_vertex.distance_squared_to(closest)
		if distance_sq >= best_distance_sq:
			continue
		best_distance_sq = distance_sq
		best_binding = {
			"tri": tri_index,
			"surface": int(tri["surface"]),
			"ia": int(tri["ia"]),
			"ib": int(tri["ib"]),
			"ic": int(tri["ic"]),
			"weights": weights
		}
	return best_binding

func _triangle_barycentric(point: Vector3, a: Vector3, b: Vector3, c: Vector3) -> Vector3:
	var v0: Vector3 = b - a
	var v1: Vector3 = c - a
	var v2: Vector3 = point - a
	var d00: float = v0.dot(v0)
	var d01: float = v0.dot(v1)
	var d11: float = v1.dot(v1)
	var d20: float = v2.dot(v0)
	var d21: float = v2.dot(v1)
	var denominator: float = d00 * d11 - d01 * d01
	if absf(denominator) < 0.0000000001:
		return Vector3(1.0, 0.0, 0.0)
	var weight_b: float = (d11 * d20 - d01 * d21) / denominator
	var weight_c: float = (d00 * d21 - d01 * d20) / denominator
	var weight_a: float = 1.0 - weight_b - weight_c
	return Vector3(weight_a, weight_b, weight_c)

func _closest_triangle_barycentric(point: Vector3, a: Vector3, b: Vector3, c: Vector3) -> Vector3:
	# Real-Time Collision Detection, Christer Ericson: closest point on triangle.
	var ab: Vector3 = b - a
	var ac: Vector3 = c - a
	var ap: Vector3 = point - a
	var d1: float = ab.dot(ap)
	var d2: float = ac.dot(ap)
	if d1 <= 0.0 and d2 <= 0.0:
		return Vector3(1.0, 0.0, 0.0)

	var bp: Vector3 = point - b
	var d3: float = ab.dot(bp)
	var d4: float = ac.dot(bp)
	if d3 >= 0.0 and d4 <= d3:
		return Vector3(0.0, 1.0, 0.0)

	var vc: float = d1 * d4 - d3 * d2
	if vc <= 0.0 and d1 >= 0.0 and d3 <= 0.0:
		var edge_ab_weight: float = d1 / maxf(d1 - d3, 0.0000001)
		return Vector3(1.0 - edge_ab_weight, edge_ab_weight, 0.0)

	var cp: Vector3 = point - c
	var d5: float = ab.dot(cp)
	var d6: float = ac.dot(cp)
	if d6 >= 0.0 and d5 <= d6:
		return Vector3(0.0, 0.0, 1.0)

	var vb: float = d5 * d2 - d1 * d6
	if vb <= 0.0 and d2 >= 0.0 and d6 <= 0.0:
		var edge_ac_weight: float = d2 / maxf(d2 - d6, 0.0000001)
		return Vector3(1.0 - edge_ac_weight, 0.0, edge_ac_weight)

	var va: float = d3 * d6 - d5 * d4
	if va <= 0.0 and (d4 - d3) >= 0.0 and (d5 - d6) >= 0.0:
		var edge_bc_denominator: float = (d4 - d3) + (d5 - d6)
		var edge_bc_weight: float = (d4 - d3) / maxf(edge_bc_denominator, 0.0000001)
		return Vector3(0.0, 1.0 - edge_bc_weight, edge_bc_weight)

	var face_denominator: float = 1.0 / maxf(va + vb + vc, 0.0000001)
	var face_weight_b: float = vb * face_denominator
	var face_weight_c: float = vc * face_denominator
	return Vector3(1.0 - face_weight_b - face_weight_c, face_weight_b, face_weight_c)

func _binding_shape_delta(binding: Dictionary, shape_index: int) -> Vector3:
	var surface := int(binding["surface"])
	var weights: Vector3 = binding["weights"]
	var da: Vector3 = _source_vertex_delta(surface, shape_index, int(binding["ia"]))
	var db: Vector3 = _source_vertex_delta(surface, shape_index, int(binding["ib"]))
	var dc: Vector3 = _source_vertex_delta(surface, shape_index, int(binding["ic"]))

	# Exact skin-point motion is the barycentric interpolation of the source
	# triangle's three blend-shape deltas.
	var skin_delta: Vector3 = da * weights.x + db * weights.y + dc * weights.z
	if not binding.has("tri") or not binding.has("neutral_vertex"):
		return skin_delta

	var tri_index := int(binding["tri"])
	if tri_index < 0 or tri_index >= _morph_triangles.size():
		return skin_delta
	var tri: Dictionary = _morph_triangles[tri_index]

	var a_local: Vector3 = _mount.to_local(Vector3(tri["a"]))
	var b_local: Vector3 = _mount.to_local(Vector3(tri["b"]))
	var c_local: Vector3 = _mount.to_local(Vector3(tri["c"]))
	var neutral_skin: Vector3 = a_local * weights.x + b_local * weights.y + c_local * weights.z
	var neutral_vertex: Vector3 = binding["neutral_vertex"]
	var neutral_offset: Vector3 = neutral_vertex - neutral_skin

	var shaped_a: Vector3 = a_local + da
	var shaped_b: Vector3 = b_local + db
	var shaped_c: Vector3 = c_local + dc
	var shaped_skin: Vector3 = shaped_a * weights.x + shaped_b * weights.y + shaped_c * weights.z

	# Rotate the hair's tiny stand-off/tube offset with the deformed triangle.
	# This captures local forehead/temple tilt in strong expressions instead of
	# merely translating the strand parallel to the original skin.
	var neutral_tangent: Vector3 = b_local - a_local
	var neutral_normal: Vector3 = neutral_tangent.cross(c_local - a_local)
	var shaped_tangent: Vector3 = shaped_b - shaped_a
	var shaped_normal: Vector3 = shaped_tangent.cross(shaped_c - shaped_a)
	if neutral_tangent.length_squared() < 0.00000001 or neutral_normal.length_squared() < 0.00000001:
		return skin_delta
	if shaped_tangent.length_squared() < 0.00000001 or shaped_normal.length_squared() < 0.00000001:
		return skin_delta

	neutral_tangent = neutral_tangent.normalized()
	neutral_normal = neutral_normal.normalized()
	var neutral_bitangent: Vector3 = neutral_normal.cross(neutral_tangent).normalized()
	shaped_tangent = shaped_tangent.normalized()
	shaped_normal = shaped_normal.normalized()
	if neutral_normal.dot(shaped_normal) < 0.0:
		shaped_normal = -shaped_normal
	var shaped_bitangent: Vector3 = shaped_normal.cross(shaped_tangent).normalized()

	var tangent_amount: float = neutral_offset.dot(neutral_tangent)
	var bitangent_amount: float = neutral_offset.dot(neutral_bitangent)
	var normal_amount: float = neutral_offset.dot(neutral_normal)
	var rotated_offset: Vector3 = shaped_tangent * tangent_amount
	rotated_offset += shaped_bitangent * bitangent_amount
	rotated_offset += shaped_normal * normal_amount

	var shaped_vertex: Vector3 = shaped_skin + rotated_offset
	return shaped_vertex - neutral_vertex

func _source_vertex_delta(surface: int, shape_index: int, vertex_index: int) -> Vector3:
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
	return _body_to_mount_basis * delta_local

func _reset_weight_cache() -> void:
	_last_bound_weights = PackedFloat32Array()
	_last_bound_weights.resize(_bound_shape_names.size())
	_last_bound_weights.fill(9999.0)

func _sync_bound_weights(force: bool) -> void:
	if _body_mesh == null or _body_mesh.mesh == null or _bound_shape_names.is_empty():
		return
	if _last_bound_weights.size() != _bound_shape_names.size():
		_reset_weight_cache()

	var targets: Array[MeshInstance3D] = []
	if _brow_instance != null:
		targets.append(_brow_instance)
	if _brow_lod1_instance != null:
		targets.append(_brow_lod1_instance)
	if _brow_lod2_instance != null:
		targets.append(_brow_lod2_instance)

	for bound_i in _bound_shape_names.size():
		var source_index: int = _bound_source_indices[bound_i]
		if source_index < 0 or source_index >= _body_mesh.mesh.get_blend_shape_count():
			continue
		var weight: float = _body_mesh.get_blend_shape_value(source_index)
		if not force and absf(weight - _last_bound_weights[bound_i]) < MORPH_WEIGHT_EPSILON:
			continue
		_last_bound_weights[bound_i] = weight

		for target in targets:
			if target == null or target.mesh == null:
				continue
			if bound_i >= target.mesh.get_blend_shape_count():
				continue
			target.set_blend_shape_value(bound_i, weight)
