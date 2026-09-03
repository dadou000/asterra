extends "res://scripts/world_authoring/world_authoring_editor_live_phase45.gd"
## Phase 46: place and reshape guided terrain features directly in the live viewport.
##
## This deliberately reuses the existing authoritative Planet Studio terrain pick:
## _screen_aim() follows rendered/contact terrain in floating-origin space and returns
## a canonical Asterra-frame direction. Viewport editing only writes the same guided
## graph parameters that Simple sliders and Node Graph already own.

const PHASE46_GUIDED := preload(
	"res://scripts/world_authoring/model/terrain_guided_feature_graph.gd")
const PHASE46_PLACE_FEATURE: int = 100
const PHASE46_EDIT_FEATURE: int = 101
const PHASE46_HANDLE_HIT_RADIUS_PX: float = 24.0
const PHASE46_PREVIEW_ALTITUDE_M: float = 8.0
const PHASE46_DRAG_INTERVAL_USEC: int = 25000

var _phase46_place_slot_ids: Array[String] = []
var _phase46_place_label: String = "terrain feature"
var _phase46_edit_slot_id: String = ""
var _phase46_drag_handle: String = ""
var _phase46_last_drag_update_usec: int = 0


func _phase45_create_preset(terrain: Resource, preset_id: String) -> void:
	# Capture the created slots around Phase 45's normal recipe path. Multi-part
	# presets are then armed as one transient placement group, while the serialized
	# result remains independent ordinary guided slots.
	var before: Dictionary = {}
	for slot: Resource in _phase43_feature_slots(terrain):
		before[String(slot.get(&"slot_id"))] = true
	super._phase45_create_preset(terrain, preset_id)
	var created_ids: Array[String] = []
	for slot: Resource in _phase43_feature_slots(terrain):
		var slot_id: String = String(slot.get(&"slot_id"))
		if not before.has(slot_id):
			created_ids.append(slot_id)
	if not created_ids.is_empty() and _phase46_has_live_camera():
		_phase46_arm_place(created_ids, TERRAIN_PRESETS.label(preset_id))
		_refresh_current_category()


func _phase43_build_feature_header(slot: Resource) -> void:
	super._phase43_build_feature_header(slot)
	if slot == null:
		return
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null or not PHASE46_GUIDED.is_guided_graph(graph):
		return
	var config: Dictionary = PHASE46_GUIDED.config_from_graph(graph)
	var area_kind: String = String(config.get("area_kind", PHASE46_GUIDED.AREA_RADIAL))
	var can_place: bool = area_kind != PHASE46_GUIDED.AREA_EVERYWHERE and _phase46_has_live_camera()
	var slot_id: String = String(slot.get(&"slot_id"))

	var row := HBoxContainer.new()
	row.name = "TerrainFeatureViewportTools"
	row.add_theme_constant_override("separation", 8)
	_workspace.add_child(row)

	var place := Button.new()
	place.name = "PlaceFeatureInViewport"
	place.text = "PLACING — click terrain" if _placement_mode == PHASE46_PLACE_FEATURE \
			and _phase46_place_slot_ids.has(slot_id) else "Place on Planet"
	place.custom_minimum_size = Vector2(190.0, 38.0)
	place.disabled = not can_place
	place.tooltip_text = "Use the authoritative live terrain pick to move this feature without changing its size."
	place.pressed.connect(func() -> void:
		if _placement_mode == PHASE46_PLACE_FEATURE and _phase46_place_slot_ids.has(slot_id):
			_set_placement_mode(PlacementMode.NONE)
		else:
			_phase46_arm_place([slot_id], String(slot.get(&"display_name")))
		_refresh_current_category()
	)
	row.add_child(place)

	var edit := Button.new()
	edit.name = "EditFeatureViewportHandles"
	edit.text = "Done with Handles" if _placement_mode == PHASE46_EDIT_FEATURE \
			and _phase46_edit_slot_id == slot_id else "Edit Viewport Handles"
	edit.custom_minimum_size = Vector2(190.0, 38.0)
	edit.disabled = not can_place
	edit.tooltip_text = "Drag the on-world center, radius, ring or geographic edge handles."
	edit.pressed.connect(func() -> void:
		if _placement_mode == PHASE46_EDIT_FEATURE and _phase46_edit_slot_id == slot_id:
			_set_placement_mode(PlacementMode.NONE)
		else:
			_phase46_edit_slot_id = slot_id
			_phase46_drag_handle = ""
			_set_placement_mode(PHASE46_EDIT_FEATURE)
		_refresh_current_category()
	)
	row.add_child(edit)

	var coordinates := Label.new()
	coordinates.name = "ViewportFeatureCoordinates"
	coordinates.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	coordinates.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	coordinates.modulate = Color(0.62, 0.73, 0.82)
	coordinates.text = _phase46_location_summary(config)
	row.add_child(coordinates)

	if not can_place:
		var note := Label.new()
		note.text = "Viewport placement needs a live camera and a geographic area." if area_kind != PHASE46_GUIDED.AREA_EVERYWHERE \
			else "Everywhere has no geographic handle; choose a placed area first."
		note.modulate = Color(0.54, 0.64, 0.72)
		_workspace.add_child(note)


