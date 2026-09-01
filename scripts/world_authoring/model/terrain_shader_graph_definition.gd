class_name TerrainShaderGraphDefinition
extends Resource
## Serializable node graph used by terrain displacement/material authoring slots.
##
## Phase 29 treats the production renderer as an explicit graph source instead of
## an opaque editor-only block. Existing graphs remain compatible: displacement
## graphs default to additive DELTA output and the first five material output ports
## keep their historical ordering. New production-flow graphs can opt into
## ABSOLUTE_HEIGHT, which the compiler converts back to an authoritative delta.

enum Domain {
	DISPLACEMENT,
	MATERIAL,
}

enum DisplacementOutputMode {
	DELTA,
	ABSOLUTE_HEIGHT,
}

const CATEGORY_OUTPUT := "Output"
const CATEGORY_TERRAIN_SOURCE := "Terrain source"
const CATEGORY_GEOMORPH := "Geomorph"
const CATEGORY_WORLD_DATA := "World data"
const CATEGORY_CLASSIFICATION := "Classification"
const CATEGORY_SURFACE_PBR := "Surface / PBR"
const CATEGORY_TEXTURES := "Textures"
const CATEGORY_MATH := "Math"
const CATEGORY_UTILITY := "Utility"

const NODE_CATEGORIES: Array[String] = [
	CATEGORY_OUTPUT,
	CATEGORY_TERRAIN_SOURCE,
	CATEGORY_GEOMORPH,
	CATEGORY_WORLD_DATA,
	CATEGORY_CLASSIFICATION,
	CATEGORY_SURFACE_PBR,
	CATEGORY_TEXTURES,
	CATEGORY_MATH,
	CATEGORY_UTILITY,
]

# The first five material base inputs existed before Phase 29. Keep them and append
# Specular rather than inserting it in the middle so old graph port numbers survive.
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
	"material_primary",
	"material_secondary",
	"micro_layer",
	"camera_distance_m",
	"clipmap_level",
	"time_s",
]

const DISPLACEMENT_GAME_INPUTS: Array[String] = [
	"terrain_height_m",
	"biome_id",
	"clipmap_level",
	"time_s",
]

# Backward-compatible union used by older editor code. New editors should call
# game_input_options(domain) so displacement does not advertise render-only data.
const GAME_INPUTS: Array[String] = [
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
	"material_primary",
	"material_secondary",
	"micro_layer",
	"camera_distance_m",
	"clipmap_level",
	"time_s",
]

const NODE_TYPES: Array[String] = [
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
	"SMOOTHSTEP",
	"REMAP",
	"MIX",
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
	"GAME_INPUT": CATEGORY_WORLD_DATA,
	"NOISE": CATEGORY_TERRAIN_SOURCE,
	"NOISE_LAYER": CATEGORY_GEOMORPH,
	"RIDGED_MOUNTAINS": CATEGORY_GEOMORPH,
	"EROSION_CHANNELS": CATEGORY_GEOMORPH,
	"SEDIMENT_DEPOSIT": CATEGORY_GEOMORPH,
	"TEXTURE_2D": CATEGORY_TEXTURES,
	"TRIPLANAR": CATEGORY_TEXTURES,
	"NORMAL_BLEND": CATEGORY_SURFACE_PBR,
	"ADD": CATEGORY_MATH,
	"SUBTRACT": CATEGORY_MATH,
	"MULTIPLY": CATEGORY_MATH,
	"DIVIDE": CATEGORY_MATH,
	"MIN": CATEGORY_MATH,
	"MAX": CATEGORY_MATH,
	"ABS": CATEGORY_MATH,
	"POWER": CATEGORY_MATH,
	"CLAMP": CATEGORY_MATH,
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
	# Identity graph for the exact renderer result that exists before authored
	# composition. It is intentionally opt-in; opening Planet Studio stays GPU-neutral.
	domain = next_domain
	nodes.clear()
	links.clear()
	if domain == Domain.DISPLACEMENT:
		displacement_output_mode = DisplacementOutputMode.ABSOLUTE_HEIGHT
		var source_id: String = add_node("GAME_INPUT", Vector2(80.0, 180.0), {
			"source": "terrain_height_m",
		})
		var output_id: String = add_node("OUTPUT_DISPLACEMENT", Vector2(520.0, 180.0), {})
		connect_nodes(source_id, 0, output_id, 0)
	else:
		displacement_output_mode = DisplacementOutputMode.DELTA
		var output_id: String = add_node("OUTPUT_MATERIAL", Vector2(620.0, 180.0), {})
		var sources: Array[String] = [
			"base_albedo",
			"base_normal",
			"base_roughness",
			"base_metallic",
			"base_ao",
			"base_specular",
		]
		for port: int in sources.size():
			var source_id: String = add_node("GAME_INPUT",
				Vector2(80.0, 70.0 + float(port) * 92.0), {"source": sources[port]})
			connect_nodes(source_id, 0, output_id, port)
	revision += 1

func add_node(node_type: String, position: Vector2, parameters: Dictionary = {}) -> String:
	var resolved_type := node_type if NODE_TYPES.has(node_type) else "CONSTANT_FLOAT"
	var node_id := make_node_id(resolved_type)
	nodes.append({
		"id": node_id,
		"type": resolved_type,
		"position": position,
		"parameters": parameters.duplicate(true),
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
		if node_type == "OUTPUT_DISPLACEMENT" and next_domain != Domain.DISPLACEMENT:
			continue
		if node_type == "OUTPUT_MATERIAL" and next_domain != Domain.MATERIAL:
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
	if source in ["material_primary", "material_secondary", "biome_id", "rock_id"]:
		return CATEGORY_CLASSIFICATION
	if source in ["soil_sand", "soil_silt", "soil_clay", "soil_depth_m",
			"surface_sediment_m", "temperature", "precipitation", "temperature_range",
			"moisture", "vegetation_biomass", "erodibility", "strata_dip", "uplift",
			"flow_x", "flow_y", "hydrology"]:
		return CATEGORY_WORLD_DATA
	if source in ["terrain_height_m", "micro_layer"]:
		return CATEGORY_TERRAIN_SOURCE
	return CATEGORY_UTILITY

static func make_graph_id(label: String) -> String:
	var safe := label.strip_edges().to_lower().replace(" ", "-")
	if safe.is_empty():
		safe = "terrain-graph"
	return "%s-%d-%d" % [safe, Time.get_ticks_usec(), randi() & 0x7fffffff]

static func make_node_id(node_type: String) -> String:
	return "%s-%d-%d" % [node_type.to_lower(), Time.get_ticks_usec(), randi() & 0x7fffffff]
