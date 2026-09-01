class_name TerrainShaderGraphDefinition
extends Resource
## Serializable node graph used by terrain displacement/material authoring slots.
##
## Phase 32 keeps the Phase 31 production boundary nodes and adds serialized
## production-control nodes for the renderer stages that used to be shader-only.
## Their defaults are the exact current production values, so Reset Flow preserves
## the existing terrain while making the implementation discoverable/editable.

enum Domain {
	DISPLACEMENT,
	MATERIAL,
}

enum DisplacementOutputMode {
	DELTA,
	ABSOLUTE_HEIGHT,
}

const CATEGORY_OUTPUT := "Output"
const CATEGORY_PRODUCTION := "Production stages"
const CATEGORY_TERRAIN_SOURCE := "Terrain source"
const CATEGORY_GEOMORPH := "Geomorph"
const CATEGORY_WORLD_DATA := "World data"
const CATEGORY_CLASSIFICATION := "Classification"
const CATEGORY_MICRODETAIL := "Microdetail"
const CATEGORY_SURFACE_PBR := "Surface / PBR"
const CATEGORY_TEXTURES := "Textures"
const CATEGORY_MATH := "Math"
const CATEGORY_UTILITY := "Utility"

const NODE_CATEGORIES: Array[String] = [
	CATEGORY_OUTPUT,
	CATEGORY_PRODUCTION,
	CATEGORY_TERRAIN_SOURCE,
	CATEGORY_GEOMORPH,
	CATEGORY_WORLD_DATA,
	CATEGORY_CLASSIFICATION,
	CATEGORY_MICRODETAIL,
	CATEGORY_SURFACE_PBR,
	CATEGORY_TEXTURES,
	CATEGORY_MATH,
	CATEGORY_UTILITY,
]

const MATERIAL_GAME_INPUTS: Array[String] = [
	"base_albedo",
	"base_normal",
	"base_roughness",
	"base_metallic",
	"base_ao",
	"base_specular",
	"world_position",
	"planet_direction",
	"terrain_height_m",
	"surface_normal",
	"slope",
	"biome_id",
	"temperature",
	"precipitation",
	"temperature_range",
	"moisture",
	"vegetation_biomass",
	"soil_sand",
	"soil_silt",
	"soil_clay",
	"soil_depth_m",
	"surface_sediment_m",
	"rock_id",
	"erodibility",
	"strata_dip",
	"uplift",
	"flow_x",
	"flow_y",
	"hydrology",
	"soil",
	"surface",
	"geology",
	"structure",
	"climate",
	"landform",
	"material_primary",
	"material_secondary",
	"rock_mix",
	"micro_layer",
	"camera_distance_m",
	"clipmap_level",
	"time_s",
]

const DISPLACEMENT_GAME_INPUTS: Array[String] = [
	"generated_height_m",
	"sculpt_delta_m",
	"terrain_height_m",
	"biome_id",
	"clipmap_level",
	"time_s",
]

const GAME_INPUTS: Array[String] = [
	"generated_height_m",
	"sculpt_delta_m",
	"base_albedo",
	"base_normal",
	"base_roughness",
	"base_metallic",
	"base_ao",
	"base_specular",
	"world_position",
	"planet_direction",
	"terrain_height_m",
	"surface_normal",
	"slope",
	"biome_id",
	"temperature",
	"precipitation",
	"temperature_range",
	"moisture",
	"vegetation_biomass",
	"soil_sand",
	"soil_silt",
	"soil_clay",
	"soil_depth_m",
	"surface_sediment_m",
	"rock_id",
	"erodibility",
	"strata_dip",
	"uplift",
	"flow_x",
	"flow_y",
	"hydrology",
	"soil",
	"surface",
	"geology",
	"structure",
	"climate",
	"landform",
	"material_primary",
	"material_secondary",
	"rock_mix",
	"micro_layer",
	"camera_distance_m",
	"clipmap_level",
	"time_s",
]

const DISPLACEMENT_ONLY_NODES: Array[String] = [
	"PRODUCTION_GENERATED_HEIGHT",
	"PRODUCTION_GEOMORPH_SETTINGS",
	"PRODUCTION_SCULPT_DELTA",
	"NOISE_LAYER",
	"RIDGED_MOUNTAINS",
	"EROSION_CHANNELS",
	"SEDIMENT_DEPOSIT",
	"OUTPUT_DISPLACEMENT",
]