func _phase46_arm_place(slot_ids: Array[String], label: String) -> void:
	_phase46_place_slot_ids = slot_ids.duplicate()
	_phase46_place_label = label
	_phase46_edit_slot_id = ""
	_phase46_drag_handle = ""
	_set_placement_mode(PHASE46_PLACE_FEATURE)
	_set_status("%s ready for placement. Click the live terrain; RMB/Esc cancels." % label)


func _set_placement_mode(mode: int) -> void:
	if mode != PHASE46_PLACE_FEATURE:
		_phase46_place_slot_ids.clear()
	if mode != PHASE46_EDIT_FEATURE:
		_phase46_edit_slot_id = ""
		_phase46_drag_handle = ""
	super._set_placement_mode(mode)


func _placement_status_text() -> String:
	if _placement_mode == PHASE46_PLACE_FEATURE:
		return "TERRAIN FEATURE PLACEMENT — click terrain to place %s • RMB/Esc cancel • TAB navigate" % _phase46_place_label
	if _placement_mode == PHASE46_EDIT_FEATURE:
		return "TERRAIN FEATURE HANDLES — drag the cyan handles • RMB/Esc finish • TAB navigate"
	return super._placement_status_text()


func _continuous_drag_mode() -> bool:
	if _placement_mode == PHASE46_EDIT_FEATURE:
		return true
	return super._continuous_drag_mode()


func _unhandled_input(event: InputEvent) -> void:
	# The base class owns all existing placement/navigation input. We only intercept
	# release to commit the exact final drag sample and refresh numeric controls.
	if _placement_mode == PHASE46_EDIT_FEATURE and event is InputEventMouseButton:
		var button := event as InputEventMouseButton
		if button.button_index == MOUSE_BUTTON_LEFT and not button.pressed and not _phase46_drag_handle.is_empty():
			if _is_live_viewport_point(button.position):
				_last_hit = _screen_aim(button.position)
				if not _last_hit.is_empty():
					_phase46_apply_drag_handle(Vector3(_last_hit.get("dir", Vector3.ZERO)), true)
			_phase46_drag_handle = ""
			_refresh_current_category()
			get_viewport().set_input_as_handled()
			return
	super._unhandled_input(event)


func _place_current_hit(continuous: bool) -> void:
	if _placement_mode == PHASE46_PLACE_FEATURE:
		if _last_hit.is_empty():
			_set_status("Viewport pick did not intersect terrain.")
			return
		var direction := Vector3(_last_hit.get("dir", Vector3.ZERO))
		if direction.length_squared() < 0.99:
			return
		_phase46_stage_center(_phase46_place_slot_ids, direction,
			"Place terrain feature in viewport")
		_set_status("Placed %s from the authoritative terrain pick." % _phase46_place_label)
		_set_placement_mode(PlacementMode.NONE)
		_refresh_current_category()
		return
	if _placement_mode == PHASE46_EDIT_FEATURE:
		if _last_hit.is_empty():
			return
		if not continuous:
			_phase46_drag_handle = _phase46_pick_handle(get_viewport().get_mouse_position())
			if _phase46_drag_handle.is_empty():
				_set_status("Grab a cyan viewport handle to move or resize the selected feature.")
			else:
				_set_status("Dragging %s handle." % _phase46_drag_handle.replace("_", " "))
			return
		if _phase46_drag_handle.is_empty():
			return
		_phase46_apply_drag_handle(Vector3(_last_hit.get("dir", Vector3.ZERO)), false)
		return
	super._place_current_hit(continuous)


