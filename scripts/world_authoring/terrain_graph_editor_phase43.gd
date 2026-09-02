extends "res://scripts/world_authoring/terrain_graph_editor_phase41.gd"
## Phase 43: beginner-facing authored displacement features.
##
## Guided features are ordinary node graphs created by TerrainGuidedFeatureGraph.
## Simple mode edits that exact serialized graph; Node Graph reveals the same nodes.
## Arbitrary custom graphs are never rewritten or interpreted as guided features.

const GUIDED := preload("res://scripts/world_authoring/model/terrain_guided_feature_graph.gd")

var _feature_simple_panel: PanelContainer
var _feature_mode_picker: OptionButton
var _feature_advanced_children: Array[Control] = []
var _feature_show_graph: bool = false


func _build_ui() -> void:
	super._build_ui()
	if _graph == null or int(_graph.get(&"domain")) != GRAPH_SCRIPT.Domain.DISPLACEMENT:
		return
	if not GUIDED.is_guided_graph(_graph):
		return

	_feature_advanced_children.clear()
	for child: Node in get_children():
		if child is Control and child.name != "TerrainTransactionBar":
			_feature_advanced_children.append(child as Control)
	_build_feature_mode_bar()
	_build_feature_simple_panel()
	_apply_feature_mode()


func _build_feature_mode_bar() -> void:
	var bar := HBoxContainer.new()
	bar.name = "GuidedFeatureModeBar"
	bar.add_theme_constant_override("separation", 8)
	add_child(bar)
	move_child(bar, 0)

	var label := Label.new()
	label.text = "Feature editing"
	label.custom_minimum_size.x = 120.0
	bar.add_child(label)

	_feature_mode_picker = OptionButton.new()
	_feature_mode_picker.custom_minimum_size.x = 210.0
	_feature_mode_picker.add_item("Simple")
	_feature_mode_picker.set_item_metadata(0, 0)
	_feature_mode_picker.add_item("Node Graph")
	_feature_mode_picker.set_item_metadata(1, 1)
	_feature_mode_picker.select(1 if _feature_show_graph else 0)
	_feature_mode_picker.item_selected.connect(func(index: int) -> void:
		_feature_show_graph = int(_feature_mode_picker.get_item_metadata(index)) == 1
		_apply_feature_mode()
	)
	bar.add_child(_feature_mode_picker)

	var explanation := Label.new()
	explanation.text = "Simple = effect + location · Node Graph = the exact generated terrain flow"
	explanation.modulate = Color(0.60, 0.70, 0.78)
	explanation.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	explanation.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	bar.add_child(explanation)


func _build_feature_simple_panel() -> void:
	_feature_simple_panel = PanelContainer.new()
	_feature_simple_panel.name = "GuidedTerrainFeatureEditor"
	_feature_simple_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_feature_simple_panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_feature_simple_panel.custom_minimum_size = Vector2(820.0, 590.0)
	add_child(_feature_simple_panel)
	move_child(_feature_simple_panel, 1)

	var outer := VBoxContainer.new()
	outer.add_theme_constant_override("separation", 10)
	_feature_simple_panel.add_child(outer)

	var heading := Label.new()
	heading.text = "TERRAIN FEATURE"
	heading.add_theme_font_size_override("font_size", 19)
	outer.add_child(heading)

	var intro := Label.new()
	intro.text = "Choose what this layer does and where it happens. Planet Studio builds the node graph for you; rendered terrain and contact/physics still use the same authoritative graph."
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	intro.modulate = Color(0.66, 0.76, 0.83)
	outer.add_child(intro)

	var config: Dictionary = GUIDED.config_from_graph(_graph)
	_build_effect_section(outer, config)
	var separator := HSeparator.new()
	outer.add_child(separator)
	_build_area_section(outer, config)

	var note := Label.new()
	note.text = "Protected editing: invalid candidates never replace the last valid terrain. Use Node Graph only when you need custom math or routing."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.modulate = Color(0.58, 0.72, 0.66)
	outer.add_child(note)


