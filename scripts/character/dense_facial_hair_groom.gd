extends "res://scripts/character/natural_facial_hair_groom.gd"

## Dense beard surface binder.
##
## The beard is no longer generated in XY and projected/raycast back onto the
## face. Every follicle is born directly on a real neutral face triangle using
## deterministic low-discrepancy, area-weighted barycentric sampling. The root
## therefore already owns the exact source triangle + barycentric coordinates.
##
## LOD0/1 facial animation is also root-bound instead of rebinding every prism
## vertex to skin. One triangle frame is evaluated per follicle and each short
## strand follows that frame rigidly. This is both more realistic (a real hair
## is attached at its follicle, not stretched by a facial morph) and removes the
## dominant per-vertex nearest-triangle/raycast build cost.
##
## Geometry is built directly as indexed ArrayMesh data. No SurfaceTool normal,
## tangent, projection, or binding pass is required for the beard.

const BEARD_LOD1_MAX_DENSITY_MULTIPLIER := 6.0
const BEARD_LOD2_MAX_DENSITY_MULTIPLIER := 1.5
const BEARD_BASE_ACCEPTED_FRACTION := 0.70
const BEARD_ROOT_CLEARANCE := 0.00022
const BEARD_SURFACE_OUTWARD_BIAS := 0.10
const BEARD_MAX_SAMPLE_ATTEMPTS_FACTOR := 10

const R2_A := 0.7548776662466927
const R2_B := 0.5698402909980532
const R2_C := 0.4384471871911697
const R2_D := 0.3281732437128990
const R2_E := 0.6823278038280193

var _beard_sampler_triangles: PackedInt32Array = PackedInt32Array()
var _beard_sampler_cumulative_area: PackedFloat32Array = PackedFloat32Array()
var _beard_sampler_total_area := 0.0
var _beard_sampler_ready := false
var _beard_sampler_build_ms := 0.0

var _beard_strands_by_lod: Dictionary = {}
var _beard_vertex_coords_by_lod: Dictionary = {}
var _beard_used_triangles_by_lod: Dictionary = {}
var _beard_hair_count_by_lod: Dictionary = {}
var _beard_mesh_build_ms: Dictionary = {}
var _beard_morph_build_ms: Dictionary = {}

func configure(character: Node3D, meshes: Array[MeshInstance3D], character_bottom: float, character_height: float, front_sign: float = 1.0, rebuild_initial: bool = true) -> bool:
	_clear_surface_beard_cache()
	return super.configure(character, meshes, character_bottom, character_height, front_sign, rebuild_initial)

func set_front_sign(sign_value: float) -> void:
	_clear_surface_beard_cache()
	super.set_front_sign(sign_value)

func _clear_surface_beard_cache() -> void:
	_beard_sampler_triangles = PackedInt32Array()
	_beard_sampler_cumulative_area = PackedFloat32Array()
	_beard_sampler_total_area = 0.0
	_beard_sampler_ready = false
	_beard_sampler_build_ms = 0.0
	_beard_strands_by_lod.clear()
	_beard_vertex_coords_by_lod.clear()
	_beard_used_triangles_by_lod.clear()
	_beard_hair_count_by_lod.clear()
	_beard_mesh_build_ms.clear()
	_beard_morph_build_ms.clear()

func _apply_natural_facial_defaults() -> void:
	mustache_settings["density"] = 1.0
	mustache_settings["width"] = 0.86
	mustache_settings["thickness"] = 1.15
	mustache_settings["length"] = 0.0055
	mustache_settings["strand_width"] = 0.00042
	mustache_settings["middle_gap"] = 0.007
	mustache_settings["droop"] = 0.18
	mustache_settings["height_offset"] = 0.0
	mustache_settings["forward_offset"] = 0.00055
	mustache_settings["messiness"] = 0.12

	beard_settings["density"] = 1.0
	beard_settings["density_multiplier"] = 30.0
	beard_settings["coverage"] = 0.68
	beard_settings["fullness"] = 1.10
	beard_settings["length"] = 0.0055
	beard_settings["chin_length"] = 0.0065
	beard_settings["strand_width"] = 0.00042
	beard_settings["height_offset"] = 0.0
	beard_settings["forward_offset"] = 0.00055
	beard_settings["messiness"] = 0.12