func _phase46_apply_drag_handle(direction: Vector3, force: bool) -> void:
	if direction.length_squared() < 0.99 or _phase46_edit_slot_id.is_empty():
		return
	var now_usec: int = Time.get_ticks_usec()
	if not force and now_usec - _phase46_last_drag_update_usec < PHASE46_DRAG_INTERVAL_USEC:
		return
	_phase46_last_drag_update_usec = now_usec
	var slot: Resource = _phase46_feature_slot(_phase46_edit_slot_id)
	if slot == null:
		return
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null or not PHASE46_GUIDED.is_guided_graph(graph):
		return
	var config: Dictionary = PHASE46_GUIDED.config_from_graph(graph)
	var handle: String = _phase46_drag_handle
	if handle == "center":
		_phase46_stage_center([_phase46_edit_slot_id], direction, "Move terrain feature in viewport")
		return

	var lat_lon: Vector2 = _phase46_lat_lon_from_direction(direction)
	var value_key: String = ""
	var value: float = 0.0
	match handle:
		"north":
			value_key = "north_deg"
			value = maxf(lat_lon.x, float(config.get("south_deg", -90.0)))
		"south":
			value_key = "south_deg"
			value = minf(lat_lon.x, float(config.get("north_deg", 90.0)))
		"west":
			value_key = "west_deg"
			value = lat_lon.y
		"east":
			value_key = "east_deg"
			value = lat_lon.y
		"radius":
			value_key = "radius_deg"
			value = _phase46_angle_from_center(config, direction)
		"inner_radius":
			value_key = "inner_radius_deg"
			value = minf(_phase46_angle_from_center(config, direction),
				float(config.get("outer_radius_deg", 180.0)))
		"outer_radius":
			value_key = "outer_radius_deg"
			value = maxf(_phase46_angle_from_center(config, direction),
				float(config.get("inner_radius_deg", 0.0)))
		_:
			return
	_session.stage_action("Resize terrain feature in viewport", func() -> void:
		PHASE46_GUIDED.set_config_value(graph, value_key, value)
	, WorldAuthoringSession.ApplyScope.GRAPH)
	_update_preview()


func _phase46_stage_center(slot_ids: Array[String], direction: Vector3, action_name: String) -> void:
	if _session == null or direction.length_squared() < 0.99:
		return
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null:
		return
	var ids: Array[String] = slot_ids.duplicate()
	_session.stage_action(action_name, func() -> void:
		for slot_id: String in ids:
			var slot: Resource = terrain.call("find_shader_slot", slot_id) as Resource
			if slot == null:
				continue
			var graph: Resource = slot.get(&"graph") as Resource
			if graph != null and PHASE46_GUIDED.is_guided_graph(graph):
				_phase46_set_center_in_graph(graph, direction)
	, WorldAuthoringSession.ApplyScope.GRAPH)
	_update_preview()


