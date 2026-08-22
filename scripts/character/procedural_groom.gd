extends Node3D

## Runtime procedural groom prototype for the Asterra human.
## Generates crossed-ribbon hair/brow strands as a single ArrayMesh and mounts
## them to the head bone so the groom follows head motion without adding one
## node per strand.

var _character: Node3D
var _source_meshes: Array[MeshInstance3D] = []
var _body_mesh: MeshInstance3D
var _skeleton: Skeleton3D
var _mount: Node3D
var _hair_instance: MeshInstance3D
var _brow_instance: MeshInstance3D
var _hair_material: StandardMaterial3D
var _brow_material: StandardMaterial3D

var _character_bottom := 0.0
var _character_height := 1.75
var _front_sign := 1.0
var _head: Dictionary = {}
var _head_bone_name := "none"

var hair_settings := {
	"enabled": true,
	"style": 0,
	"density": 0.72,
	"length": 0.09,
	"width": 0.00032,
	"tip_thickness": 0.08,
	"curl": 0.10,
	"gravity": 0.38,
	"hairline": 0.52,
	"front_hairline": 0.52,
	"side_hairline": 0.68,
	"back_hairline": 0.76,
	"scalp_scale": 1.0,
	"root_lift": 0.0012,
	"color": Color("352218"),
	"root_color": Color("352218"),
	"tip_color": Color("4d3326"),
	"gradient_bias": 1.35,
	# Frontal control. Long styles put their roots at the hairline and their tips
	# over the eyes; thinning and shortening the front is how a groom stops that
	# without moving the hairline itself.
	"front_density": 1.0,
	"front_length": 0.62,
	# 0 = swept back, 1 = parted. A part is a styling constraint, not an
	# equilibrium, so it is authored into the comb field and held by the rest
	# shape rather than left for gravity to decide.
	"part_style": 0,
	"part_offset": 0.0,
	"part_strength": 0.6,
	"volume": 0.38,
	"sheen": 0.55,
	"sheen_roughness": 0.52,
	"physics_enabled": true,
	"physics_stiffness": 0.68,
	"physics_damping": 0.92,
	"physics_gravity": 0.55,
	"wind_response": 0.12
}

var brow_settings := {
	"enabled": true,
	"density": 0.68,
	"width": 1.0,
	"strand_width": 0.00045,
	"arch": 0.45,
	"height_offset": 0.0,
	"forward_offset": 0.0015,
	"color": Color("3a281e")
}

func configure(character: Node3D, meshes: Array[MeshInstance3D], character_bottom: float, character_height: float, front_sign: float = 1.0, rebuild_initial: bool = true) -> bool:
	_character = character
	_source_meshes = meshes
	_character_bottom = character_bottom
	_character_height = maxf(character_height, 0.5)
	_front_sign = 1.0 if front_sign >= 0.0 else -1.0
	_body_mesh = _find_largest_mesh()
	if _body_mesh == null:
		push_warning("Procedural groom: could not identify a body mesh")
		return false

	_estimate_head()
	_create_mount()
	_create_render_nodes()
	_apply_material_settings()
	if rebuild_initial:
		rebuild_all()
	return true

func set_front_sign(sign_value: float) -> void:
	_front_sign = 1.0 if sign_value >= 0.0 else -1.0
	if _body_mesh != null:
		_estimate_head()
		rebuild_all()

func apply_hair(settings: Dictionary) -> void:
	for key in settings.keys():
		hair_settings[key] = settings[key]
	_apply_material_settings()
	rebuild_hair()

func set_hair_runtime_setting(key: StringName, value: Variant) -> void:
	# Physics controls are intentionally live. Rebuilding the render population
	# would reset guide history and hide the response being tuned.
	hair_settings[key] = value

func apply_brows(settings: Dictionary) -> void:
	for key in settings.keys():
		brow_settings[key] = settings[key]
	_apply_material_settings()
	rebuild_brows()

func rebuild_all() -> void:
	rebuild_hair()
	rebuild_brows()

