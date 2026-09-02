class_name TerrainBeginnerSurfaceCatalog
extends RefCounted
## Human-facing metadata for the production terrain Surface graph.
## Every entry maps directly to an existing production control node/parameter.

const MODE_SIMPLE := 0
const MODE_DETAILED := 1
const MODE_NODE_GRAPH := 2


static func mode_name(mode: int) -> String:
	match mode:
		MODE_SIMPLE: return "Simple"
		MODE_DETAILED: return "Detailed"
		MODE_NODE_GRAPH: return "Node Graph"
	return "Simple"


static func controls_for_mode(mode: int) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	for item: Dictionary in _controls():
		if mode >= int(item.get("minimum_mode", MODE_SIMPLE)) and mode <= MODE_DETAILED:
			out.append(item.duplicate(true))
	return out


static func _number(node_type: String, key: String, title: String, description: String,
		category: String, default_value: float, minimum: float, maximum: float,
		step: float, unit: String = "", minimum_mode: int = MODE_SIMPLE) -> Dictionary:
	return {
		"kind":"number", "node_type":node_type, "key":key, "title":title,
		"description":description, "category":category, "default":default_value,
		"min":minimum, "max":maximum, "step":step, "unit":unit,
		"minimum_mode":minimum_mode,
	}


static func _toggle(node_type: String, key: String, title: String, description: String,
		category: String, default_value: bool, minimum_mode: int = MODE_SIMPLE) -> Dictionary:
	return {
		"kind":"toggle", "node_type":node_type, "key":key, "title":title,
		"description":description, "category":category, "default":default_value,
		"minimum_mode":minimum_mode,
	}


static func _color(node_type: String, key: String, title: String, description: String,
		category: String, default_value: Color, minimum_mode: int = MODE_SIMPLE) -> Dictionary:
	return {
		"kind":"color", "node_type":node_type, "key":key, "title":title,
		"description":description, "category":category, "default":default_value,
		"minimum_mode":minimum_mode,
	}