func _phase46_set_center_in_graph(graph: Resource, direction: Vector3) -> void:
	var config: Dictionary = PHASE46_GUIDED.config_from_graph(graph)
	if config.is_empty():
		return
	var lat_lon: Vector2 = _phase46_lat_lon_from_direction(direction)
	var area_kind: String = String(config.get("area_kind", PHASE46_GUIDED.AREA_RADIAL))
	match area_kind:
		PHASE46_GUIDED.AREA_RADIAL, PHASE46_GUIDED.AREA_RING:
			PHASE46_GUIDED.set_config_value(graph, "center_latitude_deg", lat_lon.x)
			PHASE46_GUIDED.set_config_value(graph, "center_longitude_deg", lat_lon.y)
		PHASE46_GUIDED.AREA_REGION:
			var south: float = float(config.get("south_deg", -30.0))
			var north: float = float(config.get("north_deg", 30.0))
			var half_lat: float = maxf((north - south) * 0.5, 0.0)
			var center_lat: float = clampf(lat_lon.x, -90.0 + half_lat, 90.0 - half_lat)
			var span_lon: float = _phase46_eastward_span_deg(
				float(config.get("west_deg", -45.0)), float(config.get("east_deg", 45.0)))
			PHASE46_GUIDED.set_config_value(graph, "south_deg", center_lat - half_lat)
			PHASE46_GUIDED.set_config_value(graph, "north_deg", center_lat + half_lat)
			if span_lon > 1e-5 and span_lon < 359.999:
				PHASE46_GUIDED.set_config_value(graph, "west_deg", _phase46_wrap_longitude(lat_lon.y - span_lon * 0.5))
				PHASE46_GUIDED.set_config_value(graph, "east_deg", _phase46_wrap_longitude(lat_lon.y + span_lon * 0.5))
		PHASE46_GUIDED.AREA_LATITUDE:
			var south: float = float(config.get("south_deg", -30.0))
			var north: float = float(config.get("north_deg", 30.0))
			var half: float = maxf((north - south) * 0.5, 0.0)
			var center: float = clampf(lat_lon.x, -90.0 + half, 90.0 - half)
			PHASE46_GUIDED.set_config_value(graph, "south_deg", center - half)
			PHASE46_GUIDED.set_config_value(graph, "north_deg", center + half)
		PHASE46_GUIDED.AREA_LONGITUDE:
			var span: float = _phase46_eastward_span_deg(
				float(config.get("west_deg", -45.0)), float(config.get("east_deg", 45.0)))
			if span > 1e-5 and span < 359.999:
				PHASE46_GUIDED.set_config_value(graph, "west_deg", _phase46_wrap_longitude(lat_lon.y - span * 0.5))
				PHASE46_GUIDED.set_config_value(graph, "east_deg", _phase46_wrap_longitude(lat_lon.y + span * 0.5))


func _phase46_location_summary(config: Dictionary) -> String:
	var area_kind: String = String(config.get("area_kind", PHASE46_GUIDED.AREA_RADIAL))
	match area_kind:
		PHASE46_GUIDED.AREA_RADIAL:
			return "Center %.1f°, %.1f° · radius %.1f°" % [
				float(config.get("center_latitude_deg", 0.0)),
				float(config.get("center_longitude_deg", 0.0)),
				float(config.get("radius_deg", 0.0))]
		PHASE46_GUIDED.AREA_RING:
			return "Center %.1f°, %.1f° · ring %.1f°–%.1f°" % [
				float(config.get("center_latitude_deg", 0.0)),
				float(config.get("center_longitude_deg", 0.0)),
				float(config.get("inner_radius_deg", 0.0)),
				float(config.get("outer_radius_deg", 0.0))]
		PHASE46_GUIDED.AREA_REGION:
			return "Lat %.1f°…%.1f° · lon %.1f°…%.1f°" % [
				float(config.get("south_deg", 0.0)), float(config.get("north_deg", 0.0)),
				float(config.get("west_deg", 0.0)), float(config.get("east_deg", 0.0))]
		PHASE46_GUIDED.AREA_LATITUDE:
			return "Latitude %.1f°…%.1f°" % [float(config.get("south_deg", 0.0)), float(config.get("north_deg", 0.0))]
		PHASE46_GUIDED.AREA_LONGITUDE:
			return "Longitude %.1f°…%.1f°" % [float(config.get("west_deg", 0.0)), float(config.get("east_deg", 0.0))]
	return "Whole planet"


func _phase46_has_live_camera() -> bool:
	if _camera != null and is_instance_valid(_camera):
		return true
	if _player == null and _world_host != null:
		_player = _world_host.get("player") as Node
	if _player != null:
		_camera = _player.get("camera") as Camera3D
	if _camera == null:
		_camera = get_viewport().get_camera_3d()
	return _camera != null


