extends "res://scripts/character/fast_morph_bound_brow_groom.gd"

## Procedural mustache + beard using the same pipeline as the eyebrows:
## - roots and strand control points are projected onto the actual neutral face;
## - LOD0 uses full triangular-prism strands, LOD1 keeps half the follicles,
##   and LOD2 keeps a sparse cheap representation;
## - LOD0/1 receive exact barycentric transfer of the imported face morph field,
##   so mouth, cheek, jaw and other expressions move the facial hair with skin;
## - expensive binding/morph construction happens only when the groom is built.

const FACIAL_LOD1_DISTANCE := 6.0
const FACIAL_LOD2_DISTANCE := 15.0
const FACIAL_LOD_CHECK_INTERVAL := 0.10
const FACIAL_MORPH_SYNC_INTERVAL := 1.0 / 60.0
const FACIAL_MORPH_EPSILON := 0.0001
const FACIAL_MORPH_RELEVANCE := 0.00005
const FACIAL_LOD1_STRIDE := 2
const FACIAL_LOD2_STRIDE := 5

var mustache_settings := {
	"enabled": true,
	"density": 0.72,
	"width": 0.82,
	"thickness": 0.78,
	"length": 0.0075,
	"strand_width": 0.00034,
	"middle_gap": 0.006,
	"droop": 0.32,
	"height_offset": 0.0,
	"forward_offset": 0.00065,
	"messiness": 0.18,
	"color": Color("3b2b21")
}

var beard_settings := {
	"enabled": true,
	"density": 0.68,
	"coverage": 0.58,
	"fullness": 0.72,
	"length": 0.008,
	"chin_length": 0.014,
	"strand_width": 0.00036,
	"height_offset": 0.0,
	"forward_offset": 0.00065,
	"messiness": 0.22,
	"color": Color("3b2b21")
}

var _mustache_lod0: MeshInstance3D
var _mustache_lod1: MeshInstance3D
var _mustache_lod2: MeshInstance3D
var _beard_lod0: MeshInstance3D
var _beard_lod1: MeshInstance3D
var _beard_lod2: MeshInstance3D
var _facial_hair_material: StandardMaterial3D

var _mustache_shape_names: Array[StringName] = []
var _mustache_source_indices: PackedInt32Array = PackedInt32Array()
var _mustache_last_weights: PackedFloat32Array = PackedFloat32Array()
var _beard_shape_names: Array[StringName] = []
var _beard_source_indices: PackedInt32Array = PackedInt32Array()
var _beard_last_weights: PackedFloat32Array = PackedFloat32Array()

var _facial_lod_elapsed := 0.0
var _facial_morph_elapsed := 0.0
var _active_facial_lod := 0
var _last_facial_distance := -1.0
var _facial_bind_failures := 0

func _create_render_nodes() -> void:
	super._create_render_nodes()
	_mustache_lod0 = _make_facial_node("ProceduralMustacheLOD0")
	_mustache_lod1 = _make_facial_node("ProceduralMustacheLOD1")
	_mustache_lod2 = _make_facial_node("ProceduralMustacheLOD2")
	_beard_lod0 = _make_facial_node("ProceduralBeardLOD0")
	_beard_lod1 = _make_facial_node("ProceduralBeardLOD1")
	_beard_lod2 = _make_facial_node("ProceduralBeardLOD2")
	_facial_hair_material = StandardMaterial3D.new()
	_facial_hair_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	_facial_hair_material.roughness = 0.50
	_facial_hair_material.metallic = 0.0
	_facial_hair_material.anisotropy_enabled = true
	_facial_hair_material.anisotropy = 0.55

func _make_facial_node(node_name: String) -> MeshInstance3D:
	var node := MeshInstance3D.new()
	node.name = node_name
	node.visible = false
	_mount.add_child(node)
	return node

func _apply_material_settings() -> void:
	super._apply_material_settings()
	if _facial_hair_material == null:
		return
	var mustache_color: Color = mustache_settings.get("color", Color("3b2b21"))
	var beard_color: Color = beard_settings.get("color", mustache_color)
	_facial_hair_material.albedo_color = mustache_color.lerp(beard_color, 0.5)

func rebuild_all() -> void:
	super.rebuild_all()
	rebuild_facial_hair()

