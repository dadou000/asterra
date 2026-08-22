extends "res://scripts/character/advanced_surface_groom.gd"

## Brow rendering refinement.
##
## LOD0/1 use tiny triangular-prism hairs instead of single flat ribbons. A
## strand therefore keeps visible screen-space thickness from frontal, oblique
## and temple views instead of disappearing whenever its ribbon turns edge-on.
## Follicle-row position also changes growth direction slightly, creating the
## overlapping lower-upward / upper-outward pattern visible in real brows.
##
## LOD2 deliberately avoids the old solid brow strip. It uses a small field of
## wider tapered macro-hairs. At its intended distance they merge into the brow
## mass, while forced close-up testing remains much softer than an opaque slab.

const BROW_TUBE_SIDES := 3
const BROW_LOD2_MACRO_STRANDS := 52
const BROW_LOD2_WIDTH_COMPENSATION := 1.70

func _build_brow_mesh(lod: int) -> ArrayMesh:
	if lod == 2:
		return _build_brow_lod2_mesh()

	var density: float = clampf(float(brow_settings.get("density", 1.0)), 0.05, 1.0)
	var brow_width: float = clampf(float(brow_settings.get("width", 0.73)), 0.55, 1.45)
	var requested_width: float = clampf(float(brow_settings.get("strand_width", 0.00045)), 0.00008, 0.0012)
	var arch: float = clampf(float(brow_settings.get("arch", 0.33)), 0.0, 1.25)
	var height_offset: float = clampf(float(brow_settings.get("height_offset", -0.013)), -0.035, 0.035)
	var forward_control: float = clampf(float(brow_settings.get("forward_offset", 0.0025)), -0.010, 0.025)
	var middle_spacing: float = clampf(float(brow_settings.get("middle_spacing", 0.008)), -0.020, 0.060)
	var inner_fade_ratio: float = clampf(float(brow_settings.get("inner_fade_ratio", 0.14)), 0.0, 0.50)
	var outer_fade_ratio: float = clampf(float(brow_settings.get("outer_fade_ratio", 0.18)), 0.0, 0.50)
	var thickness_scale: float = clampf(float(brow_settings.get("thickness", 0.67)), 0.30, 2.50)
	var messiness: float = clampf(float(brow_settings.get("messiness", 0.16)), 0.0, 1.0)
	var artistic_offset: float = forward_control - 0.0015

	var center: Vector3 = _head["center"]
	var rx: float = float(_head["rx"])
	var ry: float = float(_head["ry"])
	var rz: float = float(_head["rz"])

	var count_per_side: int = clampi(int(round(lerpf(80.0, 255.0, density))), 54, 280)
	# Radius rather than ribbon half-width. Keeping this slightly smaller than
	# the old flat ribbon avoids making the brow heavier merely because the hair
	# is now visible from every angle.
	var strand_radius: float = requested_width * 0.24
	if lod == 1:
		strand_radius *= 1.38

	var inner_x: float = clampf(middle_spacing * 0.5, -rx * 0.22, rx * 0.62)
	var nominal_outer_x: float = rx * 0.79 * brow_width
	var outer_x: float = maxf(nominal_outer_x, inner_x + rx * 0.16)

	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)

	for side_i in 2:
		var side: float = -1.0 if side_i == 0 else 1.0
		for i in count_per_side:
			if lod == 1 and (i % BROW_LOD1_STRIDE) != 0:
				continue

			var h0: float = _hash01(float(i) * 13.91 + side * 5.2)
			var h1: float = _hash01(float(i) * 29.37 + side * 17.4)
			var h2: float = _hash01(float(i) * 47.11 + side * 2.9)
			var h3: float = _hash01(float(i) * 71.03 + side * 11.7)
			var h4: float = _hash01(float(i) * 97.41 + side * 23.1)
			var h5: float = _hash01(float(i) * 131.7 + side * 7.6)

			var t: float = (float(i) + 0.5) / float(count_per_side)
			t = clampf(t + (h0 - 0.5) / float(count_per_side) * lerpf(0.25, 3.2, messiness), 0.0, 1.0)

			var inner_factor: float = 1.0
			if inner_fade_ratio > 0.0001:
				inner_factor = smoothstep(0.0, inner_fade_ratio, t)
			var outer_factor: float = 1.0
			if outer_fade_ratio > 0.0001:
				outer_factor = smoothstep(0.0, outer_fade_ratio, 1.0 - t)
			if h4 > clampf(inner_factor * outer_factor, 0.0, 1.0):
				continue

			var x_offset: float = lerpf(inner_x, outer_x, t)
			var x_jitter: float = (h1 - 0.5) * _character_height * 0.0014 * messiness
			var x: float = center.x + side * (x_offset + x_jitter)

			var centerline_y: float = _brow_centerline_y(t, center.y, ry, arch, height_offset)
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

			# Real brows are not a set of equal parallel strokes. Lower-row hairs
			# tend to rise into the brow body while upper-row hairs lie more outward.
			# band_random gives us that follicle-row position at effectively zero cost.
			var lateral: float = lerpf(0.34, 1.0, smoothstep(0.0, 0.82, t))
			var vertical: float = lerpf(0.94, -0.18, smoothstep(0.0, 1.0, t))
			lateral += band_random * 0.13
			vertical -= band_random * 0.27
			lateral += (h2 - 0.5) * 0.28 * messiness
			vertical += (h3 - 0.5) * 0.34 * messiness
			var flow2: Vector2 = Vector2(side * lateral, vertical).normalized()

			var body_factor: float = clampf(1.0 - absf(t - 0.42) / 0.58, 0.0, 1.0)
			var strand_len: float = _character_height * lerpf(0.0028, 0.0048, body_factor)
			strand_len *= lerpf(1.0 - 0.18 * messiness, 1.0 + 0.18 * messiness, h0)

			var tip_x: float = x + flow2.x * strand_len
			var tip_y: float = y + flow2.y * strand_len
			var tip_fallback_z: float = _ellipsoid_front_z(tip_x, tip_y, center, rx, ry, rz)
			var tip_place: Dictionary = _project_to_face(tip_x, tip_y, tip_fallback_z, artistic_offset)
			var tip: Vector3 = tip_place["point"]
			var tip_normal: Vector3 = tip_place["normal"]

			var root_local: Vector3 = _mount.to_local(root)
			var tip_local: Vector3 = _mount.to_local(tip)
			var root_normal_local: Vector3 = (_mount.global_transform.basis.inverse() * root_normal).normalized()
			var tip_normal_local: Vector3 = (_mount.global_transform.basis.inverse() * tip_normal).normalized()

			if lod == 1:
				_add_tri_tube_segment(st, root_local, tip_local, root_normal_local, tip_normal_local, strand_radius, strand_radius * 0.08, 0.0, 1.0)
			else:
				var mid_x: float = x + flow2.x * strand_len * 0.54
				var mid_y: float = y + flow2.y * strand_len * 0.54
				var mid_fallback_z: float = _ellipsoid_front_z(mid_x, mid_y, center, rx, ry, rz)
				var mid_place: Dictionary = _project_to_face(mid_x, mid_y, mid_fallback_z, artistic_offset)
				var mid: Vector3 = mid_place["point"]
				var mid_normal: Vector3 = mid_place["normal"]
				# Keep the arc subtle. The old larger lift exaggerated ribbon twist.
				var lift: float = lerpf(0.00004, 0.00014 + 0.00010 * messiness, h3)
				mid += mid_normal * lift
				var mid_local: Vector3 = _mount.to_local(mid)
				var mid_normal_local: Vector3 = (_mount.global_transform.basis.inverse() * mid_normal).normalized()
				_add_tri_tube_segment(st, root_local, mid_local, root_normal_local, mid_normal_local, strand_radius, strand_radius * 0.78, 0.0, 0.54)
				_add_tri_tube_segment(st, mid_local, tip_local, mid_normal_local, tip_normal_local, strand_radius * 0.78, strand_radius * 0.06, 0.54, 1.0)

	st.generate_normals()
	st.generate_tangents()
	return st.commit()

