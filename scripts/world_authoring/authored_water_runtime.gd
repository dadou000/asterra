class_name AuthoredWaterRuntime
extends Node
## Runtime compiler for Planet Studio lakes and rivers.
##
## The authoring resources remain the source of truth. This node turns their
## body-space polygon/Bézier data into disposable local meshes positioned through
## Frames, so floating-origin shifts never require rewriting the mesh vertices.
## Lakes are triangulated in a local tangent plane and recursively subdivided back
## onto the sphere. Rivers are adaptive Bézier ribbons. The surface shader samples
## the same procedural terrain + persistent/active edit textures as the terrain
## stack and clips water where the current ground rises through it.

const SHADER_PATH := "res://shaders/authored_water_surface.gdshader"
const LAKE_MAX_EDGE_M: float = 96.0
const LAKE_MAX_SUBDIVISION: int = 8
const RIVER_MIN_SAMPLE_STEP_M: float = 3.0
const RIVER_MAX_SAMPLE_STEP_M: float = 24.0
const RIVER_MAX_SEGMENT_SAMPLES: int = 256
const LAKE_SURFACE_BIAS_M: float = 0.035
const RIVER_SURFACE_BIAS_M: float = 0.08

var _session: WorldAuthoringSession
var _world_host: Node
var _shader: Shader
var _records: Array[Dictionary] = []
var _dirty: bool = true
var _last_body_id: String = ""
var _compiled_lakes: int = 0
var _compiled_rivers: int = 0
var _compiled_triangles: int = 0


func bind(session: WorldAuthoringSession, world_host: Node) -> void:
	_session = session
	_world_host = world_host


func _ready() -> void:
	_shader = load(SHADER_PATH) as Shader
	if _shader == null:
		push_error("Planet Studio authored-water shader could not be loaded.")
		set_process(false)
		return
	if _session != null:
		if not _session.changed.is_connected(_on_authoring_changed):
			_session.changed.connect(_on_authoring_changed)
		if not _session.preset_loaded.is_connected(_on_preset_loaded):
			_session.preset_loaded.connect(_on_preset_loaded)
	if Planet.has_signal("world_ready") and not Planet.world_ready.is_connected(_on_world_ready):
		Planet.world_ready.connect(_on_world_ready)
	_dirty = true


func _process(_delta: float) -> void:
	if _session == null or _world_host == null:
		_set_visible(false)
		return
	var body: Resource = _session.active_body() as Resource
	var body_id: String = String(body.get(&"body_id")) if body != null else ""
	if body_id != _last_body_id:
		_last_body_id = body_id
		_dirty = true
	if _dirty:
		_rebuild()
	var runtime_ready: bool = Planet.ready_state and Planet.cfg != null
	_set_visible(runtime_ready)
	if not runtime_ready:
		return
	_sync_runtime_bindings()


func mark_dirty() -> void:
	_dirty = true


func _on_authoring_changed(_dirty_state: bool, _apply_scope: int) -> void:
	_dirty = true


func _on_preset_loaded(_path: String) -> void:
	_dirty = true


func _on_world_ready(_fields: PlanetFields) -> void:
	_dirty = true


func _rebuild() -> void:
	_dirty = false
	_clear_records()
	_compiled_lakes = 0
	_compiled_rivers = 0
	_compiled_triangles = 0
	if _session == null or _world_host == null or _shader == null:
		return
	var body: Resource = _session.active_body() as Resource
	var water: Resource = _session.active_water_profile() as Resource
	if body == null or water == null:
		return
	var body_radius: float = maxf(float(body.get(&"radius_m")), 1.0)
	var features_value: Variant = water.get(&"authored_features")
	if not (features_value is Array):
		return
	for feature_value: Variant in features_value as Array:
		var feature: Resource = feature_value as Resource
		if feature == null or not bool(feature.get(&"enabled")):
			continue
		var feature_type: int = int(feature.get(&"feature_type"))
		var built: Dictionary = _build_river(feature, body_radius) if feature_type == 1 \
			else _build_lake(feature, body_radius)
		if built.is_empty():
			continue
		var mesh: ArrayMesh = built.get("mesh") as ArrayMesh
		var anchor: Vec3D = built.get("anchor") as Vec3D
		if mesh == null or anchor == null:
			continue
		var material := ShaderMaterial.new()
		material.shader = _shader
		_configure_feature_material(material, feature, water, feature_type)
		var instance := MeshInstance3D.new()
		instance.name = "AuthoredWater_%s" % String(feature.get(&"feature_id"))
		instance.mesh = mesh
		instance.material_override = material
		instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		instance.gi_mode = GeometryInstance3D.GI_MODE_DISABLED
		instance.extra_cull_margin = 64.0
		_world_host.add_child(instance)
		_records.append({
			"feature_id": String(feature.get(&"feature_id")),
			"feature_type": feature_type,
			"instance": instance,
			"material": material,
			"anchor": anchor,
			"triangles": int(built.get("triangles", 0)),
		})
		_compiled_triangles += int(built.get("triangles", 0))
		if feature_type == 1:
			_compiled_rivers += 1
		else:
			_compiled_lakes += 1


