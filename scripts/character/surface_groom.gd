extends "res://scripts/character/procedural_groom.gd"

## Surface-conforming brow refinement for the Asterra groom prototype.
## Brow design happens in a stable 2D brow space, but every control point is
## projected back onto the actual imported forehead mesh. Roots are distributed
## through a brow-shaped band instead of a single line and each strand is drawn
## as a flat ribbon parallel to the local skin surface.

var _face_triangles: PackedVector3Array = PackedVector3Array()

func rebuild_brows() -> void:
	if _brow_instance == null or _mount == null or _head.is_empty():
		return
	_brow_instance.visible = bool(brow_settings.get("enabled", true))
	if not _brow_instance.visible:
		_brow_instance.mesh = null
		return

	_ensure_face_triangle_cache()

	var density: float = clampf(float(brow_settings.get("density", 0.68)), 0.05, 1.0)
	var brow_width: float = clampf(float(brow_settings.get("width", 1.0)), 0.55, 1.45)
	var requested_width: float = clampf(float(brow_settings.get("strand_width", 0.00045)), 0.00008, 0.0012)
	var arch: float = clampf(float(brow_settings.get("arch", 0.45)), 0.0, 1.25)
	var height_offset: float = clampf(float(brow_settings.get("height_offset", 0.0)), -0.035, 0.035)
	var forward_control: float = clampf(float(brow_settings.get("forward_offset", 0.0015)), -0.010, 0.025)

	# The old UI default was +1.5 mm because the previous ellipsoid placement
	# needed a manual push. Surface projection makes that correction unnecessary,
	# so treat the old default value as the new neutral position.
	var artistic_offset: float = forward_control - 0.0015

	var center: Vector3 = _head["center"]
	var rx: float = float(_head["rx"])
	var ry: float = float(_head["ry"])
	var rz: float = float(_head["rz"])

	# More roots than the first prototype, but each root is a much thinner flat
	# ribbon. This creates the layered mass of a real eyebrow instead of a comb.
	var count_per_side: int = clampi(int(round(lerpf(70.0, 235.0, density))), 48, 260)
	var ribbon_half_width: float = requested_width * 0.32

	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)

	for side_i in 2:
		var side: float = -1.0 if side_i == 0 else 1.0
		for i in count_per_side:
			var h0: float = _hash01(float(i) * 13.91 + side * 5.2)
			var h1: float = _hash01(float(i) * 29.37 + side * 17.4)
			var h2: float = _hash01(float(i) * 47.11 + side * 2.9)
			var h3: float = _hash01(float(i) * 71.03 + side * 11.7)

			# Low-discrepancy travel along the brow plus a very small horizontal
			# jitter. The roots are then scattered vertically across a shaped band.
			var t: float = (float(i) + 0.5) / float(count_per_side)
			t = clampf(t + (h0 - 0.5) / float(count_per_side) * 1.8, 0.0, 1.0)

			var inner_x: float = rx * 0.18
			var outer_x: float = rx * 0.79 * brow_width
			var x_offset: float = lerpf(inner_x, outer_x, t)
			var x_jitter: float = (h1 - 0.5) * _character_height * 0.0008
			var x: float = center.x + side * (x_offset + x_jitter)

			var centerline_y: float = _brow_centerline_y(t, center.y, ry, arch, height_offset)
			# Natural brows are thickest from the inner third through the body and
			# taper strongly into the tail. Average several hashes to bias roots
			# toward the center of the band rather than its edges.
			var band_random: float = ((h1 + h2 + h3) / 3.0 - 0.5) * 2.0
			var inner_thickness: float = _character_height * 0.0060
			var tail_thickness: float = _character_height * 0.0020
			var thickness: float = lerpf(inner_thickness, tail_thickness, pow(t, 1.35))
			# Slightly fuller body around 35-55% of the brow.
			thickness *= 1.0 + sin(t * PI) * 0.18
			var y: float = centerline_y + band_random * thickness * 0.50

			var fallback_z: float = _ellipsoid_front_z(x, y, center, rx, ry, rz)
			var root_place: Dictionary = _project_to_face(x, y, fallback_z, artistic_offset)
			var root: Vector3 = root_place["point"]
			var root_normal: Vector3 = root_place["normal"]

			# Growth rotates from more upright inner hairs to increasingly lateral
			# outer hairs. A small deterministic angular variation prevents rows.
			var lateral: float = lerpf(0.36, 1.0, smoothstep(0.0, 0.82, t))
			var vertical: float = lerpf(0.94, -0.18, smoothstep(0.0, 1.0, t))
			lateral += (h2 - 0.5) * 0.12
			vertical += (h3 - 0.5) * 0.14
			var flow2 := Vector2(side * lateral, vertical).normalized()

			var body_factor: float = 1.0 - absf(t - 0.42) / 0.58
			body_factor = clampf(body_factor, 0.0, 1.0)
			var strand_len: float = _character_height * lerpf(0.0031, 0.0052, body_factor)
			strand_len *= lerpf(0.88, 1.12, h0)

			# Project the middle and tip independently. That makes each short hair
			# follow forehead curvature instead of being a rotated 3D spike.
			var mid_x: float = x + flow2.x * strand_len * 0.52
			var mid_y: float = y + flow2.y * strand_len * 0.52
			var tip_x: float = x + flow2.x * strand_len
			var tip_y: float = y + flow2.y * strand_len
			var mid_fallback_z: float = _ellipsoid_front_z(mid_x, mid_y, center, rx, ry, rz)
			var tip_fallback_z: float = _ellipsoid_front_z(tip_x, tip_y, center, rx, ry, rz)
			var mid_place: Dictionary = _project_to_face(mid_x, mid_y, mid_fallback_z, artistic_offset)
			var tip_place: Dictionary = _project_to_face(tip_x, tip_y, tip_fallback_z, artistic_offset)
			var mid: Vector3 = mid_place["point"]
			var tip: Vector3 = tip_place["point"]
			var mid_normal: Vector3 = mid_place["normal"]
			var tip_normal: Vector3 = tip_place["normal"]

			# Tiny lift through the middle produces a hair-like arc while keeping
			# both ends attached to the skin. It is intentionally sub-millimetre.
			mid += mid_normal * lerpf(0.00010, 0.00030, h3)

			var root_local: Vector3 = _mount.to_local(root)
			var mid_local: Vector3 = _mount.to_local(mid)
			var tip_local: Vector3 = _mount.to_local(tip)
			var root_normal_local: Vector3 = (_mount.global_transform.basis.inverse() * root_normal).normalized()
			var mid_normal_local: Vector3 = (_mount.global_transform.basis.inverse() * mid_normal).normalized()
			var tip_normal_local: Vector3 = (_mount.global_transform.basis.inverse() * tip_normal).normalized()

			_add_skin_ribbon(st, root_local, mid_local, root_normal_local, mid_normal_local, ribbon_half_width, ribbon_half_width * 0.72, 0.0, 0.52)
			_add_skin_ribbon(st, mid_local, tip_local, mid_normal_local, tip_normal_local, ribbon_half_width * 0.72, ribbon_half_width * 0.06, 0.52, 1.0)

	st.generate_normals()
	st.generate_tangents()
	_brow_instance.mesh = st.commit()
	_brow_instance.material_override = _brow_material