func _phase46_feature_slot(slot_id: String) -> Resource:
	if _session == null or slot_id.is_empty():
		return null
	var terrain: Resource = _session.active_terrain_profile() as Resource
	return terrain.call("find_shader_slot", slot_id) as Resource if terrain != null else null


func _phase46_angle_from_center(config: Dictionary, direction: Vector3) -> float:
	var center := _phase46_direction_from_lat_lon(
		float(config.get("center_latitude_deg", 0.0)),
		float(config.get("center_longitude_deg", 0.0)))
	return rad_to_deg(acos(clampf(center.dot(direction.normalized()), -1.0, 1.0)))


func _phase46_lat_lon_from_direction(direction: Vector3) -> Vector2:
	var d := direction.normalized()
	return Vector2(rad_to_deg(asin(clampf(d.y, -1.0, 1.0))),
		_phase46_wrap_longitude(rad_to_deg(atan2(d.x, d.z))))


func _phase46_direction_from_lat_lon(latitude_deg: float, longitude_deg: float) -> Vector3:
	var lat := deg_to_rad(latitude_deg)
	var lon := deg_to_rad(longitude_deg)
	var cos_lat := cos(lat)
	return Vector3(sin(lon) * cos_lat, sin(lat), cos(lon) * cos_lat).normalized()


func _phase46_wrap_longitude(value_deg: float) -> float:
	return fposmod(value_deg + 180.0, 360.0) - 180.0


func _phase46_eastward_span_deg(west_deg: float, east_deg: float) -> float:
	return fposmod(east_deg - west_deg + 360.0, 360.0)


func _phase46_offset_direction(center: Vector3, angular_deg: float, bearing_rad: float = 0.0) -> Vector3:
	var c := center.normalized()
	var east := Vector3(c.z, 0.0, -c.x)
	if east.length_squared() < 1e-8:
		east = Vector3.RIGHT
	else:
		east = east.normalized()
	var north := c.cross(east).normalized()
	var tangent := (east * cos(bearing_rad) + north * sin(bearing_rad)).normalized()
	var angle := deg_to_rad(clampf(angular_deg, 0.0, 180.0))
	return (c * cos(angle) + tangent * sin(angle)).normalized()


func _phase46_handle_directions(config: Dictionary) -> Dictionary:
	var out: Dictionary = {}
	var area_kind: String = String(config.get("area_kind", PHASE46_GUIDED.AREA_RADIAL))
	var facing := _phase46_camera_facing_lat_lon()
	match area_kind:
		PHASE46_GUIDED.AREA_RADIAL:
			var center := _phase46_direction_from_lat_lon(float(config.get("center_latitude_deg", 0.0)), float(config.get("center_longitude_deg", 0.0)))
			out["center"] = center
			out["radius"] = _phase46_offset_direction(center, float(config.get("radius_deg", 15.0)))
		PHASE46_GUIDED.AREA_RING:
			var center := _phase46_direction_from_lat_lon(float(config.get("center_latitude_deg", 0.0)), float(config.get("center_longitude_deg", 0.0)))
			out["center"] = center
			out["outer_radius"] = _phase46_offset_direction(center, float(config.get("outer_radius_deg", 20.0)), 0.0)
			out["inner_radius"] = _phase46_offset_direction(center, float(config.get("inner_radius_deg", 8.0)), PI * 0.5)
		PHASE46_GUIDED.AREA_REGION:
			var west: float = float(config.get("west_deg", -45.0))
			var east: float = float(config.get("east_deg", 45.0))
			var center_lon := _phase46_wrap_longitude(west + _phase46_eastward_span_deg(west, east) * 0.5)
			var center_lat := (float(config.get("south_deg", -30.0)) + float(config.get("north_deg", 30.0))) * 0.5
			out["center"] = _phase46_direction_from_lat_lon(center_lat, center_lon)
			out["north"] = _phase46_direction_from_lat_lon(float(config.get("north_deg", 30.0)), center_lon)
			out["south"] = _phase46_direction_from_lat_lon(float(config.get("south_deg", -30.0)), center_lon)
			out["west"] = _phase46_direction_from_lat_lon(center_lat, west)
			out["east"] = _phase46_direction_from_lat_lon(center_lat, east)
		PHASE46_GUIDED.AREA_LATITUDE:
			var center_lat := (float(config.get("south_deg", -30.0)) + float(config.get("north_deg", 30.0))) * 0.5
			out["center"] = _phase46_direction_from_lat_lon(center_lat, facing.y)
			out["north"] = _phase46_direction_from_lat_lon(float(config.get("north_deg", 30.0)), facing.y)
			out["south"] = _phase46_direction_from_lat_lon(float(config.get("south_deg", -30.0)), facing.y)
		PHASE46_GUIDED.AREA_LONGITUDE:
			var west: float = float(config.get("west_deg", -45.0))
			var east: float = float(config.get("east_deg", 45.0))
			var center_lon := _phase46_wrap_longitude(west + _phase46_eastward_span_deg(west, east) * 0.5)
			out["center"] = _phase46_direction_from_lat_lon(facing.x, center_lon)
			out["west"] = _phase46_direction_from_lat_lon(facing.x, west)
			out["east"] = _phase46_direction_from_lat_lon(facing.x, east)
	return out