const MATERIAL_ONLY_NODES: Array[String] = [
	"PRODUCTION_ALBEDO",
	"PRODUCTION_NORMAL",
	"PRODUCTION_ROUGHNESS",
	"PRODUCTION_METALLIC",
	"PRODUCTION_AO",
	"PRODUCTION_SPECULAR",
	"PRODUCTION_CLASSIFIER_SETTINGS",
	"PRODUCTION_MICRORELIEF_SETTINGS",
	"PRODUCTION_ANTITILE_SETTINGS",
	"PRODUCTION_ROCK_PBR_SETTINGS",
	"PRODUCTION_SCAN_PBR_SETTINGS",
	"PRODUCTION_SCAN_TEXTURES",
	"CLASSIFIER_PRIMARY",
	"CLASSIFIER_SECONDARY",
	"CHANNEL_R",
	"CHANNEL_G",
	"CHANNEL_B",
	"CHANNEL_A",
	"COMBINE_RGB",
	"NORMAL_BLEND",
	"TRIPLANAR",
	"OUTPUT_MATERIAL",
]

const PRODUCTION_CONTROL_NODES: Array[String] = [
	"PRODUCTION_GEOMORPH_SETTINGS",
	"PRODUCTION_CLASSIFIER_SETTINGS",
	"PRODUCTION_MICRORELIEF_SETTINGS",
	"PRODUCTION_ANTITILE_SETTINGS",
	"PRODUCTION_ROCK_PBR_SETTINGS",
	"PRODUCTION_SCAN_PBR_SETTINGS",
	"PRODUCTION_SCAN_TEXTURES",
]

const NODE_TYPES: Array[String] = [
	"PRODUCTION_GENERATED_HEIGHT",
	"PRODUCTION_GEOMORPH_SETTINGS",
	"PRODUCTION_SCULPT_DELTA",
	"PRODUCTION_ALBEDO",
	"PRODUCTION_NORMAL",
	"PRODUCTION_ROUGHNESS",
	"PRODUCTION_METALLIC",
	"PRODUCTION_AO",
	"PRODUCTION_SPECULAR",
	"PRODUCTION_CLASSIFIER_SETTINGS",
	"PRODUCTION_MICRORELIEF_SETTINGS",
	"PRODUCTION_ANTITILE_SETTINGS",
	"PRODUCTION_ROCK_PBR_SETTINGS",
	"PRODUCTION_SCAN_PBR_SETTINGS",
	"PRODUCTION_SCAN_TEXTURES",
	"CLASSIFIER_PRIMARY",
	"CLASSIFIER_SECONDARY",
	"GAME_INPUT",
	"TEXTURE_2D",
	"CONSTANT_FLOAT",
	"CONSTANT_COLOR",
	"ADD",
	"SUBTRACT",
	"MULTIPLY",
	"DIVIDE",
	"MIN",
	"MAX",
	"ABS",
	"POWER",
	"CLAMP",
	"SATURATE",
	"ONE_MINUS",
	"SMOOTHSTEP",
	"REMAP",
	"MIX",
	"CHANNEL_R",
	"CHANNEL_G",
	"CHANNEL_B",
	"CHANNEL_A",
	"COMBINE_RGB",
	"NORMAL_BLEND",
	"NOISE",
	"NOISE_LAYER",
	"RIDGED_MOUNTAINS",
	"EROSION_CHANNELS",
	"SEDIMENT_DEPOSIT",
	"TRIPLANAR",
	"OUTPUT_DISPLACEMENT",
	"OUTPUT_MATERIAL",
]

