class_name TerrainGuidedFeatureGraph
extends RefCounted
## Canonical graph topology used by the simplified authored-displacement UI.
##
## The UI never owns a second terrain representation. A guided feature is serialized
## as ordinary TerrainShaderGraphDefinition nodes and therefore runs through the same
## transactional compiler, GPU vertex bytecode and CPU/contact evaluator as Node Graph.

const GRAPH := preload("res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")

const VERSION: int = 1
const MARKER_KEY := "guided_feature_version"
const ROLE_KEY := "guided_role"
const ROLE_EFFECT := "effect"
const ROLE_MASK := "mask"
const ROLE_OUTPUT := "output"

const EFFECT_HEIGHT := "height_offset"
const EFFECT_NOISE := "noise_detail"
const EFFECT_MOUNTAINS := "ridged_mountains"
const EFFECT_CHANNELS := "erosion_channels"
const EFFECT_DEPOSIT := "sediment_deposit"

const AREA_EVERYWHERE := "everywhere"
const AREA_LATITUDE := "latitude"
const AREA_LONGITUDE := "longitude"
const AREA_REGION := "region"
const AREA_RADIAL := "radial"
const AREA_RING := "ring"

const EFFECTS: Array[String] = [
	EFFECT_HEIGHT,
	EFFECT_NOISE,
	EFFECT_MOUNTAINS,
	EFFECT_CHANNELS,
	EFFECT_DEPOSIT,
]

const AREAS: Array[String] = [
	AREA_EVERYWHERE,
	AREA_LATITUDE,
	AREA_LONGITUDE,
	AREA_REGION,
	AREA_RADIAL,
	AREA_RING,
]


static func default_config() -> Dictionary:
	return {
		MARKER_KEY: VERSION,
		"effect_kind": EFFECT_HEIGHT,
		"area_kind": AREA_RADIAL,
		"amount_m": 25.0,
		"scale": 6.0,
		"passes": 3,
		"seed": 1337,
		"south_deg": -30.0,
		"north_deg": 30.0,
		"west_deg": -45.0,
		"east_deg": 45.0,
		"latitude_feather_deg": 5.0,
		"longitude_feather_deg": 5.0,
		"center_latitude_deg": 0.0,
		"center_longitude_deg": 0.0,
		"radius_deg": 15.0,
		"inner_radius_deg": 8.0,
		"outer_radius_deg": 20.0,
		"radial_feather_deg": 5.0,
		"invert": false,
	}


static func normalized_config(source: Dictionary) -> Dictionary:
	var out: Dictionary = default_config()
	for key: Variant in source.keys():
		out[key] = source[key]
	var effect_kind: String = String(out.get("effect_kind", EFFECT_HEIGHT))
	if not EFFECTS.has(effect_kind):
		effect_kind = EFFECT_HEIGHT
	out["effect_kind"] = effect_kind
	var area_kind: String = String(out.get("area_kind", AREA_RADIAL))
	if not AREAS.has(area_kind):
		area_kind = AREA_RADIAL
	out["area_kind"] = area_kind
	out[MARKER_KEY] = VERSION
	out["amount_m"] = clampf(float(out.get("amount_m", 25.0)), -100000.0, 100000.0)
	out["scale"] = clampf(float(out.get("scale", 6.0)), 0.01, 10000.0)
	out["passes"] = clampi(int(out.get("passes", 3)), 1, 4)
	out["seed"] = clampi(int(out.get("seed", 1337)), 0, 1048575)
	out["south_deg"] = clampf(float(out.get("south_deg", -30.0)), -90.0, 90.0)
	out["north_deg"] = clampf(float(out.get("north_deg", 30.0)), -90.0, 90.0)
	out["west_deg"] = clampf(float(out.get("west_deg", -45.0)), -180.0, 180.0)
	out["east_deg"] = clampf(float(out.get("east_deg", 45.0)), -180.0, 180.0)
	out["latitude_feather_deg"] = clampf(float(out.get("latitude_feather_deg", 5.0)), 0.0, 90.0)
	out["longitude_feather_deg"] = clampf(float(out.get("longitude_feather_deg", 5.0)), 0.0, 180.0)
	out["center_latitude_deg"] = clampf(float(out.get("center_latitude_deg", 0.0)), -90.0, 90.0)
	out["center_longitude_deg"] = clampf(float(out.get("center_longitude_deg", 0.0)), -180.0, 180.0)
	out["radius_deg"] = clampf(float(out.get("radius_deg", 15.0)), 0.0, 180.0)
	var inner_radius: float = clampf(float(out.get("inner_radius_deg", 8.0)), 0.0, 180.0)
	var outer_radius: float = clampf(float(out.get("outer_radius_deg", 20.0)), 0.0, 180.0)
	if inner_radius > outer_radius:
		outer_radius = inner_radius
	out["inner_radius_deg"] = inner_radius
	out["outer_radius_deg"] = outer_radius
	out["radial_feather_deg"] = clampf(float(out.get("radial_feather_deg", 5.0)), 0.0, 180.0)
	out["invert"] = bool(out.get("invert", false))
	return out