func _create_render_nodes() -> void:
	super._create_render_nodes()
	if _facial_hair_material != null:
		_facial_hair_material.anisotropy_enabled = false
		_facial_hair_material.anisotropy = 0.0
		_facial_hair_material.roughness = 0.68

func _effective_beard_density_multiplier(lod: int, requested: float) -> float:
	if lod <= 0:
		return requested
	if lod == 1:
		return minf(requested, BEARD_LOD1_MAX_DENSITY_MULTIPLIER)
	return minf(requested, BEARD_LOD2_MAX_DENSITY_MULTIPLIER)

func _lod_population_scale(lod: int) -> float:
	if lod <= 0:
		return 1.0
	if lod == 1:
		return 0.50
	return 0.20

func _ensure_beard_surface_sampler() -> bool:
	if _beard_sampler_ready:
		return not _beard_sampler_triangles.is_empty()
	var started_usec: int = Time.get_ticks_usec()
	_beard_sampler_ready = true

	if not _ensure_morph_source_cache() or _head.is_empty() or _mount == null:
		_beard_sampler_build_ms = float(Time.get_ticks_usec() - started_usec) / 1000.0
		return false

	var center: Vector3 = _head["center"]
	var rx: float = maxf(float(_head["rx"]), 0.001)
	var ry: float = maxf(float(_head["ry"]), 0.001)
	var front: Vector3 = Vector3(0.0, 0.0, _front_sign)
	var total_area := 0.0

	for tri_index in _morph_triangles.size():
		var tri: Dictionary = _morph_triangles[tri_index]
		var a: Vector3 = tri["a"]
		var b: Vector3 = tri["b"]
		var c: Vector3 = tri["c"]
		var centroid: Vector3 = (a + b + c) / 3.0
		var ax: float = absf(centroid.x - center.x) / rx
		var down: float = (center.y - centroid.y) / ry
		if ax > 0.94 or down < 0.015 or down > 0.82:
			continue

		var cross_value: Vector3 = (b - a).cross(c - a)
		var twice_area: float = cross_value.length()
		if twice_area < 0.00000001:
			continue
		var normal: Vector3 = cross_value / twice_area
		var radial: Vector3 = centroid - center
		if radial.length_squared() > 0.000001 and normal.dot(radial) < 0.0:
			normal = -normal
		# Keep side/jaw triangles, but reject clearly rear-facing scalp/neck faces.
		if normal.dot(front) < -0.42:
			continue

		var area: float = twice_area * 0.5
		total_area += area
		_beard_sampler_triangles.append(tri_index)
		_beard_sampler_cumulative_area.append(total_area)

	_beard_sampler_total_area = total_area
	_beard_sampler_build_ms = float(Time.get_ticks_usec() - started_usec) / 1000.0
	return not _beard_sampler_triangles.is_empty() and total_area > 0.0

func _select_surface_triangle(area_position: float) -> int:
	if _beard_sampler_triangles.is_empty():
		return -1
	var lo := 0
	var hi: int = _beard_sampler_cumulative_area.size() - 1
	while lo < hi:
		var mid: int = (lo + hi) >> 1
		if area_position <= _beard_sampler_cumulative_area[mid]:
			hi = mid
		else:
			lo = mid + 1
	return _beard_sampler_triangles[lo]

func _sequence01(index: int, multiplier: float, rotation: float) -> float:
	return fposmod(0.5 + float(index + 1) * multiplier + rotation, 1.0)

func _sample_triangle_weights(sample_u: float, sample_v: float) -> Vector3:
	var su: float = sqrt(clampf(sample_u, 0.0, 1.0))
	return Vector3(1.0 - su, su * (1.0 - sample_v), su * sample_v)

