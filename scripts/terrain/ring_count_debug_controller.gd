extends Node
## Small non-persistent UI for overriding the automatic terrain ring count.
## Automatic mode remains the production default and follows the horizon-safe
## planet coverage logic in GroundGeometryClipmap.

const REFRESH_S := 0.20

var _menu: DebugMenu
var _toggle: CheckButton
var _label: Label
var _slider: HSlider
var _refresh_left := 0.0


func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	process_priority = 41
	get_tree().node_added.connect(_on_node_added)
	call_deferred("_scan")


func _process(delta: float) -> void:
	if _menu == null or not is_instance_valid(_menu) or not _menu.visible:
		return
	_refresh_left -= delta
	if _refresh_left <= 0.0:
		_refresh_left = REFRESH_S
		_refresh_controls()


func _on_node_added(node: Node) -> void:
	if node is DebugMenu:
		call_deferred("_install", node)


func _scan() -> void:
	var found := _find_debug_menu(get_tree().root)
	if found != null:
		_install(found)


func _find_debug_menu(node: Node) -> DebugMenu:
	if node is DebugMenu:
		return node as DebugMenu
	for child: Node in node.get_children():
		var found := _find_debug_menu(child)
		if found != null:
			return found
	return null


func _find_named_vbox(node: Node, target_name: String) -> VBoxContainer:
	if node is VBoxContainer and node.name == target_name:
		return node as VBoxContainer
	for child: Node in node.get_children():
		var found := _find_named_vbox(child, target_name)
		if found != null:
			return found
	return null


func _install(menu: DebugMenu) -> void:
	if menu == null or not is_instance_valid(menu):
		return
	var box := _find_named_vbox(menu, "TerrainControls")
	if box == null:
		return
	if box.get_node_or_null("ManualRingControls") != null:
		_menu = menu
		return

	_menu = menu
	var controls := VBoxContainer.new()
	controls.name = "ManualRingControls"
	controls.add_theme_constant_override("separation", 5)
	box.add_child(HSeparator.new())
	box.add_child(controls)

	var title := Label.new()
	title.text = "Ring-count manipulator"
	title.add_theme_font_size_override("font_size", 14)
	title.add_theme_color_override("font_color", Color(0.86, 0.91, 1.0))
	controls.add_child(title)

	_toggle = CheckButton.new()
	_toggle.text = "Manual ring count"
	_toggle.custom_minimum_size = Vector2(0, 34)
	_toggle.add_theme_font_size_override("font_size", 13)
	_toggle.toggled.connect(_on_manual_toggled)
	controls.add_child(_toggle)

	var note := Label.new()
	note.text = "     OFF = automatic planet/horizon coverage with enough rings to avoid terrain holes. ON = diagnostic manual override."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.add_theme_font_size_override("font_size", 11)
	note.add_theme_color_override("font_color", Color(0.58, 0.65, 0.76))
	controls.add_child(note)

	_label = Label.new()
	_label.add_theme_font_size_override("font_size", 13)
	controls.add_child(_label)

	_slider = HSlider.new()
	_slider.min_value = 0.0
	_slider.max_value = 14.0
	_slider.step = 1.0
	_slider.custom_minimum_size = Vector2(0, 28)
	_slider.value_changed.connect(_on_ring_count_changed)
	controls.add_child(_slider)

	_refresh_controls()


func _on_manual_toggled(enabled: bool) -> void:
	if GroundGeometryClipmap.has_method("set_debug_manual_ring_count_enabled"):
		GroundGeometryClipmap.call("set_debug_manual_ring_count_enabled", enabled)
	_refresh_controls()


func _on_ring_count_changed(value: float) -> void:
	if _toggle == null or not _toggle.button_pressed:
		return
	if GroundGeometryClipmap.has_method("set_debug_manual_ring_count"):
		GroundGeometryClipmap.call("set_debug_manual_ring_count", int(round(value)))
	_refresh_controls()


func _refresh_controls() -> void:
	if _toggle == null or _slider == null or _label == null:
		return
	if not GroundGeometryClipmap.has_method("gpu_stream_stats"):
		_toggle.disabled = true
		_slider.editable = false
		_label.text = "Ring controls unavailable"
		return

	var manual := false
	if GroundGeometryClipmap.has_method("debug_manual_ring_count_enabled"):
		manual = bool(GroundGeometryClipmap.call("debug_manual_ring_count_enabled"))
	_toggle.set_pressed_no_signal(manual)
	_slider.editable = manual

	var stats: Dictionary = GroundGeometryClipmap.call("gpu_stream_stats")
	var automatic_count := int(stats.get("automatic_ring_count", stats.get("physical_ring_instances", 0)))
	var manual_count := automatic_count
	if GroundGeometryClipmap.has_method("debug_manual_ring_count"):
		manual_count = int(GroundGeometryClipmap.call("debug_manual_ring_count"))
	var shown_count := manual_count if manual else automatic_count
	_slider.set_value_no_signal(float(shown_count))

	var min_level := int(stats.get("active_min_level", 0))
	var active_max := int(stats.get("active_max_level", min_level))
	if manual:
		_label.text = "Manual: %d rings  •  active L%d–L%d" % [shown_count, min_level, active_max]
	else:
		_label.text = "AUTO: %d rings  •  active L%d–L%d  •  no-hole planet coverage" % [automatic_count, min_level, active_max]
