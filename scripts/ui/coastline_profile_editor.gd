class_name CoastlineProfileEditor
extends CanvasLayer
## Runtime curve editor for terrain offsets measured geodesically away from the
## macro shoreline. Its x axis exists only seaward; Planet rejects every inland
## sample regardless of the curve.

signal apply_requested(points: PackedVector2Array)

class CoastProfileGraph extends Control:
	signal selection_changed(index: int)
	signal curve_changed(index: int)

	const LEFT := 66.0
	const RIGHT := 24.0
	const TOP := 26.0
	const BOTTOM := 46.0
	const POINT_RADIUS := 7.0

	var points: Array[Vector2] = [] # x = kilometres seaward, y = metres offset
	var selected := -1
	var dragging := false
	var min_height := -5000.0
	var max_height := 2000.0

	func _init() -> void:
		custom_minimum_size = Vector2(760.0, 390.0)
		mouse_default_cursor_shape = Control.CURSOR_CROSS

	func set_points_m(source: PackedVector2Array) -> void:
		points.clear()
		for point in source:
			points.append(Vector2(point.x / 1000.0, point.y))
		if points.size() < 2:
			points = [Vector2.ZERO, Vector2(1000.0, 0.0)]
		points[0] = Vector2.ZERO
		selected = mini(1, points.size() - 1)
		_expand_height_range()
		queue_redraw()
		selection_changed.emit(selected)

	func points_m() -> PackedVector2Array:
		var out := PackedVector2Array()
		for point in points:
			out.append(Vector2(point.x * 1000.0, point.y))
		return out

	func selected_point() -> Vector2:
		return points[selected] if selected >= 0 and selected < points.size() else Vector2.ZERO

	func set_selected_point(distance_km: float, height_m: float) -> void:
		if selected < 0 or selected >= points.size():
			return
		if selected == 0:
			points[0] = Vector2.ZERO
		else:
			var low: float = points[selected - 1].x + 0.1
			var high: float = points[selected + 1].x - 0.1 if selected + 1 < points.size() else 3000.0
			points[selected] = Vector2(clampf(distance_km, low, maxf(low, high)),
				clampf(height_m, -12000.0, 8000.0))
		_expand_height_range()
		queue_redraw()
		curve_changed.emit(selected)

	func add_point() -> void:
		if points.size() < 2:
			return
		var insert_at := selected + 1 if selected >= 0 else points.size() - 1
		insert_at = clampi(insert_at, 1, points.size() - 1)
		var a: Vector2 = points[insert_at - 1]
		var b: Vector2 = points[insert_at]
		points.insert(insert_at, a.lerp(b, 0.5))
		selected = insert_at
		queue_redraw()
		selection_changed.emit(selected)
		curve_changed.emit(selected)

	func remove_selected() -> void:
		if selected <= 0 or selected >= points.size() - 1:
			return
		points.remove_at(selected)
		selected = mini(selected, points.size() - 1)
		queue_redraw()
		selection_changed.emit(selected)
		curve_changed.emit(selected)

	func _gui_input(event: InputEvent) -> void:
		if event is InputEventMouseButton:
			if event.button_index == MOUSE_BUTTON_LEFT:
				if event.pressed:
					var nearest := _nearest_point(event.position)
					if event.double_click and nearest < 0:
						_insert_at_graph_position(event.position)
						accept_event()
						return
					if nearest >= 0:
						selected = nearest
						dragging = selected > 0
						selection_changed.emit(selected)
						queue_redraw()
						accept_event()
				else:
					dragging = false
			elif event.button_index == MOUSE_BUTTON_RIGHT and event.pressed:
				var nearest := _nearest_point(event.position)
				if nearest > 0 and nearest < points.size() - 1:
					selected = nearest
					remove_selected()
					accept_event()
		elif event is InputEventMouseMotion and dragging and selected > 0:
			var value := _from_graph(event.position)
			var low: float = points[selected - 1].x + 0.1
			var high: float = points[selected + 1].x - 0.1 if selected + 1 < points.size() else 3000.0
			points[selected] = Vector2(clampf(value.x, low, maxf(low, high)),
				clampf(value.y, -12000.0, 8000.0))
			_expand_height_range()
			queue_redraw()
			curve_changed.emit(selected)
			accept_event()

	func _insert_at_graph_position(position: Vector2) -> void:
		var value := _from_graph(position)
		var insert_at := 1
		while insert_at < points.size() and points[insert_at].x < value.x:
			insert_at += 1
		if insert_at >= points.size():
			insert_at = points.size() - 1
		var low: float = points[insert_at - 1].x + 0.1
		var high: float = points[insert_at].x - 0.1
		if high <= low:
			return
		value.x = clampf(value.x, low, high)
		points.insert(insert_at, value)
		selected = insert_at
		queue_redraw()
		selection_changed.emit(selected)
		curve_changed.emit(selected)

	func _nearest_point(position: Vector2) -> int:
		var best := -1
		var best_distance := POINT_RADIUS * 2.2
		for i in points.size():
			var distance := position.distance_to(_to_graph(points[i]))
			if distance < best_distance:
				best_distance = distance
				best = i
		return best

	func _plot_rect() -> Rect2:
		return Rect2(LEFT, TOP, maxf(1.0, size.x - LEFT - RIGHT),
			maxf(1.0, size.y - TOP - BOTTOM))

	func _distance_max() -> float:
		return maxf(1.0, points[-1].x if not points.is_empty() else 1000.0)

	func _to_graph(value: Vector2) -> Vector2:
		var rect := _plot_rect()
		return Vector2(rect.position.x + value.x / _distance_max() * rect.size.x,
			rect.end.y - inverse_lerp(min_height, max_height, value.y) * rect.size.y)

	func _from_graph(position: Vector2) -> Vector2:
		var rect := _plot_rect()
		return Vector2(
			clampf((position.x - rect.position.x) / rect.size.x, 0.0, 1.0) * _distance_max(),
			lerpf(max_height, min_height,
				clampf((position.y - rect.position.y) / rect.size.y, 0.0, 1.0)))

	func _expand_height_range() -> void:
		var low := -250.0
		var high := 250.0
		for point in points:
			low = minf(low, point.y)
			high = maxf(high, point.y)
		var span := maxf(500.0, high - low)
		min_height = floor((low - span * 0.18) / 100.0) * 100.0
		max_height = ceil((high + span * 0.18) / 100.0) * 100.0

	func _draw() -> void:
		var rect := _plot_rect()
		draw_rect(rect, Color(0.018, 0.035, 0.055, 0.96), true)
		draw_rect(rect, Color(0.28, 0.53, 0.68, 0.7), false, 1.0)
		var font := ThemeDB.fallback_font
		for i in 6:
			var t := float(i) / 5.0
			var x := rect.position.x + rect.size.x * t
			draw_line(Vector2(x, rect.position.y), Vector2(x, rect.end.y),
				Color(0.25, 0.42, 0.52, 0.26), 1.0)
			var km := _distance_max() * t
			draw_string(font, Vector2(x - 18.0, rect.end.y + 20.0), "%g" % km,
				HORIZONTAL_ALIGNMENT_CENTER, 42.0, 12, Color(0.65, 0.75, 0.82))
		for i in 5:
			var t := float(i) / 4.0
			var y := rect.position.y + rect.size.y * t
			draw_line(Vector2(rect.position.x, y), Vector2(rect.end.x, y),
				Color(0.25, 0.42, 0.52, 0.26), 1.0)
			var metres := lerpf(max_height, min_height, t)
			draw_string(font, Vector2(4.0, y + 4.0), "%+.0f m" % metres,
				HORIZONTAL_ALIGNMENT_RIGHT, 56.0, 12, Color(0.65, 0.75, 0.82))
		var zero_y := _to_graph(Vector2(0.0, 0.0)).y
		if zero_y >= rect.position.y and zero_y <= rect.end.y:
			draw_line(Vector2(rect.position.x, zero_y), Vector2(rect.end.x, zero_y),
				Color(0.72, 0.86, 0.92, 0.72), 1.5)
		if points.size() >= 2:
			var line := PackedVector2Array()
			for point in points:
				line.append(_to_graph(point))
			draw_polyline(line, Color(0.20, 0.82, 0.95), 3.0, true)
			for i in points.size():
				var color := Color(1.0, 0.76, 0.24) if i == selected else Color(0.78, 0.94, 1.0)
				draw_circle(_to_graph(points[i]), POINT_RADIUS, color)
		draw_string(font, Vector2(rect.position.x, 16.0), "COAST  →  DISTANCE TOWARD SEA",
			HORIZONTAL_ALIGNMENT_LEFT, -1.0, 12, Color(0.45, 0.90, 1.0))
		draw_string(font, Vector2(rect.end.x - 225.0, size.y - 7.0),
			"distance seaward (km)", HORIZONTAL_ALIGNMENT_RIGHT, 225.0, 12,
			Color(0.65, 0.75, 0.82))