## x = follicle acceptance weight
## y = desired lateral flow in world X
## z = desired vertical flow in world Y
## w = chin-length blend
func _beard_field(world_point: Vector3, coverage: float, fullness: float) -> Vector4:
	var center: Vector3 = _head["center"]
	var rx: float = maxf(float(_head["rx"]), 0.001)
	var ry: float = maxf(float(_head["ry"]), 0.001)
	var dx: float = world_point.x - center.x
	var ax: float = absf(dx) / rx
	var down: float = (center.y - world_point.y) / ry
	if ax > 0.88 or down < 0.02 or down > 0.79:
		return Vector4.ZERO

	# Keep actual lips and mouth opening clear, but allow a soul patch directly
	# below the lower edge of this ellipse.
	var mouth_center_y: float = center.y - ry * 0.22
	var mouth_x: float = absf(dx) / maxf(rx * 0.40, 0.001)
	var mouth_y: float = absf(world_point.y - mouth_center_y) / maxf(ry * 0.125, 0.001)
	if mouth_x * mouth_x + mouth_y * mouth_y < 1.0:
		return Vector4.ZERO

	var side: float = -1.0 if dx < 0.0 else 1.0
	var outerness: float = clampf((ax - 0.28) / 0.56, 0.0, 1.0)

	# Cheek field. The upper edge rises toward the sideburn while the inner cheek
	# starts lower, matching normal facial-hair distribution.
	var cheek_top: float = lerpf(0.235, 0.095, coverage) - 0.055 * outerness
	var cheek_bottom := 0.575
	var cheek_vertical: float = smoothstep(cheek_top, cheek_top + 0.055, down)
	cheek_vertical *= 1.0 - smoothstep(cheek_bottom - 0.055, cheek_bottom, down)
	var cheek_side: float = smoothstep(0.255, 0.335, ax)
	cheek_side *= 1.0 - smoothstep(0.80, 0.875, ax)
	var cheek: float = cheek_vertical * cheek_side
	cheek *= lerpf(0.72, 1.0, coverage)

	# Jaw field. This overlaps both cheek and chin intentionally, eliminating the
	# artificial diagonal gaps produced by the previous independent region masks.
	var jaw_u: float = clampf(ax / 0.82, 0.0, 1.0)
	var jaw_center: float = lerpf(0.705, 0.515, pow(jaw_u, 1.35))
	var jaw_half_width: float = 0.072 * lerpf(0.88, 1.16, clampf(fullness, 0.15, 1.35))
	var jaw_distance: float = absf(down - jaw_center)
	var jaw: float = 1.0 - smoothstep(jaw_half_width * 0.62, jaw_half_width, jaw_distance)
	jaw *= 1.0 - smoothstep(0.82, 0.88, ax)

	# Chin/goatee field, including the soul-patch connection below the mouth.
	var chin_x: float = 1.0 - smoothstep(0.285, 0.355, ax)
	var chin_y: float = smoothstep(0.315, 0.385, down)
	chin_y *= 1.0 - smoothstep(0.73, 0.79, down)
	var chin: float = chin_x * chin_y

	# Connector fills the natural transition from the lower cheek around the
	# mouth corners into the jaw/chin. This is the area that previously produced
	# the most obvious beard holes at high density.
	var connector_x: float = smoothstep(0.16, 0.24, ax) * (1.0 - smoothstep(0.47, 0.54, ax))
	var connector_y: float = smoothstep(0.39, 0.46, down) * (1.0 - smoothstep(0.61, 0.67, down))
	var connector: float = connector_x * connector_y * 0.94

	var weight: float = maxf(maxf(cheek, jaw), maxf(chin, connector))
	weight *= clampf(0.78 + fullness * 0.20, 0.72, 1.0)
	if weight <= 0.0001:
		return Vector4.ZERO

	var lateral := 0.0
	if chin >= jaw and chin >= cheek and chin >= connector:
		lateral = side * ax * 0.09
	elif connector >= cheek and connector >= jaw:
		lateral = -side * 0.06
	elif jaw >= cheek:
		lateral = -side * 0.22 * jaw_u
	else:
		var cheek_down: float = clampf((down - cheek_top) / maxf(cheek_bottom - cheek_top, 0.001), 0.0, 1.0)
		lateral = -side * lerpf(0.18, 0.08, cheek_down)

	var chin_mix: float = maxf(chin, connector * 0.55)
	chin_mix *= smoothstep(0.40, 0.72, down)
	return Vector4(clampf(weight, 0.0, 1.0), lateral, -1.0, clampf(chin_mix, 0.0, 1.0))