func rebuild_hair() -> void:
	if _hair_instance == null or _mount == null or _head.is_empty():
		return
	_hair_instance.visible = bool(hair_settings.get("enabled", true))
	if not _hair_instance.visible:
		_hair_instance.mesh = null
		return

	var density := clampf(float(hair_settings.get("density", 0.58)), 0.05, 1.0)
	var length := clampf(float(hair_settings.get("length", 0.085)), 0.004, 0.55)
	var render_width := clampf(float(hair_settings.get("width", 0.0009)), 0.00012, 0.004)
	var curl := clampf(float(hair_settings.get("curl", 0.08)), 0.0, 1.0)
	var gravity := clampf(float(hair_settings.get("gravity", 0.42)), 0.0, 1.25)
	var hairline := clampf(float(hair_settings.get("hairline", 0.46)), 0.0, 1.0)
	var scalp_scale := clampf(float(hair_settings.get("scalp_scale", 1.0)), 0.75, 1.25)
	var root_lift := clampf(float(hair_settings.get("root_lift", 0.0025)), 0.0, 0.012)
	var style := int(hair_settings.get("style", 0))

	var target_strands := clampi(int(round(lerpf(220.0, 1450.0, density))), 120, 1600)
	var segments := clampi(int(ceil(length / 0.018)), 1, 18)
	if length < 0.02:
		segments = 1

	var center: Vector3 = _head["center"]
	var rx := float(_head["rx"]) * scalp_scale
	var ry := float(_head["ry"]) * scalp_scale
	var rz := float(_head["rz"]) * scalp_scale
	var front := Vector3(0.0, 0.0, _front_sign)
	var back := -front
	var golden_angle := PI * (3.0 - sqrt(5.0))
	var max_theta := 1.73
	var cos_max := cos(max_theta)

	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var produced := 0
	var attempts := target_strands * 3

	for i in attempts:
		if produced >= target_strands:
			break
		var u := (float(i) + 0.5) / float(attempts)
		var cos_theta := lerpf(1.0, cos_max, u)
		var sin_theta := sqrt(maxf(0.0, 1.0 - cos_theta * cos_theta))
		var phi := float(i) * golden_angle + _hash01(float(i) * 7.13) * 0.18
		var nx := sin_theta * cos(phi)
		var ny := cos_theta
		var nz := sin_theta * sin(phi)
		var frontness := nz * _front_sign

		# Front roots are clipped into a controllable hairline while side/back
		# roots are allowed lower around the skull.
		var line_limit := lerpf(-0.08, 0.46, hairline)
		if frontness > 0.0:
			var required_y := line_limit * lerpf(0.30, 1.0, frontness)
			if ny < required_y:
				continue
		if absf(nx) > 0.90 and ny < -0.02:
			continue

		var root := center + Vector3(rx * nx, ry * ny, rz * nz)
		var normal := Vector3(nx / maxf(rx, 0.001), ny / maxf(ry, 0.001), nz / maxf(rz, 0.001)).normalized()
		root += normal * root_lift

		var tangent_a := normal.cross(Vector3.UP)
		if tangent_a.length_squared() < 0.0001:
			tangent_a = Vector3.RIGHT
		tangent_a = tangent_a.normalized()
		var tangent_b := normal.cross(tangent_a).normalized()
		var phase := _hash01(float(i) * 31.71 + 0.37) * TAU
		var random_side := (_hash01(float(i) * 19.19) - 0.5) * 2.0

		var flow := back * 0.10
		var style_gravity := gravity
		match style:
			1: # swept back
				flow = back * 0.78
			2: # side part left
				flow = Vector3(-0.62, 0.0, -0.24 * _front_sign)
			3: # side part right
				flow = Vector3(0.62, 0.0, -0.24 * _front_sign)
			4: # spiky
				flow = normal * 0.34
				style_gravity *= 0.20

		var points := PackedVector3Array()
		for j in segments + 1:
			var t := float(j) / float(segments)
			var radial_scale := 1.0 - clampf(style_gravity * 0.48 * t, 0.0, 0.72)
			var p := root
			p += normal * length * t * radial_scale
			p += Vector3.DOWN * length * style_gravity * t * t
			p += flow * length * t * t
			var curl_radius := length * curl * (0.018 + 0.070 * t)
			var curl_angle := phase + t * TAU * (0.35 + curl * 1.85)
			p += tangent_a * sin(curl_angle) * curl_radius * t
			p += tangent_b * (cos(curl_angle) - cos(phase)) * curl_radius * 0.62 * t
			p += tangent_a * random_side * length * 0.025 * t * t
			points.append(_mount.to_local(p))

		for j in segments:
			var t0 := float(j) / float(segments)
			var t1 := float(j + 1) / float(segments)
			var w0 := render_width * lerpf(1.0, 0.22, t0)
			var w1 := render_width * lerpf(1.0, 0.08, t1)
			_add_crossed_segment(st, points[j], points[j + 1], w0, w1, t0, t1)
		produced += 1

	if produced == 0:
		_hair_instance.mesh = null
		return
	st.generate_normals()
	st.generate_tangents()
	_hair_instance.mesh = st.commit()
	_hair_instance.material_override = _hair_material

