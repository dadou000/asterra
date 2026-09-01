extends "res://scripts/world_authoring/terrain_graph_editor_phase30.gd"
## Phase 31 graph UX: the old render-owned terrain stages are ordinary nodes.
##
## The node picker is domain-filtered and category-labelled so raw world data,
## classifier outputs, production PBR, textures and math do not collapse into one
## undifferentiated list. Unlike the legacy graph editor there is deliberately no
## implicit/non-deleteable production block: the visible production nodes are the
## production boundary, and can be deleted, rerouted, scaled or replaced.

const PHASE31_SPECIAL_TYPES: Array[String] = [
	"PRODUCTION_GENERATED_HEIGHT",
	"PRODUCTION_SCULPT_DELTA",
	"PRODUCTION_ALBEDO",
	"PRODUCTION_NORMAL",
	"PRODUCTION_ROUGHNESS",
	"PRODUCTION_METALLIC",
	"PRODUCTION_AO",
	"PRODUCTION_SPECULAR",
	"CLASSIFIER_PRIMARY",
	"CLASSIFIER_SECONDARY",
	"CHANNEL_R",
	"CHANNEL_G",
	"CHANNEL_B",
	"CHANNEL_A",
	"COMBINE_RGB",
	"SATURATE",
	"ONE_MINUS",
]

const PRODUCTION_SHAPE_SLOT_ID_PHASE31 := "production-terrain-shape"
const PRODUCTION_SURFACE_SLOT_ID_PHASE31 := "production-terrain-surface"


func _build_ui() -> void:
	super._build_ui()
	_phase31_rebuild_node_picker()


# Phase 23/29 drew a synthetic renderer-owned source behind authored graphs. That
# representation is exactly what Phase 31 replaces, so suppress both its node and
# its implicit connections. Production data now enters only through serialized,
# removable graph nodes.
func _create_production_base_node() -> void:
	pass


func _connect_production_base() -> void:
	pass


func _phase31_rebuild_node_picker() -> void:
	if _node_type_picker == null or _graph == null:
		return
	_node_type_picker.clear()
	var domain: int = int(_graph.get(&"domain"))
	for category: String in GRAPH_SCRIPT.node_categories(domain):
		for node_type: String in GRAPH_SCRIPT.node_catalog(domain, category):
			if node_type.begins_with("OUTPUT_") or QUICK_OPERATION_TYPES.has(node_type):
				continue
			_node_type_picker.add_item("%s  ·  %s" % [category, _pretty(node_type)])
			_node_type_picker.set_item_metadata(_node_type_picker.item_count - 1, node_type)
	_node_type_picker.tooltip_text = "Nodes are grouped by the production terrain stage they control."


func _graph_node_title(node_type: String, node_data: Dictionary) -> String:
	match node_type:
		"PRODUCTION_GENERATED_HEIGHT": return "GENERATED TERRAIN"
		"PRODUCTION_SCULPT_DELTA": return "SCULPT / EDIT DELTA"
		"PRODUCTION_ALBEDO": return "PRODUCTION ALBEDO"
		"PRODUCTION_NORMAL": return "PRODUCTION NORMAL"
		"PRODUCTION_ROUGHNESS": return "PRODUCTION ROUGHNESS"
		"PRODUCTION_METALLIC": return "PRODUCTION METALLIC"
		"PRODUCTION_AO": return "PRODUCTION AO"
		"PRODUCTION_SPECULAR": return "PRODUCTION SPECULAR"
		"CLASSIFIER_PRIMARY": return "CLASSIFIER · PRIMARY WEIGHTS"
		"CLASSIFIER_SECONDARY": return "CLASSIFIER · SECONDARY WEIGHTS"
		"CHANNEL_R": return "CHANNEL R"
		"CHANNEL_G": return "CHANNEL G"
		"CHANNEL_B": return "CHANNEL B"
		"CHANNEL_A": return "CHANNEL A"
		"COMBINE_RGB": return "COMBINE RGB"
		"SATURATE": return "SATURATE 0…1"
		"ONE_MINUS": return "ONE MINUS"
	return super._graph_node_title(node_type, node_data)