func _oriented_triangle_frame(tri: Dictionary, world_point: Vector3) -> Dictionary:
	var a_local: Vector3 = tri["al"]
	var b_local: Vector3 = tri["bl"]
	var c_local: Vector3 = tri["cl"]
	var tangent: Vector3 = b_local - a_local
	var normal: Vector3 = tangent.cross(c_local - a_local)
	if tangent.length_squared() < 0.00000001 or normal.length_squared() < 0.00000001:
		return {}
	tangent = tangent.normalized()
	normal = normal.normalized()
	var normal_world: Vector3 = (_mount.global_transform.basis * normal).normalized()
	var radial: Vector3 = world_point - Vector3(_head["center"])
	if radial.length_squared() > 0.000001 and normal_world.dot(radial) < 0.0:
		normal = -normal
	var bitangent: Vector3 = normal.cross(tangent).normalized()
	return {"t": tangent, "b": bitangent, "n": normal}

func _append_ring_vertex(
	vertices: PackedVector3Array,
	normals: PackedVector3Array,
	uvs: PackedVector2Array,
	coords: PackedVector3Array,
	vertex_index: int,
	center: Vector3,
	root: Vector3,
	axis_a: Vector3,
	axis_b: Vector3,
	radius: float,
	angle: float,
	v_coord: float,
	neutral_tangent: Vector3,
	neutral_bitangent: Vector3,
	neutral_normal: Vector3
) -> void:
	var radial: Vector3 = axis_a * cos(angle) + axis_b * sin(angle)
	var point: Vector3 = center + radial * radius
	vertices[vertex_index] = point
	normals[vertex_index] = radial.normalized()
	uvs[vertex_index] = Vector2(angle / TAU, v_coord)
	var root_offset: Vector3 = point - root
	coords[vertex_index] = Vector3(
		root_offset.dot(neutral_tangent),
		root_offset.dot(neutral_bitangent),
		root_offset.dot(neutral_normal)
	)