func apply_mustache(settings: Dictionary) -> void:
	for key in settings.keys():
		mustache_settings[key] = settings[key]
	_apply_material_settings()
	rebuild_facial_hair()

func apply_beard(settings: Dictionary) -> void:
	for key in settings.keys():
		beard_settings[key] = settings[key]
	_apply_material_settings()
	rebuild_facial_hair()

func apply_facial_hair(mustache: Dictionary, beard: Dictionary) -> void:
	for key in mustache.keys():
		mustache_settings[key] = mustache[key]
	for key in beard.keys():
		beard_settings[key] = beard[key]
	_apply_material_settings()
	rebuild_facial_hair()

func rebuild_facial_hair() -> void:
	if _mount == null or _head.is_empty():
		return
	if _mustache_lod0 == null or _beard_lod0 == null:
		return

	_fast_binding_cache.clear()
	_facial_bind_failures = 0
	_ensure_morph_source_cache()

	_mustache_shape_names.clear()
	_mustache_source_indices = PackedInt32Array()
	_beard_shape_names.clear()
	_beard_source_indices = PackedInt32Array()

	if bool(mustache_settings.get("enabled", true)):
		_mustache_lod0.mesh = _create_facial_morph_bound_mesh(_build_mustache_mesh(0), 0, true)
		_mustache_lod1.mesh = _create_facial_morph_bound_mesh(_build_mustache_mesh(1), 1, true)
		_mustache_lod2.mesh = _build_mustache_mesh(2)
	else:
		_mustache_lod0.mesh = null
		_mustache_lod1.mesh = null
		_mustache_lod2.mesh = null

	if bool(beard_settings.get("enabled", true)):
		_beard_lod0.mesh = _create_facial_morph_bound_mesh(_build_beard_mesh(0), 0, false)
		_beard_lod1.mesh = _create_facial_morph_bound_mesh(_build_beard_mesh(1), 1, false)
		_beard_lod2.mesh = _build_beard_mesh(2)
	else:
		_beard_lod0.mesh = null
		_beard_lod1.mesh = null
		_beard_lod2.mesh = null

	for node in [_mustache_lod0, _mustache_lod1, _mustache_lod2, _beard_lod0, _beard_lod1, _beard_lod2]:
		var mesh_node: MeshInstance3D = node
		if mesh_node != null:
			mesh_node.material_override = _facial_hair_material

	_reset_facial_weight_caches()
	_sync_facial_morph_weights(true)
	_update_facial_lod_visibility(true)

func _process(delta: float) -> void:
	super._process(delta)

	_facial_lod_elapsed += delta
	if _facial_lod_elapsed >= FACIAL_LOD_CHECK_INTERVAL:
		_facial_lod_elapsed = 0.0
		_update_facial_lod_visibility()

	_facial_morph_elapsed += delta
	if _facial_morph_elapsed >= FACIAL_MORPH_SYNC_INTERVAL:
		_facial_morph_elapsed = 0.0
		_sync_facial_morph_weights(false)

## Broaden the optimized source cache from the eyebrow strip to the whole front
## face. The spatial grid still keeps per-strand binding searches local.
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
	_morph_cell_size = maxf(_character_height * 0.010, 0.012)

	var center: Vector3 = _head["center"]
	var rx: float = float(_head["rx"])
	var ry: float = float(_head["ry"])
	var min_x: float = center.x - rx * 1.18
	var max_x: float = center.x + rx * 1.18
	var min_y: float = center.y - ry * 0.86
	var max_y: float = center.y + ry * 0.56
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
			var indexed_triangle_count: int = indices.size() / 3
			for tri_i in indexed_triangle_count:
				var ia: int = indices[tri_i * 3]
				var ib: int = indices[tri_i * 3 + 1]
				var ic: int = indices[tri_i * 3 + 2]
				if ia < 0 or ib < 0 or ic < 0 or ia >= vertices.size() or ib >= vertices.size() or ic >= vertices.size():
					continue
				_append_fast_triangle(surface, ia, ib, ic, vertices, center, front_axis, min_x, max_x, min_y, max_y)
		else:
			var plain_triangle_count: int = vertices.size() / 3
			for tri_i in plain_triangle_count:
				var base: int = tri_i * 3
				_append_fast_triangle(surface, base, base + 1, base + 2, vertices, center, front_axis, min_x, max_x, min_y, max_y)

	if _morph_triangles.is_empty():
		_morph_status = "no morphable face triangles"
		return false
	_morph_status = "fast full-face barycentric binding"
	return true

