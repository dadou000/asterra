extends Node3D
## Multi-body live preview for Planet Studio.
##
## All bodies stay in one stable system/world coordinate frame. Selecting a body
## changes only the editor camera interest point; it never redefines Frames or moves
## the rest of the system underneath renderers that assume a fixed planet centre.
## The one detailed terrestrial runtime can replace its lightweight preview sphere,
## while parents, moons and siblings remain at their staged orbital positions.

const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")

const PREVIEW_RADIAL_SEGMENTS: int = 72
const PREVIEW_RINGS: int = 48
const MIN_RADIUS_M: float = 1.0
const FAMILY_FRAME_DISTANCE_RATIO: float = 40.0
const FALLBACK_ORBIT_PARENT_RADII: float = 4.0
const FALLBACK_ORBIT_CHILD_RADII: float = 3.0
const FALLBACK_ANOMALY_DEG: float = 35.0

var _system: Resource
var _selected_body_id: String = ""
var _detailed_body_id: String = ""
var _records: Dictionary = {}
var _world_positions: Dictionary = {}
var _selected_visual_radius_m: float = 1.0
var _family_frame_radius_m: float = 1.0
var _system_extent_m: float = 1.0
var _sphere_mesh: SphereMesh
var _corona_mesh: SphereMesh


func _ready() -> void:
	_build_shared_meshes()
	visible = false
	set_process(true)


func _process(_delta: float) -> void:
	if visible:
		_sync_floating_origin()
		_sync_camera_clip()


func show_system(system: Resource, selected_body_id: String,
		detailed_body_id: String = "") -> void:
	_system = system
	_selected_body_id = selected_body_id
	_detailed_body_id = detailed_body_id
	if _system == null:
		hide_preview()
		return
	_system.call("ensure_valid")
	if _sphere_mesh == null:
		_build_shared_meshes()
	_rebuild_system_preview()
	visible = not _records.is_empty()
	_sync_floating_origin()
	_sync_camera_clip()


## Compatibility entry point for older callers. The body is placed at the system
## origin because no hierarchy/orbit information is available through this API.
func show_body(body: Resource) -> void:
	_clear_records()
	_system = null
	_selected_body_id = String(body.get(&"body_id")) if body != null else ""
	_detailed_body_id = ""
	_world_positions.clear()
	if body == null:
		hide_preview()
		return
	var world := Vec3D.new()
	_world_positions[_selected_body_id] = world
	var record: Dictionary = _create_body_record(body, world, world, true)
	_records[_selected_body_id] = record
	_selected_visual_radius_m = float(record.get("visual_radius_m", 1.0))
	_family_frame_radius_m = _selected_visual_radius_m
	_system_extent_m = _selected_visual_radius_m
	visible = true
	_sync_floating_origin()
	_sync_camera_clip()


func hide_preview() -> void:
	visible = false


func body_id() -> String:
	return _selected_body_id


func detailed_body_id() -> String:
	return _detailed_body_id


func visual_radius_m() -> float:
	return _selected_visual_radius_m


func family_frame_radius_m() -> float:
	return _family_frame_radius_m


func system_extent_m() -> float:
	return _system_extent_m


func preview_body_count() -> int:
	return _records.size()


func selected_center_world() -> Vec3D:
	var center: Vec3D = _world_positions.get(_selected_body_id) as Vec3D
	return center.dup() if center != null else Vec3D.new()


func body_world_position(body_id: String) -> Vec3D:
	var center: Vec3D = _world_positions.get(body_id) as Vec3D
	return center.dup() if center != null else Vec3D.new()


func _build_shared_meshes() -> void:
	_sphere_mesh = SphereMesh.new()
	_sphere_mesh.radius = 1.0
	_sphere_mesh.height = 2.0
	_sphere_mesh.radial_segments = PREVIEW_RADIAL_SEGMENTS
	_sphere_mesh.rings = PREVIEW_RINGS

	_corona_mesh = SphereMesh.new()
	_corona_mesh.radius = 1.0
	_corona_mesh.height = 2.0
	_corona_mesh.radial_segments = PREVIEW_RADIAL_SEGMENTS
	_corona_mesh.rings = PREVIEW_RINGS