func _build_brow_lod2_mesh() -> ArrayMesh:
	var density: float = clampf(float(brow_settings.get("density", 1.0)), 0.05, 1.0)
	var brow_width: float = clampf(float(brow_settings.get("width", 0.73)), 0.55, 1.45)
	var requested_width: float = clampf(float(brow_settings.get("strand_width", 0.00045)), 0.00008, 0.0012)
	var arch: float = clampf(float(brow_settings.get("arch", 0.33)), 0.0, 1.25)
	var height_offset: float = clampf(float(brow_settings.get("height_offset", -0.013)), -0.035, 0.035)
	var forward_control: float = clampf(float(brow_settings.get("forward_offset", 0.0025)), -0.010, 0.025)
	var middle_spacing: float = clampf(float(brow_settings.get("middle_spacing", 0.008)), -0.020, 0.060)
	var inner_fade_ratio: float = clampf(float(brow_settings.get("inner_fade_ratio", 0.14)), 0.0, 0.50)
	var outer_fade_ratio: float = clampf(float(brow_settings.get("outer_fade_ratio", 0.18)), 0.0, 0.50)
	var thickness_scale: float = clampf(float(brow_settings.get("thickness", 0.67)), 0.30, 2.50)
	var messiness: float = clampf(float(brow_settings.get("messiness", 0.16)), 0.0, 1.0)
	var artistic_offset: float = forward_control - 0.0015

	var center: Vector3 = _head["center"]
	var rx: float = float(_head["rx"])
	var ry: float = float(_head["ry"])
	var rz: float = float(_head["rz"])
	var inner_x: float = clampf(middle_spacing * 0.5, -rx * 0.22, rx * 0.62)
	var nominal_outer_x: float = rx * 0.79 * brow_width
	var outer_x: float = maxf(nominal_outer_x, inner_x + rx * 0.16)
	var count_per_side: int = clampi(int(round(lerpf(22.0, float(BROW_LOD2_MACRO_STRANDS), density))), 18, BROW_LOD2_MACRO_STRANDS)
	var macro_half_width: float = requested_width * BROW_LOD2_WIDTH_COMPENSATION * 0.50

	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)

	for side_i in 2:
		var side: float = -1.0 if side_i == 0 else 1.0
		for i in count_per_side:
			var h0: float = _hash01(float(i) * 17.91 + side * 3.7)
			var h1: float = _hash01(float(i) * 37.13 + side * 13.4)
			var h2: float = _hash01(float(i) * 61.07 + side * 7.9)
			var h3: float = _hash01(float(i) * 83.39 + side * 19.2)
			var t: float = (float(i) + 0.5) / float(count_per_side)
			t = clampf(t + (h0 - 0.5) / float(count_per_side) * 1.8, 0.0, 1.0)

			var inner_factor: float = 1.0
			if inner_fade_ratio > 0.0001:
				inner_factor = smoothstep(0.0, inner_fade_ratio, t)
			var outer_factor: float = 1.0
			if outer_fade_ratio > 0.0001:
				outer_factor = smoothstep(0.0, outer_fade_ratio, 1.0 - t)
			if h3 > clampf(inner_factor * outer_factor, 0.0, 1.0):
				continue

			var x_offset: float = lerpf(inner_x, outer_x, t)
			var x: float = center.x + side * (x_offset + (h1 - 0.5) * _character_height * 0.0007 * messiness)
			var centerline_y: float = _brow_centerline_y(t, center.y, ry, arch, height_offset)
			var band_pos: float = (h1 + h2 - 1.0)
			var inner_band: float = _character_height * 0.0060
			var tail_band: float = _character_height * 0.0020
			var band_thickness: float = lerpf(inner_band, tail_band, pow(t, 1.35)) * thickness_scale
			var y: float = centerline_y + band_pos * band_thickness * 0.42

			var root_fallback_z: float = _ellipsoid_front_z(x, y, center, rx, ry, rz)
			var root_place: Dictionary = _project_to_face(x, y, root_fallback_z, artistic_offset)
			var root: Vector3 = root_place["point"]
			var root_normal: Vector3 = root_place["normal"]

			var lateral: float = lerpf(0.40, 1.0, smoothstep(0.0, 0.85, t)) + band_pos * 0.10
			var vertical: float = lerpf(0.86, -0.16, t) - band_pos * 0.20
			var flow2: Vector2 = Vector2(side * lateral, vertical).normalized()
			var body_factor: float = clampf(1.0 - absf(t - 0.42) / 0.58, 0.0, 1.0)
			var strand_len: float = _character_height * lerpf(0.0027, 0.0045, body_factor)
			strand_len *= lerpf(0.90, 1.10, h2)

			var tip_x: float = x + flow2.x * strand_len
			var tip_y: float = y + flow2.y * strand_len
			var tip_fallback_z: float = _ellipsoid_front_z(tip_x, tip_y, center, rx, ry, rz)
			var tip_place: Dictionary = _project_to_face(tip_x, tip_y, tip_fallback_z, artistic_offset)
			var tip: Vector3 = tip_place["point"]
			var tip_normal: Vector3 = tip_place["normal"]

			var root_local: Vector3 = _mount.to_local(root)
			var tip_local: Vector3 = _mount.to_local(tip)
			var root_normal_local: Vector3 = (_mount.global_transform.basis.inverse() * root_normal).normalized()
			var tip_normal_local: Vector3 = (_mount.global_transform.basis.inverse() * tip_normal).normalized()
			_add_skin_ribbon(st, root_local, tip_local, root_normal_local, tip_normal_local, macro_half_width, macro_half_width * 0.12, 0.0, 1.0)

	st.generate_normals()
	st.generate_tangents()
	return st.commit()