func _project_facial_point(x: float, y: float, offset: float) -> Dictionary:
	var center: Vector3 = _head["center"]
	var rx: float = float(_head["rx"])
	var ry: float = float(_head["ry"])
	var rz: float = float(_head["rz"])
	var fallback_z: float = _ellipsoid_front_z(x, y, center, rx, ry, rz)
	var target := Vector3(x, y, fallback_z)
	var front := Vector3(0.0, 0.0, _front_sign)
	var outward: Vector3 = target - center
	if outward.length_squared() < 0.000001:
		outward = front
	else:
		outward = outward.normalized()
	if outward.dot(front) < 0.08:
		outward = (outward * 0.58 + front * 0.42).normalized()

	var ray_origin: Vector3 = target + outward * maxf(_character_height * 0.035, 0.045)
	var ray_direction: Vector3 = -outward
	var candidates: Array[int] = _candidate_triangles(target, 1)
	var hit: Dictionary = _project_on_candidates(ray_origin, ray_direction, outward, candidates)
	if hit.is_empty():
		candidates = _candidate_triangles(target, 2)
		hit = _project_on_candidates(ray_origin, ray_direction, outward, candidates)
	if not hit.is_empty():
		var point: Vector3 = hit["point"]
		var normal: Vector3 = hit["normal"]
		return {"point": point + normal * (0.00022 + offset), "normal": normal, "projected": true}
	return {"point": target + outward * (0.0008 + offset), "normal": outward, "projected": false}

func _project_on_candidates(origin: Vector3, direction: Vector3, outward: Vector3, candidates: Array[int]) -> Dictionary:
	var best_t := INF
	var best_normal := outward
	for tri_index in candidates:
		if tri_index < 0 or tri_index >= _morph_triangles.size():
			continue
		var tri: Dictionary = _morph_triangles[tri_index]
		var a: Vector3 = tri["a"]
		var b: Vector3 = tri["b"]
		var c: Vector3 = tri["c"]
		var t: float = _ray_triangle_distance(origin, direction, a, b, c)
		if t < 0.0 or t >= best_t:
			continue
		best_t = t
		var normal: Vector3 = (b - a).cross(c - a)
		if normal.length_squared() > 0.00000001:
			normal = normal.normalized()
			if normal.dot(outward) < 0.0:
				normal = -normal
			best_normal = normal
	if best_t >= INF:
		return {}
	return {"point": origin + direction * best_t, "normal": best_normal}

