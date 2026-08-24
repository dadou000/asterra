class_name ScreenHistogramPlot
extends Control
## Lightweight logarithmic histogram view for the Exposure debug tab.

var _snapshot: Dictionary = {}


func _ready() -> void:
	custom_minimum_size = Vector2(0.0, 210.0)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	queue_redraw()


func set_snapshot(snapshot: Dictionary) -> void:
	_snapshot = snapshot.duplicate(true)
	queue_redraw()


func clear() -> void:
	_snapshot.clear()
	queue_redraw()


func _draw() -> void:
	var rect := Rect2(Vector2.ZERO, size)
	draw_rect(rect, Color(0.025, 0.03, 0.045, 0.95), true)
	draw_rect(rect, Color(0.25, 0.31, 0.42, 0.9), false, 1.0)

	if _snapshot.is_empty():
		draw_string(get_theme_default_font(), Vector2(14.0, 28.0),
			"Waiting for HDR histogram…", HORIZONTAL_ALIGNMENT_LEFT, -1.0, 12,
			Color(0.62, 0.68, 0.77))
		return

	var bins_value: Variant = _snapshot.get("bins")
	if not (bins_value is PackedInt32Array):
		return
	var bins := bins_value as PackedInt32Array
	if bins.is_empty():
		return

	var min_ev := float(_snapshot.get("min_ev", -12.0))
	var max_ev := float(_snapshot.get("max_ev", 8.0))
	var max_count := maxi(int(_snapshot.get("max_count", 1)), 1)
	var plot_left := 38.0
	var plot_right := maxf(size.x - 12.0, plot_left + 20.0)
	var plot_top := 12.0
	var plot_bottom := maxf(size.y - 31.0, plot_top + 20.0)
	var plot_width := plot_right - plot_left
	var plot_height := plot_bottom - plot_top

	# Stop grid. Histogram range is relative to 18% middle grey, so 0 EV is the
	# middle-grey reference and every vertical division is physically meaningful.
	for ev_tick in range(int(ceil(min_ev)), int(floor(max_ev)) + 1, 2):
		var x := plot_left + (float(ev_tick) - min_ev) / (max_ev - min_ev) * plot_width
		draw_line(Vector2(x, plot_top), Vector2(x, plot_bottom),
			Color(0.14, 0.17, 0.23, 0.9), 1.0)
		draw_string(get_theme_default_font(), Vector2(x - 10.0, size.y - 9.0),
			"%+d" % ev_tick, HORIZONTAL_ALIGNMENT_LEFT, -1.0, 9,
			Color(0.48, 0.55, 0.65))

	var log_peak := log(1.0 + float(max_count))
	var bin_width := plot_width / float(bins.size())
	for i in bins.size():
		var normalized := log(1.0 + float(bins[i])) / maxf(log_peak, 1.0e-6)
		var bar_height := normalized * plot_height
		var x0 := plot_left + float(i) * bin_width
		var bar_rect := Rect2(
			Vector2(x0 + 0.5, plot_bottom - bar_height),
			Vector2(maxf(bin_width - 1.0, 1.0), bar_height))
		draw_rect(bar_rect, Color(0.55, 0.72, 0.95, 0.86), true)

	# Middle grey reference.
	var middle_x := plot_left + (0.0 - min_ev) / (max_ev - min_ev) * plot_width
	draw_line(Vector2(middle_x, plot_top), Vector2(middle_x, plot_bottom),
		Color(1.0, 0.78, 0.34, 0.95), 1.5)

	# Percentile markers make it easy to see whether a tiny highlight is driving the
	# frame or whether the entire distribution actually moved.
	_draw_ev_marker(float(_snapshot.get("p50_ev", 0.0)), plot_left, plot_width,
		plot_top, plot_bottom, min_ev, max_ev, Color(0.44, 1.0, 0.62, 0.95))
	_draw_ev_marker(float(_snapshot.get("p95_ev", 0.0)), plot_left, plot_width,
		plot_top, plot_bottom, min_ev, max_ev, Color(1.0, 0.48, 0.40, 0.95))

	draw_string(get_theme_default_font(), Vector2(6.0, plot_top + 11.0),
		"log", HORIZONTAL_ALIGNMENT_LEFT, -1.0, 9, Color(0.48, 0.55, 0.65))
	draw_string(get_theme_default_font(), Vector2(plot_left, size.y - 9.0),
		"EV relative to 18% grey", HORIZONTAL_ALIGNMENT_LEFT, -1.0, 9,
		Color(0.58, 0.64, 0.73))


func _draw_ev_marker(ev: float, plot_left: float, plot_width: float,
		plot_top: float, plot_bottom: float, min_ev: float, max_ev: float,
		color: Color) -> void:
	var t := clampf((ev - min_ev) / maxf(max_ev - min_ev, 1.0e-6), 0.0, 1.0)
	var x := plot_left + t * plot_width
	draw_line(Vector2(x, plot_top), Vector2(x, plot_bottom), color, 1.0)
