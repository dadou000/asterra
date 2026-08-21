extends "res://scripts/character/surface_groom.gd"

## Advanced brow shaping on top of the surface-conforming groom.
## Adds independent inner spacing, end fades, overall brow-band thickness,
## and controlled follicle/growth messiness while keeping all points projected
## to the actual forehead/temple surface.

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

	# New creator controls.
	# middle_spacing is the physical gap between the inner edges of both brows.
	# Zero lets both sides meet; negative values deliberately overlap for a
	# stronger unibrow/continuous center section.
	var middle_spacing: float = clampf(float(brow_settings.get("middle_spacing", 0.030)), -0.020, 0.060)
	var inner_fade_ratio: float = clampf(float(brow_settings.get("inner_fade_ratio", 0.10)), 0.0, 0.50)
	var outer_fade_ratio: float = clampf(float(brow_settings.get("outer_fade_ratio", 0.18)), 0.0, 0.50)
	var thickness_scale: float = clampf(float(brow_settings.get("thickness", 1.0)), 0.30, 2.50)
	var messiness: float = clampf(float(brow_settings.get("messiness", 0.18)), 0.0, 1.0)

	# The older UI uses +1.5 mm as its visual neutral position. Keep that
	# convention while surface projection handles the actual skin clearance.
	var artistic_offset: float = forward_control - 0.0015

	var center: Vector3 = _head["center"]
	var rx: float = float(_head["rx"])
	var ry: float = float(_head["ry"])
	var rz: float = float(_head["rz"])

	var count_per_side: int = clampi(int(round(lerpf(80.0, 255.0, density))), 54, 280)
	var ribbon_half_width: float = requested_width * 0.30
	var inner_x: float = middle_spacing * 0.5
	# Prevent pathological settings from reversing the brow completely while
	# still allowing inner overlap across the center line.
	inner_x = clampf(inner_x, -rx * 0.22, rx * 0.62)
	var nominal_outer_x: float = rx * 0.79 * brow_width
	var outer_x: float = maxf(nominal_outer_x, inner_x + rx * 0.16)

	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)

	for side_i in 2:
		var side: float = -1.0 if side_i == 0 else 1.0
		for i in count_per_side:
			var h0: float = _hash01(float(i) * 13.91 + side * 5.2)
			var h1: float = _hash01(float(i) * 29.37 + side * 17.4)
			var h2: float = _hash01(float(i) * 47.11 + side * 2.9)
			var h3: float = _hash01(float(i) * 71.03 + side * 11.7)
			var h4: float = _hash01(float(i) * 97.41 + side * 23.1)
			var h5: float = _hash01(float(i) * 131.7 + side * 7.6)

			var t: float = (float(i) + 0.5) / float(count_per_side)
			# Messiness adds irregular follicle spacing, but zero remains orderly.
			t = clampf(t + (h0 - 0.5) / float(count_per_side) * lerpf(0.25, 3.2, messiness), 0.0, 1.0)

			# Density fades are defined as fractions of total brow length. A ratio
			# of zero disables that end fade completely.
			var inner_factor: float = 1.0
			if inner_fade_ratio > 0.0001:
				inner_factor = smoothstep(0.0, inner_fade_ratio, t)
			var outer_factor: float = 1.0
			if outer_fade_ratio > 0.0001:
				outer_factor = smoothstep(0.0, outer_fade_ratio, 1.0 - t)
			var keep_probability: float = clampf(inner_factor * outer_factor, 0.0, 1.0)
			if h4 > keep_probability:
				continue

			var x_offset: float = lerpf(inner_x, outer_x, t)
			var x_jitter: float = (h1 - 0.5) * _character_height * 0.0014 * messiness
			var x: float = center.x + side * (x_offset + x_jitter)

			var centerline_y: float = _brow_centerline_y(t, center.y, ry, arch, height_offset)

			# Multi-row follicles: roots fill a shaped vertical band instead of a
			# single curve. Thickness control scales this whole band.
			var band_random: float = ((h1 + h2 + h3) / 3.0 - 0.5) * 2.0
			var inner_band: float = _character_height * 0.0060
			var tail_band: float = _character_height * 0.0020
			var band_thickness: float = lerpf(inner_band, tail_band, pow(t, 1.35))
			band_thickness *= 1.0 + sin(t * PI) * 0.18
			band_thickness *= thickness_scale
			var messy_y: float = (h5 - 0.5) * _character_height * 0.0012 * messiness
			var y: float = centerline_y + band_random * band_thickness * 0.50 + messy_y

			var fallback_z: float = _ellipsoid_front_z(x, y, center, rx, ry, rz)
			var root_place: Dictionary = _project_to_face(x, y, fallback_z, artistic_offset)
			var root: Vector3 = root_place["point"]
			var root_normal: Vector3 = root_place["normal"]

			# Natural growth direction: upright at the inner brow, gradually
			# rotating lateral and slightly down toward the tail. Messiness affects
			# angle, length and arc without breaking surface conformity.
			var lateral: float = lerpf(0.34, 1.0, smoothstep(0.0, 0.82, t))
			var vertical: float = lerpf(0.94, -0.18, smoothstep(0.0, 1.0, t))
			lateral += (h2 - 0.5) * 0.34 * messiness
			vertical += (h3 - 0.5) * 0.40 * messiness
			var flow2 := Vector2(side * lateral, vertical).normalized()

			var body_factor: float = 1.0 - absf(t - 0.42) / 0.58
			body_factor = clampf(body_factor, 0.0, 1.0)
			var strand_len: float = _character_height * lerpf(0.0030, 0.0051, body_factor)
			strand_len *= lerpf(1.0 - 0.22 * messiness, 1.0 + 0.22 * messiness, h0)

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

			var lift: float = lerpf(0.00010, 0.00024 + 0.00030 * messiness, h3)
			mid += mid_normal * lift

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
