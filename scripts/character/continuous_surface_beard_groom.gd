extends "res://scripts/character/dense_facial_hair_groom.gd"

## Continuous direct-surface beard sampler.
##
## The previous front-depth atlas solved some rear-surface leakage but selected
## whole triangles against a coarse XY depth grid. On the imported MPFB head that
## produced obvious polygon islands: neighboring face triangles could fall on
## opposite sides of the depth tolerance even though both belong to the same skin
## surface.
##
## This pass removes that atlas completely. Beard candidates are selected from a
## continuous 3D anatomical field and, when skinning data is present, from their
## actual HEAD-bone influence. Follicles are still born directly on the source
## triangles, so the fast barycentric root binding / facial morph system from
## dense_facial_hair_groom.gd is preserved with no per-hair raycasts.

const BEARD_HEAD_WEIGHT_MIN := 0.12
const BEARD_HEAD_WEIGHT_LOWER_MIN := 0.34
const BEARD_HEAD_WEIGHT_NECK_MIN := 0.52

var _continuous_sampler_candidates := 0
var _continuous_sampler_rejected_field := 0
var _continuous_sampler_rejected_skin := 0
var _continuous_sampler_head_weighted := 0

func _clear_surface_beard_cache() -> void:
	_continuous_sampler_candidates = 0
	_continuous_sampler_rejected_field = 0
	_continuous_sampler_rejected_skin = 0
	_continuous_sampler_head_weighted = 0
	super._clear_surface_beard_cache()

func _triangle_head_influence(tri: Dictionary) -> float:
	if _skeleton == null:
		return -1.0
	var head_bone: int = _find_head_bone(_skeleton)
	if head_bone < 0:
		return -1.0

	var surface: int = int(tri.get("surface", -1))
	var arrays: Array = _morph_surface_arrays.get(surface, [])
	if arrays.is_empty():
		return -1.0
	if arrays.size() <= Mesh.ARRAY_WEIGHTS or arrays[Mesh.ARRAY_WEIGHTS] == null:
		return -1.0
	if arrays.size() <= Mesh.ARRAY_BONES or arrays[Mesh.ARRAY_BONES] == null:
		return -1.0
	if arrays.size() <= Mesh.ARRAY_VERTEX or arrays[Mesh.ARRAY_VERTEX] == null:
		return -1.0

	var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	if vertices.is_empty():
		return -1.0
	var weights: PackedFloat32Array = PackedFloat32Array(arrays[Mesh.ARRAY_WEIGHTS])
	var bones: PackedInt32Array = PackedInt32Array(arrays[Mesh.ARRAY_BONES])
	if weights.is_empty() or bones.is_empty() or weights.size() != bones.size():
		return -1.0

	var influences_per_vertex: int = weights.size() / vertices.size()
	if influences_per_vertex <= 0:
		return -1.0

	var total := 0.0
	var valid_vertices := 0
	for vertex_index in [int(tri["ia"]), int(tri["ib"]), int(tri["ic"])]:
		if vertex_index < 0 or vertex_index >= vertices.size():
			continue
		var vertex_weight := 0.0
		var base: int = vertex_index * influences_per_vertex
		for influence_i in influences_per_vertex:
			var packed_i: int = base + influence_i
			if packed_i < 0 or packed_i >= bones.size():
				continue
			if bones[packed_i] == head_bone:
				vertex_weight += weights[packed_i]
		total += vertex_weight
		valid_vertices += 1

	if valid_vertices <= 0:
		return -1.0
	return total / float(valid_vertices)

func _beard_candidate_geometry_ok(world_point: Vector3, head_weight: float) -> bool:
	var center: Vector3 = _head["center"]
	var rx: float = maxf(float(_head["rx"]), 0.001)
	var ry: float = maxf(float(_head["ry"]), 0.001)
	var rz: float = maxf(float(_head["rz"]), 0.001)
	var ax: float = absf(world_point.x - center.x) / rx
	var down: float = (center.y - world_point.y) / ry
	var front_depth: float = (world_point.z - center.z) * _front_sign / rz

	# The beard belongs to the lower head, never the lateral neck. Keep the
	# underside of the chin, but progressively require more frontal depth toward
	# the lower/outer boundary.
	if ax > 0.91 or down < 0.015 or down > 0.79:
		return false
	if down > 0.70 and ax > 0.46:
		return false

	var side_mix: float = smoothstep(0.34, 0.86, ax)
	if down > 0.56:
		var min_depth: float = lerpf(0.08, -0.16, side_mix)
		if front_depth < min_depth:
			return false
	elif ax < 0.54 and front_depth < 0.02:
		# Central beard points are on the front of the chin/mouth area. This rejects
		# throat/inner-neck triangles that can share the same X/Y coordinates.
		return false

	if head_weight >= 0.0:
		var required_weight := BEARD_HEAD_WEIGHT_MIN
		if down > 0.56:
			required_weight = BEARD_HEAD_WEIGHT_LOWER_MIN
		if down > 0.66 or (down > 0.58 and ax > 0.58):
			required_weight = BEARD_HEAD_WEIGHT_NECK_MIN
		if head_weight < required_weight:
			return false

	return true

