extends "res://scripts/character/facial_hair_groom.gd"

## Anatomical placement pass for procedural mustache + beard.
##
## This keeps the existing expensive-build / cheap-runtime morph-transfer
## pipeline. Only neutral follicle placement and growth directions change:
## - mustache roots follow a curved upper-lip band with a soft philtrum split,
##   denser body under the nostrils, corner taper and a lateral growth fan;
## - beard roots are sampled from separate cheek, jaw and chin regions instead
##   of one broad rectangle, which removes the sparse needles high on the face;
## - roots/tips that cannot be projected onto the real face are discarded rather
##   than falling back to the head ellipsoid and creating floating hairs.

func configure(character: Node3D, meshes: Array[MeshInstance3D], character_bottom: float, character_height: float, front_sign: float = 1.0, rebuild_initial: bool = true) -> bool:
	_apply_natural_facial_defaults()
	return super.configure(character, meshes, character_bottom, character_height, front_sign, rebuild_initial)

func _apply_natural_facial_defaults() -> void:
	mustache_settings["density"] = 0.86
	mustache_settings["width"] = 0.82
	mustache_settings["thickness"] = 0.72
	mustache_settings["length"] = 0.0055
	mustache_settings["strand_width"] = 0.00030
	mustache_settings["middle_gap"] = 0.008
	mustache_settings["droop"] = 0.20
	mustache_settings["height_offset"] = 0.0
	mustache_settings["forward_offset"] = 0.00055
	mustache_settings["messiness"] = 0.14

	beard_settings["density"] = 1.0
	beard_settings["density_multiplier"] = 3.0
	beard_settings["coverage"] = 1.0
	beard_settings["fullness"] = 1.30
	beard_settings["length"] = 0.016
	beard_settings["chin_length"] = 0.022
	beard_settings["strand_width"] = 0.00065
	beard_settings["height_offset"] = 0.0
	beard_settings["forward_offset"] = 0.00055
	beard_settings["messiness"] = 0.18

func _create_render_nodes() -> void:
	super._create_render_nodes()
	# Short facial hair should read as dark fibre rather than bright metallic
	# needles under studio lights.
	if _facial_hair_material != null:
		_facial_hair_material.roughness = 0.66
		_facial_hair_material.anisotropy = 0.30

func _build_mustache_mesh(lod: int) -> ArrayMesh:
	var density: float = clampf(float(mustache_settings.get("density", 0.86)), 0.05, 1.0)
	var width_scale: float = clampf(float(mustache_settings.get("width", 0.82)), 0.45, 1.35)
	var thickness: float = clampf(float(mustache_settings.get("thickness", 0.72)), 0.30, 1.80)
	var length: float = clampf(float(mustache_settings.get("length", 0.0055)), 0.0015, 0.030)
	var requested_width: float = clampf(float(mustache_settings.get("strand_width", 0.00030)), 0.00008, 0.0012)
	var middle_gap: float = clampf(float(mustache_settings.get("middle_gap", 0.008)), 0.0, 0.030)
	var droop: float = clampf(float(mustache_settings.get("droop", 0.20)), 0.0, 1.0)
	var height_offset: float = clampf(float(mustache_settings.get("height_offset", 0.0)), -0.025, 0.025)
	var forward_offset: float = clampf(float(mustache_settings.get("forward_offset", 0.00055)), -0.002, 0.008)
	var messiness: float = clampf(float(mustache_settings.get("messiness", 0.14)), 0.0, 1.0)

	var center: Vector3 = _head["center"]
	var rx: float = float(_head["rx"])
	var ry: float = float(_head["ry"])
	var count_per_side: int = clampi(int(round(lerpf(90.0, 260.0, density))), 60, 280)

	# The previous 0.58*rx span made the mustache extend onto the cheeks. A real
	# upper-lip field is much closer to mouth width.
	var outer_x: float = rx * 0.44 * width_scale
	var inner_x: float = minf(middle_gap * 0.5, outer_x * 0.58)
	var base_y: float = center.y - ry * 0.145 + height_offset
	var radius: float = requested_width * 0.24
	if lod == 1:
		radius *= 1.38
	elif lod >= 2:
		radius *= 1.80

	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var inv_basis: Basis = _mount.global_transform.basis.inverse()

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
			var h4: float = _hash01(float(i) * 163.7 + side * 23.4)

			var t: float = (float(i) + 0.5) / float(count_per_side)
			t = clampf(t + (h0 - 0.5) / float(count_per_side) * lerpf(0.35, 2.2, messiness), 0.0, 1.0)

			# Soft philtrum edge + soft mouth-corner taper. Density is strongest in
			# the inner/middle body under the nostrils rather than at the tail.
			var inner_fade: float = smoothstep(0.0, 0.11, t)
			var outer_fade: float = smoothstep(0.0, 0.20, 1.0 - t)
			var nostril_distance: float = (t - 0.30) / 0.24
			var nostril_bias: float = 0.72 + 0.28 * exp(-(nostril_distance * nostril_distance))
			var keep_probability: float = clampf(inner_fade * outer_fade * nostril_bias, 0.0, 1.0)
			if h4 > keep_probability:
				continue

			var x_offset: float = lerpf(inner_x, outer_x, t)
			var x_jitter: float = (h1 - 0.5) * _character_height * 0.00055 * messiness
			var x: float = center.x + side * (x_offset + x_jitter)

			# Curved upper-lip band: inner roots sit slightly higher under the nose,
			# then the band descends smoothly toward the mouth corner.
			var centerline_y: float = base_y
			centerline_y += ry * 0.018 * (1.0 - t)
			centerline_y -= ry * (0.016 + 0.030 * droop) * pow(t, 1.45)
			var band_half: float = ry * 0.022 * thickness * lerpf(1.0, 0.60, t)
			var band_random: float = ((h1 + h2 + h3) / 3.0 - 0.5) * 2.0
			var y: float = centerline_y + band_random * band_half
			y += (h3 - 0.5) * _character_height * 0.00045 * messiness

			var root_place: Dictionary = _project_facial_point(x, y, forward_offset)
			if not bool(root_place.get("projected", false)):
				continue
			var root: Vector3 = root_place["point"]
			var root_normal: Vector3 = root_place["normal"]

			# Natural fan: inner hairs point down/out, the body rotates lateral, and
			# only the tail receives extra droop.
			var lateral: float = lerpf(0.28, 0.92, smoothstep(0.0, 0.92, t))
			var vertical: float = -lerpf(0.76, 0.30, smoothstep(0.0, 1.0, t))
			vertical -= droop * smoothstep(0.58, 1.0, t) * 0.22
			lateral += (h2 - 0.5) * 0.18 * messiness
			vertical += (h3 - 0.5) * 0.18 * messiness
			var flow: Vector2 = Vector2(side * lateral, vertical).normalized()

			var body_peak: float = 1.0 - clampf(absf(t - 0.40) / 0.60, 0.0, 1.0)
			var strand_len: float = length * lerpf(0.78, 1.06, body_peak)
			strand_len *= lerpf(0.88, 1.12, h0)

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
				_add_natural_tri_tube_segment(st, root_local, tip_local, root_normal_local, tip_normal_local, radius, radius * 0.08, 0.0, 1.0)
			else:
				var mid_x: float = x + flow.x * strand_len * 0.54
				var mid_y: float = y + flow.y * strand_len * 0.54
				var mid_place: Dictionary = _project_facial_point(mid_x, mid_y, forward_offset)
				var mid: Vector3
				var mid_normal: Vector3
				if bool(mid_place.get("projected", false)):
					mid = mid_place["point"]
					mid_normal = mid_place["normal"]
				else:
					mid = root.lerp(tip, 0.54)
					mid_normal = (root_normal + tip_normal).normalized()
				mid += mid_normal * lerpf(0.000035, 0.00012, h3)
				var mid_local: Vector3 = _mount.to_local(mid)
				var mid_normal_local: Vector3 = (inv_basis * mid_normal).normalized()
				_add_natural_tri_tube_segment(st, root_local, mid_local, root_normal_local, mid_normal_local, radius, radius * 0.76, 0.0, 0.54)
				_add_natural_tri_tube_segment(st, mid_local, tip_local, mid_normal_local, tip_normal_local, radius * 0.76, radius * 0.06, 0.54, 1.0)

	st.generate_normals()
	st.generate_tangents()
	return st.commit()