func _add_tri_tube_segment(
	st: SurfaceTool,
	a: Vector3,
	b: Vector3,
	normal_a: Vector3,
	normal_b: Vector3,
	radius_a: float,
	radius_b: float,
	v0: float,
	v1: float
) -> void:
	var direction: Vector3 = b - a
	if direction.length_squared() < 0.00000001:
		return
	direction = direction.normalized()

	var tangent_a: Vector3 = normal_a.cross(direction)
	if tangent_a.length_squared() < 0.000001:
		tangent_a = Vector3.UP.cross(direction)
	if tangent_a.length_squared() < 0.000001:
		tangent_a = Vector3.RIGHT
	tangent_a = tangent_a.normalized()
	var binormal_a: Vector3 = direction.cross(tangent_a).normalized()

	var tangent_b: Vector3 = normal_b.cross(direction)
	if tangent_b.length_squared() < 0.000001:
		tangent_b = tangent_a
	tangent_b = tangent_b.normalized()
	if tangent_a.dot(tangent_b) < 0.0:
		tangent_b = -tangent_b
	var binormal_b: Vector3 = direction.cross(tangent_b).normalized()
	if binormal_a.dot(binormal_b) < 0.0:
		binormal_b = -binormal_b

	var ring_a: Array[Vector3] = []
	var ring_b: Array[Vector3] = []
	for side_i in BROW_TUBE_SIDES:
		# Offset the phase so one face lies approximately against the skin rather
		# than placing a sharp triangular edge on it.
		var angle: float = TAU * float(side_i) / float(BROW_TUBE_SIDES) + PI * 0.5
		var radial_a: Vector3 = tangent_a * cos(angle) + binormal_a * sin(angle)
		var radial_b: Vector3 = tangent_b * cos(angle) + binormal_b * sin(angle)
		ring_a.append(a + radial_a * radius_a)
		ring_b.append(b + radial_b * radius_b)

	for side_i in BROW_TUBE_SIDES:
		var next_i: int = (side_i + 1) % BROW_TUBE_SIDES
		_add_quad(st, ring_a[side_i], ring_a[next_i], ring_b[next_i], ring_b[side_i], v0, v1)