const NODE_CATEGORY_BY_TYPE: Dictionary = {
	"OUTPUT_DISPLACEMENT": CATEGORY_OUTPUT,
	"OUTPUT_MATERIAL": CATEGORY_OUTPUT,
	"PRODUCTION_GENERATED_HEIGHT": CATEGORY_PRODUCTION,
	"PRODUCTION_SCULPT_DELTA": CATEGORY_PRODUCTION,
	"PRODUCTION_ALBEDO": CATEGORY_PRODUCTION,
	"PRODUCTION_NORMAL": CATEGORY_PRODUCTION,
	"PRODUCTION_ROUGHNESS": CATEGORY_PRODUCTION,
	"PRODUCTION_METALLIC": CATEGORY_PRODUCTION,
	"PRODUCTION_AO": CATEGORY_PRODUCTION,
	"PRODUCTION_SPECULAR": CATEGORY_PRODUCTION,
	"PRODUCTION_GEOMORPH_SETTINGS": CATEGORY_GEOMORPH,
	"PRODUCTION_CLASSIFIER_SETTINGS": CATEGORY_CLASSIFICATION,
	"PRODUCTION_MICRORELIEF_SETTINGS": CATEGORY_MICRODETAIL,
	"PRODUCTION_ANTITILE_SETTINGS": CATEGORY_MICRODETAIL,
	"PRODUCTION_ROCK_PBR_SETTINGS": CATEGORY_SURFACE_PBR,
	"PRODUCTION_SCAN_PBR_SETTINGS": CATEGORY_SURFACE_PBR,
	"PRODUCTION_SCAN_TEXTURES": CATEGORY_TEXTURES,
	"CLASSIFIER_PRIMARY": CATEGORY_CLASSIFICATION,
	"CLASSIFIER_SECONDARY": CATEGORY_CLASSIFICATION,
	"GAME_INPUT": CATEGORY_WORLD_DATA,
	"NOISE": CATEGORY_TERRAIN_SOURCE,
	"NOISE_LAYER": CATEGORY_GEOMORPH,
	"RIDGED_MOUNTAINS": CATEGORY_GEOMORPH,
	"EROSION_CHANNELS": CATEGORY_GEOMORPH,
	"SEDIMENT_DEPOSIT": CATEGORY_GEOMORPH,
	"TEXTURE_2D": CATEGORY_TEXTURES,
	"TRIPLANAR": CATEGORY_TEXTURES,
	"NORMAL_BLEND": CATEGORY_SURFACE_PBR,
	"COMBINE_RGB": CATEGORY_SURFACE_PBR,
	"CHANNEL_R": CATEGORY_UTILITY,
	"CHANNEL_G": CATEGORY_UTILITY,
	"CHANNEL_B": CATEGORY_UTILITY,
	"CHANNEL_A": CATEGORY_UTILITY,
	"ADD": CATEGORY_MATH,
	"SUBTRACT": CATEGORY_MATH,
	"MULTIPLY": CATEGORY_MATH,
	"DIVIDE": CATEGORY_MATH,
	"MIN": CATEGORY_MATH,
	"MAX": CATEGORY_MATH,
	"ABS": CATEGORY_MATH,
	"POWER": CATEGORY_MATH,
	"CLAMP": CATEGORY_MATH,
	"SATURATE": CATEGORY_MATH,
	"ONE_MINUS": CATEGORY_MATH,
	"SMOOTHSTEP": CATEGORY_MATH,
	"REMAP": CATEGORY_MATH,
	"MIX": CATEGORY_MATH,
	"CONSTANT_FLOAT": CATEGORY_UTILITY,
	"CONSTANT_COLOR": CATEGORY_UTILITY,
}

@export var graph_id: String = ""
@export var display_name: String = "Terrain Graph"
@export_enum("Displacement", "Material") var domain: int = Domain.DISPLACEMENT
@export_enum("Additive delta", "Absolute production height") var displacement_output_mode: int = DisplacementOutputMode.DELTA
@export var revision: int = 1
@export var nodes: Array[Dictionary] = []
@export var links: Array[Dictionary] = []

func ensure_valid() -> void:
	if graph_id.is_empty():
		graph_id = make_graph_id(display_name)
	if nodes.is_empty():
		create_default_graph(domain)
	_remove_invalid_links()

func create_default_graph(next_domain: int) -> void:
	domain = next_domain
	if domain == Domain.DISPLACEMENT:
		displacement_output_mode = DisplacementOutputMode.DELTA
	nodes.clear()
	links.clear()
	var output_type := "OUTPUT_DISPLACEMENT" if domain == Domain.DISPLACEMENT else "OUTPUT_MATERIAL"
	add_node(output_type, Vector2(520.0, 180.0), {})
	revision += 1

