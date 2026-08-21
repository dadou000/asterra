extends "res://scripts/character/procedural_groom.gd"

## Brow placement refinement for the Asterra groom prototype.
## The base generator uses an estimated head ellipsoid. This subclass keeps
## that estimate for 2D brow layout, but projects every brow root onto the
## actual imported body mesh and derives a local skin normal from the hit
## triangle. Brow hairs then grow mostly tangent to that normal, with a small
## outward lift, which prevents both roots and tips from entering the forehead.

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
	var strand_width: float = clampf(float(brow_settings.get("strand_width", 0.00045)), 0.00012, 0.002)
	var arch: float = clampf(float(brow_settings.get("arch", 0.45)), 0.0, 1.25)
	var height_offset: float = clampf(float(brow_settings.get("height_offset", 0.0)), -0.035, 0.035)
	var forward_offset: float = clampf(float(brow_settings.get("forward_offset", 0.0015)), -0.010, 0.025)
	var center: Vector3 = _head["center"]
	var rx: float = float(_head["rx"])
	var ry: float = float(_head["ry"])
	var rz: float = float(_head["rz"])
	var count_per_side: int = clampi(int(round(lerpf(20.0, 92.0, density))), 12, 110)

	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)

	for side_i in 2:
		var side: float = -1.0 if side_i == 0 else 1.0
		for i in count_per_side:
			var jitter: float = (_hash01(float(i) * 13.91 + side * 5.2) - 0.5) / float(count_per_side)
			var t: float = clampf((float(i) + 0.5) / float(count_per_side) + jitter, 0.0, 1.0)

			# The ellipsoid still gives us a stable 2D design space for width/arch.
			# Depth is discarded below and replaced by the real skin intersection.
			var inner_x: float = rx * 0.18
			var outer_x: float = rx * 0.79 * brow_width
			var x_offset: float = lerpf(inner_x, outer_x, t)
			var x: float = center.x + side * x_offset
			var base_y: float = center.y + ry * 0.37 + height_offset
			var y: float = base_y + sin(t * PI) * (_character_height * 0.0065 * arch)
			y -= t * _character_height * 0.0024

			var xn: float = (x - center.x) / maxf(rx, 0.001)
			var yn: float = (y - center.y) / maxf(ry, 0.001)
			var ellipsoid_surface: float = sqrt(maxf(0.04, 1.0 - xn * xn - yn * yn))
			var fallback_z: float = center.z + _front_sign * rz * ellipsoid_surface
			var placement: Dictionary = _project_to_face(x, y, fallback_z, forward_offset)
			var root: Vector3 = placement["point"]
			var skin_normal: Vector3 = placement["normal"]

			# Eyebrow hairs lie along the skin rather than pointing through it.
			# Inner hairs are more vertical, outer hairs turn increasingly lateral.
			var raw_flow := Vector3(side * (0.50 + 0.46 * t), 0.78 - 0.94 * t, 0.0)
			var tangent: Vector3 = raw_flow - skin_normal * raw_flow.dot(skin_normal)
			if tangent.length_squared() < 0.000001:
				tangent = Vector3(side, 0.0, 0.0)
			tangent = tangent.normalized()
			var direction: Vector3 = (tangent * 0.985 + skin_normal * 0.17).normalized()

			var strand_len: float = _character_height * lerpf(0.0035, 0.0054, 1.0 - absf(t - 0.45))
			var length_jitter: float = lerpf(0.88, 1.12, _hash01(float(i) * 21.77 + side * 3.7))
			var tip: Vector3 = root + direction * strand_len * length_jitter

			var root_local: Vector3 = _mount.to_local(root)
			var tip_local: Vector3 = _mount.to_local(tip)
			_add_crossed_segment(st, root_local, tip_local, strand_width, strand_width * 0.12, 0.0, 1.0)

	st.generate_normals()
	st.generate_tangents()
	_brow_instance.mesh = st.commit()
	_brow_instance.material_override = _brow_material

func _ensure_face_triangle_cache() -> void:
	if not _face_triangles.is_empty():
		return
	if _body_mesh == null or _body_mesh.mesh == null:
		return

	# Only cache the head/face region. This keeps projection cheap while the
	# brow sliders are dragged and avoids accidental hits on unrelated geometry.
	var head_top: float = float(_head.get("top", _character_bottom + _character_height))
	var min_face_y: float = head_top - _character_height * 0.19

	for surface in _body_mesh.mesh.get_surface_count():
		var arrays: Array = _body_mesh.mesh.surface_get_arrays(surface)
		if arrays.size() <= Mesh.ARRAY_VERTEX or arrays[Mesh.ARRAY_VERTEX] == null:
			continue
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
	var origin := Vector3(x, y, float(_head["center"].z) + _front_sign * start_distance)

	var best_t: float = INF
	var best_normal: Vector3 = front
	var tri_count: int = _face_triangles.size() / 3
	for tri_i in tri_count:
		var base: int = tri_i * 3
		var a: Vector3 = _face_triangles[base]
		var b: Vector3 = _face_triangles[base + 1]
		var c: Vector3 = _face_triangles[base + 2]
		var t: float = _ray_triangle_distance(origin, ray_direction, a, b, c)
		if t < 0.0 or t >= best_t:
			continue
		best_t = t
		var n: Vector3 = (b - a).cross(c - a)
		if n.length_squared() > 0.00000001:
			n = n.normalized()
			if n.dot(front) < 0.0:
				n = -n
			best_normal = n

	if best_t < INF:
		var hit: Vector3 = origin + ray_direction * best_t
		# Fixed clearance keeps ribbon thickness off the skin. Forward offset is
		# retained as an artistic control and is applied after real projection.
		var clearance: float = 0.00085
		return {
			"point": hit + best_normal * clearance + front * user_forward_offset,
			"normal": best_normal,
			"projected": true
		}

	# Graceful fallback if the imported body topology changes and no triangle
	# can be hit at this brow coordinate.
	return {
		"point": Vector3(x, y, fallback_z) + front * (0.002 + user_forward_offset),
		"normal": front,
		"projected": false
	}

func _ray_triangle_distance(origin: Vector3, direction: Vector3, a: Vector3, b: Vector3, c: Vector3) -> float:
	# Moller-Trumbore ray/triangle intersection. Returns distance along the ray,
	# or -1 when there is no forward intersection.
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
	var t: float = edge2.dot(qvec) * inv_det
	if t <= 0.000001:
		return -1.0
	return t
