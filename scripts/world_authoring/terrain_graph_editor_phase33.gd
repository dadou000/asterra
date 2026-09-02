extends "res://scripts/world_authoring/terrain_graph_editor_phase32.gd"
## Phase 33/34: deep production terrain controls. These remain settings nodes so
## the canonical production Surface flow stays a zero-bytecode identity graph.

const PHASE33_ADVANCED_TYPES: Array[String] = [
	"PRODUCTION_CLASSIFIER_THRESHOLDS",
	"PRODUCTION_SURFACE_PALETTE",
	"PRODUCTION_MICRORELIEF_SETTINGS",
	"PRODUCTION_ROCK_PBR_SETTINGS",
]


func _graph_node_title(node_type: String, node_data: Dictionary) -> String:
	match node_type:
		"PRODUCTION_CLASSIFIER_THRESHOLDS": return "PRODUCTION · CLASSIFIER THRESHOLDS"
		"PRODUCTION_SURFACE_PALETTE": return "PRODUCTION · PALETTE / MATERIALS"
	return super._graph_node_title(node_type, node_data)


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

	match node_type:
		"PRODUCTION_CLASSIFIER_THRESHOLDS":
			_build_classifier_thresholds(graph_node, node_id, parameters)
		"PRODUCTION_SURFACE_PALETTE":
			_build_surface_palette(graph_node, node_id, parameters)
		"PRODUCTION_MICRORELIEF_SETTINGS":
			_build_microrelief(graph_node, node_id, parameters)
		"PRODUCTION_ROCK_PBR_SETTINGS":
			_build_rock_pbr(graph_node, node_id, parameters)
	_add_node_action_row(graph_node, node_id)


