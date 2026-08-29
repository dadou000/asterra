class_name TerrainShaderGraphDefinition
extends Resource
## Serializable node graph used by terrain displacement/material authoring slots.
## Runtime compilation is intentionally separate from the editor model: graphs
## can be authored and round-tripped before the compiler is enabled.

enum Domain {
	DISPLACEMENT,
	MATERIAL,
}

const GAME_INPUTS: Array[String] = [
	"world_position",
	"planet_direction",
	"terrain_height_m",
	"surface_normal",
	"slope",
	"biome_id",
	"temperature",
	"precipitation",
	"moisture",
	"vegetation_biomass",
	"soil_sand",
	"soil_silt",
	"soil_clay",
	"soil_depth_m",
	"rock_id",
	"erodibility",
	"sediment",
	"flow",
	"hydrology",
	"uplift",
	"strata_dip",
	"terrain_edit_delta_m",
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
	"TRIPLANAR",
	"OUTPUT_DISPLACEMENT",
	"OUTPUT_MATERIAL",
]

@export var graph_id: String = ""
@export var display_name: String = "Terrain Graph"
@export_enum("Displacement", "Material") var domain: int = Domain.DISPLACEMENT
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
	nodes.clear()
	links.clear()
	var output_type := "OUTPUT_DISPLACEMENT" if domain == Domain.DISPLACEMENT else "OUTPUT_MATERIAL"
	add_node(output_type, Vector2(520.0, 180.0), {})
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

static func make_graph_id(label: String) -> String:
	var safe := label.strip_edges().to_lower().replace(" ", "-")
	if safe.is_empty():
		safe = "terrain-graph"
	return "%s-%d-%d" % [safe, Time.get_ticks_usec(), randi() & 0x7fffffff]

static func make_node_id(node_type: String) -> String:
	return "%s-%d-%d" % [node_type.to_lower(), Time.get_ticks_usec(), randi() & 0x7fffffff]
