extends "res://scripts/world_authoring/terrain_graph_editor_phase29.gd"
## Phase 30 presentation for the decomposed production Shape flow.


func _graph_node_title(node_type: String, node_data: Dictionary) -> String:
	if node_type == "GAME_INPUT":
		var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
		var source: String = String(parameters.get("source", ""))
		if source == "generated_height_m":
			return "GENERATED TERRAIN"
		if source == "sculpt_delta_m":
			return "SCULPT / EDIT DELTA"
	return super._graph_node_title(node_type, node_data)


func _friendly_input_name(source: String) -> String:
	if source == "generated_height_m":
		return "Generated terrain height"
	if source == "sculpt_delta_m":
		return "Sculpt / edit delta"
	return super._friendly_input_name(source)
