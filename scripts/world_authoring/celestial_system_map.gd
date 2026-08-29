class_name CelestialSystemMap
extends Control
## Lightweight interactive orbital hierarchy map for Planet Studio. It is not a
## physics ephemeris view; it preserves parent/orbit relationships and lets the
## author switch bodies directly from the map.

signal body_selected(body_id: String)

var _system: Resource
var _active_body_id: String = ""
var _body_positions: Dictionary = {}
var _body_radii_px: Dictionary = {}

func setup(system: Resource, active_body_id: String) -> void:
	_system = system
	_active_body_id = active_body_id
	custom_minimum_size = Vector2(760.0, 360.0)
	mouse_filter = Control.MOUSE_FILTER_STOP
	queue_redraw()

func _notification(what: int) -> void:
	if what == NOTIFICATION_RESIZED:
		queue_redraw()

func _draw() -> void:
	draw_rect(Rect2(Vector2.ZERO, size), Color(0.025, 0.045, 0.065, 1.0), true)
	_body_positions.clear()
	_body_radii_px.clear()
	if _system == null:
		return
	var bodies: Array = _system.get(&"bodies")
	if bodies.is_empty():
		return
	var roots: Array[Resource] = []
	for body_value: Variant in bodies:
		var body := body_value as Resource
		if body != null and String(body.get(&"parent_body_id")).is_empty():
			roots.append(body)
	if roots.is_empty():
		return
	var center := size * 0.5
	for root_index: int in roots.size():
		var root := roots[root_index]
		var root_offset := Vector2.ZERO
		if roots.size() > 1:
			var root_angle := TAU * float(root_index) / float(roots.size())
			root_offset = Vector2(cos(root_angle), sin(root_angle)) * minf(size.x, size.y) * 0.24
		_layout_body(root, center + root_offset, 0)
	_draw_hierarchy_lines()
	_draw_bodies()
	var hint := "Click a body to switch active authoring target"
	draw_string(get_theme_default_font(), Vector2(12.0, size.y - 12.0), hint, HORIZONTAL_ALIGNMENT_LEFT, -1.0, 12, Color(0.50, 0.61, 0.71))

func _layout_body(body: Resource, position: Vector2, depth: int) -> void:
	var body_id := String(body.get(&"body_id"))
	if body_id.is_empty() or _body_positions.has(body_id):
		return
	_body_positions[body_id] = position
	_body_radii_px[body_id] = clampf(12.0 - float(depth) * 1.5, 5.0, 14.0)
	var children: Array = _system.call("children_of", body_id)
	if children.is_empty():
		return
	var max_axis := 1.0
	for child_value: Variant in children:
		var child := child_value as Resource
		if child == null:
			continue
		var orbit := child.get(&"orbit") as Resource
		if orbit != null:
			max_axis = maxf(max_axis, float(orbit.get(&"semi_major_axis_m")))
	for child_index: int in children.size():
		var child: Resource = children[child_index] as Resource
		if child == null:
			continue
		var orbit := child.get(&"orbit") as Resource
		var axis := float(orbit.get(&"semi_major_axis_m")) if orbit != null else 0.0
		var axis_factor := 0.55 + 0.45 * sqrt(clampf(axis / max_axis, 0.0, 1.0)) if max_axis > 1.0 else 1.0
		var base_radius := 58.0 + float(depth) * 18.0 + float(child_index) * 22.0
		var radius := base_radius * axis_factor
		var anomaly_deg := float(orbit.get(&"mean_anomaly_at_epoch_deg")) if orbit != null else 0.0
		var angle := deg_to_rad(anomaly_deg) + TAU * float(child_index) / maxf(1.0, float(children.size()))
		var child_position := position + Vector2(cos(angle), sin(angle)) * radius
		_layout_body(child, child_position, depth + 1)

func _draw_hierarchy_lines() -> void:
	var bodies: Array = _system.get(&"bodies")
	for body_value: Variant in bodies:
		var body := body_value as Resource
		if body == null:
			continue
		var body_id := String(body.get(&"body_id"))
		var parent_id := String(body.get(&"parent_body_id"))
		if parent_id.is_empty() or not _body_positions.has(body_id) or not _body_positions.has(parent_id):
			continue
		var center: Vector2 = _body_positions[parent_id]
		var point: Vector2 = _body_positions[body_id]
		var radius := center.distance_to(point)
		draw_arc(center, radius, 0.0, TAU, 80, Color(0.18, 0.28, 0.38, 0.55), 1.0, true)
		draw_line(center, point, Color(0.16, 0.24, 0.32, 0.45), 1.0, true)

func _draw_bodies() -> void:
	var bodies: Array = _system.get(&"bodies")
	for body_value: Variant in bodies:
		var body := body_value as Resource
		if body == null:
			continue
		var body_id := String(body.get(&"body_id"))
		if not _body_positions.has(body_id):
			continue
		var position: Vector2 = _body_positions[body_id]
		var radius := float(_body_radii_px.get(body_id, 8.0))
		var body_type := int(body.get(&"body_type"))
		var color := _body_color(body_type)
		if body_id == _active_body_id:
			draw_circle(position, radius + 5.0, Color(0.25, 0.78, 1.0, 0.28))
			draw_arc(position, radius + 7.0, 0.0, TAU, 36, Color(0.35, 0.86, 1.0), 2.0, true)
		draw_circle(position, radius, color)
		draw_string(get_theme_default_font(), position + Vector2(radius + 7.0, 4.0), String(body.get(&"display_name")), HORIZONTAL_ALIGNMENT_LEFT, -1.0, 13, Color(0.84, 0.90, 0.95))

func _gui_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		var mouse_event := event as InputEventMouseButton
		if mouse_event.button_index != MOUSE_BUTTON_LEFT or not mouse_event.pressed:
			return
		var best_id := ""
		var best_distance := 24.0
		for key: Variant in _body_positions:
			var body_id := String(key)
			var point: Vector2 = _body_positions[key]
			var distance := point.distance_to(mouse_event.position)
			if distance < best_distance:
				best_distance = distance
				best_id = body_id
		if not best_id.is_empty():
			body_selected.emit(best_id)
			accept_event()

func _body_color(body_type: int) -> Color:
	match body_type:
		0: return Color(1.0, 0.78, 0.34)
		2: return Color(0.66, 0.70, 0.76)
		3: return Color(0.64, 0.52, 0.40)
		4: return Color(0.72, 0.48, 0.72)
		_: return Color(0.30, 0.62, 0.92)
