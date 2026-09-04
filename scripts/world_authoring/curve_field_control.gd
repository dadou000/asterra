class_name CurveFieldControl
extends Control
## Small interactive response-curve editor: drag control points, double-click
## empty space to add one (up to CurveFieldData.MAX_POINTS), right-click an
## interior point to remove it. Used wherever a normalized [0,1]->[0,1] shape
## is authored -- Biome Terrain's noise response curve, Biome Texture's
## gradient distribution curve -- rather than a fixed linear ramp.
##
## Deliberately NOT a full Bezier/tangent-handle curve editor: points
## interpolate via a fixed smoothstep rule (see CurveFieldData), so there is
## nothing else to author per-point, no tangent handles to drag, and the
## GPU/CPU evaluation stays trivially identical. That is a real capability
## trade-off against, say, Godot's own editor-only Curve resource widget
## (which this project can't use anyway -- it lives in editor/ and is not
## available to a runtime Control), not an accident of scope.

signal curve_changed(points: PackedFloat32Array)

const POINT_RADIUS: float = 6.0
const HIT_RADIUS: float = 12.0
const MARGIN: float = 10.0
const SAMPLES: int = 48

var points: PackedFloat32Array = CurveFieldData.identity()
var _dragging_index: int = -1
var _hover_index: int = -1


func _ready() -> void:
	custom_minimum_size = Vector2(240.0, 120.0)
	mouse_filter = Control.MOUSE_FILTER_STOP
	queue_redraw()


func set_points(new_points: PackedFloat32Array) -> void:
	points = new_points.duplicate()
	queue_redraw()


func get_points() -> PackedFloat32Array:
	return points.duplicate()


func _plot_rect() -> Rect2:
	return Rect2(Vector2(MARGIN, MARGIN), size - Vector2(MARGIN * 2.0, MARGIN * 2.0))


func _to_screen(point: Vector2, plot: Rect2) -> Vector2:
	return plot.position + Vector2(point.x * plot.size.x, (1.0 - point.y) * plot.size.y)


func _from_screen(screen: Vector2, plot: Rect2) -> Vector2:
	var x: float = (screen.x - plot.position.x) / maxf(plot.size.x, 1.0)
	var y: float = 1.0 - (screen.y - plot.position.y) / maxf(plot.size.y, 1.0)
	return Vector2(clampf(x, 0.0, 1.0), clampf(y, 0.0, 1.0))


func _draw() -> void:
	var plot: Rect2 = _plot_rect()
	draw_rect(Rect2(Vector2.ZERO, size), Color(0.05, 0.06, 0.09, 0.95), true)
	draw_rect(plot, Color(0.09, 0.11, 0.15, 1.0), true)
	draw_rect(plot, Color(0.28, 0.34, 0.44, 0.9), false, 1.0)

	# Quarter gridlines -- purely a visual reference, not tied to point snapping.
	for i: int in range(1, 4):
		var f: float = float(i) / 4.0
		var gx: float = plot.position.x + f * plot.size.x
		var gy: float = plot.position.y + f * plot.size.y
		draw_line(Vector2(gx, plot.position.y), Vector2(gx, plot.end.y),
			Color(0.2, 0.24, 0.31, 0.7), 1.0)
		draw_line(Vector2(plot.position.x, gy), Vector2(plot.end.x, gy),
			Color(0.2, 0.24, 0.31, 0.7), 1.0)

	# The curve itself, sampled at fixed intervals -- the same evaluation the
	# shader/CPU runtime uses, so what you see here is what you get.
	var prev: Vector2 = _to_screen(Vector2(0.0, CurveFieldData.evaluate(points, 0.0)), plot)
	for i: int in range(1, SAMPLES + 1):
		var x: float = float(i) / float(SAMPLES)
		var next: Vector2 = _to_screen(Vector2(x, CurveFieldData.evaluate(points, x)), plot)
		draw_line(prev, next, Color(0.55, 0.85, 0.55, 0.95), 2.0)
		prev = next

	var count: int = CurveFieldData.point_count(points)
	for i: int in count:
		var p: Vector2 = CurveFieldData.get_point(points, i)
		var screen_p: Vector2 = _to_screen(p, plot)
		var is_endpoint: bool = i == 0 or i == count - 1
		var color: Color = Color(1.0, 0.85, 0.3, 1.0) if i == _dragging_index \
			else (Color(0.85, 0.9, 0.95, 1.0) if i == _hover_index else Color(0.7, 0.76, 0.82, 1.0))
		draw_circle(screen_p, POINT_RADIUS, color)
		if is_endpoint:
			draw_arc(screen_p, POINT_RADIUS + 2.0, 0.0, TAU, 16, Color(0.4, 0.46, 0.55, 0.9), 1.0)


func _point_index_at(screen_pos: Vector2, plot: Rect2) -> int:
	var count: int = CurveFieldData.point_count(points)
	var best_index: int = -1
	var best_dist: float = HIT_RADIUS
	for i: int in count:
		var screen_p: Vector2 = _to_screen(CurveFieldData.get_point(points, i), plot)
		var d: float = screen_p.distance_to(screen_pos)
		if d <= best_dist:
			best_dist = d
			best_index = i
	return best_index


func _gui_input(event: InputEvent) -> void:
	var plot: Rect2 = _plot_rect()
	if event is InputEventMouseMotion:
		var motion := event as InputEventMouseMotion
		if _dragging_index >= 0:
			var value: Vector2 = _from_screen(motion.position, plot)
			points = CurveFieldData.move_point(points, _dragging_index, value.x, value.y)
			curve_changed.emit(points)
			queue_redraw()
		else:
			var hover: int = _point_index_at(motion.position, plot)
			if hover != _hover_index:
				_hover_index = hover
				queue_redraw()
		accept_event()
		return

	if event is InputEventMouseButton:
		var button := event as InputEventMouseButton
		if button.button_index == MOUSE_BUTTON_LEFT:
			if button.pressed:
				if button.double_click:
					var existing: int = _point_index_at(button.position, plot)
					if existing < 0 and CurveFieldData.can_add_point(points):
						var value: Vector2 = _from_screen(button.position, plot)
						points = CurveFieldData.insert_point(points, value.x, value.y)
						curve_changed.emit(points)
						queue_redraw()
				else:
					_dragging_index = _point_index_at(button.position, plot)
			else:
				_dragging_index = -1
			accept_event()
		elif button.button_index == MOUSE_BUTTON_RIGHT and button.pressed:
			var target: int = _point_index_at(button.position, plot)
			if target > 0 and target < CurveFieldData.point_count(points) - 1:
				points = CurveFieldData.remove_point(points, target)
				curve_changed.emit(points)
				queue_redraw()
			accept_event()