static func _controls() -> Array[Dictionary]:
	return [
		_number("PRODUCTION_CLASSIFIER_SETTINGS", "rock_scale", "Rock amount",
			"Scales exposed rock in the existing terrain classifier.", "Material Balance", 1.0, 0.0, 3.0, 0.01),
		_number("PRODUCTION_CLASSIFIER_SETTINGS", "soil_scale", "Soil amount",
			"Scales mineral soil coverage without changing terrain geometry.", "Material Balance", 1.0, 0.0, 3.0, 0.01),
		_number("PRODUCTION_CLASSIFIER_SETTINGS", "vegetation_scale", "Vegetation amount",
			"Scales the vegetation surface contribution where climate and slope allow it.", "Material Balance", 1.0, 0.0, 3.0, 0.01),
		_number("PRODUCTION_CLASSIFIER_SETTINGS", "sand_scale", "Sand amount",
			"Scales sand coverage in arid and loose-material regions.", "Material Balance", 1.0, 0.0, 3.0, 0.01),
		_number("PRODUCTION_CLASSIFIER_SETTINGS", "mud_scale", "Mud amount",
			"Scales wet/saturated mud coverage.", "Material Balance", 1.0, 0.0, 3.0, 0.01),
		_number("PRODUCTION_CLASSIFIER_SETTINGS", "snow_scale", "Snow amount",
			"Scales snow coverage after the existing climate and slope rules.", "Material Balance", 1.0, 0.0, 3.0, 0.01),
		_number("PRODUCTION_CLASSIFIER_SETTINGS", "scree_scale", "Scree amount",
			"Scales loose angular rock on suitable slopes.", "Material Balance", 1.0, 0.0, 3.0, 0.01, "", MODE_DETAILED),
		_number("PRODUCTION_CLASSIFIER_SETTINGS", "gravel_scale", "Gravel amount",
			"Scales gravel coverage in the existing classifier.", "Material Balance", 1.0, 0.0, 3.0, 0.01, "", MODE_DETAILED),

		_number("PRODUCTION_CLASSIFIER_SETTINGS", "albedo_chroma", "Color richness",
			"Controls terrain color saturation/chroma after classification.", "Visual Character", 1.24, 0.4, 2.0, 0.01),
		_number("PRODUCTION_CLASSIFIER_SETTINGS", "albedo_contrast", "Color contrast",
			"Controls contrast of the production terrain albedo.", "Visual Character", 1.10, 0.5, 2.0, 0.01),
		_number("PRODUCTION_CLASSIFIER_SETTINGS", "roughness_scale", "Surface roughness",
			"Scales material roughness globally while preserving per-material differences.", "Visual Character", 1.0, 0.25, 2.0, 0.01),
		_number("PRODUCTION_CLASSIFIER_SETTINGS", "roughness_bias", "Roughness bias",
			"Adds or removes roughness after the material classifier.", "Visual Character", 0.0, -0.5, 0.5, 0.01, "", MODE_DETAILED),

		_toggle("PRODUCTION_MICRORELIEF_SETTINGS", "enabled", "Microrelief",
			"Enables the existing material-specific microrelief layer.", "Surface Detail", true),
		_number("PRODUCTION_MICRORELIEF_SETTINGS", "strength", "Microrelief strength",
			"Controls small material relief used by the production surface shader.", "Surface Detail", 1.0, 0.0, 3.0, 0.01),
		_number("PRODUCTION_ANTITILE_SETTINGS", "strength", "Anti-tiling strength",
			"Breaks up visible texture repetition without changing source textures.", "Surface Detail", 1.0, 0.0, 3.0, 0.01),
		_toggle("PRODUCTION_ROCK_PBR_SETTINGS", "enabled", "Procedural rock PBR",
			"Enables the existing geology-aware procedural rock detail pass.", "Surface Detail", true),
		_number("PRODUCTION_ROCK_PBR_SETTINGS", "detail_strength", "Rock detail",
			"Strength of procedural rock fracture/detail structure.", "Surface Detail", 1.0, 0.0, 3.0, 0.01),
		_number("PRODUCTION_ROCK_PBR_SETTINGS", "normal_strength", "Rock normal strength",
			"Strength of procedural rock normal detail.", "Surface Detail", 1.0, 0.0, 3.0, 0.01, "", MODE_DETAILED),
		_number("PRODUCTION_ROCK_PBR_SETTINGS", "color_strength", "Rock color variation",
			"Strength of procedural geology color variation.", "Surface Detail", 1.0, 0.0, 3.0, 0.01, "", MODE_DETAILED),
		_toggle("PRODUCTION_SCAN_PBR_SETTINGS", "enabled", "Scanned ground PBR",
			"Enables the existing scanned ground/grass/mud/forest texture pass.", "Surface Detail", true),
		_number("PRODUCTION_SCAN_PBR_SETTINGS", "transfer_strength", "Scanned texture influence",
			"Controls how strongly scanned PBR textures transfer onto classified surfaces.", "Surface Detail", 0.60, 0.0, 2.0, 0.01),
		_number("PRODUCTION_SCAN_PBR_SETTINGS", "ground_metres", "Ground texture size",
			"World-space size of the ground scan texture projection.", "Surface Detail", 2.0, 0.1, 20.0, 0.1, "m", MODE_DETAILED),
		_number("PRODUCTION_SCAN_PBR_SETTINGS", "grass_metres", "Grass texture size",
			"World-space size of the grass scan texture projection.", "Surface Detail", 2.0, 0.1, 20.0, 0.1, "m", MODE_DETAILED),
		_number("PRODUCTION_SCAN_PBR_SETTINGS", "mud_metres", "Mud texture size",
			"World-space size of the mud scan texture projection.", "Surface Detail", 1.0, 0.1, 20.0, 0.1, "m", MODE_DETAILED),
		_number("PRODUCTION_SCAN_PBR_SETTINGS", "forest_metres", "Forest texture size",
			"World-space size of the forest-floor scan texture projection.", "Surface Detail", 2.0, 0.1, 20.0, 0.1, "m", MODE_DETAILED),

		_color("PRODUCTION_SURFACE_PALETTE", "rock_granite", "Granite",
			"Base granite color used by the production geology palette.", "Key Colors", Color(0.335,0.305,0.275), MODE_DETAILED),
		_color("PRODUCTION_SURFACE_PALETTE", "rock_basalt", "Basalt",
			"Base basalt color used by the production geology palette.", "Key Colors", Color(0.085,0.082,0.080), MODE_DETAILED),
		_color("PRODUCTION_SURFACE_PALETTE", "soil_sandy", "Sandy soil",
			"Base sandy-soil color.", "Key Colors", Color(0.385,0.225,0.075)),
		_color("PRODUCTION_SURFACE_PALETTE", "soil_humus", "Humus",
			"Base dark organic-soil color.", "Key Colors", Color(0.028,0.017,0.006), MODE_DETAILED),
		_color("PRODUCTION_SURFACE_PALETTE", "vegetation_grass", "Grass",
			"Base grass color before lighting and scan texture transfer.", "Key Colors", Color(0.185,0.255,0.042)),
		_color("PRODUCTION_SURFACE_PALETTE", "vegetation_lush", "Lush vegetation",
			"Base lush/forest vegetation color.", "Key Colors", Color(0.010,0.075,0.016)),
		_color("PRODUCTION_SURFACE_PALETTE", "sand_low", "Sand",
			"Lower/darker sand color used by the production gradient.", "Key Colors", Color(0.485,0.285,0.095)),
		_color("PRODUCTION_SURFACE_PALETTE", "mud_tint", "Mud",
			"Mud tint used by wet/saturated surfaces.", "Key Colors", Color(0.42,0.36,0.30)),
		_color("PRODUCTION_SURFACE_PALETTE", "snow", "Snow",
			"Base snow color.", "Key Colors", Color(0.84,0.89,0.94)),

		_number("PRODUCTION_CLASSIFIER_THRESHOLDS", "rock_slope_start_deg", "Rock starts on slope",
			"Slope where forced exposed-rock classification begins.", "Classification Rules", 43.0, 0.0, 89.0, 0.5, "°", MODE_DETAILED),
		_number("PRODUCTION_CLASSIFIER_THRESHOLDS", "rock_slope_end_deg", "Rock fully exposed",
			"Slope where forced exposed-rock classification becomes full strength.", "Classification Rules", 58.0, 0.0, 89.0, 0.5, "°", MODE_DETAILED),
		_number("PRODUCTION_CLASSIFIER_THRESHOLDS", "vegetation_slope_start_deg", "Vegetation slope fade",
			"Slope where vegetation begins fading from steep terrain.", "Classification Rules", 32.0, 0.0, 89.0, 0.5, "°", MODE_DETAILED),
		_number("PRODUCTION_CLASSIFIER_THRESHOLDS", "vegetation_slope_end_deg", "Vegetation slope cutoff",
			"Slope where vegetation is fully removed by the classifier.", "Classification Rules", 46.0, 0.0, 89.0, 0.5, "°", MODE_DETAILED),
		_number("PRODUCTION_CLASSIFIER_THRESHOLDS", "snow_temp_start_c", "Snow temperature start",
			"Temperature where snow classification starts transitioning.", "Classification Rules", -8.0, -40.0, 20.0, 0.5, "°C", MODE_DETAILED),
		_number("PRODUCTION_CLASSIFIER_THRESHOLDS", "snow_temp_end_c", "Snow temperature end",
			"Warmer end of the main snow transition.", "Classification Rules", 2.0, -40.0, 20.0, 0.5, "°C", MODE_DETAILED),
	]