func _build_effect_section(parent: VBoxContainer, config: Dictionary) -> void:
	var title := Label.new()
	title.text = "1 — WHAT SHOULD HAPPEN?"
	title.add_theme_font_size_override("font_size", 16)
	parent.add_child(title)

	var picker_row := HBoxContainer.new()
	picker_row.add_theme_constant_override("separation", 8)
	parent.add_child(picker_row)
	var label := Label.new()
	label.text = "Effect"
	label.custom_minimum_size.x = 165.0
	picker_row.add_child(label)
	var picker := OptionButton.new()
	picker.custom_minimum_size.x = 300.0
	var current: String = String(config.get("effect_kind", GUIDED.EFFECT_HEIGHT))
	for effect_kind: String in GUIDED.EFFECTS:
		picker.add_item(GUIDED.effect_label(effect_kind))
		picker.set_item_metadata(picker.item_count - 1, effect_kind)
		if effect_kind == current:
			picker.select(picker.item_count - 1)
	picker.item_selected.connect(func(index: int) -> void:
		_rebuild_guided_kind("effect_kind", String(picker.get_item_metadata(index)))
	)
	picker_row.add_child(picker)

	var effect_kind: String = current
	var amount_label: String = "Height change"
	var amount_min: float = -2000.0
	var amount_max: float = 2000.0
	var amount_tip: String = "Positive raises terrain; negative lowers it."
	match effect_kind:
		GUIDED.EFFECT_NOISE:
			amount_label = "Roughness height"
			amount_min = 0.0
			amount_max = 500.0
			amount_tip = "Height of the added natural roughness."
		GUIDED.EFFECT_MOUNTAINS:
			amount_label = "Mountain height"
			amount_min = 0.0
			amount_max = 3000.0
			amount_tip = "Maximum contribution of the ridged mountain pattern."
		GUIDED.EFFECT_CHANNELS:
			amount_label = "Cut depth"
			amount_min = 0.0
			amount_max = 1000.0
			amount_tip = "Maximum depth removed by the channel pattern."
		GUIDED.EFFECT_DEPOSIT:
			amount_label = "Deposit height"
			amount_min = 0.0
			amount_max = 1000.0
			amount_tip = "Maximum height added by the deposit pattern."
	_add_guided_number(parent, amount_label, "amount_m",
		float(config.get("amount_m", 25.0)), amount_min, amount_max, 1.0, "m", amount_tip)

	if effect_kind != GUIDED.EFFECT_HEIGHT:
		_add_guided_number(parent, "Pattern density", "scale",
			float(config.get("scale", 6.0)), 0.1, 100.0, 0.1, "",
			"Higher values create smaller, more frequent features; lower values create broader features.")
		_add_guided_number(parent, "Detail passes", "passes",
			float(config.get("passes", 3)), 1.0, 4.0, 1.0, "",
			"Number of deterministic detail octaves. Higher values add smaller structure.", false)