static func is_guided_graph(graph: Resource) -> bool:
	return not _output_node_id(graph).is_empty() and int(config_from_graph(graph).get(MARKER_KEY, 0)) == VERSION


static func is_blank_displacement_graph(graph: Resource) -> bool:
	if graph == null or int(graph.get(&"domain")) != GRAPH.Domain.DISPLACEMENT:
		return false
	var nodes: Array = graph.get(&"nodes") as Array
	var links: Array = graph.get(&"links") as Array
	if nodes.size() != 1 or not links.is_empty():
		return false
	return String((nodes[0] as Dictionary).get("type", "")) == "OUTPUT_DISPLACEMENT"


static func can_open_in_simple_mode(graph: Resource) -> bool:
	return is_guided_graph(graph) or is_blank_displacement_graph(graph)


static func config_from_graph(graph: Resource) -> Dictionary:
	if graph == null:
		return {}
	var output_id: String = _output_node_id(graph)
	if output_id.is_empty():
		return {}
	for node_value: Variant in graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		if String(node.get("id", "")) != output_id:
			continue
		var parameters: Dictionary = node.get("parameters", {}) as Dictionary
		if int(parameters.get(MARKER_KEY, 0)) != VERSION:
			return {}
		return normalized_config(parameters)
	return {}


static func rebuild(graph: Resource, source_config: Dictionary) -> bool:
	if graph == null:
		return false
	var config: Dictionary = normalized_config(source_config)
	graph.set(&"domain", GRAPH.Domain.DISPLACEMENT)
	graph.set(&"displacement_output_mode", GRAPH.DisplacementOutputMode.DELTA)
	var empty_nodes: Array[Dictionary] = []
	var empty_links: Array[Dictionary] = []
	graph.set(&"nodes", empty_nodes)
	graph.set(&"links", empty_links)

	var effect_id: String = _build_effect(graph, config)
	var mask_id: String = _build_mask(graph, config)
	if effect_id.is_empty() or mask_id.is_empty():
		return false
	var multiply_id: String = String(graph.call("add_node", "MULTIPLY",
		Vector2(520.0, 210.0), {ROLE_KEY:"combine"}))
	var output_parameters: Dictionary = config.duplicate(true)
	output_parameters[ROLE_KEY] = ROLE_OUTPUT
	var output_id: String = String(graph.call("add_node", "OUTPUT_DISPLACEMENT",
		Vector2(800.0, 210.0), output_parameters))
	if multiply_id.is_empty() or output_id.is_empty():
		return false
	return bool(graph.call("connect_nodes", effect_id, 0, multiply_id, 0)) \
		and bool(graph.call("connect_nodes", mask_id, 0, multiply_id, 1)) \
		and bool(graph.call("connect_nodes", multiply_id, 0, output_id, 0))


static func set_config_value(graph: Resource, key: String, value: Variant) -> bool:
	if graph == null or not is_guided_graph(graph):
		return false
	var config: Dictionary = config_from_graph(graph)
	config[key] = value
	config = normalized_config(config)
	var output_id: String = _output_node_id(graph)
	if output_id.is_empty():
		return false
	for config_key: Variant in config.keys():
		graph.call("set_node_parameter", output_id, String(config_key), config[config_key])

	var effect_id: String = _role_node_id(graph, ROLE_EFFECT)
	var mask_id: String = _role_node_id(graph, ROLE_MASK)
	if key in ["amount_m", "scale", "passes", "seed"] and not effect_id.is_empty():
		if String(config.get("effect_kind", EFFECT_HEIGHT)) == EFFECT_HEIGHT:
			if key == "amount_m":
				graph.call("set_node_parameter", effect_id, "value", float(config["amount_m"]))
		else:
			match key:
				"amount_m": graph.call("set_node_parameter", effect_id, "amount", float(config["amount_m"]))
				"scale": graph.call("set_node_parameter", effect_id, "scale", float(config["scale"]))
				"passes": graph.call("set_node_parameter", effect_id, "passes", int(config["passes"]))
				"seed": graph.call("set_node_parameter", effect_id, "seed", int(config["seed"]))
	if key in ["south_deg", "north_deg", "west_deg", "east_deg", "latitude_feather_deg",
			"longitude_feather_deg", "center_latitude_deg", "center_longitude_deg", "radius_deg",
			"inner_radius_deg", "outer_radius_deg", "radial_feather_deg", "invert"] \
			and not mask_id.is_empty():
		_apply_mask_config(graph, mask_id, config)
	return true