var _panel: PanelContainer
var _graph: CoastProfileGraph
var _distance_spin: SpinBox
var _height_spin: SpinBox
var _apply_button: Button
var _syncing := false

func _ready() -> void:
	layer = 21
	visible = false
	process_mode = Node.PROCESS_MODE_ALWAYS
	_build_ui()

func open() -> void:
	_graph.set_points_m(Planet.coast_profile_points())
	visible = true

func close() -> void:
	visible = false

func _build_ui() -> void:
	_panel = PanelContainer.new()
	_panel.set_anchors_preset(Control.PRESET_CENTER)
	_panel.position = Vector2(-430.0, -285.0)
	_panel.custom_minimum_size = Vector2(860.0, 570.0)
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.035, 0.055, 0.075, 0.98)
	style.border_color = Color(0.24, 0.68, 0.82, 0.9)
	style.set_border_width_all(1)
	style.set_corner_radius_all(6)
	style.set_content_margin_all(16)
	_panel.add_theme_stylebox_override("panel", style)
	add_child(_panel)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 8)
	_panel.add_child(box)
	var title := Label.new()
	title.text = "COASTLINE TERRAIN PROFILE"
	title.add_theme_font_size_override("font_size", 18)
	title.add_theme_color_override("font_color", Color(0.78, 0.95, 1.0))
	box.add_child(title)
	var note := Label.new()
	note.text = "Height offsets are applied only seaward from the macro coastline. Inland terrain is untouched."
	note.add_theme_font_size_override("font_size", 12)
	note.add_theme_color_override("font_color", Color(0.65, 0.75, 0.82))
	box.add_child(note)

	_graph = CoastProfileGraph.new()
	_graph.selection_changed.connect(_on_selection_changed)
	_graph.curve_changed.connect(_on_curve_changed)
	box.add_child(_graph)

	var edit_row := HBoxContainer.new()
	edit_row.add_theme_constant_override("separation", 10)
	box.add_child(edit_row)
	edit_row.add_child(_label("Selected distance (km)"))
	_distance_spin = SpinBox.new()
	_distance_spin.min_value = 0.0
	_distance_spin.max_value = 3000.0
	_distance_spin.step = 0.1
	_distance_spin.custom_minimum_size.x = 120.0
	_distance_spin.value_changed.connect(_on_numeric_changed)
	edit_row.add_child(_distance_spin)
	edit_row.add_child(_label("Height offset (m)"))
	_height_spin = SpinBox.new()
	_height_spin.min_value = -12000.0
	_height_spin.max_value = 8000.0
	_height_spin.step = 1.0
	_height_spin.custom_minimum_size.x = 120.0
	_height_spin.value_changed.connect(_on_numeric_changed)
	edit_row.add_child(_height_spin)
	var add := Button.new()
	add.text = "Add point"
	add.pressed.connect(_graph.add_point)
	edit_row.add_child(add)
	var remove := Button.new()
	remove.text = "Remove"
	remove.pressed.connect(_graph.remove_selected)
	edit_row.add_child(remove)

	var help := Label.new()
	help.text = "Drag points or enter exact values · double-click to add · right-click to remove · the coast point is locked at 0 m"
	help.add_theme_font_size_override("font_size", 11)
	help.add_theme_color_override("font_color", Color(0.55, 0.67, 0.74))
	box.add_child(help)

	var buttons := HBoxContainer.new()
	buttons.alignment = BoxContainer.ALIGNMENT_END
	buttons.add_theme_constant_override("separation", 8)
	box.add_child(buttons)
	var reset := Button.new()
	reset.text = "Flat profile"
	reset.pressed.connect(_on_reset)
	buttons.add_child(reset)
	var close_button := Button.new()
	close_button.text = "Close"
	close_button.pressed.connect(close)
	buttons.add_child(close_button)
	_apply_button = Button.new()
	_apply_button.text = "Apply and rebuild terrain"
	_apply_button.pressed.connect(_on_apply)
	buttons.add_child(_apply_button)