func _phase46_camera_facing_lat_lon() -> Vector2:
	if not _phase46_has_live_camera():
		return Vector2.ZERO
	var camera_world: Vec3D = Frames.to_world(_camera.global_position)
	return _phase46_lat_lon_from_direction(camera_world.normalized().to_v3())


func _phase46_pick_handle(screen_position: Vector2) -> String:
	var slot: Resource = _phase46_feature_slot(_phase46_edit_slot_id)
	if slot == null:
		return ""
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return ""
	var handles: Dictionary = _phase46_handle_directions(PHASE46_GUIDED.config_from_graph(graph))
	var best_name: String = ""
	var best_distance: float = PHASE46_HANDLE_HIT_RADIUS_PX
	for handle_value: Variant in handles.keys():
		var handle_name: String = String(handle_value)
		var projected: Vector2 = _phase46_project_direction(Vector3(handles[handle_name]))
		if not is_finite(projected.x) or not is_finite(projected.y):
			continue
		var distance: float = screen_position.distance_to(projected)
		if distance <= best_distance:
			best_distance = distance
			best_name = handle_name
	return best_name


func _phase46_project_direction(direction: Vector3) -> Vector2:
	if not _phase46_has_live_camera() or Planet.cfg == null or direction.length_squared() < 0.99:
		return Vector2(INF, INF)
	var world_point: Vec3D = Vec3D.from_v3(direction.normalized()).mul(
		float(Planet.cfg.planet_radius) + PHASE46_PREVIEW_ALTITUDE_M)
	var render_point: Vector3 = Frames.to_render(world_point)
	if _camera.is_position_behind(render_point):
		return Vector2(INF, INF)
	return _camera.unproject_position(render_point)


func _update_preview() -> void:
	if _placement_mode != PHASE46_PLACE_FEATURE and _placement_mode != PHASE46_EDIT_FEATURE:
		super._update_preview()
		return
	if _preview_mesh == null:
		return
	_preview_mesh.clear_surfaces()
	if _navigation_active or Planet.cfg == null:
		return

	if _placement_mode == PHASE46_PLACE_FEATURE:
		var preview_direction := Vector3(_last_hit.get("dir", Vector3.ZERO)) if not _last_hit.is_empty() else Vector3.ZERO
		for slot_id: String in _phase46_place_slot_ids:
			var slot: Resource = _phase46_feature_slot(slot_id)
			if slot == null:
				continue
			var graph: Resource = slot.get(&"graph") as Resource
			if graph == null:
				continue
			var config: Dictionary = PHASE46_GUIDED.config_from_graph(graph)
			if preview_direction.length_squared() > 0.99:
				config = _phase46_preview_config_centered(config, preview_direction)
			_phase46_draw_config(config, Color(1.0, 0.72, 0.20, 1.0), false)
		if preview_direction.length_squared() > 0.99:
			_phase46_draw_handle_cross(preview_direction, Color(1.0, 0.82, 0.28, 1.0), 1.35)
		return

	var slot: Resource = _phase46_feature_slot(_phase46_edit_slot_id)
	if slot == null:
		return
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return
	var config: Dictionary = PHASE46_GUIDED.config_from_graph(graph)
	_phase46_draw_config(config, Color(0.18, 0.88, 1.0, 1.0), true)