static func summary(graph: Resource) -> String:
	if not is_guided_graph(graph):
		return "Custom node graph"
	var config: Dictionary = config_from_graph(graph)
	return "%s · %s" % [effect_label(String(config.get("effect_kind", EFFECT_HEIGHT))),
		area_label(String(config.get("area_kind", AREA_RADIAL)))]


static func effect_label(effect_kind: String) -> String:
	match effect_kind:
		EFFECT_HEIGHT: return "Raise / Lower"
		EFFECT_NOISE: return "Natural Roughness"
		EFFECT_MOUNTAINS: return "Ridged Mountains"
		EFFECT_CHANNELS: return "Erosion Channels"
		EFFECT_DEPOSIT: return "Sediment Deposit"
	return "Raise / Lower"


static func area_label(area_kind: String) -> String:
	match area_kind:
		AREA_EVERYWHERE: return "Everywhere"
		AREA_LATITUDE: return "Latitude Band"
		AREA_LONGITUDE: return "Longitude Band"
		AREA_REGION: return "Geographic Region"
		AREA_RADIAL: return "Radial Area"
		AREA_RING: return "Ring Area"
	return "Radial Area"


static func _build_effect(graph: Resource, config: Dictionary) -> String:
	var effect_kind: String = String(config.get("effect_kind", EFFECT_HEIGHT))
	if effect_kind == EFFECT_HEIGHT:
		return String(graph.call("add_node", "CONSTANT_FLOAT", Vector2(80.0, 125.0), {
			"value":float(config.get("amount_m", 25.0)), ROLE_KEY:ROLE_EFFECT,
		}))
	var zero_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(50.0, 80.0), {"value":0.0, ROLE_KEY:"effect_base"}))
	var node_type: String = "NOISE_LAYER"
	match effect_kind:
		EFFECT_MOUNTAINS: node_type = "RIDGED_MOUNTAINS"
		EFFECT_CHANNELS: node_type = "EROSION_CHANNELS"
		EFFECT_DEPOSIT: node_type = "SEDIMENT_DEPOSIT"
		_: node_type = "NOISE_LAYER"
	var operation_id: String = String(graph.call("add_node", node_type,
		Vector2(285.0, 125.0), {
			"scale":float(config.get("scale", 6.0)),
			"amount":float(config.get("amount_m", 25.0)),
			"passes":int(config.get("passes", 3)),
			"seed":int(config.get("seed", 1337)),
			ROLE_KEY:ROLE_EFFECT,
		}))
	if zero_id.is_empty() or operation_id.is_empty() \
			or not bool(graph.call("connect_nodes", zero_id, 0, operation_id, 0)):
		return ""
	return operation_id


static func _build_mask(graph: Resource, config: Dictionary) -> String:
	var area_kind: String = String(config.get("area_kind", AREA_RADIAL))
	if area_kind == AREA_EVERYWHERE:
		return String(graph.call("add_node", "CONSTANT_FLOAT", Vector2(285.0, 340.0), {
			"value":1.0, ROLE_KEY:ROLE_MASK,
		}))
	var parameters: Dictionary = {ROLE_KEY:ROLE_MASK, "invert":bool(config.get("invert", false))}
	match area_kind:
		AREA_LATITUDE:
			parameters["axis"] = "latitude"
			parameters["south_deg"] = float(config.get("south_deg", -30.0))
			parameters["north_deg"] = float(config.get("north_deg", 30.0))
			parameters["feather_deg"] = float(config.get("latitude_feather_deg", 5.0))
		AREA_LONGITUDE:
			parameters["axis"] = "longitude"
			parameters["south_deg"] = float(config.get("west_deg", -45.0))
			parameters["north_deg"] = float(config.get("east_deg", 45.0))
			parameters["feather_deg"] = float(config.get("longitude_feather_deg", 5.0))
		AREA_REGION:
			parameters["axis"] = "region"
			parameters["south_deg"] = float(config.get("south_deg", -30.0))
			parameters["north_deg"] = float(config.get("north_deg", 30.0))
			parameters["feather_deg"] = float(config.get("latitude_feather_deg", 5.0))
			parameters["west_deg"] = float(config.get("west_deg", -45.0))
			parameters["east_deg"] = float(config.get("east_deg", 45.0))
			parameters["longitude_feather_deg"] = float(config.get("longitude_feather_deg", 5.0))
		AREA_RING:
			parameters["axis"] = "ring"
			parameters["center_latitude_deg"] = float(config.get("center_latitude_deg", 0.0))
			parameters["center_longitude_deg"] = float(config.get("center_longitude_deg", 0.0))
			parameters["inner_radius_deg"] = float(config.get("inner_radius_deg", 8.0))
			parameters["outer_radius_deg"] = float(config.get("outer_radius_deg", 20.0))
			parameters["feather_deg"] = float(config.get("radial_feather_deg", 5.0))
		_:
			parameters["axis"] = "radial"
			parameters["center_latitude_deg"] = float(config.get("center_latitude_deg", 0.0))
			parameters["center_longitude_deg"] = float(config.get("center_longitude_deg", 0.0))
			parameters["radius_deg"] = float(config.get("radius_deg", 15.0))
			parameters["feather_deg"] = float(config.get("radial_feather_deg", 5.0))
	return String(graph.call("add_node", "LATITUDE_MASK", Vector2(285.0, 340.0), parameters))


