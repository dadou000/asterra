extends "res://scripts/world_authoring/terrain_graph_editor_phase43.gd"
## Phase 44: approachable production Surface controls.
##
## Simple and Detailed are presentation layers over the same serialized production
## Surface settings nodes used by Node Graph and the resident terrain shader. No
## alternate material state or simplified shader exists here.

const SURFACE_CATALOG := preload(
	"res://scripts/world_authoring/model/terrain_beginner_surface_catalog.gd")
const PRODUCTION_SURFACE_SLOT_ID := "production-terrain-surface"

var _surface_editing_mode: int = SURFACE_CATALOG.MODE_SIMPLE
var _surface_mode_picker: OptionButton
var _surface_guided_panel: PanelContainer
var _surface_guided_content: VBoxContainer
var _surface_advanced_children: Array[Control] = []


func _build_ui() -> void:
	super._build_ui()
	if _graph == null or int(_graph.get(&"domain")) != GRAPH_SCRIPT.Domain.MATERIAL \
			or _slot == null or String(_slot.get(&"slot_id")) != PRODUCTION_SURFACE_SLOT_ID:
		return

	_surface_advanced_children.clear()
	for child: Node in get_children():
		if child is Control:
			_surface_advanced_children.append(child as Control)

	_build_surface_mode_picker()
	_build_surface_guided_panel()
	_apply_surface_editing_mode()


func _build_surface_mode_picker() -> void:
	var bar := HBoxContainer.new()
	bar.name = "SurfaceEditingModeBar"
	bar.add_theme_constant_override("separation", 8)
	add_child(bar)
	move_child(bar, 0)

	var label := Label.new()
	label.text = "Editing mode"
	label.custom_minimum_size.x = 110.0
	bar.add_child(label)

	_surface_mode_picker = OptionButton.new()
	_surface_mode_picker.custom_minimum_size.x = 220.0
	for mode: int in [
		SURFACE_CATALOG.MODE_SIMPLE,
		SURFACE_CATALOG.MODE_DETAILED,
		SURFACE_CATALOG.MODE_NODE_GRAPH,
	]:
		_surface_mode_picker.add_item(SURFACE_CATALOG.mode_name(mode))
		_surface_mode_picker.set_item_metadata(_surface_mode_picker.item_count - 1, mode)
	_surface_mode_picker.select(_surface_editing_mode)
	_surface_mode_picker.item_selected.connect(func(index: int) -> void:
		_surface_editing_mode = int(_surface_mode_picker.get_item_metadata(index))
		_apply_surface_editing_mode()
	)
	bar.add_child(_surface_mode_picker)

	var explanation := Label.new()
	explanation.text = "Simple = useful material controls · Detailed = physical thresholds and scales · Node Graph = complete production Surface"
	explanation.modulate = Color(0.60, 0.70, 0.78)
	explanation.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	explanation.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	bar.add_child(explanation)


func _build_surface_guided_panel() -> void:
	_surface_guided_panel = PanelContainer.new()
	_surface_guided_panel.name = "GuidedTerrainSurfaceEditor"
	_surface_guided_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_surface_guided_panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_surface_guided_panel.custom_minimum_size = Vector2(820.0, 620.0)
	add_child(_surface_guided_panel)
	move_child(_surface_guided_panel, 1)

	var outer := VBoxContainer.new()
	outer.add_theme_constant_override("separation", 8)
	_surface_guided_panel.add_child(outer)

	var heading := HBoxContainer.new()
	outer.add_child(heading)
	var title := Label.new()
	title.text = "SURFACE APPEARANCE"
	title.add_theme_font_size_override("font_size", 19)
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	heading.add_child(title)
	var reset := Button.new()
	reset.text = "Reset Visible Controls"
	reset.tooltip_text = "Restore only the Surface controls shown in the current mode to production defaults"
	reset.pressed.connect(_reset_visible_surface_controls)
	heading.add_child(reset)

	var intro := Label.new()
	intro.text = "Tune the materials already produced by terrain classification. These controls directly edit the production classifier, palette, microrelief, anti-tiling, procedural rock and scanned-PBR settings."
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	intro.modulate = Color(0.66, 0.76, 0.83)
	outer.add_child(intro)

	var scroll := ScrollContainer.new()
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	outer.add_child(scroll)
	_surface_guided_content = VBoxContainer.new()
	_surface_guided_content.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_surface_guided_content.add_theme_constant_override("separation", 5)
	scroll.add_child(_surface_guided_content)

	var note := Label.new()
	note.text = "All values remain part of the production Surface graph. Switching to Node Graph reveals the exact same settings nodes; no conversion occurs."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.modulate = Color(0.58, 0.72, 0.66)
	outer.add_child(note)


func _apply_surface_editing_mode() -> void:
	var guided: bool = _surface_editing_mode != SURFACE_CATALOG.MODE_NODE_GRAPH
	if _surface_guided_panel != null:
		_surface_guided_panel.visible = guided
	for control: Control in _surface_advanced_children:
		if control != null and is_instance_valid(control):
			control.visible = not guided
	if _surface_mode_picker != null:
		_surface_mode_picker.select(_surface_editing_mode)
	if guided:
		_rebuild_surface_controls()
	elif _graph_edit != null:
		call_deferred("_frame_graph_content")


