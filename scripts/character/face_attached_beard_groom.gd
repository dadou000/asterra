extends "res://scripts/character/point_filtered_beard_groom.gd"

## Face-attachment discriminator for the direct surface beard.
##
## The previous point-filtered pass removed topology-shaped hard masks, but the
## broad body mesh still contains throat/neck skin close enough to the estimated
## head ellipsoid to receive follicles. This layer derives a compact 3D map from
## the imported character itself:
##
## - vertices that participate in facial blend shapes receive a high face score;
## - actual head-bone skin weight is used as a second ownership signal when the
##   imported mesh exposes bone/weight arrays;
## - the beard density field is multiplied by this attachment score per sampled
##   follicle, not per triangle;
## - triangle sampling is importance-weighted by several point samples of the
##   same continuous field, so 30x density is spent on useful facial surface
##   instead of repeatedly sampling and rejecting the neck.
##
## The final follicle still stores the exact source triangle and barycentric
## coordinates supplied by dense_facial_hair_groom.gd. No raycasts, closest-point
## searches, or placeholder geometry are introduced.

const FACE_ATTACHMENT_GRID_SCALE := 0.0026 # ~4.6 mm at 1.75 m.
const FACE_ATTACHMENT_MIN_STEP := 0.0035
const FACE_MORPH_LOW_SCALE := 0.000028
const FACE_MORPH_HIGH_SCALE := 0.00050
const FACE_GRID_NEIGHBOR_WEIGHT := 0.55

var _face_attachment_grid: Dictionary = {}
var _face_attachment_ready := false
var _face_attachment_step := 0.0045
var _face_attachment_vertices := 0
var _face_attachment_active_vertices := 0
var _face_attachment_head_vertices := 0
var _face_attachment_build_ms := 0.0
var _importance_sampler_triangles := 0

func _clear_surface_beard_cache() -> void:
	_face_attachment_grid.clear()
	_face_attachment_ready = false
	_face_attachment_vertices = 0
	_face_attachment_active_vertices = 0
	_face_attachment_head_vertices = 0
	_face_attachment_build_ms = 0.0
	_importance_sampler_triangles = 0
	super._clear_surface_beard_cache()

func _attachment_cell(point: Vector3) -> Vector3i:
	return Vector3i(
		int(floor(point.x / _face_attachment_step)),
		int(floor(point.y / _face_attachment_step)),
		int(floor(point.z / _face_attachment_step))
	)

func _vertex_head_weight(arrays: Array, vertex_index: int, vertex_count: int, head_bone: int) -> float:
	if head_bone < 0 or vertex_count <= 0:
		return -1.0
	if arrays.size() <= Mesh.ARRAY_WEIGHTS or arrays[Mesh.ARRAY_WEIGHTS] == null:
		return -1.0
	if arrays.size() <= Mesh.ARRAY_BONES or arrays[Mesh.ARRAY_BONES] == null:
		return -1.0
	var weights: PackedFloat32Array = PackedFloat32Array(arrays[Mesh.ARRAY_WEIGHTS])
	var bones: PackedInt32Array = PackedInt32Array(arrays[Mesh.ARRAY_BONES])
	if weights.is_empty() or bones.is_empty() or weights.size() != bones.size():
		return -1.0
	var influences_per_vertex: int = weights.size() / vertex_count
	if influences_per_vertex <= 0:
		return -1.0
	var base: int = vertex_index * influences_per_vertex
	if base < 0 or base + influences_per_vertex > weights.size():
		return -1.0
	var result := 0.0
	for influence_i in influences_per_vertex:
		var packed_i: int = base + influence_i
		if bones[packed_i] == head_bone:
			result += weights[packed_i]
	return clampf(result, 0.0, 1.0)

func _vertex_max_face_morph_motion(surface: int, vertex_index: int, base_vertex: Vector3, blend_sets: Array) -> float:
	var max_motion := 0.0
	for shape_index in mini(_source_shape_count, blend_sets.size()):
		var shape_arrays: Array = blend_sets[shape_index]
		if shape_arrays.size() <= Mesh.ARRAY_VERTEX or shape_arrays[Mesh.ARRAY_VERTEX] == null:
			continue
		var shape_vertices: PackedVector3Array = PackedVector3Array(shape_arrays[Mesh.ARRAY_VERTEX])
		if vertex_index < 0 or vertex_index >= shape_vertices.size():
			continue
		var delta: Vector3
		if _source_blend_mode == Mesh.BLEND_SHAPE_MODE_NORMALIZED:
			delta = shape_vertices[vertex_index] - base_vertex
		else:
			delta = shape_vertices[vertex_index]
		# Convert local displacement to world scale before thresholding. Rotation
		# does not change length; scale does.
		var world_delta: Vector3 = _body_mesh.global_transform.basis * delta
		max_motion = maxf(max_motion, world_delta.length())
	return max_motion

