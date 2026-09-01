extends "res://scripts/world_authoring/terrain_graph_editor_phase31.gd"
## Phase 32 graph UX for the renderer stages that were still locked inside shader
## includes. These are serialized settings nodes, not a second rendering system.

const PHASE32_CONTROL_TYPES: Array[String] = [
	"PRODUCTION_GEOMORPH_SETTINGS",
	"PRODUCTION_CLASSIFIER_SETTINGS",
	"PRODUCTION_MICRORELIEF_SETTINGS",
	"PRODUCTION_ANTITILE_SETTINGS",
	"PRODUCTION_ROCK_PBR_SETTINGS",
	"PRODUCTION_SCAN_PBR_SETTINGS",
	"PRODUCTION_SCAN_TEXTURES",
]


func _graph_node_title(node_type: String, node_data: Dictionary) -> String:
	match node_type:
		"PRODUCTION_GEOMORPH_SETTINGS": return "PRODUCTION · GEOMORPH"
		"PRODUCTION_CLASSIFIER_SETTINGS": return "PRODUCTION · SURFACE CLASSIFIER"
		"PRODUCTION_MICRORELIEF_SETTINGS": return "PRODUCTION · MICRORELIEF"
		"PRODUCTION_ANTITILE_SETTINGS": return "PRODUCTION · ANTI-TILING"
		"PRODUCTION_ROCK_PBR_SETTINGS": return "PRODUCTION · ROCK PBR"
		"PRODUCTION_SCAN_PBR_SETTINGS": return "PRODUCTION · SCANNED PBR"
		"PRODUCTION_SCAN_TEXTURES": return "PRODUCTION · SCAN TEXTURES"
	return super._graph_node_title(node_type, node_data)


