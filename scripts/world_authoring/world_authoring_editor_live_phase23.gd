extends "res://scripts/world_authoring/world_authoring_editor_live_phase22.gd"
## Phase 23: system-frame celestial interest camera.
##
## Planet Studio navigation is deliberately separate from gameplay's planet-centred
## player controller. The player's canonical world_pos remains in the fixed system
## frame, while camera up/forward/WASD are computed around the currently selected
## body's absolute centre. Selecting a moon therefore never reinterprets Asterra's
## terrain/ocean/cloud coordinates as moon-local coordinates.

const INTEREST_MIN_RADIUS_M: float = 1.0
const INTEREST_MIN_SPEED_M_S: float = 8.0
const INTEREST_MAX_SPEED_M_S: float = 90000.0
const INTEREST_SPEED_ALTITUDE_SCALE: float = 0.55
const INTEREST_SPRINT_MULTIPLIER: float = 6.0

var _interest_enabled: bool = false
var _interest_center_world: Vec3D = Vec3D.new()
var _interest_radius_m: float = INTEREST_MIN_RADIUS_M
var _interest_uses_detailed_surface: bool = true


func set_camera_interest(center_world: Vec3D, radius_m: float,
		uses_detailed_surface: bool) -> void:
	_interest_center_world = center_world.dup() if center_world != null else Vec3D.new()
	_interest_radius_m = maxf(radius_m, INTEREST_MIN_RADIUS_M)
	_interest_uses_detailed_surface = uses_detailed_surface
	_interest_enabled = true
	if _player != null:
		# Gameplay movement assumes the active planet is system origin. Planet Studio
		# owns movement while an arbitrary celestial interest frame is active.
		_player.set("input_enabled", false)
		if _player.has_method("set_mouse_captured"):
			_player.call("set_mouse_captured", false)
	_sync_interest_camera_transform()


func camera_interest_center_world() -> Vec3D:
	return _interest_center_world.dup()


func camera_interest_radius_m() -> float:
	return _interest_radius_m


func _process(delta: float) -> void:
	super._process(delta)
	if _world_host == null or not _interest_enabled:
		return
	if _navigation_active:
		_update_interest_navigation(delta)


func _set_navigation(enabled: bool) -> void:
	if not _interest_enabled:
		super._set_navigation(enabled)
		return
	_navigation_active = enabled
	_camera_look_active = false
	if _player != null:
		# Never enable gameplay movement here. Its radial frame is intentionally tied
		# to the production planet; this editor layer performs system-frame movement.
		_player.set("input_enabled", false)
		if _player.has_method("set_mouse_captured"):
			_player.call("set_mouse_captured", false)
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	_last_hit.clear()
	_update_preview()
	if _live_status_label != null:
		_live_status_label.text = "MOVE ENABLED — WASD • hold RMB to look • TAB returns to authoring" \
			if enabled else _placement_status_text()


func _rotate_camera(relative: Vector2) -> void:
	if not _interest_enabled:
		super._rotate_camera(relative)
		return
	if _player == null:
		return
	var next_yaw: float = float(_player.get("yaw")) + relative.x * CAMERA_LOOK_SENS
	var next_pitch: float = clampf(
		float(_player.get("pitch")) - relative.y * CAMERA_LOOK_SENS,
		-1.55,
		1.55)
	_player.set("yaw", next_yaw)
	_player.set("pitch", next_pitch)
	_sync_interest_camera_transform()
	_last_hit.clear()
	_update_preview()


func _update_interest_navigation(delta: float) -> void:
	if _player == null:
		return
	var world_pos: Vec3D = _player.get("world_pos") as Vec3D
	if world_pos == null:
		return
	var radial: Vec3D = world_pos.sub(_interest_center_world)
	var up: Vector3 = radial.normalized().to_v3() if radial.length_sq() > 1.0 \
		else Vector3.UP
	var basis: Array = _interest_basis(up)
	var forward: Vector3 = basis[0]
	var right: Vector3 = basis[1]
	var wish := Vector3.ZERO
	if Input.is_action_pressed("move_forward"):
		wish += forward
	if Input.is_action_pressed("move_back"):
		wish -= forward
	if Input.is_action_pressed("move_right"):
		wish += right
	if Input.is_action_pressed("move_left"):
		wish -= right
	if Input.is_action_pressed("move_up"):
		wish += up
	if Input.is_action_pressed("move_down"):
		wish -= up
	if wish.length_squared() <= 1.0e-8:
		return

	var altitude_m: float = maxf(radial.length() - _interest_radius_m, 1.0)
	var speed: float = clampf(
		altitude_m * INTEREST_SPEED_ALTITUDE_SCALE,
		INTEREST_MIN_SPEED_M_S,
		INTEREST_MAX_SPEED_M_S)
	if Input.is_action_pressed("sprint"):
		speed *= INTEREST_SPRINT_MULTIPLIER
	world_pos = world_pos.add(Vec3D.from_v3(wish.normalized()).mul(speed * delta))
	_player.set("world_pos", world_pos)
	Frames.maintain_origin(Frames.to_render(world_pos))
	_sync_interest_camera_transform()
	_player.emit_signal("moved", world_pos)


func _interest_basis(up: Vector3) -> Array:
	var reference := Vector3.UP
	if absf(up.dot(reference)) > 0.995:
		reference = Vector3.RIGHT
	var east: Vector3 = reference.cross(up).normalized()
	var north: Vector3 = up.cross(east).normalized()
	var yaw: float = float(_player.get("yaw")) if _player != null else 0.0
	var forward: Vector3 = (north * cos(yaw) + east * sin(yaw)).normalized()
	var right: Vector3 = forward.cross(up).normalized()
	return [forward, right]