func _build_lake(feature: Resource, body_radius: float) -> Dictionary:
	var polygon_value: Variant = feature.get(&"lake_polygon_body_m")
	if not (polygon_value is PackedVector3Array):
		return {}
	var polygon: PackedVector3Array = polygon_value as PackedVector3Array
	if polygon.size() < 3:
		return {}
	var center_dir := Vector3.ZERO
	for point: Vector3 in polygon:
		if point.length_squared() > 1.0:
			center_dir += point.normalized()
	if center_dir.length_squared() < 1e-8:
		return {}
	center_dir = center_dir.normalized()
	var tangent: Array = CubeSphere.tangent_basis(center_dir)
	var right: Vector3 = tangent[0]
	var up: Vector3 = tangent[1]
	var projected := PackedVector2Array()
	var directions: Array[Vector3] = []
	for point: Vector3 in polygon:
		if point.length_squared() <= 1.0:
			return {}
		var direction: Vector3 = point.normalized()
		var denom: float = direction.dot(center_dir)
		if denom <= 0.01:
			# One local mesh is intentionally limited to less than a hemisphere.
			return {}
		projected.append(Vector2(
			direction.dot(right) / denom * body_radius,
			direction.dot(up) / denom * body_radius))
		directions.append(direction)
	var triangle_indices: PackedInt32Array = Geometry2D.triangulate_polygon(projected)
	if triangle_indices.size() < 3:
		return {}
	var surface_radius: float = maxf(
		body_radius + float(feature.get(&"surface_level_m")) + LAKE_SURFACE_BIAS_M,
		1.0)
	var anchor := Vec3D.from_v3(center_dir).mul(surface_radius)
	var vertices: Array[Vector3] = []
	var normals: Array[Vector3] = []
	var uvs: Array[Vector2] = []
	var uv2s: Array[Vector2] = []
	var depth_m: float = maxf(float(feature.get(&"default_depth_m")), 0.0)
	for triangle_offset: int in range(0, triangle_indices.size(), 3):
		var ia: int = triangle_indices[triangle_offset]
		var ib: int = triangle_indices[triangle_offset + 1]
		var ic: int = triangle_indices[triangle_offset + 2]
		_append_lake_triangle(
			directions[ia], directions[ib], directions[ic],
			surface_radius, anchor, depth_m, 0,
			vertices, normals, uvs, uv2s)
	var mesh: ArrayMesh = _mesh_from_triangle_arrays(vertices, normals, uvs, uv2s)
	if mesh == null:
		return {}
	return {
		"mesh": mesh,
		"anchor": anchor,
		"triangles": vertices.size() / 3,
	}


func _append_lake_triangle(a: Vector3, b: Vector3, c: Vector3,
		surface_radius: float, anchor: Vec3D, depth_m: float, subdivision: int,
		vertices: Array[Vector3], normals: Array[Vector3],
		uvs: Array[Vector2], uv2s: Array[Vector2]) -> void:
	var edge_ab: float = _surface_arc_distance(a, b, surface_radius)
	var edge_bc: float = _surface_arc_distance(b, c, surface_radius)
	var edge_ca: float = _surface_arc_distance(c, a, surface_radius)
	var longest: float = maxf(edge_ab, maxf(edge_bc, edge_ca))
	if longest > LAKE_MAX_EDGE_M and subdivision < LAKE_MAX_SUBDIVISION:
		var ab: Vector3 = (a + b).normalized()
		var bc: Vector3 = (b + c).normalized()
		var ca: Vector3 = (c + a).normalized()
		var next: int = subdivision + 1
		_append_lake_triangle(a, ab, ca, surface_radius, anchor, depth_m, next, vertices, normals, uvs, uv2s)
		_append_lake_triangle(ab, b, bc, surface_radius, anchor, depth_m, next, vertices, normals, uvs, uv2s)
		_append_lake_triangle(ca, bc, c, surface_radius, anchor, depth_m, next, vertices, normals, uvs, uv2s)
		_append_lake_triangle(ab, bc, ca, surface_radius, anchor, depth_m, next, vertices, normals, uvs, uv2s)
		return
	_append_surface_vertex(a, surface_radius, anchor, depth_m, 0.0, 0.0, vertices, normals, uvs, uv2s)
	_append_surface_vertex(b, surface_radius, anchor, depth_m, 0.0, 0.0, vertices, normals, uvs, uv2s)
	_append_surface_vertex(c, surface_radius, anchor, depth_m, 0.0, 0.0, vertices, normals, uvs, uv2s)