func _brow_centerline_y(t: float, center_y: float, ry: float, arch: float, height_offset: float) -> float:
	var y: float = center_y + ry * 0.37 + height_offset
	# Broad natural arch with the peak slightly outside center.
	var arch_t: float = clampf(t / 0.72, 0.0, 1.0)
	y += sin(arch_t * PI) * (_character_height * 0.0065 * arch)
	y -= t * _character_height * 0.0024
	return y

func _ellipsoid_front_z(x: float, y: float, center: Vector3, rx: float, ry: float, rz: float) -> float:
	var xn: float = (x - center.x) / maxf(rx, 0.001)
	var yn: float = (y - center.y) / maxf(ry, 0.001)
	var surface: float = sqrt(maxf(0.04, 1.0 - xn * xn - yn * yn))
	return center.z + _front_sign * rz * surface

func _add_skin_ribbon(st: SurfaceTool, a: Vector3, b: Vector3, normal_a: Vector3, normal_b: Vector3, width_a: float, width_b: float, v0: float, v1: float) -> void:
	var direction: Vector3 = b - a
	if direction.length_squared() < 0.00000001:
		return
	direction = direction.normalized()

	# Width axes live in the tangent plane of the skin. This is the key
	# difference from the old crossed-ribbon renderer, which made brow hairs
	# look rotated/perpendicular when seen from an oblique angle.
	var side_a: Vector3 = normal_a.cross(direction)
	if side_a.length_squared() < 0.000001:
		side_a = Vector3.UP.cross(direction)
	if side_a.length_squared() < 0.000001:
		side_a = Vector3.RIGHT
	side_a = side_a.normalized()

	var side_b: Vector3 = normal_b.cross(direction)
	if side_b.length_squared() < 0.000001:
		side_b = side_a
	side_b = side_b.normalized()
	if side_a.dot(side_b) < 0.0:
		side_b = -side_b

	_add_quad(
		st,
		a - side_a * width_a,
		a + side_a * width_a,
		b + side_b * width_b,
		b - side_b * width_b,
		v0,
		v1
	)