func _build_beard_mesh(lod: int) -> ArrayMesh:
	var started_usec: int = Time.get_ticks_usec()
	if not _ensure_beard_surface_sampler():
		_beard_mesh_build_ms[lod] = float(Time.get_ticks_usec() - started_usec) / 1000.0
		return ArrayMesh.new()

	var density: float = clampf(float(beard_settings.get("density", 1.0)), 0.05, 1.0)
	var requested_multiplier: float = clampf(float(beard_settings.get("density_multiplier", 30.0)), 1.0, 100.0)
	var density_multiplier: float = _effective_beard_density_multiplier(lod, requested_multiplier)
	var coverage: float = clampf(float(beard_settings.get("coverage", 0.68)), 0.0, 1.0)
	var fullness: float = clampf(float(beard_settings.get("fullness", 1.10)), 0.15, 1.35)
	var base_length: float = clampf(float(beard_settings.get("length", 0.0055)), 0.001, 0.045)
	var chin_length: float = clampf(float(beard_settings.get("chin_length", 0.0065)), 0.001, 0.060)
	var requested_width: float = clampf(float(beard_settings.get("strand_width", 0.00042)), 0.00008, 0.0015)
	var forward_offset: float = clampf(float(beard_settings.get("forward_offset", 0.00055)), -0.002, 0.010)
	var messiness: float = clampf(float(beard_settings.get("messiness", 0.12)), 0.0, 1.0)

	var base_count: int = clampi(int(round(lerpf(260.0, 920.0, density))), 180, 980)
	var target_hairs: int = int(round(float(base_count) * density_multiplier * BEARD_BASE_ACCEPTED_FRACTION * _lod_population_scale(lod)))
	target_hairs = clampi(target_hairs, 80, 70000)

	var ring_count := 3 if lod == 0 else 2
	var vertices_per_hair: int = ring_count * 3
	var indices_per_hair: int = (ring_count - 1) * 18
	var max_vertices: int = target_hairs * vertices_per_hair
	var max_indices: int = target_hairs * indices_per_hair
	var vertices := PackedVector3Array()
	var normals := PackedVector3Array()
	var uvs := PackedVector2Array()
	var root_coords := PackedVector3Array()
	var indices := PackedInt32Array()
	vertices.resize(max_vertices)
	normals.resize(max_vertices)
	uvs.resize(max_vertices)
	root_coords.resize(max_vertices)
	indices.resize(max_indices)

	var strands: Array[Dictionary] = []
	strands.resize(target_hairs)
	var used_triangles: Dictionary = {}
	var accepted := 0
	var index_cursor := 0
	var max_attempts: int = max(target_hairs * BEARD_MAX_SAMPLE_ATTEMPTS_FACTOR, 4096)
	var inv_mount_basis: Basis = _mount.global_transform.basis.inverse()

	for attempt in max_attempts:
		if accepted >= target_hairs:
			break
		var q_area: float = _sequence01(attempt, R2_A, 0.137)
		var tri_index: int = _select_surface_triangle(q_area * _beard_sampler_total_area)
		if tri_index < 0 or tri_index >= _morph_triangles.size():
			continue
		var tri: Dictionary = _morph_triangles[tri_index]
		var q_u: float = _sequence01(attempt, R2_B, 0.311)
		var q_v: float = _sequence01(attempt, R2_C, 0.719)
		var weights: Vector3 = _sample_triangle_weights(q_u, q_v)
		var a_world: Vector3 = tri["a"]
		var b_world: Vector3 = tri["b"]
		var c_world: Vector3 = tri["c"]
		var skin_world: Vector3 = a_world * weights.x + b_world * weights.y + c_world * weights.z
		var field: Vector4 = _beard_field(skin_world, coverage, fullness)
		if field.x <= 0.0001:
			continue
		var q_accept: float = _sequence01(attempt, R2_D, 0.473)
		if q_accept > field.x:
			continue

		var frame: Dictionary = _oriented_triangle_frame(tri, skin_world)
		if frame.is_empty():
			continue
		var neutral_tangent: Vector3 = frame["t"]
		var neutral_bitangent: Vector3 = frame["b"]
		var neutral_normal: Vector3 = frame["n"]
		var a_local: Vector3 = tri["al"]
		var b_local: Vector3 = tri["bl"]
		var c_local: Vector3 = tri["cl"]
		var skin_local: Vector3 = a_local * weights.x + b_local * weights.y + c_local * weights.z
		var clearance: float = BEARD_ROOT_CLEARANCE + forward_offset
		var root: Vector3 = skin_local + neutral_normal * clearance

		var q_flow: float = _sequence01(attempt, R2_E, 0.193)
		var lateral: float = field.y + (q_flow - 0.5) * 0.18 * messiness
		var vertical: float = field.z + (_sequence01(attempt, R2_C, 0.051) - 0.5) * 0.14 * messiness
		var desired_world: Vector3 = Vector3(lateral, vertical, 0.0)
		var desired_local: Vector3 = inv_mount_basis * desired_world
		var surface_flow: Vector3 = desired_local - neutral_normal * desired_local.dot(neutral_normal)
		if surface_flow.length_squared() < 0.00000001:
			surface_flow = neutral_bitangent
		else:
			surface_flow = surface_flow.normalized()
		var growth: Vector3 = (surface_flow + neutral_normal * BEARD_SURFACE_OUTWARD_BIAS).normalized()

		var length_scale: float = lerpf(0.86, 1.0, field.w)
		var strand_len: float = lerpf(base_length * length_scale, chin_length, field.w)
		strand_len *= lerpf(0.88, 1.12, _sequence01(attempt, R2_B, 0.847))
		var radius: float = requested_width * 0.23
		if lod == 1:
			radius *= 1.40
		elif lod >= 2:
			radius *= 1.88

		var mid: Vector3 = root + growth * strand_len * 0.52 + neutral_normal * lerpf(0.000035, 0.00012, q_flow)
		var tip: Vector3 = root + growth * strand_len
		var axis_a: Vector3 = growth.cross(neutral_normal)
		if axis_a.length_squared() < 0.00000001:
			axis_a = growth.cross(neutral_tangent)
		axis_a = axis_a.normalized()
		var axis_b: Vector3 = growth.cross(axis_a).normalized()

		var vertex_start: int = accepted * vertices_per_hair
		var ring_centers: Array[Vector3] = [root, tip]
		var ring_radii: Array[float] = [radius, radius * 0.07]
		if ring_count == 3:
			ring_centers = [root, mid, tip]
			ring_radii = [radius, radius * 0.78, radius * 0.06]

		for ring_i in ring_count:
			var v_coord: float = float(ring_i) / float(maxi(ring_count - 1, 1))
			for side_i in 3:
				var angle: float = TAU * float(side_i) / 3.0
				var vertex_index: int = vertex_start + ring_i * 3 + side_i
				_append_ring_vertex(
					vertices, normals, uvs, root_coords, vertex_index,
					ring_centers[ring_i], root, axis_a, axis_b, ring_radii[ring_i],
					angle, v_coord, neutral_tangent, neutral_bitangent, neutral_normal
				)

		for segment_i in ring_count - 1:
			var ring_a: int = vertex_start + segment_i * 3
			var ring_b: int = ring_a + 3
			for side_i in 3:
				var next_side: int = (side_i + 1) % 3
				indices[index_cursor] = ring_a + side_i
				indices[index_cursor + 1] = ring_b + side_i
				indices[index_cursor + 2] = ring_b + next_side
				indices[index_cursor + 3] = ring_a + side_i
				indices[index_cursor + 4] = ring_b + next_side
				indices[index_cursor + 5] = ring_a + next_side
				index_cursor += 6

		strands[accepted] = {
			"tri": tri_index,
			"weights": weights,
			"root": root,
			"clearance": clearance,
			"nt": neutral_tangent,
			"nb": neutral_bitangent,
			"nn": neutral_normal,
			"vertex_start": vertex_start,
			"vertex_count": vertices_per_hair
		}
		used_triangles[tri_index] = true
		accepted += 1

	strands.resize(accepted)
	vertices.resize(accepted * vertices_per_hair)
	normals.resize(accepted * vertices_per_hair)
	uvs.resize(accepted * vertices_per_hair)
	root_coords.resize(accepted * vertices_per_hair)
	indices.resize(index_cursor)

	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_NORMAL] = normals
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	if not vertices.is_empty():
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
		mesh.custom_aabb = mesh.get_aabb().grow(_character_height * 0.012)

	_beard_strands_by_lod[lod] = strands
	_beard_vertex_coords_by_lod[lod] = root_coords
	_beard_used_triangles_by_lod[lod] = used_triangles
	_beard_hair_count_by_lod[lod] = accepted
	_beard_mesh_build_ms[lod] = float(Time.get_ticks_usec() - started_usec) / 1000.0
	return mesh

