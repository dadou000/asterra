extends "res://scripts/world_authoring/terrain_graph_editor_phase30.gd"
## Phase 31 graph UX: the old render-owned terrain stages are ordinary nodes.
##
## The node picker is domain-filtered and category-labelled so raw world data,
## classifier outputs, production PBR, textures and math do not collapse into one
## undifferentiated list.

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


func _build_ui() -> void:
	super._build_ui()
	_phase31_rebuild_node_picker()


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
			_add_source_row(graph_node, "Rendered surface albedo")
		"PRODUCTION_NORMAL":
			_add_source_row(graph_node, "Rendered PBR normal")
		"PRODUCTION_ROUGHNESS":
			_add_source_row(graph_node, "Rendered roughness")
		"PRODUCTION_METALLIC":
			_add_source_row(graph_node, "Rendered metallic")
		"PRODUCTION_AO":
			_add_source_row(graph_node, "Rendered ambient occlusion")
		"PRODUCTION_SPECULAR":
			_add_source_row(graph_node, "Rendered specular")
		"CLASSIFIER_PRIMARY":
			_add_source_row(graph_node, "Rock / soil / vegetation / sand weights")
		"CLASSIFIER_SECONDARY":
			_add_source_row(graph_node, "Mud / snow / sediment / wet weights")
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


func _friendly_input_name(source: String) -> String:
	match source:
		"generated_height_m": return "Generated terrain height"
		"sculpt_delta_m": return "Sculpt / edit delta"
		"soil": return "Soil vector · sand/silt/clay"
		"surface": return "Surface vector · depth/moisture/biomass/sediment"
		"geology": return "Geology vector"
		"structure": return "Structure / uplift vector"
		"climate": return "Climate vector"
		"landform": return "Landform classifier vector"
		"rock_mix": return "Rock material mix"
	return super._friendly_input_name(source)