func _phase46_preview_config_centered(source: Dictionary, direction: Vector3) -> Dictionary:
	var config: Dictionary = source.duplicate(true)
	var lat_lon := _phase46_lat_lon_from_direction(direction)
	var area_kind: String = String(config.get("area_kind", PHASE46_GUIDED.AREA_RADIAL))
	match area_kind:
		PHASE46_GUIDED.AREA_RADIAL, PHASE46_GUIDED.AREA_RING:
			config["center_latitude_deg"] = lat_lon.x
			config["center_longitude_deg"] = lat_lon.y
		PHASE46_GUIDED.AREA_REGION:
			var half_lat := maxf((float(config.get("north_deg", 30.0)) - float(config.get("south_deg", -30.0))) * 0.5, 0.0)
			var center_lat := clampf(lat_lon.x, -90.0 + half_lat, 90.0 - half_lat)
			var span_lon := _phase46_eastward_span_deg(float(config.get("west_deg", -45.0)), float(config.get("east_deg", 45.0)))
			config["south_deg"] = center_lat - half_lat
			config["north_deg"] = center_lat + half_lat
			if span_lon > 1e-5 and span_lon < 359.999:
				config["west_deg"] = _phase46_wrap_longitude(lat_lon.y - span_lon * 0.5)
				config["east_deg"] = _phase46_wrap_longitude(lat_lon.y + span_lon * 0.5)
		PHASE46_GUIDED.AREA_LATITUDE:
			var half := maxf((float(config.get("north_deg", 30.0)) - float(config.get("south_deg", -30.0))) * 0.5, 0.0)
			var center := clampf(lat_lon.x, -90.0 + half, 90.0 - half)
			config["south_deg"] = center - half
			config["north_deg"] = center + half
		PHASE46_GUIDED.AREA_LONGITUDE:
			var span := _phase46_eastward_span_deg(float(config.get("west_deg", -45.0)), float(config.get("east_deg", 45.0)))
			if span > 1e-5 and span < 359.999:
				config["west_deg"] = _phase46_wrap_longitude(lat_lon.y - span * 0.5)
				config["east_deg"] = _phase46_wrap_longitude(lat_lon.y + span * 0.5)
	return config


func _phase46_draw_config(config: Dictionary, color: Color, draw_handles: bool) -> void:
	var area_kind: String = String(config.get("area_kind", PHASE46_GUIDED.AREA_RADIAL))
	match area_kind:
		PHASE46_GUIDED.AREA_RADIAL:
			var center := _phase46_direction_from_lat_lon(float(config.get("center_latitude_deg", 0.0)), float(config.get("center_longitude_deg", 0.0)))
			_phase46_draw_angular_circle(center, float(config.get("radius_deg", 15.0)), color)
		PHASE46_GUIDED.AREA_RING:
			var center := _phase46_direction_from_lat_lon(float(config.get("center_latitude_deg", 0.0)), float(config.get("center_longitude_deg", 0.0)))
			_phase46_draw_angular_circle(center, float(config.get("inner_radius_deg", 8.0)), color)
			_phase46_draw_angular_circle(center, float(config.get("outer_radius_deg", 20.0)), color)
		PHASE46_GUIDED.AREA_REGION:
			_phase46_draw_region(config, color)
		PHASE46_GUIDED.AREA_LATITUDE:
			_phase46_draw_latitude(float(config.get("south_deg", -30.0)), color)
			_phase46_draw_latitude(float(config.get("north_deg", 30.0)), color)
		PHASE46_GUIDED.AREA_LONGITUDE:
			_phase46_draw_longitude(float(config.get("west_deg", -45.0)), color)
			_phase46_draw_longitude(float(config.get("east_deg", 45.0)), color)
	if draw_handles:
		for handle_value: Variant in _phase46_handle_directions(config).keys():
			var handle_name := String(handle_value)
			var handle_color := Color(1.0, 0.42, 0.20, 1.0) if handle_name == "center" else Color(0.30, 1.0, 1.0, 1.0)
			_phase46_draw_handle_cross(Vector3(_phase46_handle_directions(config)[handle_name]), handle_color, 1.0)