func _build_mustache_mesh(lod: int) -> ArrayMesh:
	var density: float = clampf(float(mustache_settings.get("density", 0.72)), 0.05, 1.0)
	var width_scale: float = clampf(float(mustache_settings.get("width", 0.82)), 0.45, 1.35)
	var thickness: float = clampf(float(mustache_settings.get("thickness", 0.78)), 0.30, 1.80)
	var length: float = clampf(float(mustache_settings.get("length", 0.0075)), 0.0015, 0.030)
	var requested_width: float = clampf(float(mustache_settings.get("strand_width", 0.00034)), 0.00008, 0.0012)
	var middle_gap: float = clampf(float(mustache_settings.get("middle_gap", 0.006)), 0.0, 0.030)
	var droop: float = clampf(float(mustache_settings.get("droop", 0.32)), 0.0, 1.0)
	var height_offset: float = clampf(float(mustache_settings.get("height_offset", 0.0)), -0.025, 0.025)
	var forward_offset: float = clampf(float(mustache_settings.get("forward_offset", 0.00065)), -0.002, 0.008)
	var messiness: float = clampf(float(mustache_settings.get("messiness", 0.18)), 0.0, 1.0)

	var center: Vector3 = _head["center"]
	var rx: float = float(_head["rx"])
	var ry: float = float(_head["ry"])
	var count_per_side: int = clampi(int(round(lerpf(70.0, 245.0, density))), 48, 270)
	var outer_x: float = rx * 0.58 * width_scale
	var inner_x: float = minf(middle_gap * 0.5, outer_x * 0.72)
	var base_y: float = center.y - ry * 0.16 + height_offset
	var radius: float = requested_width * 0.24
	if lod == 1:
		radius *= 1.40
	elif lod >= 2:
		radius *= 1.85

	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	for side_i in 2:
		var side: float = -1.0 if side_i == 0 else 1.0
		for i in count_per_side:
			if lod == 1 and (i % FACIAL_LOD1_STRIDE) != 0:
				continue
			if lod >= 2 and (i % FACIAL_LOD2_STRIDE) != 0:
				continue

			var h0: float = _hash01(float(i) * 19.31 + side * 3.1)
			var h1: float = _hash01(float(i) * 37.73 + side * 11.2)
			var h2: float = _hash01(float(i) * 71.17 + side * 5.4)
			var h3: float = _hash01(float(i) * 113.9 + side * 17.7)
			var t: float = (float(i) + 0.5) / float(count_per_side)
			t = clampf(t + (h0 - 0.5) / float(count_per_side) * lerpf(0.4, 2.8, messiness), 0.0, 1.0)

			var x_offset: float = lerpf(inner_x, outer_x, t)
			var x: float = center.x + side * (x_offset + (h1 - 0.5) * _character_height * 0.0009 * messiness)
			var band_half: float = ry * 0.030 * thickness * lerpf(1.0, 0.58, t)
			var y: float = base_y + (h2 - 0.5) * 2.0 * band_half
			y -= pow(t, 1.55) * ry * 0.050 * droop

			var root_place: Dictionary = _project_facial_point(x, y, forward_offset)
			var root: Vector3 = root_place["point"]
			var root_normal: Vector3 = root_place["normal"]

			var lateral: float = lerpf(0.18, 0.72, t)
			var vertical: float = -lerpf(1.0, 0.42, t)
			lateral += (h2 - 0.5) * 0.24 * messiness
			vertical += (h3 - 0.5) * 0.30 * messiness
			var flow := Vector2(side * lateral, vertical).normalized()
			var strand_len: float = length * lerpf(0.72, 1.18, h0)
			strand_len *= lerpf(0.78, 1.05, sin(t * PI))

			var tip_x: float = x + flow.x * strand_len
			var tip_y: float = y + flow.y * strand_len
			var tip_place: Dictionary = _project_facial_point(tip_x, tip_y, forward_offset)
			var tip: Vector3 = tip_place["point"]
			var tip_normal: Vector3 = tip_place["normal"]
			var root_local: Vector3 = _mount.to_local(root)
			var tip_local: Vector3 = _mount.to_local(tip)
			var inv_basis: Basis = _mount.global_transform.basis.inverse()
			var root_normal_local: Vector3 = (inv_basis * root_normal).normalized()
			var tip_normal_local: Vector3 = (inv_basis * tip_normal).normalized()

			if lod >= 1:
				_add_natural_tri_tube_segment(st, root_local, tip_local, root_normal_local, tip_normal_local, radius, radius * 0.08, 0.0, 1.0)
			else:
				var mid_x: float = x + flow.x * strand_len * 0.55
				var mid_y: float = y + flow.y * strand_len * 0.55
				var mid_place: Dictionary = _project_facial_point(mid_x, mid_y, forward_offset)
				var mid: Vector3 = mid_place["point"]
				var mid_normal: Vector3 = mid_place["normal"]
				mid += mid_normal * lerpf(0.00005, 0.00020, h3)
				var mid_local: Vector3 = _mount.to_local(mid)
				var mid_normal_local: Vector3 = (inv_basis * mid_normal).normalized()
				_add_natural_tri_tube_segment(st, root_local, mid_local, root_normal_local, mid_normal_local, radius, radius * 0.76, 0.0, 0.55)
				_add_natural_tri_tube_segment(st, mid_local, tip_local, mid_normal_local, tip_normal_local, radius * 0.76, radius * 0.06, 0.55, 1.0)

	st.generate_normals()
	st.generate_tangents()
	return st.commit()

