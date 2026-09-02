extends "res://scripts/world_authoring/terrain_graph_editor_phase34.gd"
## Phase 35: approachable terrain authoring modes.
##
## Simple and Detailed are presentation layers over the SAME serialized production
## graph used by Node Graph. They never own terrain state. A slider ultimately calls
## the inherited `_set_node_parameter()` and therefore follows the exact same
## session, runtime compiler, CPU contact and GPU renderer path as expert editing.

const BEGINNER_CATALOG := preload(
	"res://scripts/world_authoring/model/terrain_beginner_parameter_catalog.gd")

var _editing_mode: int = BEGINNER_CATALOG.MODE_SIMPLE
var _mode_picker: OptionButton
var _guided_panel: PanelContainer
var _guided_content: VBoxContainer
var _guided_status: Label
var _advanced_children: Array[Control] = []
var _geomorph_node_id: String = ""


func _build_ui() -> void:
	super._build_ui()
	if _graph == null or int(_graph.get(&"domain")) != GRAPH_SCRIPT.Domain.DISPLACEMENT:
		return
	_geomorph_node_id = _find_node_of_type(BEGINNER_CATALOG.GEOMORPH_NODE_TYPE)
	if _geomorph_node_id.is_empty():
		return

	# Everything created by the inherited graph editor remains intact. Guided modes
	# merely hide those Controls; switching back to Node Graph reveals the exact same
	# GraphEdit instance and serialized graph.
	_advanced_children.clear()
	for child: Node in get_children():
		if child is Control:
			_advanced_children.append(child as Control)

	_build_mode_picker()
	_build_guided_panel()
	_apply_editing_mode()


func _build_mode_picker() -> void:
	var bar := HBoxContainer.new()
	bar.name = "TerrainEditingModeBar"
	bar.add_theme_constant_override("separation", 8)
	add_child(bar)
	move_child(bar, 0)

	var label := Label.new()
	label.text = "Editing mode"
	label.custom_minimum_size.x = 110.0
	bar.add_child(label)

	_mode_picker = OptionButton.new()
	_mode_picker.custom_minimum_size.x = 220.0
	for mode: int in [
		BEGINNER_CATALOG.MODE_SIMPLE,
		BEGINNER_CATALOG.MODE_DETAILED,
		BEGINNER_CATALOG.MODE_NODE_GRAPH,
	]:
		_mode_picker.add_item(BEGINNER_CATALOG.mode_name(mode))
		_mode_picker.set_item_metadata(_mode_picker.item_count - 1, mode)
	_mode_picker.selected = 0
	_mode_picker.item_selected.connect(func(index: int) -> void:
		_editing_mode = int(_mode_picker.get_item_metadata(index))
		_apply_editing_mode()
	)
	bar.add_child(_mode_picker)

	var explanation := Label.new()
	explanation.text = "Simple uses everyday terrain controls · Detailed exposes physical sizes · Node Graph gives full structure"
	explanation.modulate = Color(0.60, 0.70, 0.78)
	explanation.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	explanation.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	bar.add_child(explanation)


func _build_guided_panel() -> void:
	_guided_panel = PanelContainer.new()
	_guided_panel.name = "GuidedTerrainEditor"
	_guided_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_guided_panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_guided_panel.custom_minimum_size = Vector2(760.0, 520.0)
	add_child(_guided_panel)
	move_child(_guided_panel, 1)

	var outer := VBoxContainer.new()
	outer.add_theme_constant_override("separation", 8)
	_guided_panel.add_child(outer)

	var intro := HBoxContainer.new()
	outer.add_child(intro)
	var title := Label.new()
	title.name = "GuidedModeTitle"
	title.text = "SHAPE THE TERRAIN"
	title.add_theme_font_size_override("font_size", 18)
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	intro.add_child(title)
	var reset := Button.new()
	reset.text = "Reset These Controls"
	reset.tooltip_text = "Restore the controls shown in this mode to the production defaults"
	reset.pressed.connect(_reset_visible_guided_controls)
	intro.add_child(reset)

	var tip := Label.new()
	tip.text = "Move a control and watch the terrain. You do not need to understand shaders or nodes. Values in metres describe real world size."
	tip.modulate = Color(0.67, 0.76, 0.83)
	tip.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	outer.add_child(tip)

	var scroll := ScrollContainer.new()
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	outer.add_child(scroll)
	_guided_content = VBoxContainer.new()
	_guided_content.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_guided_content.add_theme_constant_override("separation", 5)
	scroll.add_child(_guided_content)

	_guided_status = Label.new()
	_guided_status.text = "Protected editing: if an edit is incomplete or invalid, the last valid terrain stays visible."
	_guided_status.modulate = Color(0.58, 0.72, 0.66)
	_guided_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	outer.add_child(_guided_status)