func _build_classifier_thresholds(node: GraphNode, node_id: String, p: Dictionary) -> void:
	_add_settings_note(node, "Physical slope, soil-depth and climate thresholds used by the existing rock / soil / vegetation / sand / mud / snow / scree / gravel classifier.")
	_add_settings_note(node, "PRIMARY · stability / exposed rock")
	_add_float_setting(node, node_id, p, "Soil repose dry (deg)", "soil_repose_dry_deg", 42.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Soil repose wet (deg)", "soil_repose_wet_deg", 27.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Loose margin below (deg)", "loose_margin_low_deg", 4.0, 0.0, 30.0, 0.1)
	_add_float_setting(node, node_id, p, "Loose margin above (deg)", "loose_margin_high_deg", 5.0, 0.0, 30.0, 0.1)
	_add_float_setting(node, node_id, p, "Sand slope start (deg)", "sand_slope_start_deg", 29.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Sand slope end (deg)", "sand_slope_end_deg", 36.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Vegetation slope start (deg)", "vegetation_slope_start_deg", 32.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Vegetation slope end (deg)", "vegetation_slope_end_deg", 46.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Thin soil start (m)", "thin_soil_start_m", 0.06, 0.0, 20.0, 0.005)
	_add_float_setting(node, node_id, p, "Thin soil end (m)", "thin_soil_end_m", 0.55, 0.0, 20.0, 0.005)
	_add_float_setting(node, node_id, p, "Forced rock slope start", "rock_slope_start_deg", 43.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Forced rock slope end", "rock_slope_end_deg", 58.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Mountain rock slope start", "mountain_rock_slope_start_deg", 48.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Mountain rock slope end", "mountain_rock_slope_end_deg", 62.0, 0.0, 89.0, 0.1)

	_add_settings_note(node, "PRIMARY · climate / soil")
	_add_float_setting(node, node_id, p, "Arid bare start", "arid_bare_start", 0.58, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, p, "Arid bare end", "arid_bare_end", 0.94, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, p, "Bare precip start (mm)", "bare_precip_start_mm", 220.0, 0.0, 10000.0, 1.0)
	_add_float_setting(node, node_id, p, "Bare precip end (mm)", "bare_precip_end_mm", 650.0, 0.0, 10000.0, 1.0)
	_add_float_setting(node, node_id, p, "Bare temp start (C)", "bare_temp_start_c", -14.0, -100.0, 100.0, 0.1)
	_add_float_setting(node, node_id, p, "Bare temp end (C)", "bare_temp_end_c", -3.0, -100.0, 100.0, 0.1)
	_add_float_setting(node, node_id, p, "Dune arid start", "dune_arid_start", 0.55, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, p, "Dune arid end", "dune_arid_end", 0.92, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, p, "Dune precip start (mm)", "dune_precip_start_mm", 300.0, 0.0, 10000.0, 1.0)
	_add_float_setting(node, node_id, p, "Dune precip end (mm)", "dune_precip_end_mm", 850.0, 0.0, 10000.0, 1.0)
	_add_float_setting(node, node_id, p, "Growth temp start (C)", "thermal_growth_start_c", -7.0, -100.0, 100.0, 0.1)
	_add_float_setting(node, node_id, p, "Growth temp end (C)", "thermal_growth_end_c", 4.0, -100.0, 100.0, 0.1)
	_add_float_setting(node, node_id, p, "Vegetation depth start (m)", "vegetation_depth_start_m", 0.04, 0.0, 20.0, 0.005)
	_add_float_setting(node, node_id, p, "Vegetation depth end (m)", "vegetation_depth_end_m", 0.28, 0.0, 20.0, 0.005)
	_add_float_setting(node, node_id, p, "Mineral soil depth start", "soil_depth_start_m", 0.025, 0.0, 20.0, 0.005)
	_add_float_setting(node, node_id, p, "Mineral soil depth end", "soil_depth_end_m", 0.22, 0.0, 20.0, 0.005)

	_add_settings_note(node, "SECONDARY · mud / saturation")
	_add_float_setting(node, node_id, p, "Mud slope start (deg)", "mud_slope_start_deg", 12.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Mud slope end (deg)", "mud_slope_end_deg", 28.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Saturated wet start", "saturated_wet_start", 0.68, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, p, "Saturated wet end", "saturated_wet_end", 0.92, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, p, "Saturated precip start", "saturated_precip_start_mm", 350.0, 0.0, 10000.0, 1.0)
	_add_float_setting(node, node_id, p, "Saturated precip end", "saturated_precip_end_mm", 900.0, 0.0, 10000.0, 1.0)

	_add_settings_note(node, "SECONDARY · snow")
	_add_float_setting(node, node_id, p, "Snow temp start (C)", "snow_temp_start_c", -8.0, -100.0, 100.0, 0.1)
	_add_float_setting(node, node_id, p, "Snow temp end (C)", "snow_temp_end_c", 2.0, -100.0, 100.0, 0.1)
	_add_float_setting(node, node_id, p, "Snow slope start (deg)", "snow_slope_start_deg", 28.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Snow slope end (deg)", "snow_slope_end_deg", 50.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Marginal snow temp start", "marginal_snow_temp_start_c", -2.0, -100.0, 100.0, 0.1)
	_add_float_setting(node, node_id, p, "Marginal snow temp end", "marginal_snow_temp_end_c", 5.0, -100.0, 100.0, 0.1)
	_add_float_setting(node, node_id, p, "Snow precip start (mm)", "snow_precip_start_mm", 500.0, 0.0, 10000.0, 1.0)
	_add_float_setting(node, node_id, p, "Snow precip end (mm)", "snow_precip_end_mm", 1400.0, 0.0, 10000.0, 1.0)

	_add_settings_note(node, "SECONDARY · scree / gravel")
	_add_float_setting(node, node_id, p, "Scree slope start", "scree_slope_start_deg", 25.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Scree full slope", "scree_slope_full_deg", 34.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Scree fade slope", "scree_slope_fade_deg", 45.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Scree end slope", "scree_slope_end_deg", 55.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Gravel slope start", "gravel_slope_start_deg", 18.0, 0.0, 89.0, 0.1)
	_add_float_setting(node, node_id, p, "Gravel slope end", "gravel_slope_end_deg", 34.0, 0.0, 89.0, 0.1)


func _build_surface_palette(node: GraphNode, node_id: String, p: Dictionary) -> void:
	_add_settings_note(node, "Exact colours and material roughness targets used by the current production terrain. Edit these instead of changing shader source.")
	_add_settings_note(node, "ROCK FAMILIES")
	_add_color_setting(node, node_id, p, "Granite", "rock_granite", Color(0.335, 0.305, 0.275))
	_add_color_setting(node, node_id, p, "Basalt", "rock_basalt", Color(0.085, 0.082, 0.080))
	_add_color_setting(node, node_id, p, "Gabbro", "rock_gabbro", Color(0.105, 0.104, 0.098))
	_add_color_setting(node, node_id, p, "Gneiss", "rock_gneiss", Color(0.250, 0.240, 0.232))
	_add_color_setting(node, node_id, p, "Schist", "rock_schist", Color(0.175, 0.170, 0.158))
	_add_color_setting(node, node_id, p, "Sandstone", "rock_sandstone", Color(0.365, 0.230, 0.140))
	_add_color_setting(node, node_id, p, "Shale", "rock_shale", Color(0.155, 0.146, 0.132))
	_add_color_setting(node, node_id, p, "Limestone", "rock_limestone", Color(0.415, 0.400, 0.352))
	_add_color_setting(node, node_id, p, "Dolomite", "rock_dolomite", Color(0.375, 0.358, 0.318))
	_add_color_setting(node, node_id, p, "Conglomerate", "rock_conglomerate", Color(0.265, 0.222, 0.180))
	_add_color_setting(node, node_id, p, "Quartzite", "rock_quartzite", Color(0.455, 0.442, 0.422))
	_add_color_setting(node, node_id, p, "Rhyolitic tuff", "rock_tuff", Color(0.330, 0.298, 0.260))
	_add_color_setting(node, node_id, p, "Serpentinite", "rock_serpentinite", Color(0.120, 0.145, 0.118))

	_add_settings_note(node, "SOIL / VEGETATION / LOOSE MATERIAL")
	_add_color_setting(node, node_id, p, "Sandy soil", "soil_sandy", Color(0.385, 0.225, 0.075))
	_add_color_setting(node, node_id, p, "Silty soil", "soil_silty", Color(0.220, 0.120, 0.040))
	_add_color_setting(node, node_id, p, "Clay soil", "soil_clayey", Color(0.315, 0.075, 0.028))
	_add_color_setting(node, node_id, p, "Humus", "soil_humus", Color(0.028, 0.017, 0.006))
	_add_color_setting(node, node_id, p, "Dry vegetation", "vegetation_dry", Color(0.285, 0.175, 0.030))
	_add_color_setting(node, node_id, p, "Grass", "vegetation_grass", Color(0.185, 0.255, 0.042))
	_add_color_setting(node, node_id, p, "Wet vegetation", "vegetation_wet", Color(0.030, 0.155, 0.030))
	_add_color_setting(node, node_id, p, "Lush forest", "vegetation_lush", Color(0.010, 0.075, 0.016))
	_add_color_setting(node, node_id, p, "Cold vegetation", "vegetation_cold", Color(0.095, 0.105, 0.045))
	_add_color_setting(node, node_id, p, "Sand low", "sand_low", Color(0.485, 0.285, 0.095))
	_add_color_setting(node, node_id, p, "Sand high", "sand_high", Color(0.660, 0.445, 0.155))
	_add_color_setting(node, node_id, p, "Mud tint", "mud_tint", Color(0.42, 0.36, 0.30))
	_add_color_setting(node, node_id, p, "Snow", "snow", Color(0.84, 0.89, 0.94))
	_add_color_setting(node, node_id, p, "Gravel", "gravel", Color(0.205, 0.185, 0.155))
	_add_float_setting(node, node_id, p, "Scree rock mix", "scree_rock_mix", 0.80, 0.0, 2.0, 0.01)
	_add_float_setting(node, node_id, p, "Scree brightness add", "scree_add", 0.026, -1.0, 1.0, 0.001)
	_add_float_setting(node, node_id, p, "Gravel rock mix", "gravel_rock_mix", 0.44, 0.0, 1.0, 0.01)

	_add_settings_note(node, "ROUGHNESS TARGETS")
	_add_float_setting(node, node_id, p, "Fallback", "roughness_fallback", 0.90, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, p, "Rock", "roughness_rock", 0.76, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, p, "Soil", "roughness_soil", 0.92, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, p, "Vegetation", "roughness_vegetation", 0.96, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, p, "Sand", "roughness_sand", 0.82, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, p, "Mud dry", "roughness_mud_dry", 0.72, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, p, "Mud wet", "roughness_mud_wet", 0.48, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, p, "Snow", "roughness_snow", 0.92, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, p, "Scree", "roughness_scree", 0.86, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, p, "Gravel", "roughness_gravel", 0.80, 0.0, 1.0, 0.01)


func _build_microrelief(node: GraphNode, node_id: String, parameters: Dictionary) -> void:
	_add_settings_note(node, "Controls the existing sub-metre material microrelief stage.")
	_add_bool_setting(node, node_id, parameters, "Enabled", "enabled", true)
	_add_float_setting(node, node_id, parameters, "Overall strength", "strength", 1.0, 0.0, 4.0, 0.01)
	_add_settings_note(node, "Per-material geometric relief gains. 1.0 is the original renderer response.")
	_add_float_setting(node, node_id, parameters, "Rock relief", "rock_scale", 1.0, 0.0, 4.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Soil relief", "soil_scale", 1.0, 0.0, 4.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Sand relief", "sand_scale", 1.0, 0.0, 4.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Mud relief", "mud_scale", 1.0, 0.0, 4.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Snow relief", "snow_scale", 1.0, 0.0, 4.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Gravel relief", "gravel_scale", 1.0, 0.0, 4.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Scree relief", "scree_scale", 1.0, 0.0, 4.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Base relief noise", "base_noise_scale", 1.0, 0.0, 4.0, 0.01)


func _build_rock_pbr(node: GraphNode, node_id: String, parameters: Dictionary) -> void:
	_add_settings_note(node, "Controls geology-specific procedural rock response.")
	_add_bool_setting(node, node_id, parameters, "Enabled", "enabled", true)
	_add_float_setting(node, node_id, parameters, "Detail", "detail_strength", 1.0, 0.0, 4.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Normal", "normal_strength", 1.0, 0.0, 4.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Colour", "color_strength", 1.0, 0.0, 4.0, 0.01)
	_add_settings_note(node, "Procedural rock scale. Cell sizes snap to exact divisors of the 4096 m detail wrap.")
	_add_float_setting(node, node_id, parameters, "Coarse cell (m)", "coarse_cell_m", 4.0, 1.0, 512.0, 1.0)
	_add_float_setting(node, node_id, parameters, "Fine cell (m)", "fine_cell_m", 1.0, 1.0, 512.0, 1.0)
	_add_float_setting(node, node_id, parameters, "Coarse seed", "coarse_seed", 11665.0, 1.0, 2147483000.0, 1.0)
	_add_float_setting(node, node_id, parameters, "Fine seed", "fine_seed", 35415.0, 1.0, 2147483000.0, 1.0)
	_add_settings_note(node, "Distance / LOD response for rock colour, fine structure and normals.")
	_add_float_setting(node, node_id, parameters, "Coarse fade near (m)", "coarse_fade_near_m", 850.0, 0.0, 20000.0, 1.0)
	_add_float_setting(node, node_id, parameters, "Coarse fade far (m)", "coarse_fade_far_m", 3400.0, 0.0, 50000.0, 1.0)
	_add_float_setting(node, node_id, parameters, "Fine fade near (m)", "fine_fade_near_m", 130.0, 0.0, 10000.0, 1.0)
	_add_float_setting(node, node_id, parameters, "Fine fade far (m)", "fine_fade_far_m", 920.0, 0.0, 20000.0, 1.0)
	_add_float_setting(node, node_id, parameters, "Normal fade near (m)", "normal_fade_near_m", 90.0, 0.0, 10000.0, 1.0)
	_add_float_setting(node, node_id, parameters, "Normal fade far (m)", "normal_fade_far_m", 620.0, 0.0, 20000.0, 1.0)
	_add_settings_note(node, "Weathering, fracture and final roughness response.")
	_add_float_setting(node, node_id, parameters, "Weatherability pivot", "weatherability_pivot", 0.45, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Weather roughness", "weatherability_roughness", 0.055, -0.5, 0.5, 0.001)
	_add_float_setting(node, node_id, parameters, "Family roughness min", "family_roughness_min", 0.48, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Family roughness max", "family_roughness_max", 0.97, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Fracture darkening", "fracture_color_base", 0.09, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Fault fracture darkening", "fracture_color_fault", 0.15, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Fracture roughness", "fracture_roughness", 0.040, 0.0, 0.5, 0.001)
	_add_float_setting(node, node_id, parameters, "Normal fracture mix", "normal_fracture_mix", 0.55, 0.0, 2.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Output roughness min", "output_roughness_min", 0.45, 0.0, 1.0, 0.01)
	_add_float_setting(node, node_id, parameters, "Output roughness max", "output_roughness_max", 0.98, 0.0, 1.0, 0.01)


func _add_color_setting(node: GraphNode, node_id: String, parameters: Dictionary,
		label_text: String, key: String, default_value: Color) -> void:
	var row := HBoxContainer.new()
	row.custom_minimum_size = Vector2(315.0, 32.0)
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 178.0
	row.add_child(label)
	var picker := ColorPickerButton.new()
	var stored: Variant = parameters.get(key, default_value)
	picker.color = stored as Color if stored is Color else default_value
	picker.edit_alpha = false
	picker.custom_minimum_size = Vector2(120.0, 28.0)
	picker.color_changed.connect(func(value: Color) -> void:
		_set_node_parameter(node_id, key, value)
	)
	row.add_child(picker)
	node.add_child(row)