func _sync_interest_camera_transform() -> void:
	if not _interest_enabled or _player == null:
		return
	if _camera == null:
		_camera = _player.get("camera") as Camera3D
	if _camera == null:
		return
	var world_pos: Vec3D = _player.get("world_pos") as Vec3D
	if world_pos == null:
		return
	_player.position = Frames.to_render(world_pos)
	var radial: Vec3D = world_pos.sub(_interest_center_world)
	var up: Vector3 = radial.normalized().to_v3() if radial.length_sq() > 1.0 \
		else Vector3.UP
	var basis: Array = _interest_basis(up)
	var forward: Vector3 = basis[0]
	var flat_right: Vector3 = basis[1]
	var pitch: float = float(_player.get("pitch"))
	var look: Vector3 = (forward * cos(pitch) + up * sin(pitch)).normalized()
	var camera_right: Vector3 = look.cross(up).normalized()
	if camera_right.length_squared() < 1.0e-8:
		camera_right = flat_right
	var true_up: Vector3 = camera_right.cross(look).normalized()
	_camera.transform.basis = Basis(camera_right, true_up, -look)
	_camera.position = Vector3.ZERO


func _screen_aim(screen_position: Vector2) -> Dictionary:
	if not _interest_enabled or _interest_uses_detailed_surface:
		return super._screen_aim(screen_position)
	if _camera == null:
		_camera = _player.get("camera") as Camera3D if _player != null else null
	if _camera == null:
		return {}
	var ray_origin: Vec3D = Frames.to_world(_camera.global_position)
	var ray: Vector3 = _camera.project_ray_normal(screen_position).normalized()
	var offset: Vec3D = ray_origin.sub(_interest_center_world)
	var offset_v3 := offset.to_v3()
	var b: float = ray.dot(offset_v3)
	var c: float = offset.length_sq() - _interest_radius_m * _interest_radius_m
	var discriminant: float = b * b - c
	if discriminant < 0.0:
		return {}
	var root: float = sqrt(discriminant)
	var distance_m: float = -b - root
	if distance_m < 0.0:
		distance_m = -b + root
	# Family framing can intentionally place the camera millions of metres away.
	# Keep the ordinary terrain pick cap for detailed terrain, but allow the exact
	# analytic staged sphere hit to reach its selected body from that framing view.
	var staged_pick_max: float = maxf(
		PICK_MAX_RANGE_M,
		offset.length() + _interest_radius_m * 2.0)
	if distance_m < 0.0 or distance_m > staged_pick_max:
		return {}
	var system_world: Vec3D = ray_origin.add(Vec3D.from_v3(ray).mul(distance_m))
	var body_local: Vec3D = system_world.sub(_interest_center_world)
	var direction: Vector3 = body_local.normalized().to_v3()
	var body_surface: Vec3D = Vec3D.from_v3(direction).mul(_interest_radius_m)
	return {
		"world": body_surface,
		"system_world": system_world,
		"dir": direction,
		"distance": distance_m,
		"height": 0.0,
	}


func _draw_surface_ring(direction: Vector3, height: float,
		radius_m: float, color: Color) -> void:
	if not _interest_enabled or _interest_uses_detailed_surface:
		super._draw_surface_ring(direction, height, radius_m, color)
		return
	if direction.length_squared() < 0.99 or _preview_mesh == null:
		return
	var up := direction.normalized()
	var reference := Vector3.UP if absf(up.dot(Vector3.UP)) < 0.95 else Vector3.RIGHT
	var tangent_x := reference.cross(up).normalized()
	var tangent_y := up.cross(tangent_x).normalized()
	var base_radius: float = _interest_radius_m + height + 0.08
	_preview_mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
	for index: int in 65:
		var angle: float = TAU * float(index) / 64.0
		var offset: Vector3 = tangent_x * cos(angle) * radius_m \
			+ tangent_y * sin(angle) * radius_m
		var body_local: Vec3D = Vec3D.from_v3(up).mul(base_radius).add(
			Vec3D.from_v3(offset))
		var system_world: Vec3D = _interest_center_world.add(body_local)
		_preview_mesh.surface_set_color(color)
		_preview_mesh.surface_add_vertex(Frames.to_render(system_world))
	_preview_mesh.surface_end()


func _draw_selected_water_feature() -> void:
	if not _interest_enabled or _interest_uses_detailed_surface:
		super._draw_selected_water_feature()
		return
	if _selected_water_feature_id.is_empty() or _preview_mesh == null:
		return
	var water: Resource = _session.active_water_profile() as Resource
	if water == null:
		return
	var feature: Resource = water.call("find_feature", _selected_water_feature_id) as Resource
	if feature == null:
		return
	var color := Color(1.0, 0.18, 0.78, 1.0)
	var is_river: bool = int(feature.get(&"feature_type")) \
		== WATER_FEATURE_SCRIPT.FeatureType.RIVER
	var points: Array[Vector3] = []
	if is_river:
		var segment_count: int = int(feature.call("river_segment_count"))
		for segment: int in segment_count:
			for sample_index: int in 17:
				if segment > 0 and sample_index == 0:
					continue
				points.append(Vector3(feature.call("sample_river_segment",
					segment, float(sample_index) / 16.0)))
	else:
		var polygon: PackedVector3Array = feature.get(&"lake_polygon_body_m")
		for point: Vector3 in polygon:
			points.append(point)
		if polygon.size() > 2:
			points.append(polygon[0])
	if points.size() < 2:
		return
	_preview_mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
	for point: Vector3 in points:
		var system_world: Vec3D = _interest_center_world.add(Vec3D.from_v3(point))
		_preview_mesh.surface_set_color(color)
		_preview_mesh.surface_add_vertex(Frames.to_render(system_world))
	_preview_mesh.surface_end()
