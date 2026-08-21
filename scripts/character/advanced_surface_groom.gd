extends "res://scripts/character/surface_groom.gd"

## Advanced surface-conforming brow groom with runtime LOD.
##
## LOD0 is the existing full-quality groom and is used while the active camera
## is closer than 6 m to the character's head. LOD1 uses an exact subset of the
## same follicles, so the shape remains stable when switching, but renders only
## one quarter of the hairs and one ribbon segment per hair.

const BROW_LOD1_DISTANCE := 6.0
const BROW_LOD_CHECK_INTERVAL := 0.10
const BROW_LOD1_STRIDE := 4

var _brow_lod1_instance: MeshInstance3D
var _active_brow_lod := 0
var _brow_lod_elapsed := 0.0
var _last_brow_distance := -1.0

func _create_render_nodes() -> void:
	super._create_render_nodes()
	_brow_lod1_instance = MeshInstance3D.new()
	_brow_lod1_instance.name = "ProceduralBrowsLOD1"
	_brow_lod1_instance.visible = false
	_mount.add_child(_brow_lod1_instance)

func _process(delta: float) -> void:
	_brow_lod_elapsed += delta
	if _brow_lod_elapsed < BROW_LOD_CHECK_INTERVAL:
		return
	_brow_lod_elapsed = 0.0
	_update_brow_lod_visibility()

func rebuild_brows() -> void:
	if _brow_instance == null or _brow_lod1_instance == null or _mount == null or _head.is_empty():
		return

	var enabled: bool = bool(brow_settings.get("enabled", true))
	if not enabled:
		_brow_instance.visible = false
		_brow_lod1_instance.visible = false
		_brow_instance.mesh = null
		_brow_lod1_instance.mesh = null
		return

	_ensure_face_triangle_cache()
	_brow_instance.mesh = _build_brow_mesh(0)
	_brow_lod1_instance.mesh = _build_brow_mesh(1)
	_brow_instance.material_override = _brow_material
	_brow_lod1_instance.material_override = _brow_material
	_update_brow_lod_visibility(true)

