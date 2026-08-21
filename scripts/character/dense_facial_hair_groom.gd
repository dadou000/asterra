extends "res://scripts/character/natural_facial_hair_groom.gd"

## Dense facial-hair tuning + build-performance layer.
##
## LOD0 keeps the requested full density (up to 100x), but distant LODs cap
## their effective multiplier because tens of thousands of sub-pixel hairs are
## wasted beyond 6 m. Neutral face projections are quantized/cached, the LOD0
## midpoint reuses the already-projected root/tip surface, duplicate triangle
## vertices are indexed, and beard tangents are omitted because the facial-hair
## material no longer uses anisotropy. The exact LOD0/1 facial morph transfer
## remains unchanged.

const BEARD_LOD1_MAX_DENSITY_MULTIPLIER := 6.0
const BEARD_LOD2_MAX_DENSITY_MULTIPLIER := 1.5
const FACIAL_PROJECTION_CACHE_STEP := 0.00035 # 0.35 mm; visually sub-strand.

var _facial_projection_cache: Dictionary = {}
var _facial_projection_hits := 0
var _facial_projection_misses := 0
var _beard_mesh_build_ms: Dictionary = {}
var _beard_morph_build_ms: Dictionary = {}

func configure(character: Node3D, meshes: Array[MeshInstance3D], character_bottom: float, character_height: float, front_sign: float = 1.0) -> bool:
	_clear_dense_build_cache()
	return super.configure(character, meshes, character_bottom, character_height, front_sign)

func set_front_sign(sign_value: float) -> void:
	_clear_dense_build_cache()
	super.set_front_sign(sign_value)

func _clear_dense_build_cache() -> void:
	_facial_projection_cache.clear()
	_facial_projection_hits = 0
	_facial_projection_misses = 0
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
	beard_settings["density_multiplier"] = 1.0
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
	# Tangent generation and per-morph tangent arrays were disproportionately
	# expensive at 30x+ beard density. The beard is too small for anisotropic
	# orientation to justify that cost, and disabling it also avoids bright fibre
	# highlights seen in the close-up test.
	if _facial_hair_material != null:
		_facial_hair_material.anisotropy_enabled = false
		_facial_hair_material.anisotropy = 0.0
		_facial_hair_material.roughness = 0.68

func _projection_cache_key(x: float, y: float, offset: float) -> Vector3i:
	return Vector3i(
		int(round(x / FACIAL_PROJECTION_CACHE_STEP)),
		int(round(y / FACIAL_PROJECTION_CACHE_STEP)),
		int(round(offset / FACIAL_PROJECTION_CACHE_STEP))
	)

func _project_facial_point(x: float, y: float, offset: float) -> Dictionary:
	var key: Vector3i = _projection_cache_key(x, y, offset)
	if _facial_projection_cache.has(key):
		_facial_projection_hits += 1
		var cached: Dictionary = _facial_projection_cache[key]
		return cached.duplicate()

	_facial_projection_misses += 1
	var result: Dictionary = super._project_facial_point(x, y, offset)
	_facial_projection_cache[key] = result.duplicate()
	return result

func _effective_beard_density_multiplier(lod: int, requested: float) -> float:
	if lod <= 0:
		return requested
	if lod == 1:
		return minf(requested, BEARD_LOD1_MAX_DENSITY_MULTIPLIER)
	return minf(requested, BEARD_LOD2_MAX_DENSITY_MULTIPLIER)

func _create_facial_morph_bound_mesh(neutral_mesh: ArrayMesh, lod: int, is_mustache: bool) -> ArrayMesh:
	if is_mustache:
		return super._create_facial_morph_bound_mesh(neutral_mesh, lod, is_mustache)
	var started_usec: int = Time.get_ticks_usec()
	var result: ArrayMesh = super._create_facial_morph_bound_mesh(neutral_mesh, lod, false)
	_beard_morph_build_ms[lod] = float(Time.get_ticks_usec() - started_usec) / 1000.0
	return result