func _rebuild_surface_controls() -> void:
	if _surface_guided_content == null or _graph == null:
		return
	for child: Node in _surface_guided_content.get_children():
		_surface_guided_content.remove_child(child)
		child.queue_free()

	var previous_category: String = ""
	for spec: Dictionary in SURFACE_CATALOG.controls_for_mode(_surface_editing_mode):
		var node_type: String = String(spec.get("node_type", ""))
		var node_id: String = _surface_node_id(node_type)
		if node_id.is_empty():
			continue
		var category: String = String(spec.get("category", "Surface"))
		if category != previous_category:
			_add_surface_category(category)
			previous_category = category
		var parameters: Dictionary = _surface_node_parameters(node_id)
		match String(spec.get("kind", "number")):
			"toggle": _add_surface_toggle(spec, node_id, parameters)
			"color": _add_surface_color(spec, node_id, parameters)
			_: _add_surface_number(spec, node_id, parameters)


func _add_surface_category(category: String) -> void:
	var header := Label.new()
	header.text = category.to_upper()
	header.add_theme_font_size_override("font_size", 15)
	header.modulate = Color(0.76, 0.84, 0.90)
	header.custom_minimum_size.y = 31.0
	_surface_guided_content.add_child(header)


func _add_surface_number(spec: Dictionary, node_id: String,
		parameters: Dictionary) -> void:
	var card := VBoxContainer.new()
	card.custom_minimum_size = Vector2(700.0, 68.0)
	card.add_theme_constant_override("separation", 2)
	_surface_guided_content.add_child(card)

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	card.add_child(row)
	var title := Label.new()
	title.text = String(spec.get("title", "Surface"))
	title.custom_minimum_size.x = 205.0
	title.tooltip_text = String(spec.get("description", ""))
	row.add_child(title)

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
	slider.custom_minimum_size.x = 300.0
	row.add_child(slider)

	var value := SpinBox.new()
	value.min_value = minimum
	value.max_value = maximum
	value.step = step
	value.value = current
	value.custom_minimum_size.x = 135.0
	value.allow_lesser = false
	value.allow_greater = false
	var unit: String = String(spec.get("unit", ""))
	if not unit.is_empty():
		value.suffix = " " + unit
	row.add_child(value)

	slider.value_changed.connect(func(next_value: float) -> void:
		value.set_value_no_signal(next_value)
		_set_node_parameter(node_id, key, next_value)
	)
	value.value_changed.connect(func(next_value: float) -> void:
		slider.set_value_no_signal(next_value)
		_set_node_parameter(node_id, key, next_value)
	)
	_add_surface_description(card, String(spec.get("description", "")))


func _add_surface_toggle(spec: Dictionary, node_id: String,
		parameters: Dictionary) -> void:
	var card := VBoxContainer.new()
	card.custom_minimum_size = Vector2(700.0, 58.0)
	_surface_guided_content.add_child(card)
	var key: String = String(spec.get("key", ""))
	var toggle := CheckButton.new()
	toggle.text = String(spec.get("title", "Surface"))
	toggle.button_pressed = bool(parameters.get(key, bool(spec.get("default", true))))
	toggle.tooltip_text = String(spec.get("description", ""))
	toggle.toggled.connect(func(value: bool) -> void:
		_set_node_parameter(node_id, key, value)
	)
	card.add_child(toggle)
	_add_surface_description(card, String(spec.get("description", "")))


func _add_surface_color(spec: Dictionary, node_id: String,
		parameters: Dictionary) -> void:
	var card := VBoxContainer.new()
	card.custom_minimum_size = Vector2(700.0, 62.0)
	_surface_guided_content.add_child(card)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	card.add_child(row)
	var title := Label.new()
	title.text = String(spec.get("title", "Surface color"))
	title.custom_minimum_size.x = 205.0
	row.add_child(title)
	var key: String = String(spec.get("key", ""))
	var picker := ColorPickerButton.new()
	picker.custom_minimum_size = Vector2(210.0, 34.0)
	picker.color = Color(parameters.get(key, spec.get("default", Color.WHITE)))
	picker.color_changed.connect(func(value: Color) -> void:
		_set_node_parameter(node_id, key, value)
	)
	row.add_child(picker)
	_add_surface_description(card, String(spec.get("description", "")))


func _add_surface_description(parent: VBoxContainer, text: String) -> void:
	if text.is_empty():
		return
	var description := Label.new()
	description.text = text
	description.modulate = Color(0.57, 0.66, 0.73)
	description.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	description.custom_minimum_size.x = 680.0
	parent.add_child(description)


func _reset_visible_surface_controls() -> void:
	if _session == null or _graph == null:
		return
	var specs: Array[Dictionary] = SURFACE_CATALOG.controls_for_mode(_surface_editing_mode)
	_session.call("stage_action", "Reset visible terrain Surface controls", func() -> void:
		for spec: Dictionary in specs:
			var node_id: String = _surface_node_id(String(spec.get("node_type", "")))
			if node_id.is_empty():
				continue
			_graph.call("set_node_parameter", node_id, String(spec.get("key", "")),
				spec.get("default"))
	, 2)
	_rebuild_surface_controls()


func _surface_node_id(node_type: String) -> String:
	if _graph == null:
		return ""
	for node_value: Variant in _graph.get(&"nodes") as Array:
		if node_value is Dictionary and String((node_value as Dictionary).get("type", "")) == node_type:
			return String((node_value as Dictionary).get("id", ""))
	return ""


func _surface_node_parameters(node_id: String) -> Dictionary:
	if _graph == null:
		return {}
	for node_value: Variant in _graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		if String(node.get("id", "")) == node_id:
			return (node.get("parameters", {}) as Dictionary).duplicate(true)
	return {}