func rebuild_brows() -> void:
	if _brow_instance == null or _mount == null or _head.is_empty():
		return
	_brow_instance.visible = bool(brow_settings.get("enabled", true))
	if not _brow_instance.visible:
		_brow_instance.mesh = null
		return

	var density := clampf(float(brow_settings.get("density", 0.68)), 0.05, 1.0)
	var brow_width := clampf(float(brow_settings.get("width", 1.0)), 0.55, 1.45)
	var strand_width := clampf(float(brow_settings.get("strand_width", 0.00045)), 0.00012, 0.002)
	var arch := clampf(float(brow_settings.get("arch", 0.45)), 0.0, 1.25)
	var height_offset := clampf(float(brow_settings.get("height_offset", 0.0)), -0.035, 0.035)
	var forward_offset := clampf(float(brow_settings.get("forward_offset", 0.0015)), -0.025, 0.035)
	var center: Vector3 = _head["center"]
	var rx := float(_head["rx"])
	var ry := float(_head["ry"])
	var rz := float(_head["rz"])
	var count_per_side := clampi(int(round(lerpf(20.0, 92.0, density))), 12, 110)

	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	for side_i in 2:
		var side := -1.0 if side_i == 0 else 1.0
		for i in count_per_side:
			var jitter := (_hash01(float(i) * 13.91 + side * 5.2) - 0.5) / float(count_per_side)
			var t := clampf((float(i) + 0.5) / float(count_per_side) + jitter, 0.0, 1.0)
			var inner_x := rx * 0.18
			var outer_x := rx * 0.79 * brow_width
			var x_offset := lerpf(inner_x, outer_x, t)
			var x := center.x + side * x_offset
			var base_y := center.y + ry * 0.37 + height_offset
			var y := base_y + sin(t * PI) * (_character_height * 0.0065 * arch)
			y -= t * _character_height * 0.0024
			var xn := (x - center.x) / maxf(rx, 0.001)
			var yn := (y - center.y) / maxf(ry, 0.001)
			var surface := sqrt(maxf(0.04, 1.0 - xn * xn - yn * yn))
			var z := center.z + _front_sign * rz * surface + _front_sign * forward_offset
			var root := Vector3(x, y, z)

			var strand_len := _character_height * lerpf(0.0035, 0.0054, 1.0 - absf(t - 0.45))
			var dir := Vector3(side * (0.58 + 0.30 * t), 0.70 - 0.82 * t, -0.06 * _front_sign).normalized()
			var tip := root + dir * strand_len
			var root_local := _mount.to_local(root)
			var tip_local := _mount.to_local(tip)
			_add_crossed_segment(st, root_local, tip_local, strand_width, strand_width * 0.12, 0.0, 1.0)

	st.generate_normals()
	st.generate_tangents()
	_brow_instance.mesh = st.commit()
	_brow_instance.material_override = _brow_material