func _phase46_draw_angular_circle(center: Vector3, radius_deg: float, color: Color) -> void:
	var points: Array[Vector3] = []
	for index: int in 97:
		points.append(_phase46_offset_direction(center, radius_deg, TAU * float(index) / 96.0))
	_phase46_draw_direction_line(points, color)


func _phase46_draw_latitude(latitude_deg: float, color: Color) -> void:
	var points: Array[Vector3] = []
	for index: int in 97:
		points.append(_phase46_direction_from_lat_lon(latitude_deg, -180.0 + 360.0 * float(index) / 96.0))
	_phase46_draw_direction_line(points, color)


func _phase46_draw_longitude(longitude_deg: float, color: Color) -> void:
	var points: Array[Vector3] = []
	for index: int in 65:
		points.append(_phase46_direction_from_lat_lon(-90.0 + 180.0 * float(index) / 64.0, longitude_deg))
	_phase46_draw_direction_line(points, color)


func _phase46_draw_region(config: Dictionary, color: Color) -> void:
	var south := float(config.get("south_deg", -30.0))
	var north := float(config.get("north_deg", 30.0))
	var west := float(config.get("west_deg", -45.0))
	var east := float(config.get("east_deg", 45.0))
	var span := _phase46_eastward_span_deg(west, east)
	var points: Array[Vector3] = []
	for index: int in 33:
		points.append(_phase46_direction_from_lat_lon(south,
			_phase46_wrap_longitude(west + span * float(index) / 32.0)))
	for index: int in 33:
		points.append(_phase46_direction_from_lat_lon(lerpf(south, north, float(index) / 32.0), east))
	for index: int in 33:
		points.append(_phase46_direction_from_lat_lon(north,
			_phase46_wrap_longitude(east - span * float(index) / 32.0)))
	for index: int in 33:
		points.append(_phase46_direction_from_lat_lon(lerpf(north, south, float(index) / 32.0), west))
	_phase46_draw_direction_line(points, color)


func _phase46_draw_direction_line(points: Array[Vector3], color: Color) -> void:
	if points.size() < 2 or Planet.cfg == null:
		return
	var radius: float = float(Planet.cfg.planet_radius) + PHASE46_PREVIEW_ALTITUDE_M
	_preview_mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
	for direction: Vector3 in points:
		_preview_mesh.surface_set_color(color)
		_preview_mesh.surface_add_vertex(Frames.to_render(Vec3D.from_v3(direction.normalized()).mul(radius)))
	_preview_mesh.surface_end()


func _phase46_draw_handle_cross(direction: Vector3, color: Color, scale_multiplier: float) -> void:
	if direction.length_squared() < 0.99 or Planet.cfg == null:
		return
	var d := direction.normalized()
	var east := Vector3(d.z, 0.0, -d.x)
	if east.length_squared() < 1e-8:
		east = Vector3.RIGHT
	else:
		east = east.normalized()
	var north := d.cross(east).normalized()
	var radius: float = float(Planet.cfg.planet_radius) + PHASE46_PREVIEW_ALTITUDE_M
	var render_center := Frames.to_render(Vec3D.from_v3(d).mul(radius))
	var distance_m: float = _camera.global_position.distance_to(render_center) if _phase46_has_live_camera() else 1000.0
	var arm_m: float = clampf(distance_m * 0.009 * scale_multiplier, 2.0, 5000.0)
	for axis: Vector3 in [east, north]:
		_preview_mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
		_preview_mesh.surface_set_color(color)
		_preview_mesh.surface_add_vertex(Frames.to_render(Vec3D.from_v3(d).mul(radius).add(Vec3D.from_v3(axis).mul(-arm_m))))
		_preview_mesh.surface_set_color(color)
		_preview_mesh.surface_add_vertex(Frames.to_render(Vec3D.from_v3(d).mul(radius).add(Vec3D.from_v3(axis).mul(arm_m))))
		_preview_mesh.surface_end()