func _label(text: String) -> Label:
	var label := Label.new()
	label.text = text
	label.add_theme_font_size_override("font_size", 12)
	return label

func _on_selection_changed(_index: int) -> void:
	_sync_numeric()

func _on_curve_changed(_index: int) -> void:
	_sync_numeric()

func _sync_numeric() -> void:
	if _graph == null:
		return
	_syncing = true
	var point := _graph.selected_point()
	_distance_spin.value = point.x
	_height_spin.value = point.y
	_distance_spin.editable = _graph.selected > 0
	_height_spin.editable = _graph.selected > 0
	_syncing = false

func _on_numeric_changed(_value: float) -> void:
	if _syncing:
		return
	_graph.set_selected_point(_distance_spin.value, _height_spin.value)

func _on_reset() -> void:
	_graph.set_points_m(PackedVector2Array([
		Vector2(0.0, 0.0),
		Vector2(10000.0, 0.0),
		Vector2(50000.0, 0.0),
		Vector2(250000.0, 0.0),
		Vector2(1000000.0, 0.0),
	]))

func _on_apply() -> void:
	_apply_button.disabled = true
	_apply_button.text = "Rebuilding terrain..."
	apply_requested.emit(_graph.points_m())
	_apply_button.disabled = false
	_apply_button.text = "Apply and rebuild terrain"
