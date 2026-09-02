extends "res://scripts/world_authoring/terrain_graph_editor_phase32.gd"
## Phase 33: deeper controls for production microrelief and geology-specific rock PBR.
## These remain settings nodes so the canonical production Surface flow stays a
## zero-bytecode identity graph.

const PHASE33_ADVANCED_TYPES: Array[String] = [
	"PRODUCTION_MICRORELIEF_SETTINGS",
	"PRODUCTION_ROCK_PBR_SETTINGS",
]


func _create_graph_node(node_data: Dictionary) -> void:
	var node_type: String = String(node_data.get("type", ""))
	if not PHASE33_ADVANCED_TYPES.has(node_type):
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

	if node_type == "PRODUCTION_MICRORELIEF_SETTINGS":
		_add_bool_setting(graph_node, node_id, parameters, "Enabled", "enabled", true)
		_add_float_setting(graph_node, node_id, parameters, "Overall strength", "strength", 1.0, 0.0, 4.0, 0.01)
		_add_settings_note(graph_node, "Per-material geometric relief gains. 1.0 is the original renderer response.")
		_add_float_setting(graph_node, node_id, parameters, "Rock relief", "rock_scale", 1.0, 0.0, 4.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Soil relief", "soil_scale", 1.0, 0.0, 4.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Sand relief", "sand_scale", 1.0, 0.0, 4.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Mud relief", "mud_scale", 1.0, 0.0, 4.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Snow relief", "snow_scale", 1.0, 0.0, 4.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Gravel relief", "gravel_scale", 1.0, 0.0, 4.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Scree relief", "scree_scale", 1.0, 0.0, 4.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Base relief noise", "base_noise_scale", 1.0, 0.0, 4.0, 0.01)
	else:
		_add_bool_setting(graph_node, node_id, parameters, "Enabled", "enabled", true)
		_add_float_setting(graph_node, node_id, parameters, "Detail", "detail_strength", 1.0, 0.0, 4.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Normal", "normal_strength", 1.0, 0.0, 4.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Colour", "color_strength", 1.0, 0.0, 4.0, 0.01)
		_add_settings_note(graph_node, "Procedural rock scale. Cell sizes snap to exact divisors of the 4096 m detail wrap.")
		_add_float_setting(graph_node, node_id, parameters, "Coarse cell (m)", "coarse_cell_m", 4.0, 1.0, 512.0, 1.0)
		_add_float_setting(graph_node, node_id, parameters, "Fine cell (m)", "fine_cell_m", 1.0, 1.0, 512.0, 1.0)
		_add_float_setting(graph_node, node_id, parameters, "Coarse seed", "coarse_seed", 11665.0, 1.0, 2147483000.0, 1.0)
		_add_float_setting(graph_node, node_id, parameters, "Fine seed", "fine_seed", 35415.0, 1.0, 2147483000.0, 1.0)
		_add_settings_note(graph_node, "Distance / LOD response for rock colour, fine structure and normals.")
		_add_float_setting(graph_node, node_id, parameters, "Coarse fade near (m)", "coarse_fade_near_m", 850.0, 0.0, 20000.0, 1.0)
		_add_float_setting(graph_node, node_id, parameters, "Coarse fade far (m)", "coarse_fade_far_m", 3400.0, 0.0, 50000.0, 1.0)
		_add_float_setting(graph_node, node_id, parameters, "Fine fade near (m)", "fine_fade_near_m", 130.0, 0.0, 10000.0, 1.0)
		_add_float_setting(graph_node, node_id, parameters, "Fine fade far (m)", "fine_fade_far_m", 920.0, 0.0, 20000.0, 1.0)
		_add_float_setting(graph_node, node_id, parameters, "Normal fade near (m)", "normal_fade_near_m", 90.0, 0.0, 10000.0, 1.0)
		_add_float_setting(graph_node, node_id, parameters, "Normal fade far (m)", "normal_fade_far_m", 620.0, 0.0, 20000.0, 1.0)
		_add_settings_note(graph_node, "Weathering, fracture and final roughness response.")
		_add_float_setting(graph_node, node_id, parameters, "Weatherability pivot", "weatherability_pivot", 0.45, 0.0, 1.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Weather roughness", "weatherability_roughness", 0.055, -0.5, 0.5, 0.001)
		_add_float_setting(graph_node, node_id, parameters, "Family roughness min", "family_roughness_min", 0.48, 0.0, 1.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Family roughness max", "family_roughness_max", 0.97, 0.0, 1.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Fracture darkening", "fracture_color_base", 0.09, 0.0, 1.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Fault fracture darkening", "fracture_color_fault", 0.15, 0.0, 1.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Fracture roughness", "fracture_roughness", 0.040, 0.0, 0.5, 0.001)
		_add_float_setting(graph_node, node_id, parameters, "Normal fracture mix", "normal_fracture_mix", 0.55, 0.0, 2.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Output roughness min", "output_roughness_min", 0.45, 0.0, 1.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters, "Output roughness max", "output_roughness_max", 0.98, 0.0, 1.0, 0.01)

	_add_node_action_row(graph_node, node_id)