func create_production_graph(next_domain: int) -> void:
	# Legacy Phase 29 production representation; retained for migrations/tests.
	domain = next_domain
	nodes.clear()
	links.clear()
	if domain == Domain.DISPLACEMENT:
		displacement_output_mode = DisplacementOutputMode.ABSOLUTE_HEIGHT
		var source_id: String = add_node("GAME_INPUT", Vector2(90.0, 180.0),
			{"source":"terrain_height_m"})
		var output_id: String = add_node("OUTPUT_DISPLACEMENT", Vector2(520.0, 180.0), {})
		connect_nodes(source_id, 0, output_id, 0)
	else:
		displacement_output_mode = DisplacementOutputMode.DELTA
		var output_id: String = add_node("OUTPUT_MATERIAL", Vector2(650.0, 210.0), {})
		var sources: Array[String] = [
			"base_albedo", "base_normal", "base_roughness",
			"base_metallic", "base_ao", "base_specular",
		]
		for port: int in sources.size():
			var source_id: String = add_node("GAME_INPUT",
				Vector2(90.0, 55.0 + float(port) * 96.0), {"source":sources[port]})
			connect_nodes(source_id, 0, output_id, port)
	revision += 1

func create_production_stage_graph(next_domain: int) -> void:
	# Phase 32 canonical production graph. Data-flow nodes still represent the exact
	# renderer boundary; unconnected settings nodes configure the production stages
	# that calculate those sources. Defaults below match the shader defaults.
	domain = next_domain
	nodes.clear()
	links.clear()
	if domain == Domain.DISPLACEMENT:
		displacement_output_mode = DisplacementOutputMode.ABSOLUTE_HEIGHT
		var generated_id: String = add_node("PRODUCTION_GENERATED_HEIGHT",
			Vector2(70.0, 100.0), {})
		add_node("PRODUCTION_GEOMORPH_SETTINGS", Vector2(70.0, 300.0),
			production_control_defaults("PRODUCTION_GEOMORPH_SETTINGS"))
		var sculpt_id: String = add_node("PRODUCTION_SCULPT_DELTA",
			Vector2(70.0, 650.0), {})
		var add_id: String = add_node("ADD", Vector2(430.0, 205.0), {})
		var output_id: String = add_node("OUTPUT_DISPLACEMENT", Vector2(760.0, 205.0), {})
		connect_nodes(generated_id, 0, add_id, 0)
		connect_nodes(sculpt_id, 0, add_id, 1)
		connect_nodes(add_id, 0, output_id, 0)
	else:
		displacement_output_mode = DisplacementOutputMode.DELTA
		var output_id: String = add_node("OUTPUT_MATERIAL", Vector2(740.0, 245.0), {})
		var source_types: Array[String] = [
			"PRODUCTION_ALBEDO",
			"PRODUCTION_NORMAL",
			"PRODUCTION_ROUGHNESS",
			"PRODUCTION_METALLIC",
			"PRODUCTION_AO",
			"PRODUCTION_SPECULAR",
		]
		for port: int in source_types.size():
			var source_id: String = add_node(source_types[port],
				Vector2(70.0, 55.0 + float(port) * 96.0), {})
			connect_nodes(source_id, 0, output_id, port)
		add_node("PRODUCTION_CLASSIFIER_SETTINGS", Vector2(1080.0, 30.0),
			production_control_defaults("PRODUCTION_CLASSIFIER_SETTINGS"))
		add_node("PRODUCTION_MICRORELIEF_SETTINGS", Vector2(1080.0, 370.0),
			production_control_defaults("PRODUCTION_MICRORELIEF_SETTINGS"))
		add_node("PRODUCTION_ANTITILE_SETTINGS", Vector2(1080.0, 570.0),
			production_control_defaults("PRODUCTION_ANTITILE_SETTINGS"))
		add_node("PRODUCTION_ROCK_PBR_SETTINGS", Vector2(1440.0, 30.0),
			production_control_defaults("PRODUCTION_ROCK_PBR_SETTINGS"))
		add_node("PRODUCTION_SCAN_PBR_SETTINGS", Vector2(1440.0, 300.0),
			production_control_defaults("PRODUCTION_SCAN_PBR_SETTINGS"))
		add_node("PRODUCTION_SCAN_TEXTURES", Vector2(1800.0, 30.0),
			production_control_defaults("PRODUCTION_SCAN_TEXTURES"))
	revision += 1