static func _apply_mask_config(graph: Resource, mask_id: String, config: Dictionary) -> void:
	var area_kind: String = String(config.get("area_kind", AREA_RADIAL))
	graph.call("set_node_parameter", mask_id, "invert", bool(config.get("invert", false)))
	match area_kind:
		AREA_LATITUDE:
			graph.call("set_node_parameter", mask_id, "south_deg", float(config.get("south_deg", -30.0)))
			graph.call("set_node_parameter", mask_id, "north_deg", float(config.get("north_deg", 30.0)))
			graph.call("set_node_parameter", mask_id, "feather_deg", float(config.get("latitude_feather_deg", 5.0)))
		AREA_LONGITUDE:
			graph.call("set_node_parameter", mask_id, "south_deg", float(config.get("west_deg", -45.0)))
			graph.call("set_node_parameter", mask_id, "north_deg", float(config.get("east_deg", 45.0)))
			graph.call("set_node_parameter", mask_id, "feather_deg", float(config.get("longitude_feather_deg", 5.0)))
		AREA_REGION:
			graph.call("set_node_parameter", mask_id, "south_deg", float(config.get("south_deg", -30.0)))
			graph.call("set_node_parameter", mask_id, "north_deg", float(config.get("north_deg", 30.0)))
			graph.call("set_node_parameter", mask_id, "feather_deg", float(config.get("latitude_feather_deg", 5.0)))
			graph.call("set_node_parameter", mask_id, "west_deg", float(config.get("west_deg", -45.0)))
			graph.call("set_node_parameter", mask_id, "east_deg", float(config.get("east_deg", 45.0)))
			graph.call("set_node_parameter", mask_id, "longitude_feather_deg", float(config.get("longitude_feather_deg", 5.0)))
		AREA_RING:
			graph.call("set_node_parameter", mask_id, "center_latitude_deg", float(config.get("center_latitude_deg", 0.0)))
			graph.call("set_node_parameter", mask_id, "center_longitude_deg", float(config.get("center_longitude_deg", 0.0)))
			graph.call("set_node_parameter", mask_id, "inner_radius_deg", float(config.get("inner_radius_deg", 8.0)))
			graph.call("set_node_parameter", mask_id, "outer_radius_deg", float(config.get("outer_radius_deg", 20.0)))
			graph.call("set_node_parameter", mask_id, "feather_deg", float(config.get("radial_feather_deg", 5.0)))
		AREA_RADIAL:
			graph.call("set_node_parameter", mask_id, "center_latitude_deg", float(config.get("center_latitude_deg", 0.0)))
			graph.call("set_node_parameter", mask_id, "center_longitude_deg", float(config.get("center_longitude_deg", 0.0)))
			graph.call("set_node_parameter", mask_id, "radius_deg", float(config.get("radius_deg", 15.0)))
			graph.call("set_node_parameter", mask_id, "feather_deg", float(config.get("radial_feather_deg", 5.0)))


static func _role_node_id(graph: Resource, role: String) -> String:
	if graph == null:
		return ""
	for node_value: Variant in graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		var parameters: Dictionary = node.get("parameters", {}) as Dictionary
		if String(parameters.get(ROLE_KEY, "")) == role:
			return String(node.get("id", ""))
	return ""


static func _output_node_id(graph: Resource) -> String:
	if graph == null:
		return ""
	for node_value: Variant in graph.get(&"nodes") as Array:
		if node_value is Dictionary and String((node_value as Dictionary).get("type", "")) == "OUTPUT_DISPLACEMENT":
			return String((node_value as Dictionary).get("id", ""))
	return ""