func _ensure_face_triangle_cache() -> void:
	if not _face_triangles.is_empty():
		return
	if _body_mesh == null or _body_mesh.mesh == null:
		return

	var head_top: float = float(_head.get("top", _character_bottom + _character_height))
	var min_face_y: float = head_top - _character_height * 0.19

	for surface in _body_mesh.mesh.get_surface_count():
		var arrays: Array = _body_mesh.mesh.surface_get_arrays(surface)
		if arrays.size() <= Mesh.ARRAY_VERTEX or arrays[Mesh.ARRAY_VERTEX] == null:
			continue
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var indices: PackedInt32Array = PackedInt32Array()
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
				_append_face_triangle(vertices[ia], vertices[ib], vertices[ic], min_face_y)
		else:
			var triangle_count: int = vertices.size() / 3
			for tri_i in triangle_count:
				var base: int = tri_i * 3
				_append_face_triangle(vertices[base], vertices[base + 1], vertices[base + 2], min_face_y)

func _append_face_triangle(a_local: Vector3, b_local: Vector3, c_local: Vector3, min_face_y: float) -> void:
	var a: Vector3 = _body_mesh.to_global(a_local)
	var b: Vector3 = _body_mesh.to_global(b_local)
	var c: Vector3 = _body_mesh.to_global(c_local)
	if maxf(a.y, maxf(b.y, c.y)) < min_face_y:
		return
	_face_triangles.append(a)
	_face_triangles.append(b)
	_face_triangles.append(c)

func _project_to_face(x: float, y: float, fallback_z: float, user_forward_offset: float) -> Dictionary:
	var front := Vector3(0.0, 0.0, _front_sign)
	var ray_direction: Vector3 = -front
	var start_distance: float = maxf(float(_head["rz"]) * 3.0, 0.18)
	var center: Vector3 = _head["center"]
	var origin := Vector3(x, y, center.z + _front_sign * start_distance)

	var best_t: float = INF
	var best_normal: Vector3 = front
	var tri_count: int = _face_triangles.size() / 3
	for tri_i in tri_count:
		var base: int = tri_i * 3
		var a: Vector3 = _face_triangles[base]
		var b: Vector3 = _face_triangles[base + 1]
		var c: Vector3 = _face_triangles[base + 2]
		var distance: float = _ray_triangle_distance(origin, ray_direction, a, b, c)
		if distance < 0.0 or distance >= best_t:
			continue
		best_t = distance
		var normal: Vector3 = (b - a).cross(c - a)
		if normal.length_squared() > 0.00000001:
			normal = normal.normalized()
			if normal.dot(front) < 0.0:
				normal = -normal
			best_normal = normal

	if best_t < INF:
		var hit: Vector3 = origin + ray_direction * best_t
		# Only a quarter-millimetre baseline clearance is needed now because the
		# ribbon itself lies parallel to the surface rather than crossing it.
		var clearance: float = 0.00025
		return {
			"point": hit + best_normal * clearance + front * user_forward_offset,
			"normal": best_normal,
			"projected": true
		}

	return {
		"point": Vector3(x, y, fallback_z) + front * (0.001 + user_forward_offset),
		"normal": front,
		"projected": false
	}

func _ray_triangle_distance(origin: Vector3, direction: Vector3, a: Vector3, b: Vector3, c: Vector3) -> float:
	var edge1: Vector3 = b - a
	var edge2: Vector3 = c - a
	var pvec: Vector3 = direction.cross(edge2)
	var determinant: float = edge1.dot(pvec)
	if absf(determinant) < 0.0000001:
		return -1.0
	var inv_det: float = 1.0 / determinant
	var tvec: Vector3 = origin - a
	var u: float = tvec.dot(pvec) * inv_det
	if u < 0.0 or u > 1.0:
		return -1.0
	var qvec: Vector3 = tvec.cross(edge1)
	var v: float = direction.dot(qvec) * inv_det
	if v < 0.0 or u + v > 1.0:
		return -1.0
	var distance: float = edge2.dot(qvec) * inv_det
	if distance <= 0.000001:
		return -1.0
	return distance