static func production_control_defaults(node_type: String) -> Dictionary:
	match node_type:
		"PRODUCTION_GEOMORPH_SETTINGS":
			return {
				"detail_strength": 1.0,
				"override_seed": false,
				"detail_seed": 1337,
				"warp_strength": 1.0,
				"broad_strength": 1.0,
				"mountain_strength": 1.0,
				"mid_strength": 1.0,
				"channel_strength": 1.0,
				"deposit_strength": 1.0,
				"fine_strength": 1.0,
				"dune_strength": 1.0,
				"glacial_strength": 1.0,
			}
		"PRODUCTION_CLASSIFIER_SETTINGS":
			return {
				"rock_scale": 1.0,
				"soil_scale": 1.0,
				"vegetation_scale": 1.0,
				"sand_scale": 1.0,
				"mud_scale": 1.0,
				"snow_scale": 1.0,
				"scree_scale": 1.0,
				"gravel_scale": 1.0,
				"albedo_chroma": 1.24,
				"albedo_contrast": 1.10,
				"albedo_pivot": 0.115,
				"roughness_scale": 1.0,
				"roughness_bias": 0.0,
				"roughness_min": 0.42,
				"roughness_max": 0.98,
			}
		"PRODUCTION_MICRORELIEF_SETTINGS":
			return {"enabled": true, "strength": 1.0}
		"PRODUCTION_ANTITILE_SETTINGS":
			return {"strength": 1.0}
		"PRODUCTION_ROCK_PBR_SETTINGS":
			return {
				"enabled": true,
				"detail_strength": 1.0,
				"normal_strength": 1.0,
				"color_strength": 1.0,
			}
		"PRODUCTION_SCAN_PBR_SETTINGS":
			return {
				"enabled": true,
				"ground_metres": 2.0,
				"grass_metres": 2.0,
				"mud_metres": 1.0,
				"forest_metres": 2.0,
			}
		"PRODUCTION_SCAN_TEXTURES":
			return {
				"ground_albedo": "res://assets/textures/terrain/ground003_color_2k.jpg",
				"ground_normal": "res://assets/textures/terrain/ground003_normal_gl_2k.jpg",
				"ground_roughness": "res://assets/textures/terrain/ground003_roughness_2k.jpg",
				"grass_albedo": "res://assets/textures/terrain/leafy_grass_diff_2k.jpg",
				"grass_normal": "res://assets/textures/terrain/leafy_grass_nor_gl_2k.jpg",
				"grass_roughness": "res://assets/textures/terrain/leafy_grass_rough_2k.jpg",
				"mud_albedo": "res://assets/textures/terrain/brown_mud_diff_2k.jpg",
				"mud_normal": "res://assets/textures/terrain/brown_mud_nor_gl_2k.jpg",
				"mud_roughness": "res://assets/textures/terrain/brown_mud_rough_2k.jpg",
				"forest_albedo": "res://assets/textures/terrain/forrest_ground_01_diff_2k.jpg",
				"forest_normal": "res://assets/textures/terrain/forrest_ground_01_nor_gl_2k.jpg",
				"forest_roughness": "res://assets/textures/terrain/forrest_ground_01_rough_2k.jpg",
			}
	return {}

func add_node(node_type: String, position: Vector2, parameters: Dictionary = {}) -> String:
	var resolved_type := node_type if NODE_TYPES.has(node_type) else "CONSTANT_FLOAT"
	var node_id := make_node_id(resolved_type)
	var resolved_parameters: Dictionary = parameters.duplicate(true)
	if PRODUCTION_CONTROL_NODES.has(resolved_type) and resolved_parameters.is_empty():
		resolved_parameters = production_control_defaults(resolved_type)
	nodes.append({
		"id": node_id,
		"type": resolved_type,
		"position": position,
		"parameters": resolved_parameters,
	})
	revision += 1
	return node_id

func remove_node(node_id: String) -> bool:
	for index: int in nodes.size():
		if String(nodes[index].get("id", "")) != node_id:
			continue
		nodes.remove_at(index)
		for link_index: int in range(links.size() - 1, -1, -1):
			var link: Dictionary = links[link_index]
			if String(link.get("from", "")) == node_id or String(link.get("to", "")) == node_id:
				links.remove_at(link_index)
		revision += 1
		return true
	return false

func set_node_position(node_id: String, position: Vector2) -> bool:
	for index: int in nodes.size():
		var node: Dictionary = nodes[index]
		if String(node.get("id", "")) != node_id:
			continue
		node["position"] = position
		nodes[index] = node
		revision += 1
		return true
	return false