func _ensure_face_attachment_grid() -> bool:
	if _face_attachment_ready:
		return not _face_attachment_grid.is_empty()
	var started_usec: int = Time.get_ticks_usec()
	_face_attachment_ready = true
	_face_attachment_step = maxf(FACE_ATTACHMENT_MIN_STEP, _character_height * FACE_ATTACHMENT_GRID_SCALE)

	if not _ensure_morph_source_cache() or _body_mesh == null or _body_mesh.mesh == null or _head.is_empty():
		_face_attachment_build_ms = float(Time.get_ticks_usec() - started_usec) / 1000.0
		return false

	var center: Vector3 = _head["center"]
	var rx: float = maxf(float(_head["rx"]), 0.001)
	var ry: float = maxf(float(_head["ry"]), 0.001)
	var rz: float = maxf(float(_head["rz"]), 0.001)
	var head_bone: int = _find_head_bone(_skeleton) if _skeleton != null else -1
	var morph_low: float = maxf(_character_height * FACE_MORPH_LOW_SCALE, 0.000035)
	var morph_high: float = maxf(_character_height * FACE_MORPH_HIGH_SCALE, morph_low * 4.0)

	for surface in _body_mesh.mesh.get_surface_count():
		var arrays: Array = _morph_surface_arrays.get(surface, [])
		var blend_sets: Array = _morph_surface_blends.get(surface, [])
		if arrays.is_empty() or blend_sets.is_empty():
			continue
		if arrays.size() <= Mesh.ARRAY_VERTEX or arrays[Mesh.ARRAY_VERTEX] == null:
			continue
		var vertices: PackedVector3Array = PackedVector3Array(arrays[Mesh.ARRAY_VERTEX])
		for vertex_index in vertices.size():
			var world_point: Vector3 = _body_mesh.to_global(vertices[vertex_index])
			var ax: float = absf(world_point.x - center.x) / rx
			var down: float = (center.y - world_point.y) / ry
			var depth: float = (world_point.z - center.z) * _front_sign / rz
			# Broad lower-face volume only. This keeps grid construction cheap and
			# avoids unrelated torso/limb vertices sharing neighboring cells.
			if ax > 1.08 or down < -0.10 or down > 0.98 or depth < -0.90:
				continue

			_face_attachment_vertices += 1
			var motion: float = _vertex_max_face_morph_motion(surface, vertex_index, vertices[vertex_index], blend_sets)
			var morph_score: float = smoothstep(morph_low, morph_high, motion)
			if morph_score > 0.05:
				_face_attachment_active_vertices += 1

			var raw_head_weight: float = _vertex_head_weight(arrays, vertex_index, vertices.size(), head_bone)
			var head_score := 1.0
			if raw_head_weight >= 0.0:
				head_score = smoothstep(0.08, 0.62, raw_head_weight)
				if raw_head_weight > 0.10:
					_face_attachment_head_vertices += 1

			var cell: Vector3i = _attachment_cell(world_point)
			var existing: Vector2 = _face_attachment_grid.get(cell, Vector2.ZERO)
			_face_attachment_grid[cell] = Vector2(maxf(existing.x, morph_score), maxf(existing.y, head_score))

	_face_attachment_build_ms = float(Time.get_ticks_usec() - started_usec) / 1000.0
	return not _face_attachment_grid.is_empty()

func _face_attachment_components(world_point: Vector3) -> Vector2:
	if not _ensure_face_attachment_grid():
		return Vector2(1.0, 1.0)
	var cell: Vector3i = _attachment_cell(world_point)
	var result: Vector2 = _face_attachment_grid.get(cell, Vector2.ZERO)
	# Only six axial neighbors are sampled. This closes tiny gaps between source
	# vertices without smearing facial ownership far down onto the neck.
	for offset in [
		Vector3i(1, 0, 0), Vector3i(-1, 0, 0),
		Vector3i(0, 1, 0), Vector3i(0, -1, 0),
		Vector3i(0, 0, 1), Vector3i(0, 0, -1)
	]:
		var neighbor: Vector2 = _face_attachment_grid.get(cell + offset, Vector2.ZERO)
		result.x = maxf(result.x, neighbor.x * FACE_GRID_NEIGHBOR_WEIGHT)
		result.y = maxf(result.y, neighbor.y * FACE_GRID_NEIGHBOR_WEIGHT)
	return Vector2(clampf(result.x, 0.0, 1.0), clampf(result.y, 0.0, 1.0))