func _select_surface_beard_shapes(lod: int) -> void:
	_beard_shape_names.clear()
	_beard_source_indices = PackedInt32Array()
	var used_triangles: Dictionary = _beard_used_triangles_by_lod.get(lod, {})
	if used_triangles.is_empty() or _body_mesh == null or _body_mesh.mesh == null:
		return

	for shape_index in _source_shape_count:
		var max_motion := 0.0
		for tri_variant in used_triangles.keys():
			var tri_index: int = int(tri_variant)
			if tri_index < 0 or tri_index >= _morph_triangles.size():
				continue
			var tri: Dictionary = _morph_triangles[tri_index]
			var surface: int = int(tri["surface"])
			var da: Vector3 = _source_vertex_delta(surface, shape_index, int(tri["ia"]))
			var db: Vector3 = _source_vertex_delta(surface, shape_index, int(tri["ib"]))
			var dc: Vector3 = _source_vertex_delta(surface, shape_index, int(tri["ic"]))
			max_motion = maxf(max_motion, maxf(da.length(), maxf(db.length(), dc.length())))
			if max_motion >= FACIAL_MORPH_RELEVANCE:
				break
		if max_motion < FACIAL_MORPH_RELEVANCE:
			continue
		_beard_source_indices.append(shape_index)
		_beard_shape_names.append(_body_mesh.mesh.get_blend_shape_name(shape_index))