func _ensure_beard_surface_sampler() -> bool:
	if _beard_sampler_ready:
		return not _beard_sampler_triangles.is_empty()
	var started_usec: int = Time.get_ticks_usec()
	_beard_sampler_ready = true

	if not _ensure_morph_source_cache() or _head.is_empty() or _mount == null:
		_beard_sampler_build_ms = float(Time.get_ticks_usec() - started_usec) / 1000.0
		return false

	var center: Vector3 = _head["center"]
	var front: Vector3 = Vector3(0.0, 0.0, _front_sign)
	var total_area := 0.0

	for tri_index in _morph_triangles.size():
		var tri: Dictionary = _morph_triangles[tri_index]
		var a: Vector3 = tri["a"]
		var b: Vector3 = tri["b"]
		var c: Vector3 = tri["c"]
		var centroid: Vector3 = (a + b + c) / 3.0
		_continuous_sampler_candidates += 1

		# First use the exact same smooth field that will be evaluated for every
		# sampled follicle. Candidate selection therefore cannot create polygon
		# islands at arbitrary depth-atlas boundaries.
		var broad_field: Vector4 = _beard_field(centroid, 1.0, 1.20)
		if broad_field.x <= 0.003:
			_continuous_sampler_rejected_field += 1
			continue

		var head_weight: float = _triangle_head_influence(tri)
		if head_weight >= 0.0:
			_continuous_sampler_head_weighted += 1
		if not _beard_candidate_geometry_ok(centroid, head_weight):
			_continuous_sampler_rejected_skin += 1
			continue

		var cross_value: Vector3 = (b - a).cross(c - a)
		var twice_area: float = cross_value.length()
		if twice_area < 0.00000001:
			continue
		var normal: Vector3 = cross_value / twice_area
		var radial: Vector3 = centroid - center
		if radial.length_squared() > 0.000001 and normal.dot(radial) < 0.0:
			normal = -normal
		# Sideburn/jaw triangles may be nearly perpendicular to the frontal axis,
		# but a truly rear-facing triangle does not belong to the visible beard.
		if normal.dot(front) < -0.58:
			_continuous_sampler_rejected_skin += 1
			continue

		var area: float = twice_area * 0.5
		total_area += area
		_beard_sampler_triangles.append(tri_index)
		_beard_sampler_cumulative_area.append(total_area)

	_beard_sampler_total_area = total_area
	_beard_sampler_build_ms = float(Time.get_ticks_usec() - started_usec) / 1000.0
	return not _beard_sampler_triangles.is_empty() and total_area > 0.0