func diagnostics() -> String:
	if _head.is_empty():
		return "Procedural groom: head estimate unavailable"
	return "Head bone: %s • scalp %.0f × %.0f × %.0f mm" % [
		_head_bone_name,
		float(_head["rx"]) * 2000.0,
		float(_head["ry"]) * 2000.0,
		float(_head["rz"]) * 2000.0
	]

func _apply_material_settings() -> void:
	if _hair_material != null:
		var hair_color: Color = hair_settings.get("color", Color("4b3426"))
		_hair_material.albedo_color = hair_color
		_hair_material.backlight = hair_color.lightened(0.16)
	if _brow_material != null:
		var brow_color: Color = brow_settings.get("color", Color("3a281e"))
		_brow_material.albedo_color = brow_color
		_brow_material.backlight = brow_color.lightened(0.12)

func _create_render_nodes() -> void:
	_hair_material = _make_hair_material(Color(hair_settings.get("color", Color("4b3426"))))
	_brow_material = _make_hair_material(Color(brow_settings.get("color", Color("3a281e"))))
	_brow_material.roughness = 0.56

	_hair_instance = MeshInstance3D.new()
	_hair_instance.name = "ProceduralHair"
	_mount.add_child(_hair_instance)

	_brow_instance = MeshInstance3D.new()
	_brow_instance.name = "ProceduralBrows"
	_mount.add_child(_brow_instance)

func _make_hair_material(color: Color) -> StandardMaterial3D:
	var mat := StandardMaterial3D.new()
	mat.albedo_color = color
	mat.metallic = 0.0
	mat.roughness = 0.42
	mat.cull_mode = BaseMaterial3D.CULL_DISABLED
	mat.anisotropy_enabled = true
	mat.anisotropy = 0.72
	mat.backlight_enabled = true
	mat.backlight = color.lightened(0.15)
	return mat

func _create_mount() -> void:
	_skeleton = _find_skeleton(_character)
	if _skeleton == null:
		_mount = Node3D.new()
		_mount.name = "ProceduralGroomMount"
		_character.add_child(_mount)
		_head_bone_name = "character root"
		return

	var head_idx := _find_head_bone(_skeleton)
	if head_idx < 0:
		_mount = Node3D.new()
		_mount.name = "ProceduralGroomMount"
		_character.add_child(_mount)
		_head_bone_name = "character root"
		return

	var attachment := BoneAttachment3D.new()
	attachment.name = "ProceduralGroomHeadAttachment"
	attachment.bone_idx = head_idx
	_skeleton.add_child(attachment)
	attachment.on_skeleton_update()
	_mount = attachment
	_head_bone_name = str(_skeleton.get_bone_name(head_idx))

func _find_skeleton(node: Node) -> Skeleton3D:
	if node is Skeleton3D:
		return node as Skeleton3D
	for child in node.get_children():
		var found := _find_skeleton(child)
		if found != null:
			return found
	return null

func _find_head_bone(skeleton: Skeleton3D) -> int:
	var fallback := -1
	for i in skeleton.get_bone_count():
		var raw := str(skeleton.get_bone_name(i))
		var normalized := raw.to_lower().replace("_", "").replace(".", "").replace("-", "")
		if normalized == "head":
			return i
		if fallback < 0 and normalized.contains("head") and not normalized.contains("end"):
			fallback = i
	return fallback

func _find_largest_mesh() -> MeshInstance3D:
	var best: MeshInstance3D
	var best_volume := -1.0
	for mesh_instance in _source_meshes:
		if mesh_instance.mesh == null:
			continue
		var b := mesh_instance.get_aabb()
		var volume := absf(b.size.x * b.size.y * b.size.z)
		if volume > best_volume:
			best_volume = volume
			best = mesh_instance
	return best