func _shape_triangle_frame(tri_index: int, shape_index: int, neutral_normal: Vector3) -> Dictionary:
	if tri_index < 0 or tri_index >= _morph_triangles.size():
		return {}
	var tri: Dictionary = _morph_triangles[tri_index]
	var surface: int = int(tri["surface"])
	var da: Vector3 = _source_vertex_delta(surface, shape_index, int(tri["ia"]))
	var db: Vector3 = _source_vertex_delta(surface, shape_index, int(tri["ib"]))
	var dc: Vector3 = _source_vertex_delta(surface, shape_index, int(tri["ic"]))
	var a: Vector3 = Vector3(tri["al"]) + da
	var b: Vector3 = Vector3(tri["bl"]) + db
	var c: Vector3 = Vector3(tri["cl"]) + dc
	var tangent: Vector3 = b - a
	var normal: Vector3 = tangent.cross(c - a)
	if tangent.length_squared() < 0.00000001 or normal.length_squared() < 0.00000001:
		return {}
	tangent = tangent.normalized()
	normal = normal.normalized()
	if normal.dot(neutral_normal) < 0.0:
		normal = -normal
	var bitangent: Vector3 = normal.cross(tangent).normalized()
	return {"a": a, "b0": b, "c": c, "t": tangent, "bt": bitangent, "n": normal}