func _build_river(feature: Resource, body_radius: float) -> Dictionary:
	var segment_count: int = int(feature.call("river_segment_count"))
	if segment_count <= 0:
		return {}
	var samples: Array[Dictionary] = []
	for segment: int in segment_count:
		var approximate_length: float = _approximate_river_segment_length(feature, segment)
		var middle_width: float = maxf(float(feature.call("sample_river_width", segment, 0.5)), 0.05)
		var target_step: float = clampf(middle_width * 0.45,
			RIVER_MIN_SAMPLE_STEP_M, RIVER_MAX_SAMPLE_STEP_M)
		var steps: int = clampi(int(ceil(approximate_length / maxf(target_step, 0.1))), 4,
			RIVER_MAX_SEGMENT_SAMPLES)
		for sample_index: int in range(steps + 1):
			if segment > 0 and sample_index == 0:
				continue
			var t: float = float(sample_index) / float(steps)
			var position: Vector3 = feature.call("sample_river_segment", segment, t)
			if position.length_squared() <= 1.0:
				continue
			samples.append({
				"position": position,
				"width": maxf(float(feature.call("sample_river_width", segment, t)), 0.05),
				"depth": maxf(float(feature.call("sample_river_depth", segment, t)), 0.0),
				"current": maxf(float(feature.call("sample_river_current", segment, t)), 0.0),
			})
	if samples.size() < 2:
		return {}

	# River surface_level_m is a vertical offset from the authored Bézier path. The
	# path itself carries the longitudinal elevation, unlike a level lake surface.
	var surface_offset_m: float = float(feature.get(&"surface_level_m")) + RIVER_SURFACE_BIAS_M
	var centers: Array[Vec3D] = []
	var directions: Array[Vector3] = []
	var radii: Array[float] = []
	var anchor_sum := Vec3D.new()
	for sample: Dictionary in samples:
		var position: Vector3 = sample["position"]
		var direction: Vector3 = position.normalized()
		var radius: float = maxf(position.length() + surface_offset_m, 1.0)
		var center := Vec3D.from_v3(direction).mul(radius)
		centers.append(center)
		directions.append(direction)
		radii.append(radius)
		anchor_sum = anchor_sum.add(center)
	var anchor: Vec3D = anchor_sum.mul(1.0 / float(centers.size()))

	var left_points: Array[Vec3D] = []
	var right_points: Array[Vec3D] = []
	for index: int in centers.size():
		var up: Vector3 = directions[index]
		var previous: Vec3D = centers[maxi(index - 1, 0)]
		var following: Vec3D = centers[mini(index + 1, centers.size() - 1)]
		var forward: Vector3 = following.sub(previous).to_v3()
		forward -= up * forward.dot(up)
		if forward.length_squared() < 1e-8:
			var basis: Array = CubeSphere.tangent_basis(up)
			forward = basis[1]
		else:
			forward = forward.normalized()
		var side: Vector3 = up.cross(forward).normalized()
		var half_width: float = maxf(float(samples[index]["width"]) * 0.5, 0.025)
		var theta: float = half_width / maxf(radii[index], 1.0)
		var left_dir: Vector3 = (up * cos(theta) + side * sin(theta)).normalized()
		var right_dir: Vector3 = (up * cos(theta) - side * sin(theta)).normalized()
		left_points.append(Vec3D.from_v3(left_dir).mul(radii[index]))
		right_points.append(Vec3D.from_v3(right_dir).mul(radii[index]))

	var vertices: Array[Vector3] = []
	var normals: Array[Vector3] = []
	var uvs: Array[Vector2] = []
	var uv2s: Array[Vector2] = []
	for index: int in range(samples.size() - 1):
		_append_river_vertex(left_points[index], anchor, samples[index], -1.0, vertices, normals, uvs, uv2s)
		_append_river_vertex(right_points[index], anchor, samples[index], 1.0, vertices, normals, uvs, uv2s)
		_append_river_vertex(right_points[index + 1], anchor, samples[index + 1], 1.0, vertices, normals, uvs, uv2s)
		_append_river_vertex(left_points[index], anchor, samples[index], -1.0, vertices, normals, uvs, uv2s)
		_append_river_vertex(right_points[index + 1], anchor, samples[index + 1], 1.0, vertices, normals, uvs, uv2s)
		_append_river_vertex(left_points[index + 1], anchor, samples[index + 1], -1.0, vertices, normals, uvs, uv2s)
	var mesh: ArrayMesh = _mesh_from_triangle_arrays(vertices, normals, uvs, uv2s)
	if mesh == null:
		return {}
	return {
		"mesh": mesh,
		"anchor": anchor,
		"triangles": vertices.size() / 3,
	}