func _rebuild_system_preview() -> void:
	_clear_records()
	_world_positions = _compute_world_positions(_system)
	var selected_world: Vec3D = _world_positions.get(_selected_body_id) as Vec3D
	if selected_world == null:
		selected_world = Vec3D.new()

	_selected_visual_radius_m = 1.0
	_family_frame_radius_m = 1.0
	_system_extent_m = 1.0
	var bodies: Array = _system.get(&"bodies")
	for body_value: Variant in bodies:
		var body: Resource = body_value as Resource
		if body == null:
			continue
		var body_id: String = String(body.get(&"body_id"))
		var absolute_world: Vec3D = _world_positions.get(body_id) as Vec3D
		if absolute_world == null:
			absolute_world = Vec3D.new()
		var offset_from_selected: Vec3D = absolute_world.sub(selected_world)
		# The detailed terrain/ocean renderer already represents this body. Hide only
		# its lightweight duplicate; selecting another body must not hide the detailed
		# body itself or move its centre.
		var body_visible: bool = body_id != _detailed_body_id
		var record: Dictionary = _create_body_record(
			body, absolute_world, offset_from_selected, body_visible)
		_records[body_id] = record
		var visual_radius: float = float(record.get("visual_radius_m", 1.0))
		_system_extent_m = maxf(_system_extent_m, absolute_world.length() + visual_radius)
		if body_id == _selected_body_id:
			_selected_visual_radius_m = visual_radius

	_family_frame_radius_m = _compute_family_frame_radius()


func _create_body_record(body: Resource, world: Vec3D,
		offset_from_selected: Vec3D, body_visible: bool) -> Dictionary:
	body.call("ensure_children")
	var body_id: String = String(body.get(&"body_id"))
	var body_type: int = int(body.get(&"body_type"))
	var radius_m: float = maxf(float(body.get(&"radius_m")), MIN_RADIUS_M)
	var root := Node3D.new()
	root.name = "CelestialPreview_%s" % body_id
	add_child(root)

	var surface := MeshInstance3D.new()
	surface.name = "Surface"
	surface.mesh = _sphere_mesh
	surface.scale = Vector3.ONE * radius_m
	surface.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	root.add_child(surface)

	var surface_material := StandardMaterial3D.new()
	surface_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	surface_material.cull_mode = BaseMaterial3D.CULL_BACK
	surface.material_override = surface_material

	var corona: MeshInstance3D = null
	var visual_radius: float = radius_m
	if body_type == BODY_SCRIPT.BodyType.STAR:
		visual_radius = _configure_star(body, radius_m, root, surface_material)
	else:
		_configure_solid_body(body_type, surface_material)

	root.visible = body_visible
	return {
		"body": body,
		"root": root,
		"world": world,
		"offset_from_selected": offset_from_selected,
		"surface": surface,
		"corona": corona,
		"visual_radius_m": visual_radius,
	}


func _configure_solid_body(body_type: int, material: StandardMaterial3D) -> void:
	var color := Color(0.30, 0.56, 0.78, 1.0)
	match body_type:
		BODY_SCRIPT.BodyType.MOON:
			color = Color(0.58, 0.60, 0.64, 1.0)
		BODY_SCRIPT.BodyType.DWARF:
			color = Color(0.56, 0.43, 0.32, 1.0)
		BODY_SCRIPT.BodyType.OTHER:
			color = Color(0.48, 0.42, 0.58, 1.0)
		_:
			pass
	material.albedo_color = color
	material.emission_enabled = false