func _create_graph_node(node_data: Dictionary) -> void:
	var node_type: String = String(node_data.get("type", ""))
	if not PHASE32_CONTROL_TYPES.has(node_type):
		super._create_graph_node(node_data)
		return
	var node_id: String = String(node_data.get("id", ""))
	if node_id.is_empty():
		return
	var graph_node := GraphNode.new()
	graph_node.name = node_id
	graph_node.title = _graph_node_title(node_type, node_data)
	graph_node.position_offset = Vector2(node_data.get("position", Vector2.ZERO))
	graph_node.resizable = false
	graph_node.ignore_invalid_connection_type = true
	_graph_edit.add_child(graph_node)
	var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
	_add_settings_note(graph_node, _settings_note(node_type))
	match node_type:
		"PRODUCTION_GEOMORPH_SETTINGS":
			_add_float_setting(graph_node, node_id, parameters, "Overall detail", "detail_strength", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Domain warp", "warp_strength", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "16 km broad", "broad_strength", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "6 km mountains", "mountain_strength", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "1.4 km relief", "mid_strength", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "420 m channels", "channel_strength", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Deposition", "deposit_strength", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "120/24 m detail", "fine_strength", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Dunes", "dune_strength", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Glacial shaping", "glacial_strength", 1.0, 0.0, 4.0, 0.01)
			_add_bool_setting(graph_node, node_id, parameters, "Override detail seed", "override_seed", false)
			_add_float_setting(graph_node, node_id, parameters, "Detail seed", "detail_seed", 1337.0, 1.0, 2147483000.0, 1.0)
		"PRODUCTION_CLASSIFIER_SETTINGS":
			_add_float_setting(graph_node, node_id, parameters, "Rock weight", "rock_scale", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Soil weight", "soil_scale", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Vegetation weight", "vegetation_scale", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Sand weight", "sand_scale", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Mud weight", "mud_scale", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Snow weight", "snow_scale", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Scree weight", "scree_scale", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Gravel weight", "gravel_scale", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Albedo chroma", "albedo_chroma", 1.24, 0.0, 3.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Albedo contrast", "albedo_contrast", 1.10, 0.0, 3.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Albedo pivot", "albedo_pivot", 0.115, 0.0, 1.0, 0.001)
			_add_float_setting(graph_node, node_id, parameters, "Roughness scale", "roughness_scale", 1.0, 0.0, 3.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Roughness bias", "roughness_bias", 0.0, -1.0, 1.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Roughness min", "roughness_min", 0.42, 0.0, 1.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Roughness max", "roughness_max", 0.98, 0.0, 1.0, 0.01)
		"PRODUCTION_MICRORELIEF_SETTINGS":
			_add_bool_setting(graph_node, node_id, parameters, "Enabled", "enabled", true)
			_add_float_setting(graph_node, node_id, parameters, "Strength", "strength", 1.0, 0.0, 4.0, 0.01)
		"PRODUCTION_ANTITILE_SETTINGS":
			_add_float_setting(graph_node, node_id, parameters, "Strength", "strength", 1.0, 0.0, 4.0, 0.01)
		"PRODUCTION_ROCK_PBR_SETTINGS":
			_add_bool_setting(graph_node, node_id, parameters, "Enabled", "enabled", true)
			_add_float_setting(graph_node, node_id, parameters, "Detail", "detail_strength", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Normal", "normal_strength", 1.0, 0.0, 4.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Colour", "color_strength", 1.0, 0.0, 4.0, 0.01)
		"PRODUCTION_SCAN_PBR_SETTINGS":
			_add_bool_setting(graph_node, node_id, parameters, "Enabled", "enabled", true)
			_add_float_setting(graph_node, node_id, parameters, "Ground tile (m)", "ground_metres", 2.0, 0.02, 1024.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Grass tile (m)", "grass_metres", 2.0, 0.02, 1024.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Mud tile (m)", "mud_metres", 1.0, 0.02, 1024.0, 0.01)
			_add_float_setting(graph_node, node_id, parameters, "Forest tile (m)", "forest_metres", 2.0, 0.02, 1024.0, 0.01)
		"PRODUCTION_SCAN_TEXTURES":
			for key: String in [
				"ground_albedo", "ground_normal", "ground_roughness",
				"grass_albedo", "grass_normal", "grass_roughness",
				"mud_albedo", "mud_normal", "mud_roughness",
				"forest_albedo", "forest_normal", "forest_roughness",
			]:
				_add_path_setting(graph_node, node_id, parameters, _pretty(key), key)
	_add_node_action_row(graph_node, node_id)


func _settings_note(node: String) -> String:
	match node:
		"PRODUCTION_GEOMORPH_SETTINGS": return "Controls the same generated detail bands used by the rendered/contact terrain."
		"PRODUCTION_CLASSIFIER_SETTINGS": return "Scales the physical rock/soil/vegetation/sand + mud/snow/scree/gravel classifier."
		"PRODUCTION_MICRORELIEF_SETTINGS": return "Controls the existing sub-metre material microrelief stage."
		"PRODUCTION_ANTITILE_SETTINGS": return "Controls the production stochastic anti-tiling layer."
		"PRODUCTION_ROCK_PBR_SETTINGS": return "Controls geology-specific procedural rock response."
		"PRODUCTION_SCAN_PBR_SETTINGS": return "Controls scanned triplanar PBR and its physical texture scale."
		"PRODUCTION_SCAN_TEXTURES": return "The exact production ground/grass/mud/forest texture sets."
	return "Production renderer setting."


func _add_settings_note(node: GraphNode, text: String) -> void:
	var label := Label.new()
	label.text = text
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.custom_minimum_size = Vector2(315.0, 42.0)
	label.modulate = Color(0.58, 0.69, 0.78)
	node.add_child(label)


func _add_float_setting(node: GraphNode, node_id: String, parameters: Dictionary,
		label_text: String, key: String, default_value: float,
		minimum: float, maximum: float, step: float) -> void:
	_add_operation_parameter(node, node_id, label_text, key,
		float(parameters.get(key, default_value)), minimum, maximum, step)


func _add_bool_setting(node: GraphNode, node_id: String, parameters: Dictionary,
		label_text: String, key: String, default_value: bool) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(315.0, 30.0)
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 150.0
	row.add_child(label)
	var check := CheckBox.new()
	check.button_pressed = bool(parameters.get(key, default_value))
	check.toggled.connect(func(value: bool) -> void:
		_set_node_parameter(node_id, key, value)
	)
	row.add_child(check)
	node.add_child(row)


func _add_path_setting(node: GraphNode, node_id: String, parameters: Dictionary,
		label_text: String, key: String) -> void:
	var row := VBoxContainer.new()
	row.custom_minimum_size = Vector2(390.0, 50.0)
	var label := Label.new()
	label.text = label_text
	row.add_child(label)
	var edit := LineEdit.new()
	edit.text = String(parameters.get(key, ""))
	edit.placeholder_text = "res://..."
	edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	edit.text_submitted.connect(func(value: String) -> void:
		_set_node_parameter(node_id, key, value.strip_edges())
	)
	edit.focus_exited.connect(func() -> void:
		_set_node_parameter(node_id, key, edit.text.strip_edges())
	)
	row.add_child(edit)
	node.add_child(row)