func _face_attachment_weight(world_point: Vector3) -> float:
	var components: Vector2 = _face_attachment_components(world_point)
	var center: Vector3 = _head["center"]
	var ry: float = maxf(float(_head["ry"]), 0.001)
	var down: float = (center.y - world_point.y) / ry
	# Upper cheek/sideburn may contain vertices that barely move in the imported
	# facial shapes, so retain a modest floor there. Near and below the jaw, morph
	# activity becomes mandatory; throat vertices therefore collapse toward zero.
	var strict_lower: float = smoothstep(0.48, 0.67, down)
	var morph_component: float = lerpf(maxf(components.x, 0.28), components.x, strict_lower)
	var head_component: float = maxf(components.y, 0.18)
	return clampf(morph_component * head_component, 0.0, 1.0)

func _beard_field(world_point: Vector3, coverage: float, fullness: float) -> Vector4:
	var field: Vector4 = super._beard_field(world_point, coverage, fullness)
	if field.x <= 0.0001:
		return field
	var attachment: float = _face_attachment_weight(world_point)
	field.x *= attachment
	if field.x <= 0.0005:
		return Vector4.ZERO
	return field

func _ensure_beard_surface_sampler() -> bool:
	if _beard_sampler_ready:
		return not _beard_sampler_triangles.is_empty()
	var started_usec: int = Time.get_ticks_usec()
	_beard_sampler_ready = true

	if not _ensure_morph_source_cache() or not _ensure_face_attachment_grid() or _head.is_empty():
		_beard_sampler_build_ms = float(Time.get_ticks_usec() - started_usec) / 1000.0
		return false

	var center: Vector3 = _head["center"]
	var rx: float = maxf(float(_head["rx"]), 0.001)
	var ry: float = maxf(float(_head["ry"]), 0.001)
	var front: Vector3 = Vector3(0.0, 0.0, _front_sign)
	var total_weighted_area := 0.0
	_importance_sampler_triangles = 0

	for tri_index in _morph_triangles.size():
		var tri: Dictionary = _morph_triangles[tri_index]
		var a: Vector3 = tri["a"]
		var b: Vector3 = tri["b"]
		var c: Vector3 = tri["c"]
		var centroid: Vector3 = (a + b + c) / 3.0
		var ax: float = absf(centroid.x - center.x) / rx
		var down: float = (center.y - centroid.y) / ry
		if ax > 1.02 or down < -0.04 or down > 0.90:
			continue

		var cross_value: Vector3 = (b - a).cross(c - a)
		var twice_area: float = cross_value.length()
		if twice_area < 0.00000001:
			continue
		var normal: Vector3 = cross_value / twice_area
		var radial: Vector3 = centroid - center
		if radial.length_squared() > 0.000001 and normal.dot(radial) < 0.0:
			normal = -normal
		if normal.dot(front) < -0.72:
			continue

		# Multi-point importance avoids making any single centroid decision visible
		# in the final groom. The actual per-follicle field is still evaluated after
		# barycentric sampling inside dense_facial_hair_groom.gd.
		var fa: float = _beard_field(a, 1.0, 1.20).x
		var fb: float = _beard_field(b, 1.0, 1.20).x
		var fc: float = _beard_field(c, 1.0, 1.20).x
		var fm: float = _beard_field(centroid, 1.0, 1.20).x
		var importance: float = (fa + fb + fc + fm * 2.0) / 5.0
		if importance <= 0.0002:
			continue

		# Importance sampling only changes how often a triangle is visited. The
		# point-level acceptance test still defines density, so this cannot create
		# hard triangle boundaries. A 0.16 floor keeps boundary triangles sampled.
		var sampling_bias: float = lerpf(0.16, 1.0, sqrt(clampf(importance, 0.0, 1.0)))
		var weighted_area: float = twice_area * 0.5 * sampling_bias
		total_weighted_area += weighted_area
		_beard_sampler_triangles.append(tri_index)
		_beard_sampler_cumulative_area.append(total_weighted_area)
		_importance_sampler_triangles += 1

	_beard_sampler_total_area = total_weighted_area
	_beard_sampler_build_ms = float(Time.get_ticks_usec() - started_usec) / 1000.0
	return not _beard_sampler_triangles.is_empty() and total_weighted_area > 0.0

func diagnostics() -> String:
	var base: String = super.diagnostics()
	return "%s • face attach %d verts/%d morph-active/%d head-weighted, %.0f ms • importance sampler %d tris" % [
		base,
		_face_attachment_vertices,
		_face_attachment_active_vertices,
		_face_attachment_head_vertices,
		_face_attachment_build_ms,
		_importance_sampler_triangles
	]