func _append_river_vertex(world_point: Vec3D, anchor: Vec3D, sample: Dictionary,
		cross_section: float, vertices: Array[Vector3], normals: Array[Vector3],
		uvs: Array[Vector2], uv2s: Array[Vector2]) -> void:
	var direction: Vector3 = world_point.normalized().to_v3()
	vertices.append(world_point.sub(anchor).to_v3())
	normals.append(direction)
	uvs.append(Vector2(float(sample["depth"]), float(sample["current"])))
	uv2s.append(Vector2(cross_section, 0.0))


func _append_surface_vertex(direction: Vector3, surface_radius: float, anchor: Vec3D,
		depth_m: float, current_m_s: float, cross_section: float,
		vertices: Array[Vector3], normals: Array[Vector3],
		uvs: Array[Vector2], uv2s: Array[Vector2]) -> void:
	var d: Vector3 = direction.normalized()
	var world_point := Vec3D.from_v3(d).mul(surface_radius)
	vertices.append(world_point.sub(anchor).to_v3())
	normals.append(d)
	uvs.append(Vector2(depth_m, current_m_s))
	uv2s.append(Vector2(cross_section, 0.0))


func _approximate_river_segment_length(feature: Resource, segment: int) -> float:
	var length_m: float = 0.0
	var previous: Vector3 = feature.call("sample_river_segment", segment, 0.0)
	for index: int in range(1, 9):
		var current: Vector3 = feature.call("sample_river_segment", segment, float(index) / 8.0)
		length_m += previous.distance_to(current)
		previous = current
	return maxf(length_m, 0.01)


func _mesh_from_triangle_arrays(vertices: Array[Vector3], normals: Array[Vector3],
		uvs: Array[Vector2], uv2s: Array[Vector2]) -> ArrayMesh:
	if vertices.size() < 3 or normals.size() != vertices.size() \
			or uvs.size() != vertices.size() or uv2s.size() != vertices.size():
		return null
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array(vertices)
	arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array(normals)
	arrays[Mesh.ARRAY_TEX_UV] = PackedVector2Array(uvs)
	arrays[Mesh.ARRAY_TEX_UV2] = PackedVector2Array(uv2s)
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


func _configure_feature_material(material: ShaderMaterial, feature: Resource,
		water: Resource, feature_type: int) -> void:
	material.set_shader_parameter("u_feature_type", feature_type)
	material.set_shader_parameter("u_feature_wave_amplitude_scale",
		maxf(float(feature.get(&"wave_amplitude_scale")), 0.0))
	material.set_shader_parameter("u_feature_wave_frequency_scale",
		maxf(float(feature.get(&"wave_frequency_scale")), 0.001))
	material.set_shader_parameter("u_global_wave_amplitude_scale",
		maxf(float(water.get(&"wave_amplitude_scale")), 0.0))
	material.set_shader_parameter("u_global_wave_frequency_scale",
		maxf(float(water.get(&"wave_frequency_scale")), 0.001))
	material.set_shader_parameter("u_current_scale", maxf(float(feature.get(&"current_scale")), 0.0))
	material.set_shader_parameter("u_shore_falloff_m", maxf(float(feature.get(&"shore_falloff_m")), 0.0))
	material.set_shader_parameter("u_default_depth_m", maxf(float(feature.get(&"default_depth_m")), 0.0))