func _build_area_section(parent: VBoxContainer, config: Dictionary) -> void:
	var title := Label.new()
	title.text = "2 — WHERE SHOULD IT HAPPEN?"
	title.add_theme_font_size_override("font_size", 16)
	parent.add_child(title)

	var picker_row := HBoxContainer.new()
	picker_row.add_theme_constant_override("separation", 8)
	parent.add_child(picker_row)
	var label := Label.new()
	label.text = "Area"
	label.custom_minimum_size.x = 165.0
	picker_row.add_child(label)
	var picker := OptionButton.new()
	picker.custom_minimum_size.x = 300.0
	var current: String = String(config.get("area_kind", GUIDED.AREA_RADIAL))
	for area_kind: String in GUIDED.AREAS:
		picker.add_item(GUIDED.area_label(area_kind))
		picker.set_item_metadata(picker.item_count - 1, area_kind)
		if area_kind == current:
			picker.select(picker.item_count - 1)
	picker.item_selected.connect(func(index: int) -> void:
		_rebuild_guided_kind("area_kind", String(picker.get_item_metadata(index)))
	)
	picker_row.add_child(picker)

	match current:
		GUIDED.AREA_EVERYWHERE:
			var everywhere := Label.new()
			everywhere.text = "This effect covers the entire planet for every LOD carried by this layer."
			everywhere.modulate = Color(0.60, 0.70, 0.78)
			parent.add_child(everywhere)
		GUIDED.AREA_LATITUDE:
			_add_guided_degree(parent, "South edge", "south_deg", float(config.get("south_deg", -30.0)), -90.0, 90.0)
			_add_guided_degree(parent, "North edge", "north_deg", float(config.get("north_deg", 30.0)), -90.0, 90.0)
			_add_guided_degree(parent, "Edge softness", "latitude_feather_deg", float(config.get("latitude_feather_deg", 5.0)), 0.0, 90.0)
		GUIDED.AREA_LONGITUDE:
			_add_guided_degree(parent, "West edge", "west_deg", float(config.get("west_deg", -45.0)), -180.0, 180.0)
			_add_guided_degree(parent, "East edge", "east_deg", float(config.get("east_deg", 45.0)), -180.0, 180.0)
			_add_guided_degree(parent, "Edge softness", "longitude_feather_deg", float(config.get("longitude_feather_deg", 5.0)), 0.0, 180.0)
		GUIDED.AREA_REGION:
			_add_guided_degree(parent, "South edge", "south_deg", float(config.get("south_deg", -30.0)), -90.0, 90.0)
			_add_guided_degree(parent, "North edge", "north_deg", float(config.get("north_deg", 30.0)), -90.0, 90.0)
			_add_guided_degree(parent, "Latitude softness", "latitude_feather_deg", float(config.get("latitude_feather_deg", 5.0)), 0.0, 90.0)
			_add_guided_degree(parent, "West edge", "west_deg", float(config.get("west_deg", -45.0)), -180.0, 180.0)
			_add_guided_degree(parent, "East edge", "east_deg", float(config.get("east_deg", 45.0)), -180.0, 180.0)
			_add_guided_degree(parent, "Longitude softness", "longitude_feather_deg", float(config.get("longitude_feather_deg", 5.0)), 0.0, 180.0)
		GUIDED.AREA_RADIAL:
			_add_guided_degree(parent, "Center latitude", "center_latitude_deg", float(config.get("center_latitude_deg", 0.0)), -90.0, 90.0)
			_add_guided_degree(parent, "Center longitude", "center_longitude_deg", float(config.get("center_longitude_deg", 0.0)), -180.0, 180.0)
			_add_guided_degree(parent, "Radius", "radius_deg", float(config.get("radius_deg", 15.0)), 0.0, 180.0)
			_add_guided_degree(parent, "Edge softness", "radial_feather_deg", float(config.get("radial_feather_deg", 5.0)), 0.0, 180.0)
		GUIDED.AREA_RING:
			_add_guided_degree(parent, "Center latitude", "center_latitude_deg", float(config.get("center_latitude_deg", 0.0)), -90.0, 90.0)
			_add_guided_degree(parent, "Center longitude", "center_longitude_deg", float(config.get("center_longitude_deg", 0.0)), -180.0, 180.0)
			_add_guided_degree(parent, "Inner radius", "inner_radius_deg", float(config.get("inner_radius_deg", 8.0)), 0.0, 180.0)
			_add_guided_degree(parent, "Outer radius", "outer_radius_deg", float(config.get("outer_radius_deg", 20.0)), 0.0, 180.0)
			_add_guided_degree(parent, "Edge softness", "radial_feather_deg", float(config.get("radial_feather_deg", 5.0)), 0.0, 180.0)

	if current != GUIDED.AREA_EVERYWHERE:
		var invert := CheckButton.new()
		invert.text = "Invert area — affect everything outside instead"
		invert.button_pressed = bool(config.get("invert", false))
		invert.toggled.connect(func(value: bool) -> void:
			_set_guided_value("invert", value)
		)
		parent.add_child(invert)

	var convention := Label.new()
	convention.text = "Coordinates use the planet: 0° latitude = equator; 0° longitude = +Z; +90° longitude = +X. Radial and Ring areas use true great-circle distance and remain seam/pole-safe."
	convention.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	convention.modulate = Color(0.55, 0.65, 0.73)
	parent.add_child(convention)