func set_node_parameter(node_id: String, key: String, value: Variant) -> bool:
	for index: int in nodes.size():
		var node: Dictionary = nodes[index]
		if String(node.get("id", "")) != node_id:
			continue
		var parameters: Dictionary = node.get("parameters", {}) as Dictionary
		parameters[key] = value
		node["parameters"] = parameters
		nodes[index] = node
		revision += 1
		return true
	return false

func connect_nodes(from_id: String, from_port: int, to_id: String, to_port: int) -> bool:
	if from_id == to_id or not has_node(from_id) or not has_node(to_id):
		return false
	for link: Dictionary in links:
		if String(link.get("to", "")) == to_id and int(link.get("to_port", -1)) == to_port:
			return false
	links.append({
		"from": from_id,
		"from_port": maxi(0, from_port),
		"to": to_id,
		"to_port": maxi(0, to_port),
	})
	revision += 1
	return true

func disconnect_nodes(from_id: String, from_port: int, to_id: String, to_port: int) -> bool:
	for index: int in links.size():
		var link: Dictionary = links[index]
		if String(link.get("from", "")) == from_id \
		and int(link.get("from_port", -1)) == from_port \
		and String(link.get("to", "")) == to_id \
		and int(link.get("to_port", -1)) == to_port:
			links.remove_at(index)
			revision += 1
			return true
	return false

func has_node(node_id: String) -> bool:
	for node: Dictionary in nodes:
		if String(node.get("id", "")) == node_id:
			return true
	return false

func _remove_invalid_links() -> void:
	for index: int in range(links.size() - 1, -1, -1):
		var link: Dictionary = links[index]
		if not has_node(String(link.get("from", ""))) or not has_node(String(link.get("to", ""))):
			links.remove_at(index)

static func node_category(node_type: String) -> String:
	return String(NODE_CATEGORY_BY_TYPE.get(node_type, CATEGORY_UTILITY))

static func node_catalog(next_domain: int, category: String = "") -> Array[String]:
	var out: Array[String] = []
	for node_type: String in NODE_TYPES:
		if DISPLACEMENT_ONLY_NODES.has(node_type) and next_domain != Domain.DISPLACEMENT:
			continue
		if MATERIAL_ONLY_NODES.has(node_type) and next_domain != Domain.MATERIAL:
			continue
		if not category.is_empty() and node_category(node_type) != category:
			continue
		out.append(node_type)
	return out

static func node_categories(next_domain: int) -> Array[String]:
	var out: Array[String] = []
	for category: String in NODE_CATEGORIES:
		for node_type: String in node_catalog(next_domain, category):
			if node_type.begins_with("OUTPUT_"):
				continue
			out.append(category)
			break
	return out

static func game_input_options(next_domain: int) -> Array[String]:
	return DISPLACEMENT_GAME_INPUTS.duplicate() if next_domain == Domain.DISPLACEMENT \
		else MATERIAL_GAME_INPUTS.duplicate()

static func game_input_category(source: String) -> String:
	if source.begins_with("base_"):
		return CATEGORY_SURFACE_PBR
	if source in ["material_primary", "material_secondary", "biome_id", "rock_id", "rock_mix"]:
		return CATEGORY_CLASSIFICATION
	if source in ["soil", "surface", "geology", "structure", "climate", "landform",
			"soil_sand", "soil_silt", "soil_clay", "soil_depth_m", "surface_sediment_m",
			"temperature", "precipitation", "temperature_range", "moisture",
			"vegetation_biomass", "erodibility", "strata_dip", "uplift",
			"flow_x", "flow_y", "hydrology"]:
		return CATEGORY_WORLD_DATA
	if source in ["terrain_height_m", "generated_height_m", "sculpt_delta_m", "micro_layer"]:
		return CATEGORY_TERRAIN_SOURCE
	return CATEGORY_UTILITY

static func make_graph_id(label: String) -> String:
	var safe := label.strip_edges().to_lower().replace(" ", "-")
	if safe.is_empty():
		safe = "terrain-graph"
	return "%s-%d-%d" % [safe, Time.get_ticks_usec(), randi() & 0x7fffffff]

static func make_node_id(node_type: String) -> String:
	return "%s-%d-%d" % [node_type.to_lower(), Time.get_ticks_usec(), randi() & 0x7fffffff]