func _sync_runtime_bindings() -> void:
	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	var macro: Texture2DArray = Planet.global_height_texture
	var macro_res: int = Planet.global_height_face_res
	var base_spacing: float = PI * 0.5 * planet_radius \
		/ (float(Planet.cfg.chunk_grid) * pow(2.0, 16.0))
	var detail_seed: int = Planet.cfg.stream_seed("gpu_visual_detail") & 0x00ffffff
	var detail_strength: float = maxf(0.05, Planet.cfg.detail_amplitude / 260.0)
	var edit_params: Dictionary = _sample_params("/root/TerrainEditDeltaGPU")
	var active_params: Dictionary = _sample_params("/root/TerrainDeformationGPU")
	for record: Dictionary in _records:
		var instance: MeshInstance3D = record.get("instance") as MeshInstance3D
		var material: ShaderMaterial = record.get("material") as ShaderMaterial
		var anchor: Vec3D = record.get("anchor") as Vec3D
		if instance == null or material == null or anchor == null:
			continue
		instance.position = Frames.to_render(anchor)
		material.set_shader_parameter("u_origin", origin)
		material.set_shader_parameter("u_planet_radius", planet_radius)
		material.set_shader_parameter("u_macro_elevation", macro)
		material.set_shader_parameter("u_macro_face_res", float(macro_res))
		material.set_shader_parameter("u_macro_ready", 1.0 if macro != null else 0.0)
		material.set_shader_parameter("u_base_spacing", base_spacing)
		material.set_shader_parameter("u_detail_seed", maxi(detail_seed, 1))
		material.set_shader_parameter("u_detail_strength", detail_strength)
		_bind_edit_params(material, edit_params, false)
		_bind_edit_params(material, active_params, true)


func _sample_params(path: NodePath) -> Dictionary:
	var node: Node = get_node_or_null(path)
	if node == null or not node.has_method("sample_params"):
		return {}
	var value: Variant = node.call("sample_params")
	return value as Dictionary if value is Dictionary else {}


func _bind_edit_params(material: ShaderMaterial, params: Dictionary, active: bool) -> void:
	if active:
		var ready: bool = bool(params.get("ready", false))
		material.set_shader_parameter("u_active_deform", params.get("texture"))
		material.set_shader_parameter("u_active_deform_ready", 1.0 if ready else 0.0)
		material.set_shader_parameter("u_active_deform_center_dir", params.get("center_dir", Vector3.RIGHT))
		material.set_shader_parameter("u_active_deform_center_right", params.get("center_right", Vector3.BACK))
		material.set_shader_parameter("u_active_deform_center_up", params.get("center_up", Vector3.UP))
		material.set_shader_parameter("u_active_deform_half_extent_m", float(params.get("half_extent_m", 32.0)))
		return
	var ready: bool = bool(params.get("ready", false))
	material.set_shader_parameter("u_edit_delta", params.get("texture"))
	material.set_shader_parameter("u_edit_ready", 1.0 if ready else 0.0)
	material.set_shader_parameter("u_edit_center_dir", params.get("center_dir", Vector3.RIGHT))
	material.set_shader_parameter("u_edit_center_right", params.get("center_right", Vector3.BACK))
	material.set_shader_parameter("u_edit_center_up", params.get("center_up", Vector3.UP))
	material.set_shader_parameter("u_edit_half_extent_m", float(params.get("half_extent_m", 256.0)))


func _surface_arc_distance(a: Vector3, b: Vector3, radius: float) -> float:
	return acos(clampf(a.normalized().dot(b.normalized()), -1.0, 1.0)) * maxf(radius, 1.0)


func _set_visible(value: bool) -> void:
	for record: Dictionary in _records:
		var instance: MeshInstance3D = record.get("instance") as MeshInstance3D
		if instance != null:
			instance.visible = value


func _clear_records() -> void:
	for record: Dictionary in _records:
		var instance: MeshInstance3D = record.get("instance") as MeshInstance3D
		if instance != null and is_instance_valid(instance):
			instance.queue_free()
	_records.clear()


func stats() -> Dictionary:
	return {
		"features": _records.size(),
		"lakes": _compiled_lakes,
		"rivers": _compiled_rivers,
		"triangles": _compiled_triangles,
		"terrain_clipped": true,
		"floating_origin_local_meshes": true,
		"lake_max_edge_m": LAKE_MAX_EDGE_M,
	}


func _exit_tree() -> void:
	_clear_records()