func _create_graph_node(node_data: Dictionary) -> void:
	var node_type: String = String(node_data.get("type", "CONSTANT_FLOAT"))
	if not PHASE31_SPECIAL_TYPES.has(node_type):
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

	match node_type:
		"PRODUCTION_GENERATED_HEIGHT":
			_add_source_row(graph_node, "Generated macro + geomorph height")
		"PRODUCTION_SCULPT_DELTA":
			_add_source_row(graph_node, "Sparse sculpt / edit delta")
		"PRODUCTION_ALBEDO":
			_add_source_row(graph_node, "Rendered classifier + rock/scan PBR albedo")
		"PRODUCTION_NORMAL":
			_add_source_row(graph_node, "Rendered geometry + rock/scan PBR normal")
		"PRODUCTION_ROUGHNESS":
			_add_source_row(graph_node, "Rendered classifier + rock/scan roughness")
		"PRODUCTION_METALLIC":
			_add_source_row(graph_node, "Rendered metallic")
		"PRODUCTION_AO":
			_add_source_row(graph_node, "Rendered ambient occlusion")
		"PRODUCTION_SPECULAR":
			_add_source_row(graph_node, "Rendered rock/PBR specular")
		"CLASSIFIER_PRIMARY":
			_add_source_row(graph_node, "R: rock · G: soil · B: vegetation · A: sand")
		"CLASSIFIER_SECONDARY":
			_add_source_row(graph_node, "R: mud · G: snow · B: scree · A: gravel")
		"CHANNEL_R", "CHANNEL_G", "CHANNEL_B", "CHANNEL_A":
			_add_port_row(graph_node, "Vector  →  Scalar", true, true)
		"COMBINE_RGB":
			_add_port_row(graph_node, "R  →  RGB", true, true)
			_add_port_row(graph_node, "G", true, false)
			_add_port_row(graph_node, "B", true, false)
		"SATURATE", "ONE_MINUS":
			_add_port_row(graph_node, "Value  →  Result", true, true)
	_add_node_action_row(graph_node, node_id)


func _add_source_row(node: GraphNode, text: String) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(245.0, 30.0)
	var label := Label.new()
	label.text = text
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(label)
	node.add_child(row)
	node.set_slot(0, false, 0, Color.WHITE, true, 0, Color(0.93, 0.70, 0.35))


func _add_game_input_row(node: GraphNode, node_id: String, node_data: Dictionary) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(300.0, 32.0)
	var picker := OptionButton.new()
	picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
	var current: String = String(parameters.get("source", "terrain_height_m"))
	var domain: int = int(_graph.get(&"domain")) if _graph != null else 0
	for input_name: String in GRAPH_SCRIPT.game_input_options(domain):
		var category: String = GRAPH_SCRIPT.game_input_category(input_name)
		picker.add_item("%s  ·  %s" % [category, _friendly_input_name(input_name)])
		picker.set_item_metadata(picker.item_count - 1, input_name)
		if input_name == current:
			picker.select(picker.item_count - 1)
	picker.item_selected.connect(func(index: int) -> void:
		_set_node_parameter(node_id, "source", String(picker.get_item_metadata(index)))
	)
	row.add_child(picker)
	node.add_child(row)
	node.set_slot(0, false, 0, Color.WHITE, true, 0, Color(0.93, 0.70, 0.35))


func _on_reset_graph() -> void:
	if _graph == null or _session == null:
		return
	var domain: int = int(_graph.get(&"domain"))
	var slot_id: String = String(_slot.get(&"slot_id")) if _slot != null else ""
	var reset_to_production: bool = slot_id == PRODUCTION_SHAPE_SLOT_ID_PHASE31 \
		or slot_id == PRODUCTION_SURFACE_SLOT_ID_PHASE31
	_session.call("stage_action", "Reset terrain graph", func() -> void:
		if reset_to_production and _graph.has_method("create_production_stage_graph"):
			_graph.call("create_production_stage_graph", domain)
		else:
			_graph.call("create_default_graph", domain)
	, 2)
	_request_rebuild()


func _friendly_input_name(source: String) -> String:
	match source:
		"generated_height_m": return "Generated terrain height"
		"sculpt_delta_m": return "Sculpt / edit delta"
		"base_albedo": return "Production albedo"
		"base_normal": return "Production normal"
		"base_roughness": return "Production roughness"
		"base_metallic": return "Production metallic"
		"base_ao": return "Production AO"
		"base_specular": return "Production specular"
		"soil": return "Soil vector · sand/silt/clay/organic"
		"surface": return "Surface vector · depth/moisture/biomass/sediment"
		"geology": return "Geology vector · erodibility/strata"
		"structure": return "Structure vector · uplift"
		"climate": return "Climate vector · temperature/precipitation/range"
		"hydrology": return "Hydrology vector · flow/deposition"
		"landform": return "Landform classifier vector"
		"material_primary": return "Primary material weights · rock/soil/vegetation/sand"
		"material_secondary": return "Secondary material weights · mud/snow/scree/gravel"
		"rock_mix": return "Rock material mix"
		"soil_sand": return "Soil sand fraction"
		"soil_silt": return "Soil silt fraction"
		"soil_clay": return "Soil clay fraction"
		"soil_depth_m": return "Soil depth (m)"
		"surface_sediment_m": return "Surface sediment (m)"
		"temperature": return "Temperature (°C)"
		"precipitation": return "Precipitation (mm)"
		"temperature_range": return "Temperature range (°C)"
		"moisture": return "Surface moisture"
		"vegetation_biomass": return "Vegetation biomass"
		"erodibility": return "Rock erodibility"
		"strata_dip": return "Strata dip"
		"uplift": return "Signed uplift"
		"flow_x": return "Hydrology flow X"
		"flow_y": return "Hydrology flow Y"
	return super._friendly_input_name(source)