## Continuous 3D beard density field. Unlike the first direct-surface pass this
## never assigns beard purely from X/Y; depth is part of the lower-face mask so
## the throat cannot inherit the same density as the jaw in three-quarter view.
func _beard_field(world_point: Vector3, coverage: float, fullness: float) -> Vector4:
	var center: Vector3 = _head["center"]
	var rx: float = maxf(float(_head["rx"]), 0.001)
	var ry: float = maxf(float(_head["ry"]), 0.001)
	var rz: float = maxf(float(_head["rz"]), 0.001)
	var dx: float = world_point.x - center.x
	var ax: float = absf(dx) / rx
	var down: float = (center.y - world_point.y) / ry
	var front_depth: float = (world_point.z - center.z) * _front_sign / rz
	if ax > 0.90 or down < 0.02 or down > 0.79:
		return Vector4.ZERO
	if down > 0.70 and ax > 0.46:
		return Vector4.ZERO

	# Smooth 3D lower-shell gate. Central chin requires frontal skin; farther out
	# around the mandibular angle a slightly more lateral/back surface is valid.
	var side_mix: float = smoothstep(0.32, 0.86, ax)
	if down > 0.56:
		var lower_min_depth: float = lerpf(0.07, -0.16, side_mix)
		if front_depth < lower_min_depth:
			return Vector4.ZERO
	elif ax < 0.52 and front_depth < 0.015:
		return Vector4.ZERO

	var mouth_center_y: float = center.y - ry * 0.22
	var mouth_x: float = absf(dx) / maxf(rx * 0.40, 0.001)
	var mouth_y: float = absf(world_point.y - mouth_center_y) / maxf(ry * 0.125, 0.001)
	if mouth_x * mouth_x + mouth_y * mouth_y < 1.0:
		return Vector4.ZERO

	var side: float = -1.0 if dx < 0.0 else 1.0
	var outerness: float = clampf((ax - 0.24) / 0.62, 0.0, 1.0)

	# Cheek and jaw intentionally overlap by a broad margin. Their probabilistic
	# union below keeps the transition dense rather than exposing a seam.
	var cheek_top: float = lerpf(0.235, 0.082, coverage) - 0.052 * outerness
	var cheek_bottom := 0.64
	var cheek_vertical: float = smoothstep(cheek_top, cheek_top + 0.048, down)
	cheek_vertical *= 1.0 - smoothstep(cheek_bottom - 0.060, cheek_bottom, down)
	var cheek_side: float = smoothstep(0.225, 0.305, ax)
	cheek_side *= 1.0 - smoothstep(0.82, 0.90, ax)
	var cheek: float = cheek_vertical * cheek_side * lerpf(0.76, 1.0, coverage)

	var jaw_u: float = clampf(ax / 0.84, 0.0, 1.0)
	var jaw_center: float = lerpf(0.725, 0.505, pow(jaw_u, 1.28))
	var jaw_half_width: float = 0.105 * lerpf(0.88, 1.18, clampf(fullness, 0.15, 1.35))
	var jaw_distance: float = absf(down - jaw_center)
	var jaw: float = 1.0 - smoothstep(jaw_half_width * 0.64, jaw_half_width, jaw_distance)
	jaw *= 1.0 - smoothstep(0.84, 0.90, ax)

	var chin_x: float = 1.0 - smoothstep(0.35, 0.45, ax)
	var chin_y: float = smoothstep(0.300, 0.365, down)
	chin_y *= 1.0 - smoothstep(0.76, 0.79, down)
	var chin: float = chin_x * chin_y

	var connector_x: float = smoothstep(0.12, 0.20, ax) * (1.0 - smoothstep(0.56, 0.64, ax))
	var connector_y: float = smoothstep(0.35, 0.42, down) * (1.0 - smoothstep(0.67, 0.73, down))
	var connector: float = connector_x * connector_y

	# Probabilistic union is smoother and denser than max(), especially where two
	# anatomical regions overlap around the mouth corners and jaw transition.
	var union_inverse: float = (1.0 - clampf(cheek, 0.0, 1.0))
	union_inverse *= (1.0 - clampf(jaw, 0.0, 1.0))
	union_inverse *= (1.0 - clampf(chin, 0.0, 1.0))
	union_inverse *= (1.0 - clampf(connector, 0.0, 1.0))
	var weight: float = 1.0 - union_inverse
	weight *= clampf(0.82 + fullness * 0.17, 0.74, 1.0)
	if weight <= 0.0001:
		return Vector4.ZERO

	var lateral := 0.0
	if chin >= jaw and chin >= cheek and chin >= connector:
		lateral = side * ax * 0.075
	elif connector >= cheek and connector >= jaw:
		lateral = -side * 0.050
	elif jaw >= cheek:
		lateral = -side * 0.19 * jaw_u
	else:
		var cheek_down: float = clampf((down - cheek_top) / maxf(cheek_bottom - cheek_top, 0.001), 0.0, 1.0)
		lateral = -side * lerpf(0.16, 0.065, cheek_down)

	var chin_mix: float = maxf(chin, connector * 0.55)
	chin_mix *= smoothstep(0.40, 0.74, down)
	return Vector4(clampf(weight, 0.0, 1.0), lateral, -1.0, clampf(chin_mix, 0.0, 1.0))

func diagnostics() -> String:
	var base: String = super.diagnostics()
	return "%s • continuous beard sampler %d tris, %d field reject, %d skin reject, %d head-weighted" % [
		base,
		_continuous_sampler_candidates,
		_continuous_sampler_rejected_field,
		_continuous_sampler_rejected_skin,
		_continuous_sampler_head_weighted
	]