func _add_guided_degree(parent: VBoxContainer, label_text: String, key: String,
		value: float, minimum: float, maximum: float) -> void:
	_add_guided_number(parent, label_text, key, value, minimum, maximum, 0.5, "°", "")


func _add_guided_number(parent: VBoxContainer, label_text: String, key: String,
		value: float, minimum: float, maximum: float, step: float, suffix: String,
		description: String, use_slider: bool = true) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(760.0, 34.0)
	row.add_theme_constant_override("separation", 8)
	parent.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 165.0
	if not description.is_empty():
		label.tooltip_text = description
	row.add_child(label)

	var slider: HSlider = null
	if use_slider:
		slider = HSlider.new()
		slider.min_value = minimum
		slider.max_value = maximum
		slider.step = step
		slider.value = clampf(value, minimum, maximum)
		slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		slider.custom_minimum_size.x = 360.0
		row.add_child(slider)

	var spin := SpinBox.new()
	spin.min_value = minimum
	spin.max_value = maximum
	spin.step = step
	spin.value = clampf(value, minimum, maximum)
	spin.custom_minimum_size.x = 135.0
	spin.allow_lesser = false
	spin.allow_greater = false
	spin.suffix = suffix
	row.add_child(spin)

	if slider != null:
		slider.value_changed.connect(func(next_value: float) -> void:
			spin.set_value_no_signal(next_value)
			_set_guided_value(key, int(next_value) if key == "passes" else next_value)
		)
		spin.value_changed.connect(func(next_value: float) -> void:
			slider.set_value_no_signal(next_value)
			_set_guided_value(key, int(next_value) if key == "passes" else next_value)
		)
	else:
		spin.value_changed.connect(func(next_value: float) -> void:
			_set_guided_value(key, int(next_value) if key == "passes" else next_value)
		)

	if not description.is_empty():
		var info := Label.new()
		info.text = description
		info.modulate = Color(0.52, 0.62, 0.70)
		info.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		info.custom_minimum_size.x = 760.0
		parent.add_child(info)


func _set_guided_value(key: String, value: Variant) -> void:
	if _session == null or _graph == null or not GUIDED.is_guided_graph(_graph):
		return
	_session.call("stage_action", "Change simplified terrain feature", func() -> void:
		GUIDED.set_config_value(_graph, key, value)
	, 2)
	if _status != null:
		_status.text = "Simple feature updated · rendered terrain and contact/physics recompile from the same graph."


func _rebuild_guided_kind(key: String, value: String) -> void:
	if _session == null or _graph == null:
		return
	var config: Dictionary = GUIDED.config_from_graph(_graph)
	config[key] = value
	if key == "effect_kind":
		match value:
			GUIDED.EFFECT_HEIGHT:
				config["amount_m"] = 25.0
			GUIDED.EFFECT_NOISE:
				config["amount_m"] = 20.0
				config["scale"] = 8.0
			GUIDED.EFFECT_MOUNTAINS:
				config["amount_m"] = 250.0
				config["scale"] = 3.0
			GUIDED.EFFECT_CHANNELS:
				config["amount_m"] = 40.0
				config["scale"] = 12.0
			GUIDED.EFFECT_DEPOSIT:
				config["amount_m"] = 25.0
				config["scale"] = 10.0
	_session.call("stage_action", "Change simplified terrain feature type", func() -> void:
		GUIDED.rebuild(_graph, config)
	, 2)
	_request_rebuild()


func _apply_feature_mode() -> void:
	if _feature_simple_panel != null:
		_feature_simple_panel.visible = not _feature_show_graph
	for control: Control in _feature_advanced_children:
		if control != null and is_instance_valid(control):
			control.visible = _feature_show_graph
	if _feature_mode_picker != null:
		_feature_mode_picker.select(1 if _feature_show_graph else 0)
	if _feature_show_graph and _graph_edit != null:
		call_deferred("_frame_graph_content")