func _build_beard_mesh(lod: int) -> ArrayMesh:
	var density: float = clampf(float(beard_settings.get("density", 1.0)), 0.05, 1.0)
	var density_multiplier: float = clampf(float(beard_settings.get("density_multiplier", 3.0)), 1.0, 6.0)
	var coverage: float = clampf(float(beard_settings.get("coverage", 1.0)), 0.0, 1.0)
	var fullness: float = clampf(float(beard_settings.get("fullness", 1.30)), 0.15, 1.35)
	var base_length: float = clampf(float(beard_settings.get("length", 0.016)), 0.001, 0.045)
	var chin_length: float = clampf(float(beard_settings.get("chin_length", 0.022)), 0.001, 0.060)
	var requested_width: float = clampf(float(beard_settings.get("strand_width", 0.00065)), 0.00008, 0.0015)
	var height_offset: float = clampf(float(beard_settings.get("height_offset", 0.0)), -0.035, 0.035)
	var forward_offset: float = clampf(float(beard_settings.get("forward_offset", 0.00055)), -0.002, 0.010)
	var messiness: float = clampf(float(beard_settings.get("messiness", 0.18)), 0.0, 1.0)

	var center: Vector3 = _head["center"]
	var rx: float = float(_head["rx"])
	var ry: float = float(_head["ry"])
	var target_count: int = clampi(int(round(lerpf(260.0, 920.0, density) * density_multiplier)), 180, 5600)
	# The setting is a visual strand width, so a triangular tube needs close to
	# half of it as its radius. The old 0.23 factor made even a dense groom read as
	# scattered sub-pixel stubble in a face close-up.
	var radius: float = requested_width * 0.46
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
		var keep_probability := 1.0
		var flow_lateral := 0.0
		var flow_vertical := -1.0
		var length_scale := 1.0
		var chin_mix := 0.0
		var region_kind := 0

		if h0 < 0.42:
			# CHEEK: intentionally starts below the moustache/nose zone. The cheek
			# line rises somewhat toward the sideburn and fades toward the center.
			region_kind = 0
			var side: float = -1.0 if h1 < 0.5 else 1.0
			var x_u: float = h2
			var abs_x: float = rx * lerpf(0.34, 0.80, x_u)
			var outerness: float = clampf((abs_x / maxf(rx, 0.001) - 0.34) / 0.46, 0.0, 1.0)
			var cheek_top_ratio: float = lerpf(-0.22, -0.10, coverage) + 0.055 * outerness
			var cheek_bottom_ratio := -0.53
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
			# JAW: a continuous curved band, lower at the chin and higher near the
			# mandibular angle. This is the structural mass missing in the old mask.
			region_kind = 1
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
			# CHIN / GOATEE / SOUL PATCH: compact central region below the mouth.
			# It joins the jaw band instead of leaving the disconnected sparse chin
			# visible in the reference screenshot.
			region_kind = 2
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

		# Clean lips and mouth opening. The region masks already avoid most of this
		# area; the ellipse is a final hard guard against hairs crossing the lips.
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