func _apply_editing_mode() -> void:
	var guided: bool = _editing_mode != BEGINNER_CATALOG.MODE_NODE_GRAPH
	if _guided_panel != null:
		_guided_panel.visible = guided
	for control: Control in _advanced_children:
		if control != null and is_instance_valid(control):
			control.visible = not guided
	if guided:
		_rebuild_guided_controls()
	elif _graph_edit != null:
		call_deferred("_frame_graph_content")


func _rebuild_guided_controls() -> void:
	if _guided_content == null or _graph == null:
		return
	for child: Node in _guided_content.get_children():
		_guided_content.remove_child(child)
		child.queue_free()

	var node_data: Dictionary = _node_data(_geomorph_node_id)
	var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
	var previous_category: String = ""
	for spec: Dictionary in BEGINNER_CATALOG.controls_for_mode(_editing_mode):
		var category: String = String(spec.get("category", "Terrain"))
		if category != previous_category:
			_add_category_header(category)
			previous_category = category
		_add_guided_control(spec, parameters)


func _add_category_header(category: String) -> void:
	var header := Label.new()
	header.text = category.to_upper()
	header.add_theme_font_size_override("font_size", 15)
	header.modulate = Color(0.76, 0.84, 0.90)
	header.custom_minimum_size.y = 30.0
	_guided_content.add_child(header)


func _add_guided_control(spec: Dictionary, parameters: Dictionary) -> void:
	var card := VBoxContainer.new()
	card.custom_minimum_size = Vector2(650.0, 72.0)
	card.add_theme_constant_override("separation", 2)
	_guided_content.add_child(card)

	var top := HBoxContainer.new()
	card.add_child(top)
	var title := Label.new()
	title.text = String(spec.get("title", "Terrain"))
	title.custom_minimum_size.x = 215.0
	title.tooltip_text = String(spec.get("description", ""))
	top.add_child(title)

	var key: String = String(spec.get("key", ""))
	var minimum: float = float(spec.get("min", 0.0))
	var maximum: float = float(spec.get("max", 1.0))
	var step: float = float(spec.get("step", 0.01))
	var default_value: float = float(spec.get("default", 0.0))
	var current: float = clampf(float(parameters.get(key, default_value)), minimum, maximum)

	var slider := HSlider.new()
	slider.min_value = minimum
	slider.max_value = maximum
	slider.step = step
	slider.value = current
	slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	slider.custom_minimum_size.x = 260.0
	top.add_child(slider)

	var value := SpinBox.new()
	value.min_value = minimum
	value.max_value = maximum
	value.step = step
	value.value = current
	value.custom_minimum_size.x = 125.0
	value.allow_greater = false
	value.allow_lesser = false
	var unit: String = String(spec.get("unit", ""))
	if not unit.is_empty():
		value.suffix = " " + unit
	top.add_child(value)

	slider.value_changed.connect(func(next_value: float) -> void:
		value.set_value_no_signal(next_value)
		_set_node_parameter(_geomorph_node_id, key, next_value)
	)
	value.value_changed.connect(func(next_value: float) -> void:
		slider.set_value_no_signal(next_value)
		_set_node_parameter(_geomorph_node_id, key, next_value)
	)

	var description := Label.new()
	description.text = String(spec.get("description", ""))
	description.modulate = Color(0.57, 0.66, 0.73)
	description.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	description.custom_minimum_size.x = 620.0
	card.add_child(description)


func _reset_visible_guided_controls() -> void:
	if _session == null or _graph == null or _geomorph_node_id.is_empty():
		return
	var specs: Array[Dictionary] = BEGINNER_CATALOG.controls_for_mode(_editing_mode)
	_session.call("stage_action", "Reset terrain shape controls", func() -> void:
		for spec: Dictionary in specs:
			_graph.call("set_node_parameter", _geomorph_node_id,
				String(spec.get("key", "")), float(spec.get("default", 0.0)))
	, 2)
	_rebuild_guided_controls()


func _find_node_of_type(node_type: String) -> String:
	if _graph == null:
		return ""
	for node_value: Variant in _graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		if String(node.get("type", "")) == node_type:
			return String(node.get("id", ""))
	return ""


func _node_data(node_id: String) -> Dictionary:
	if _graph == null:
		return {}
	for node_value: Variant in _graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		if String(node.get("id", "")) == node_id:
			return node
	return {}