func _create_surface_root_morph_mesh(neutral_mesh: ArrayMesh, lod: int) -> ArrayMesh:
	if neutral_mesh == null or lod >= 2 or neutral_mesh.get_surface_count() <= 0:
		return neutral_mesh
	var strands: Array = _beard_strands_by_lod.get(lod, [])
	if strands.is_empty():
		return neutral_mesh
	var root_coords: PackedVector3Array = _beard_vertex_coords_by_lod.get(lod, PackedVector3Array())
	var base_arrays: Array = neutral_mesh.surface_get_arrays(0)
	if base_arrays.size() <= Mesh.ARRAY_VERTEX or base_arrays[Mesh.ARRAY_VERTEX] == null:
		return neutral_mesh
	var base_vertices: PackedVector3Array = base_arrays[Mesh.ARRAY_VERTEX]
	if base_vertices.is_empty() or root_coords.size() != base_vertices.size():
		return neutral_mesh

	if lod == 0:
		_select_surface_beard_shapes(0)
	if _beard_shape_names.is_empty():
		return neutral_mesh

	var output := ArrayMesh.new()
	output.blend_shape_mode = Mesh.BLEND_SHAPE_MODE_RELATIVE
	for shape_name in _beard_shape_names:
		output.add_blend_shape(shape_name)

	var has_normals: bool = base_arrays.size() > Mesh.ARRAY_NORMAL and base_arrays[Mesh.ARRAY_NORMAL] != null
	var zero_normals := PackedVector3Array()
	if has_normals:
		zero_normals.resize(base_vertices.size())
	var blend_arrays: Array = []

	for bound_i in _beard_source_indices.size():
		var source_shape_index: int = _beard_source_indices[bound_i]
		var delta_vertices := PackedVector3Array()
		delta_vertices.resize(base_vertices.size())
		var frame_cache: Dictionary = {}

		for strand_variant in strands:
			var strand: Dictionary = strand_variant
			var tri_index: int = int(strand["tri"])
			var neutral_normal: Vector3 = strand["nn"]
			var shaped_frame: Dictionary
			if frame_cache.has(tri_index):
				shaped_frame = frame_cache[tri_index]
			else:
				shaped_frame = _shape_triangle_frame(tri_index, source_shape_index, neutral_normal)
				frame_cache[tri_index] = shaped_frame
			if shaped_frame.is_empty():
				continue

			var tri: Dictionary = _morph_triangles[tri_index]
			var weights: Vector3 = strand["weights"]
			var shaped_skin: Vector3 = Vector3(shaped_frame["a"]) * weights.x
			shaped_skin += Vector3(shaped_frame["b0"]) * weights.y
			shaped_skin += Vector3(shaped_frame["c"]) * weights.z
			var shaped_normal: Vector3 = shaped_frame["n"]
			var shaped_root: Vector3 = shaped_skin + shaped_normal * float(strand["clearance"])
			var shaped_tangent: Vector3 = shaped_frame["t"]
			var shaped_bitangent: Vector3 = shaped_frame["bt"]

			var vertex_start: int = int(strand["vertex_start"])
			var vertex_count: int = int(strand["vertex_count"])
			for local_i in vertex_count:
				var vertex_i: int = vertex_start + local_i
				var coord: Vector3 = root_coords[vertex_i]
				var shaped_offset: Vector3 = shaped_tangent * coord.x
				shaped_offset += shaped_bitangent * coord.y
				shaped_offset += shaped_normal * coord.z
				delta_vertices[vertex_i] = shaped_root + shaped_offset - base_vertices[vertex_i]

		var shape_arrays: Array = []
		shape_arrays.resize(Mesh.ARRAY_MAX)
		shape_arrays[Mesh.ARRAY_VERTEX] = delta_vertices
		if has_normals:
			# A shared zero-normal delta array is safe for every relative blend shape
			# and avoids allocating/filling another huge buffer for each expression.
			shape_arrays[Mesh.ARRAY_NORMAL] = zero_normals
		blend_arrays.append(shape_arrays)

	output.add_surface_from_arrays(neutral_mesh.surface_get_primitive_type(0), base_arrays, blend_arrays)
	output.custom_aabb = neutral_mesh.get_aabb().grow(_character_height * 0.018)
	return output

func _create_facial_morph_bound_mesh(neutral_mesh: ArrayMesh, lod: int, is_mustache: bool) -> ArrayMesh:
	if is_mustache:
		return super._create_facial_morph_bound_mesh(neutral_mesh, lod, true)
	var started_usec: int = Time.get_ticks_usec()
	var result: ArrayMesh = _create_surface_root_morph_mesh(neutral_mesh, lod)
	_beard_morph_build_ms[lod] = float(Time.get_ticks_usec() - started_usec) / 1000.0
	return result

func diagnostics() -> String:
	var base: String = super.diagnostics()
	var mesh0: float = float(_beard_mesh_build_ms.get(0, 0.0))
	var mesh1: float = float(_beard_mesh_build_ms.get(1, 0.0))
	var mesh2: float = float(_beard_mesh_build_ms.get(2, 0.0))
	var morph0: float = float(_beard_morph_build_ms.get(0, 0.0))
	var morph1: float = float(_beard_morph_build_ms.get(1, 0.0))
	var hair0: int = int(_beard_hair_count_by_lod.get(0, 0))
	var hair1: int = int(_beard_hair_count_by_lod.get(1, 0))
	var hair2: int = int(_beard_hair_count_by_lod.get(2, 0))
	return "%s • surface beard %d/%d/%d hairs • sampler %.0f ms • mesh %.0f/%.0f/%.0f ms • root morph %.0f/%.0f ms" % [
		base,
		hair0,
		hair1,
		hair2,
		_beard_sampler_build_ms,
		mesh0,
		mesh1,
		mesh2,
		morph0,
		morph1
	]
