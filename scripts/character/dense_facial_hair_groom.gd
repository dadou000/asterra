extends "res://scripts/character/natural_facial_hair_groom.gd"

## Dense facial-hair tuning layer.
##
## The normal Density control still covers the useful 0..1 range. An additional
## density_multiplier can now raise the actual generated follicle count up to
## 100x for close-up tuning. This is deliberately expensive at rebuild time;
## the generated/morph-bound meshes remain cheap at runtime afterwards.

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

func _build_beard_mesh(lod: int) -> ArrayMesh:
	var density: float = clampf(float(beard_settings.get("density", 1.0)), 0.05, 1.0)
	var density_multiplier: float = clampf(float(beard_settings.get("density_multiplier", 1.0)), 1.0, 100.0)
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
			# CHEEK: lower central coverage, rising toward the sideburn.
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
			# JAW: continuous curved mandibular band.
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
			# CHIN / GOATEE / SOUL PATCH.
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

		# Keep the lips and mouth opening clean.
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
			var mid_x: float = x + flow.x * strand_len * 0.52
			var mid_y: float = y + flow.y * strand_len * 0.52
			var mid_place: Dictionary = _project_facial_point(mid_x, mid_y, forward_offset)
			var mid: Vector3
			var mid_normal: Vector3
			if bool(mid_place.get("projected", false)):
				mid = mid_place["point"]
				mid_normal = mid_place["normal"]
			else:
				mid = root.lerp(tip, 0.52)
				mid_normal = (root_normal + tip_normal).normalized()
			mid += mid_normal * lerpf(0.00004, 0.00015 + strand_len * 0.006, h3)
			var mid_local: Vector3 = _mount.to_local(mid)
			var mid_normal_local: Vector3 = (inv_basis * mid_normal).normalized()
			_add_natural_tri_tube_segment(st, root_local, mid_local, root_normal_local, mid_normal_local, radius, radius * 0.78, 0.0, 0.52)
			_add_natural_tri_tube_segment(st, mid_local, tip_local, mid_normal_local, tip_normal_local, radius * 0.78, radius * 0.06, 0.52, 1.0)

	st.generate_normals()
	st.generate_tangents()
	return st.commit()
