extends "res://scripts/world_authoring/terrain_graph_editor.gd"
## Phase 29 graph UX.
##
## The production terrain is no longer represented by the synthetic locked
## `PRODUCTION ... ALWAYS PRESENT` GraphNode from Phase 28. Phase 29 stores the
## production pass-through itself in the graph Resource, so every visible source
## below is an ordinary serialized node that can be disconnected, replaced or
## routed through custom operations.


func _create_production_base_node() -> void:
	# Intentionally empty. The canonical Phase 29 production graph contains real
	# GAME_INPUT nodes for the current renderer output.
	pass


func _connect_production_base() -> void:
	# No hidden/synthetic connections. What the canvas shows is what is serialized.
	pass


func _graph_node_title(node_type: String, node_data: Dictionary) -> String:
	if node_type == "GAME_INPUT":
		var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
		var source: String = String(parameters.get("source", ""))
		match source:
			"terrain_height_m": return "CURRENT TERRAIN HEIGHT"
			"base_albedo": return "CURRENT ALBEDO"
			"base_normal": return "CURRENT NORMAL"
			"base_roughness": return "CURRENT ROUGHNESS"
			"base_metallic": return "CURRENT METALLIC"
			"base_ao": return "CURRENT AO"
			"base_specular": return "CURRENT SPECULAR"
			"biome_id": return "BIOME"
			"slope": return "SLOPE"
			"terrain_height_m": return "CURRENT TERRAIN HEIGHT"
	return super._graph_node_title(node_type, node_data)


func _create_graph_node(node_data: Dictionary) -> void:
	var node_type: String = String(node_data.get("type", "CONSTANT_FLOAT"))
	if node_type != "OUTPUT_MATERIAL":
		super._create_graph_node(node_data)
		return
	var node_id: String = String(node_data.get("id", ""))
	if node_id.is_empty():
		return
	var graph_node := GraphNode.new()
	graph_node.name = node_id
	graph_node.title = "FINAL SURFACE"
	graph_node.position_offset = Vector2(node_data.get("position", Vector2.ZERO))
	graph_node.resizable = false
	graph_node.ignore_invalid_connection_type = true
	_graph_edit.add_child(graph_node)
	for channel: String in ["Albedo", "Normal", "Roughness", "Metallic", "AO", "Specular"]:
		_add_port_row(graph_node, channel, true, false)


func _add_game_input_row(node: GraphNode, node_id: String, node_data: Dictionary) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(240.0, 32.0)
	var picker := OptionButton.new()
	picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
	var current: String = String(parameters.get("source", "terrain_height_m"))
	var domain: int = int(_graph.get(&"domain")) if _graph != null else 0
	var options: Array[String] = GRAPH_SCRIPT.game_input_options(domain)
	if not options.has(current):
		options.push_front(current)
	for input_name: String in options:
		picker.add_item(_friendly_input_name(input_name))
		picker.set_item_metadata(picker.item_count - 1, input_name)
		if input_name == current:
			picker.select(picker.item_count - 1)
	picker.item_selected.connect(func(index: int) -> void:
		_set_node_parameter(node_id, "source", String(picker.get_item_metadata(index)))
	)
	row.add_child(picker)
	node.add_child(row)
	node.set_slot(0, false, 0, Color.WHITE, true, 0, Color(0.93, 0.70, 0.35))


func _friendly_input_name(source: String) -> String:
	match source:
		"terrain_height_m": return "Current terrain height"
		"base_albedo": return "Current albedo"
		"base_normal": return "Current normal"
		"base_roughness": return "Current roughness"
		"base_metallic": return "Current metallic"
		"base_ao": return "Current ambient occlusion"
		"base_specular": return "Current specular"
		"world_position": return "World position"
		"planet_direction": return "Planet direction"
		"surface_normal": return "Surface normal"
		"biome_id": return "Biome"
		"temperature": return "Temperature"
		"precipitation": return "Precipitation"
		"temperature_range": return "Temperature range"
		"moisture": return "Moisture"
		"vegetation_biomass": return "Vegetation biomass"
		"soil_sand": return "Soil sand"
		"soil_silt": return "Soil silt"
		"soil_clay": return "Soil clay"
		"soil_depth_m": return "Soil depth"
		"surface_sediment_m": return "Surface sediment"
		"rock_id": return "Rock type"
		"erodibility": return "Erodibility"
		"strata_dip": return "Strata dip"
		"uplift": return "Uplift"
		"flow_x": return "Water flow X"
		"flow_y": return "Water flow Y"
		"hydrology": return "Hydrology"
		"material_primary": return "Primary material weights"
		"material_secondary": return "Secondary material weights"
		"micro_layer": return "Micro detail layer"
		"camera_distance_m": return "Camera distance"
		"clipmap_level": return "L level"
		"time_s": return "Time"
		_: return _pretty(source)