func _configure_star(body: Resource, radius_m: float, root: Node3D,
		material: StandardMaterial3D) -> float:
	var star: Resource = body.get(&"star_profile") as Resource
	var surface_color := Color(1.0, 0.93, 0.82, 1.0)
	var surface_intensity: float = 1.0
	var corona_color := Color(0.72, 0.84, 1.0, 1.0)
	var corona_intensity: float = 1.0
	var corona_extent: float = 2.5
	if star != null:
		surface_color = star.get(&"photosphere_color") as Color
		surface_intensity = maxf(float(star.get(&"photosphere_intensity")), 0.0)
		corona_color = star.get(&"corona_color") as Color
		corona_intensity = maxf(float(star.get(&"corona_intensity")), 0.0)
		corona_extent = clampf(float(star.get(&"corona_extent_radii")), 1.0, 12.0)

	material.albedo_color = surface_color
	material.emission_enabled = true
	material.emission = surface_color
	material.emission_energy_multiplier = maxf(surface_intensity, 0.001)

	var corona := MeshInstance3D.new()
	corona.name = "Corona"
	corona.mesh = _corona_mesh
	corona.scale = Vector3.ONE * radius_m * corona_extent
	corona.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	root.add_child(corona)
	var corona_material := StandardMaterial3D.new()
	corona_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	corona_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	corona_material.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
	corona_material.cull_mode = BaseMaterial3D.CULL_FRONT
	corona_material.no_depth_test = true
	var corona_alpha: float = clampf(0.035 + corona_intensity * 0.035, 0.02, 0.30)
	corona_material.albedo_color = Color(corona_color.r, corona_color.g, corona_color.b, corona_alpha)
	corona_material.emission_enabled = true
	corona_material.emission = corona_color
	corona_material.emission_energy_multiplier = maxf(corona_intensity * 0.35, 0.01)
	corona.material_override = corona_material
	return radius_m * corona_extent


func _compute_world_positions(system: Resource) -> Dictionary:
	var result: Dictionary = {}
	if system == null:
		return result
	var bodies: Array = system.get(&"bodies")
	var unresolved: Array[Resource] = []
	for body_value: Variant in bodies:
		var body: Resource = body_value as Resource
		if body == null:
			continue
		if String(body.get(&"parent_body_id")).is_empty():
			result[String(body.get(&"body_id"))] = Vec3D.new()
		else:
			unresolved.append(body)

	var passes: int = 0
	while not unresolved.is_empty() and passes <= bodies.size():
		passes += 1
		var progressed: bool = false
		for index: int in range(unresolved.size() - 1, -1, -1):
			var body: Resource = unresolved[index]
			var parent_id: String = String(body.get(&"parent_body_id"))
			var parent_world: Vec3D = result.get(parent_id) as Vec3D
			if parent_world == null:
				continue
			var parent: Resource = system.call("find_body", parent_id) as Resource
			result[String(body.get(&"body_id"))] = parent_world.add(_orbit_offset(body, parent))
			unresolved.remove_at(index)
			progressed = true
		if not progressed:
			break

	# ensure_valid() should prevent cycles/missing parents, but keeping unresolved
	# bodies at the origin makes a damaged preset inspectable instead of invisible.
	for body: Resource in unresolved:
		result[String(body.get(&"body_id"))] = Vec3D.new()
	return result


func _orbit_offset(body: Resource, parent: Resource) -> Vec3D:
	var orbit: Resource = body.get(&"orbit") as Resource
	var parent_radius: float = maxf(float(parent.get(&"radius_m")), MIN_RADIUS_M) if parent != null else MIN_RADIUS_M
	var child_radius: float = maxf(float(body.get(&"radius_m")), MIN_RADIUS_M)
	var semi_major_axis: float = float(orbit.get(&"semi_major_axis_m")) if orbit != null else 0.0
	var fallback_orbit: bool = semi_major_axis <= 0.0
	if fallback_orbit:
		semi_major_axis = maxf(
			parent_radius * FALLBACK_ORBIT_PARENT_RADII,
			parent_radius + child_radius * FALLBACK_ORBIT_CHILD_RADII)

	var eccentricity: float = clampf(float(orbit.get(&"eccentricity")) if orbit != null else 0.0,
		0.0, 0.999999)
	var mean_anomaly_deg: float = float(orbit.get(&"mean_anomaly_at_epoch_deg")) if orbit != null else 0.0
	if fallback_orbit and absf(mean_anomaly_deg) <= 1e-6:
		mean_anomaly_deg = FALLBACK_ANOMALY_DEG
	var mean_anomaly: float = deg_to_rad(mean_anomaly_deg)
	var eccentric_anomaly: float = mean_anomaly
	for _iteration: int in 8:
		var f: float = eccentric_anomaly - eccentricity * sin(eccentric_anomaly) - mean_anomaly
		var fp: float = maxf(1.0 - eccentricity * cos(eccentric_anomaly), 1e-8)
		eccentric_anomaly -= f / fp

	var orbital_x: float = semi_major_axis * (cos(eccentric_anomaly) - eccentricity)
	var orbital_z: float = semi_major_axis * sqrt(maxf(1.0 - eccentricity * eccentricity, 0.0)) \
		* sin(eccentric_anomaly)
	var inclination: float = deg_to_rad(float(orbit.get(&"inclination_deg")) if orbit != null else 0.0)
	var ascending_node: float = deg_to_rad(float(orbit.get(&"longitude_ascending_node_deg")) if orbit != null else 0.0)
	var periapsis: float = deg_to_rad(float(orbit.get(&"argument_periapsis_deg")) if orbit != null else 0.0)

	# Standard Keplerian orientation expressed in Asterra's Y-up render convention.
	var cos_o: float = cos(ascending_node)
	var sin_o: float = sin(ascending_node)
	var cos_w: float = cos(periapsis)
	var sin_w: float = sin(periapsis)
	var cos_i: float = cos(inclination)
	var sin_i: float = sin(inclination)
	var x: float = (cos_o * cos_w - sin_o * sin_w * cos_i) * orbital_x \
		+ (-cos_o * sin_w - sin_o * cos_w * cos_i) * orbital_z
	var y: float = (sin_w * sin_i) * orbital_x + (cos_w * sin_i) * orbital_z
	var z: float = (sin_o * cos_w + cos_o * sin_w * cos_i) * orbital_x \
		+ (-sin_o * sin_w + cos_o * cos_w * cos_i) * orbital_z
	return Vec3D.new(x, y, z)