func _build_beard_mesh(lod: int) -> ArrayMesh:
	var density: float = clampf(float(beard_settings.get("density", 0.68)), 0.05, 1.0)
	var coverage: float = clampf(float(beard_settings.get("coverage", 0.58)), 0.0, 1.0)
	var fullness: float = clampf(float(beard_settings.get("fullness", 0.72)), 0.15, 1.35)
	var base_length: float = clampf(float(beard_settings.get("length", 0.008)), 0.001, 0.045)
	var chin_length: float = clampf(float(beard_settings.get("chin_length", 0.014)), 0.001, 0.060)
	var requested_width: float = clampf(float(beard_settings.get("strand_width", 0.00036)), 0.00008, 0.0015)
	var height_offset: float = clampf(float(beard_settings.get("height_offset", 0.0)), -0.035, 0.035)
	var forward_offset: float = clampf(float(beard_settings.get("forward_offset", 0.00065)), -0.002, 0.010)
	var messiness: float = clampf(float(beard_settings.get("messiness", 0.22)), 0.0, 1.0)

	var center: Vector3 = _head["center"]
	var rx: float = float(_head["rx"])
	var ry: float = float(_head["ry"])
	var target_count: int = clampi(int(round(lerpf(220.0, 920.0, density))), 150, 980)
	var radius: float = requested_width * 0.23
	if lod == 1:
		radius *= 1.42
	elif lod >= 2:
		radius *= 1.95

	var top_y: float = center.y + ry * lerpf(-0.11, 0.075, coverage) + height_offset
	var bottom_y: float = center.y - ry * 0.73 + height_offset
	var mouth_center_y: float = center.y - ry * 0.22 + height_offset
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)

	for i in target_count:
		if lod == 1 and (i % FACIAL_LOD1_STRIDE) != 0:
			continue
		if lod >= 2 and (i % FACIAL_LOD2_STRIDE) != 0:
			continue

		var h0: float = _hash01(float(i) * 17.41 + 0.7)
		var h1: float = _hash01(float(i) * 43.87 + 2.1)
		var h2: float = _hash01(float(i) * 79.13 + 5.6)
		var h3: float = _hash01(float(i) * 127.7 + 1.9)
		var h4: float = _hash01(float(i) * 181.3 + 8.4)
		var v: float = h0
		var y: float = lerpf(top_y, bottom_y, v)

		var lower_factor: float = smoothstep(0.25, 1.0, v)
		var half_width: float = rx * lerpf(0.82, 0.56, lower_factor)
		half_width *= lerpf(0.72, 1.08, fullness)
		var x_norm: float = h1 * 2.0 - 1.0
		var x: float = center.x + x_norm * half_width

		# Keep the actual lip/mouth opening clean while retaining goatee growth
		# directly below it and side growth beside it.
		var mouth_x: float = absf(x - center.x) / maxf(rx * 0.43, 0.001)
		var mouth_y: float = absf(y - mouth_center_y) / maxf(ry * 0.115, 0.001)
		if mouth_x * mouth_x + mouth_y * mouth_y < 1.0:
			continue

		# Upper central cheeks naturally carry less beard than the jaw and sides.
		var center_factor: float = absf(x - center.x) / maxf(half_width, 0.001)
		var keep: float = lerpf(0.48 + coverage * 0.28, 1.0, maxf(lower_factor, center_factor))
		keep *= clampf(fullness, 0.15, 1.0)
		if h4 > keep:
			continue

		x += (h2 - 0.5) * _character_height * 0.0011 * messiness
		y += (h3 - 0.5) * _character_height * 0.0010 * messiness
		var root_place: Dictionary = _project_facial_point(x, y, forward_offset)
		var root: Vector3 = root_place["point"]
		var root_normal: Vector3 = root_place["normal"]

		var chin_factor: float = (1.0 - clampf(absf(x - center.x) / maxf(rx * 0.62, 0.001), 0.0, 1.0)) * lower_factor
		var strand_len: float = lerpf(base_length, chin_length, chin_factor)
		strand_len *= lerpf(0.78, 1.20, h2)
		var side_sign: float = -1.0 if x < center.x else 1.0
		var lateral: float = side_sign * center_factor * 0.22
		var vertical: float = -1.0
		lateral += (h2 - 0.5) * 0.22 * messiness
		vertical += (h3 - 0.5) * 0.18 * messiness
		var flow := Vector2(lateral, vertical).normalized()

		var tip_x: float = x + flow.x * strand_len
		var tip_y: float = y + flow.y * strand_len
		var tip_place: Dictionary = _project_facial_point(tip_x, tip_y, forward_offset)
		var tip: Vector3 = tip_place["point"]
		var tip_normal: Vector3 = tip_place["normal"]
		var inv_basis: Basis = _mount.global_transform.basis.inverse()
		var root_local: Vector3 = _mount.to_local(root)
		var tip_local: Vector3 = _mount.to_local(tip)
		var root_normal_local: Vector3 = (inv_basis * root_normal).normalized()
		var tip_normal_local: Vector3 = (inv_basis * tip_normal).normalized()

		if lod >= 1:
			_add_natural_tri_tube_segment(st, root_local, tip_local, root_normal_local, tip_normal_local, radius, radius * 0.07, 0.0, 1.0)
		else:
			var mid_x: float = x + flow.x * strand_len * 0.52
			var mid_y: float = y + flow.y * strand_len * 0.52
			var mid_place: Dictionary = _project_facial_point(mid_x, mid_y, forward_offset)
			var mid: Vector3 = mid_place["point"]
			var mid_normal: Vector3 = mid_place["normal"]
			mid += mid_normal * lerpf(0.00006, 0.00028 + strand_len * 0.012, h3)
			var mid_local: Vector3 = _mount.to_local(mid)
			var mid_normal_local: Vector3 = (inv_basis * mid_normal).normalized()
			_add_natural_tri_tube_segment(st, root_local, mid_local, root_normal_local, mid_normal_local, radius, radius * 0.78, 0.0, 0.52)
			_add_natural_tri_tube_segment(st, mid_local, tip_local, mid_normal_local, tip_normal_local, radius * 0.78, radius * 0.06, 0.52, 1.0)

	st.generate_normals()
	st.generate_tangents()
	return st.commit()