func _estimate_head() -> void:
	if _body_mesh == null or _body_mesh.mesh == null:
		_head.clear()
		return

	var body_bounds := _mesh_world_bounds(_body_mesh)
	var top := body_bounds.position.y + body_bounds.size.y
	var cutoff := top - _character_height * 0.155
	var min_x := INF
	var max_x := -INF
	var min_y := INF
	var min_z := INF
	var max_z := -INF
	var found := false

	for surface in _body_mesh.mesh.get_surface_count():
		var arrays := _body_mesh.mesh.surface_get_arrays(surface)
		if arrays.size() <= Mesh.ARRAY_VERTEX or arrays[Mesh.ARRAY_VERTEX] == null:
			continue
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		for vertex in vertices:
			var p := _body_mesh.to_global(vertex)
			if p.y < cutoff:
				continue
			min_x = minf(min_x, p.x)
			max_x = maxf(max_x, p.x)
			min_y = minf(min_y, p.y)
			min_z = minf(min_z, p.z)
			max_z = maxf(max_z, p.z)
			found = true

	if not found:
		var center := body_bounds.position + body_bounds.size * 0.5
		var rx := _character_height * 0.050
		var ry := _character_height * 0.068
		var rz := _character_height * 0.058
		_head = {
			"center": Vector3(center.x, top - ry, center.z - _front_sign * rz * 0.08),
			"rx": rx,
			"ry": ry,
			"rz": rz,
			"top": top
		}
		return

	var width := maxf(max_x - min_x, _character_height * 0.085)
	var head_height := maxf(top - min_y, _character_height * 0.115)
	var depth := maxf(max_z - min_z, _character_height * 0.085)
	var rx := width * 0.47
	var ry := head_height * 0.52
	var rz := depth * 0.42
	var center_z := (min_z + max_z) * 0.5 - _front_sign * depth * 0.065
	_head = {
		"center": Vector3((min_x + max_x) * 0.5, top - ry * 0.98, center_z),
		"rx": rx,
		"ry": ry,
		"rz": rz,
		"top": top
	}

func _mesh_world_bounds(mesh_instance: MeshInstance3D) -> AABB:
	var local := mesh_instance.get_aabb()
	var min_point := Vector3(INF, INF, INF)
	var max_point := Vector3(-INF, -INF, -INF)
	for x in 2:
		for y in 2:
			for z in 2:
				var p := local.position + Vector3(local.size.x * x, local.size.y * y, local.size.z * z)
				var world := mesh_instance.to_global(p)
				min_point.x = minf(min_point.x, world.x)
				min_point.y = minf(min_point.y, world.y)
				min_point.z = minf(min_point.z, world.z)
				max_point.x = maxf(max_point.x, world.x)
				max_point.y = maxf(max_point.y, world.y)
				max_point.z = maxf(max_point.z, world.z)
	return AABB(min_point, max_point - min_point)

func _add_crossed_segment(st: SurfaceTool, a: Vector3, b: Vector3, width_a: float, width_b: float, v0: float, v1: float) -> void:
	var direction := b - a
	if direction.length_squared() < 0.00000001:
		return
	direction = direction.normalized()
	var side_a := direction.cross(Vector3.UP)
	if side_a.length_squared() < 0.0001:
		side_a = direction.cross(Vector3.FORWARD)
	side_a = side_a.normalized()
	var side_b := direction.cross(side_a).normalized()
	_add_quad(st, a - side_a * width_a, a + side_a * width_a, b + side_a * width_b, b - side_a * width_b, v0, v1)
	_add_quad(st, a - side_b * width_a, a + side_b * width_a, b + side_b * width_b, b - side_b * width_b, v0, v1)

func _add_quad(st: SurfaceTool, a: Vector3, b: Vector3, c: Vector3, d: Vector3, v0: float, v1: float) -> void:
	st.set_uv(Vector2(0.0, v0))
	st.add_vertex(a)
	st.set_uv(Vector2(1.0, v0))
	st.add_vertex(b)
	st.set_uv(Vector2(1.0, v1))
	st.add_vertex(c)
	st.set_uv(Vector2(0.0, v0))
	st.add_vertex(a)
	st.set_uv(Vector2(1.0, v1))
	st.add_vertex(c)
	st.set_uv(Vector2(0.0, v1))
	st.add_vertex(d)

func _hash01(value: float) -> float:
	return fposmod(sin(value * 12.9898 + 78.233) * 43758.5453, 1.0)