func _build_brow_mesh(lod: int) -> ArrayMesh:
	var density: float = clampf(float(brow_settings.get("density", 0.68)), 0.05, 1.0)
	var brow_width: float = clampf(float(brow_settings.get("width", 1.0)), 0.55, 1.45)
	var requested_width: float = clampf(float(brow_settings.get("strand_width", 0.00045)), 0.00008, 0.0012)
	var arch: float = clampf(float(brow_settings.get("arch", 0.45)), 0.0, 1.25)
	var height_offset: float = clampf(float(brow_settings.get("height_offset", 0.0)), -0.035, 0.035)
	var forward_control: float = clampf(float(brow_settings.get("forward_offset", 0.0015)), -0.010, 0.025)
	var middle_spacing: float = clampf(float(brow_settings.get("middle_spacing", 0.030)), -0.020, 0.060)
	var inner_fade_ratio: float = clampf(float(brow_settings.get("inner_fade_ratio", 0.10)), 0.0, 0.50)
	var outer_fade_ratio: float = clampf(float(brow_settings.get("outer_fade_ratio", 0.18)), 0.0, 0.50)
	var thickness_scale: float = clampf(float(brow_settings.get("thickness", 1.0)), 0.30, 2.50)
	var messiness: float = clampf(float(brow_settings.get("messiness", 0.18)), 0.0, 1.0)

	# The older UI uses +1.5 mm as its visual neutral position. Surface
	# projection handles the actual skin clearance.
	var artistic_offset: float = forward_control - 0.0015

	var center: Vector3 = _head["center"]
	var rx: float = float(_head["rx"])
	var ry: float = float(_head["ry"])
	var rz: float = float(_head["rz"])

	# LOD1 deliberately samples the LOD0 follicle sequence rather than using a
	# separately distributed set. This makes most of the surviving hairs occupy
	# exactly the same roots at the 6 m transition.
	var count_per_side: int = clampi(int(round(lerpf(80.0, 255.0, density))), 54, 280)
	var ribbon_half_width: float = requested_width * 0.30
	if lod == 1:
		# Fewer hairs need a little more visual weight at distance. This is still
		# substantially thinner than the old prototype brow ribbons.
		ribbon_half_width *= 1.55

	var inner_x: float = middle_spacing * 0.5
	inner_x = clampf(inner_x, -rx * 0.22, rx * 0.62)
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
			var keep_probability: float = clampf(inner_factor * outer_factor, 0.0, 1.0)
			if h4 > keep_probability:
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

			var lateral: float = lerpf(0.34, 1.0, smoothstep(0.0, 0.82, t))
			var vertical: float = lerpf(0.94, -0.18, smoothstep(0.0, 1.0, t))
			lateral += (h2 - 0.5) * 0.34 * messiness
			vertical += (h3 - 0.5) * 0.40 * messiness
			var flow2: Vector2 = Vector2(side * lateral, vertical).normalized()

			var body_factor: float = 1.0 - absf(t - 0.42) / 0.58
			body_factor = clampf(body_factor, 0.0, 1.0)
			var strand_len: float = _character_height * lerpf(0.0030, 0.0051, body_factor)
			strand_len *= lerpf(1.0 - 0.22 * messiness, 1.0 + 0.22 * messiness, h0)

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
				# LOD1: one surface-conforming quad per surviving hair. At >= 6 m the
				# sub-millimetre arc is no longer resolvable, so the midpoint is omitted.
				_add_skin_ribbon(
					st,
					root_local,
					tip_local,
					root_normal_local,
					tip_normal_local,
					ribbon_half_width,
					ribbon_half_width * 0.05,
					0.0,
					1.0
				)
			else:
				# LOD0: preserve the existing curved two-segment strand.
				var mid_x: float = x + flow2.x * strand_len * 0.52
				var mid_y: float = y + flow2.y * strand_len * 0.52
				var mid_fallback_z: float = _ellipsoid_front_z(mid_x, mid_y, center, rx, ry, rz)
				var mid_place: Dictionary = _project_to_face(mid_x, mid_y, mid_fallback_z, artistic_offset)
				var mid: Vector3 = mid_place["point"]
				var mid_normal: Vector3 = mid_place["normal"]
				var lift: float = lerpf(0.00010, 0.00024 + 0.00030 * messiness, h3)
				mid += mid_normal * lift

				var mid_local: Vector3 = _mount.to_local(mid)
				var mid_normal_local: Vector3 = (_mount.global_transform.basis.inverse() * mid_normal).normalized()
				_add_skin_ribbon(st, root_local, mid_local, root_normal_local, mid_normal_local, ribbon_half_width, ribbon_half_width * 0.72, 0.0, 0.52)
				_add_skin_ribbon(st, mid_local, tip_local, mid_normal_local, tip_normal_local, ribbon_half_width * 0.72, ribbon_half_width * 0.06, 0.52, 1.0)

	st.generate_normals()
	st.generate_tangents()
	return st.commit()

func _update_brow_lod_visibility(force: bool = false) -> void:
	if _brow_instance == null or _brow_lod1_instance == null:
		return
	if not bool(brow_settings.get("enabled", true)):
		_brow_instance.visible = false
		_brow_lod1_instance.visible = false
		return

	var desired_lod := 0
	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera != null and _mount != null:
		_last_brow_distance = camera.global_position.distance_to(_mount.global_position)
		desired_lod = 0 if _last_brow_distance < BROW_LOD1_DISTANCE else 1
	else:
		_last_brow_distance = -1.0

	if not force and desired_lod == _active_brow_lod:
		return
	_active_brow_lod = desired_lod
	_brow_instance.visible = _active_brow_lod == 0
	_brow_lod1_instance.visible = _active_brow_lod == 1

func diagnostics() -> String:
	var base: String = super.diagnostics()
	var distance_note := "no camera"
	if _last_brow_distance >= 0.0:
		distance_note = "%.2f m" % _last_brow_distance
	return "%s • brows LOD%d @ %s (switch 6.0 m)" % [base, _active_brow_lod, distance_note]