func _create_facial_morph_bound_mesh(neutral_mesh: ArrayMesh, lod: int, is_mustache: bool) -> ArrayMesh:
	if neutral_mesh == null or lod >= 2 or neutral_mesh.get_surface_count() <= 0:
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
			_facial_bind_failures += 1
			continue
		var surface: int = int(binding["surface"])
		for source_index in [int(binding["ia"]), int(binding["ib"]), int(binding["ic"])]:
			used_source_vertices["%d:%d" % [surface, source_index]] = Vector2i(surface, source_index)

	if lod == 0:
		_select_facial_shapes(used_source_vertices, is_mustache)
	var shape_names: Array[StringName] = _mustache_shape_names if is_mustache else _beard_shape_names
	var source_indices: PackedInt32Array = _mustache_source_indices if is_mustache else _beard_source_indices
	if shape_names.is_empty():
		return neutral_mesh

	var output := ArrayMesh.new()
	output.blend_shape_mode = Mesh.BLEND_SHAPE_MODE_RELATIVE
	for shape_name in shape_names:
		output.add_blend_shape(shape_name)

	var blend_arrays: Array = []
	var has_normals: bool = base_arrays.size() > Mesh.ARRAY_NORMAL and base_arrays[Mesh.ARRAY_NORMAL] != null
	var has_tangents: bool = base_arrays.size() > Mesh.ARRAY_TANGENT and base_arrays[Mesh.ARRAY_TANGENT] != null
	for source_shape_index in source_indices:
		var delta_vertices := PackedVector3Array()
		delta_vertices.resize(vertices.size())
		for vertex_i in vertices.size():
			var binding: Dictionary = bindings[vertex_i]
			delta_vertices[vertex_i] = Vector3.ZERO if binding.is_empty() else _binding_shape_delta(binding, int(source_shape_index))

		var shape_arrays: Array = []
		shape_arrays.resize(Mesh.ARRAY_MAX)
		shape_arrays[Mesh.ARRAY_VERTEX] = delta_vertices
		if has_normals:
			var zero_normals := PackedVector3Array()
			zero_normals.resize(vertices.size())
			shape_arrays[Mesh.ARRAY_NORMAL] = zero_normals
		if has_tangents:
			var zero_tangents := PackedFloat32Array()
			zero_tangents.resize(vertices.size() * 4)
			shape_arrays[Mesh.ARRAY_TANGENT] = zero_tangents
		blend_arrays.append(shape_arrays)

	output.add_surface_from_arrays(neutral_mesh.surface_get_primitive_type(0), base_arrays, blend_arrays)
	output.custom_aabb = neutral_mesh.get_aabb().grow(_character_height * 0.018)
	return output