func _compute_family_frame_radius() -> float:
	var extent: float = maxf(_selected_visual_radius_m, MIN_RADIUS_M)
	if _system == null:
		return extent
	var selected: Resource = _system.call("find_body", _selected_body_id) as Resource
	if selected == null:
		return extent
	var selected_parent_id: String = String(selected.get(&"parent_body_id"))
	var selected_radius: float = maxf(float(selected.get(&"radius_m")), MIN_RADIUS_M)
	for key: Variant in _records:
		var body_id: String = String(key)
		if body_id == _selected_body_id:
			continue
		var record: Dictionary = _records[key] as Dictionary
		var body: Resource = record.get("body") as Resource
		var offset: Vec3D = record.get("offset_from_selected") as Vec3D
		if body == null or offset == null:
			continue
		var direct_family: bool = body_id == selected_parent_id \
			or String(body.get(&"parent_body_id")) == _selected_body_id
		if not direct_family:
			continue
		var distance: float = offset.length()
		if distance > selected_radius * FAMILY_FRAME_DISTANCE_RATIO:
			continue
		var visual_radius: float = float(record.get("visual_radius_m", 1.0))
		extent = maxf(extent, distance + visual_radius)
	return extent


func _sync_floating_origin() -> void:
	for key: Variant in _records:
		var record: Dictionary = _records[key] as Dictionary
		var root: Node3D = record.get("root") as Node3D
		var world: Vec3D = record.get("world") as Vec3D
		if root != null and world != null:
			root.position = Frames.to_render(world)


func _sync_camera_clip() -> void:
	var viewport: Viewport = get_viewport()
	if viewport == null:
		return
	var camera: Camera3D = viewport.get_camera_3d()
	if camera == null:
		return
	var selected_world: Vec3D = _world_positions.get(_selected_body_id) as Vec3D
	if selected_world == null:
		selected_world = Vec3D.new()
	var selected_center_render: Vector3 = Frames.to_render(selected_world)
	var center_distance: float = selected_center_render.distance_to(camera.global_position)
	# Never expand depth precision to the entire solar system. Only the selected
	# body's direct family is part of this close 3D framing pass; remote planets and
	# stars use their own future system-scale representation.
	camera.far = maxf(camera.far,
		center_distance + _family_frame_radius_m * 1.15)


func _clear_records() -> void:
	for key: Variant in _records:
		var record: Dictionary = _records[key] as Dictionary
		var root: Node3D = record.get("root") as Node3D
		if root != null and is_instance_valid(root):
			root.queue_free()
	_records.clear()


static func frame_distance_for_radius(radius_m: float, vertical_fov_deg: float,
		margin: float = 1.18) -> float:
	var radius: float = maxf(radius_m, MIN_RADIUS_M)
	var half_fov: float = deg_to_rad(clampf(vertical_fov_deg, 5.0, 170.0) * 0.5)
	return radius / maxf(sin(half_fov), 0.05) * maxf(margin, 1.01)