func _build_beard_mesh(lod: int) -> ArrayMesh:
	var started_usec: int = Time.get_ticks_usec()
	var density: float = clampf(float(beard_settings.get("density", 1.0)), 0.05, 1.0)
	var requested_multiplier: float = clampf(float(beard_settings.get("density_multiplier", 1.0)), 1.0, 100.0)
	var density_multiplier: float = _effective_beard_density_multiplier(lod, requested_multiplier)
	var coverage: float = clampf(float(beard_settings.get("coverage", 0.68)), 0.0, 1.0)
	var fullness: float = clampf(float(beard_settings.get("fullness", 1.10)), 0.15, 1.35)
	var base_length: float = clampf(float(beard_settings.get("length", 0.0055)), 0.001, 0.045)
	var chin_length: float = clampf(float(beard_settings.get("chin_length", 0.0065)), 0.001, 0.060)
	var requested_width: float = clampf(float(beard_settings.get("strand_width", 0.00042)), 0.00008, 0.0015)
	var height_offset: float = clampf(float(beard_settings.get("height_offset", 0.0)), -0.035, 0.035)
	var forward_offset: float = clampf(float(beard_settings.get("forward_offset", 0.00055)), -0.002, 0.010)
	var messiness: float = clampf(float(beard_settings.get("messiness", 0.12)), 0.0, 1.0)

	var center: Vector3 = _head["center"]
	var rx: float = float(_head["rx"])
	var ry: float = float(_head["ry"])
	var base_target_count: int = clampi(int(round(lerpf(260.0, 920.0, density))), 180, 980)
	var target_count: int = clampi(int(round(float(base_target_count) * density_multiplier)), 180, 92000)
	var radius: float = requested_width * 0.23
	if lod == 1:
		radius *= 1.40
	elif lod >= 2:
		radius *= 1.88

	var mouth_center_y: float = center.y - ry * 0.22 + height_offset
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var inv_basis: Basis = _mount.global_transform.basis.inverse()

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
		var h5: float = _hash01(float(i) * 227.9 + 4.2)
		var h6: float = _hash01(float(i) * 269.3 + 9.7)

		var x: float = center.x
		var y: float = center.y
		var keep_probability: float = 1.0
		var flow_lateral: float = 0.0
		var flow_vertical: float = -1.0
		var length_scale: float = 1.0
		var chin_mix: float = 0.0

		if h0 < 0.42:
			var side: float = -1.0 if h1 < 0.5 else 1.0
			var x_u: float = h2
			var abs_x: float = rx * lerpf(0.34, 0.80, x_u)
			var outerness: float = clampf((abs_x / maxf(rx, 0.001) - 0.34) / 0.46, 0.0, 1.0)
			var cheek_top_ratio: float = lerpf(-0.22, -0.10, coverage) + 0.055 * outerness
			var cheek_bottom_ratio: float = -0.53
			var down_bias: float = sqrt(h3)
			x = center.x + side * abs_x
			y = center.y + ry * lerpf(cheek_top_ratio, cheek_bottom_ratio, down_bias) + height_offset
			keep_probability = lerpf(0.45, 0.78, coverage)
			keep_probability *= lerpf(0.72, 1.0, down_bias)
			keep_probability *= lerpf(0.72, 1.0, outerness)
			flow_lateral = -side * lerpf(0.10, 0.22, 1.0 - down_bias)
			flow_vertical = -1.0
			length_scale = lerpf(0.78, 0.94, down_bias)
		elif h0 < 0.78:
			var side: float = -1.0 if h1 < 0.5 else 1.0
			var abs_x: float = rx * lerpf(0.12, 0.80, h2)
			var jaw_u: float = clampf(abs_x / maxf(rx * 0.80, 0.001), 0.0, 1.0)
			var jaw_ratio: float = lerpf(-0.70, -0.52, pow(jaw_u, 1.35))
			var jaw_band_half: float = ry * 0.052 * lerpf(0.80, 1.10, fullness)
			x = center.x + side * abs_x
			y = center.y + ry * jaw_ratio + (h3 - 0.5) * 2.0 * jaw_band_half + height_offset
			keep_probability = clampf(0.70 + fullness * 0.24, 0.0, 1.0)
			flow_lateral = -side * 0.22 * jaw_u
			flow_vertical = -1.0
			chin_mix = (1.0 - jaw_u) * 0.58
			length_scale = 0.94
		else:
			var x_norm: float = (h2 * 2.0 - 1.0) * 0.30 * lerpf(0.88, 1.06, fullness)
			var chin_v: float = pow(h3, 0.88)
			x = center.x + rx * x_norm
			y = center.y + ry * lerpf(-0.34, -0.73, chin_v) + height_offset
			keep_probability = clampf(0.78 + fullness * 0.18, 0.0, 1.0)
			var side: float = -1.0 if x < center.x else 1.0
			flow_lateral = side * absf(x_norm) * 0.10
			flow_vertical = -1.0
			chin_mix = smoothstep(0.18, 0.96, chin_v) * (1.0 - clampf(absf(x_norm) / 0.32, 0.0, 1.0) * 0.35)
			length_scale = lerpf(0.90, 1.0, chin_v)

		if h4 > keep_probability:
			continue

		var mouth_x: float = absf(x - center.x) / maxf(rx * 0.40, 0.001)
		var mouth_y: float = absf(y - mouth_center_y) / maxf(ry * 0.125, 0.001)
		if mouth_x * mouth_x + mouth_y * mouth_y < 1.0:
			continue

		x += (h5 - 0.5) * _character_height * 0.00065 * messiness
		y += (h6 - 0.5) * _character_height * 0.00060 * messiness

		var root_place: Dictionary = _project_facial_point(x, y, forward_offset)
		if not bool(root_place.get("projected", false)):
			continue
		var root: Vector3 = root_place["point"]
		var root_normal: Vector3 = root_place["normal"]

		var strand_len: float = base_length * length_scale
		strand_len = lerpf(strand_len, chin_length, clampf(chin_mix, 0.0, 1.0))
		strand_len *= lerpf(0.88, 1.12, h6)
		flow_lateral += (h2 - 0.5) * 0.14 * messiness
		flow_vertical += (h3 - 0.5) * 0.12 * messiness
		var flow: Vector2 = Vector2(flow_lateral, flow_vertical).normalized()

		var tip_x: float = x + flow.x * strand_len
		var tip_y: float = y + flow.y * strand_len
		var tip_place: Dictionary = _project_facial_point(tip_x, tip_y, forward_offset)
		if not bool(tip_place.get("projected", false)):
			continue
		var tip: Vector3 = tip_place["point"]
		var tip_normal: Vector3 = tip_place["normal"]
		var root_local: Vector3 = _mount.to_local(root)
		var tip_local: Vector3 = _mount.to_local(tip)
		var root_normal_local: Vector3 = (inv_basis * root_normal).normalized()
		var tip_normal_local: Vector3 = (inv_basis * tip_normal).normalized()

		if lod >= 1:
			_add_natural_tri_tube_segment(st, root_local, tip_local, root_normal_local, tip_normal_local, radius, radius * 0.07, 0.0, 1.0)
		else:
			# For a 4-8 mm beard, re-projecting a third point per strand is expensive
			# and visually redundant. The already exact root/tip surface points define
			# the local skin arc closely; interpolate them and keep the tiny lift.
			var mid: Vector3 = root.lerp(tip, 0.52)
			var mid_normal: Vector3 = (root_normal + tip_normal).normalized()
			if mid_normal.length_squared() < 0.00000001:
				mid_normal = root_normal
			mid += mid_normal * lerpf(0.00004, 0.00015 + strand_len * 0.006, h3)
			var mid_local: Vector3 = _mount.to_local(mid)
			var mid_normal_local: Vector3 = (inv_basis * mid_normal).normalized()
			_add_natural_tri_tube_segment(st, root_local, mid_local, root_normal_local, mid_normal_local, radius, radius * 0.78, 0.0, 0.52)
			_add_natural_tri_tube_segment(st, mid_local, tip_local, mid_normal_local, tip_normal_local, radius * 0.78, radius * 0.06, 0.52, 1.0)

	# Normals are needed for lit hair. Tangents are intentionally skipped because
	# anisotropy is disabled above; this also removes tangent arrays from every
	# generated facial blend shape. Index after normals to deduplicate the repeated
	# vertices inside each triangle pair without changing the strand silhouette.
	st.generate_normals()
	st.index()
	var mesh: ArrayMesh = st.commit()
	_beard_mesh_build_ms[lod] = float(Time.get_ticks_usec() - started_usec) / 1000.0
	return mesh

func diagnostics() -> String:
	var base: String = super.diagnostics()
	var mesh0: float = float(_beard_mesh_build_ms.get(0, 0.0))
	var mesh1: float = float(_beard_mesh_build_ms.get(1, 0.0))
	var mesh2: float = float(_beard_mesh_build_ms.get(2, 0.0))
	var morph0: float = float(_beard_morph_build_ms.get(0, 0.0))
	var morph1: float = float(_beard_morph_build_ms.get(1, 0.0))
	return "%s • beard build mesh %.0f/%.0f/%.0f ms, morph %.0f/%.0f ms • face projection cache %d hit/%d miss" % [
		base,
		mesh0,
		mesh1,
		mesh2,
		morph0,
		morph1,
		_facial_projection_hits,
		_facial_projection_misses
	]