func _select_facial_shapes(used_source_vertices: Dictionary, is_mustache: bool) -> void:
	var names: Array[StringName] = []
	var indices := PackedInt32Array()
	for shape_index in _source_shape_count:
		var max_motion := 0.0
		for source_variant in used_source_vertices.values():
			var source_vertex: Vector2i = source_variant
			var delta: Vector3 = _source_vertex_delta(source_vertex.x, shape_index, source_vertex.y)
			max_motion = maxf(max_motion, delta.length())
			if max_motion >= FACIAL_MORPH_RELEVANCE:
				break
		if max_motion < FACIAL_MORPH_RELEVANCE:
			continue
		indices.append(shape_index)
		names.append(_body_mesh.mesh.get_blend_shape_name(shape_index))

	if is_mustache:
		_mustache_shape_names = names
		_mustache_source_indices = indices
	else:
		_beard_shape_names = names
		_beard_source_indices = indices

func _reset_facial_weight_caches() -> void:
	_mustache_last_weights = PackedFloat32Array()
	_mustache_last_weights.resize(_mustache_shape_names.size())
	_mustache_last_weights.fill(9999.0)
	_beard_last_weights = PackedFloat32Array()
	_beard_last_weights.resize(_beard_shape_names.size())
	_beard_last_weights.fill(9999.0)

func _sync_facial_morph_weights(force: bool) -> void:
	_sync_facial_kind(_mustache_shape_names, _mustache_source_indices, _mustache_last_weights, [_mustache_lod0, _mustache_lod1], force)
	_sync_facial_kind(_beard_shape_names, _beard_source_indices, _beard_last_weights, [_beard_lod0, _beard_lod1], force)

func _sync_facial_kind(shape_names: Array[StringName], source_indices: PackedInt32Array, last_weights: PackedFloat32Array, targets: Array, force: bool) -> void:
	if _body_mesh == null or _body_mesh.mesh == null or shape_names.is_empty():
		return
	if last_weights.size() != shape_names.size():
		return
	for bound_i in shape_names.size():
		var source_index: int = source_indices[bound_i]
		if source_index < 0 or source_index >= _body_mesh.mesh.get_blend_shape_count():
			continue
		var weight: float = _body_mesh.get_blend_shape_value(source_index)
		if not force and absf(weight - last_weights[bound_i]) < FACIAL_MORPH_EPSILON:
			continue
		last_weights[bound_i] = weight
		for target_variant in targets:
			var target: MeshInstance3D = target_variant
			if target == null or target.mesh == null or bound_i >= target.mesh.get_blend_shape_count():
				continue
			target.set_blend_shape_value(bound_i, weight)

func _update_facial_lod_visibility(force: bool = false) -> void:
	if _mustache_lod0 == null or _beard_lod0 == null:
		return
	var desired_lod := 0
	var forced_lod: int = clampi(AppSettings.debug_forced_brow_lod, -1, 2)
	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera != null and _mount != null:
		_last_facial_distance = camera.global_position.distance_to(_mount.global_position)
	else:
		_last_facial_distance = -1.0
	if forced_lod >= 0:
		desired_lod = forced_lod
	elif _last_facial_distance >= 0.0:
		if _last_facial_distance < FACIAL_LOD1_DISTANCE:
			desired_lod = 0
		elif _last_facial_distance < FACIAL_LOD2_DISTANCE:
			desired_lod = 1
		else:
			desired_lod = 2
	if not force and desired_lod == _active_facial_lod:
		return
	_active_facial_lod = desired_lod

	var mustache_enabled: bool = bool(mustache_settings.get("enabled", true))
	var beard_enabled: bool = bool(beard_settings.get("enabled", true))
	_mustache_lod0.visible = mustache_enabled and desired_lod == 0
	_mustache_lod1.visible = mustache_enabled and desired_lod == 1
	_mustache_lod2.visible = mustache_enabled and desired_lod == 2
	_beard_lod0.visible = beard_enabled and desired_lod == 0
	_beard_lod1.visible = beard_enabled and desired_lod == 1
	_beard_lod2.visible = beard_enabled and desired_lod == 2

func diagnostics() -> String:
	var base: String = super.diagnostics()
	var distance_note := "no camera"
	if _last_facial_distance >= 0.0:
		distance_note = "%.2f m" % _last_facial_distance
	return "%s • facial hair LOD%d @ %s • mustache morphs %d • beard morphs %d • %d unbound" % [
		base,
		_active_facial_lod,
		distance_note,
		_mustache_shape_names.size(),
		_beard_shape_names.size(),
		_facial_bind_failures
	]
