class_name TerrainProductionGeomorphGraph
extends RefCounted
## Structural graph view of the resident production geomorph shader.
##
## These nodes are native production intrinsics, not approximations using the
## generic author bytecode operations. While their topology is canonical the
## renderer continues to execute the existing gpu_geomorph.gdshaderinc path and the
## authored displacement VM remains empty. Parameters are distributed to the stage
## that actually owns them so Node Graph exposes what production terrain already
## does instead of one opaque generated-height macro.

const SCHEMA := preload("res://scripts/world_authoring/model/terrain_production_geomorph_schema.gd")

const OUTPUT_MODE_ABSOLUTE: int = 1
const PRODUCTION_SHAPE_SLOT_ID := "production-terrain-shape"
const LEGACY_GENERATED_TYPE := "PRODUCTION_GENERATED_HEIGHT"
const SETTINGS_TYPE := "PRODUCTION_GEOMORPH_SETTINGS"
const START_TYPE := "PRODUCTION_GEOMORPH_START"
const COMPOSE_TYPE := "PRODUCTION_GEOMORPH_COMPOSE"
const SCULPT_TYPE := "PRODUCTION_SCULPT_DELTA"
const ADD_TYPE := "ADD"
const OUTPUT_TYPE := "OUTPUT_DISPLACEMENT"
const STAGE_PREFIX := "PRODUCTION_GEOMORPH_STAGE_"


static func stage_node_type(stage_id: String) -> String:
	return STAGE_PREFIX + stage_id.to_upper()


static func stage_id_for_node_type(node_type: String) -> String:
	if not node_type.begins_with(STAGE_PREFIX):
		return ""
	return node_type.trim_prefix(STAGE_PREFIX).to_lower()


static func native_stage_types() -> PackedStringArray:
	var out := PackedStringArray()
	for stage_id: String in SCHEMA.ordered_stage_ids():
		out.append(stage_node_type(stage_id))
	return out


static func reserved_node_types() -> PackedStringArray:
	var out := PackedStringArray([START_TYPE, COMPOSE_TYPE])
	out.append_array(native_stage_types())
	return out


static func has_structural_nodes(graph: Resource) -> bool:
	if graph == null:
		return false
	var nodes_value: Variant = graph.get(&"nodes")
	if not (nodes_value is Array):
		return false
	for node_value: Variant in nodes_value as Array:
		if not (node_value is Dictionary):
			continue
		var node_type: String = String((node_value as Dictionary).get("type", ""))
		if node_type == START_TYPE or node_type == COMPOSE_TYPE \
				or node_type.begins_with(STAGE_PREFIX):
			return true
	return false


static func is_legacy_identity_graph(graph: Resource) -> bool:
	if graph == null or int(graph.get(&"displacement_output_mode")) != OUTPUT_MODE_ABSOLUTE:
		return false
	var nodes_value: Variant = graph.get(&"nodes")
	var links_value: Variant = graph.get(&"links")
	if not (nodes_value is Array) or not (links_value is Array):
		return false
	var nodes: Array = nodes_value as Array
	var links: Array = links_value as Array
	if links.size() != 3:
		return false
	var by_type: Dictionary = {}
	var settings_count: int = 0
	for node_value: Variant in nodes:
		if not (node_value is Dictionary):
			return false
		var node: Dictionary = node_value as Dictionary
		var node_type: String = String(node.get("type", ""))
		if node_type == SETTINGS_TYPE:
			settings_count += 1
			continue
		if node_type == "GAME_INPUT":
			var source: String = String((node.get("parameters", {}) as Dictionary).get("source", ""))
			if source == "generated_height_m":
				node_type = LEGACY_GENERATED_TYPE
			elif source == "sculpt_delta_m":
				node_type = SCULPT_TYPE
			else:
				return false
		if not [LEGACY_GENERATED_TYPE, SCULPT_TYPE, ADD_TYPE, OUTPUT_TYPE].has(node_type):
			return false
		if by_type.has(node_type):
			return false
		by_type[node_type] = String(node.get("id", ""))
	if settings_count > 1 or nodes.size() != 4 + settings_count:
		return false
	for node_type: String in [LEGACY_GENERATED_TYPE, SCULPT_TYPE, ADD_TYPE, OUTPUT_TYPE]:
		if not by_type.has(node_type) or String(by_type[node_type]).is_empty():
			return false
	return _links_match(links, [
		[by_type[LEGACY_GENERATED_TYPE], 0, by_type[ADD_TYPE], 0],
		[by_type[SCULPT_TYPE], 0, by_type[ADD_TYPE], 1],
		[by_type[ADD_TYPE], 0, by_type[OUTPUT_TYPE], 0],
	])


static func migrate_legacy_identity(graph: Resource) -> bool:
	if not is_legacy_identity_graph(graph):
		return false
	var controls: Dictionary = SCHEMA.control_defaults()
	for node_value: Variant in graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		if String(node.get("type", "")) != SETTINGS_TYPE:
			continue
		for key_value: Variant in (node.get("parameters", {}) as Dictionary).keys():
			controls[key_value] = (node.get("parameters", {}) as Dictionary)[key_value]
		break
	build_canonical_graph(graph, controls)
	return true


static func build_canonical_graph(graph: Resource, controls: Dictionary = {}) -> void:
	if graph == null:
		return
	var resolved: Dictionary = SCHEMA.control_defaults()
	for key_value: Variant in controls.keys():
		if resolved.has(key_value):
			resolved[key_value] = controls[key_value]

	var nodes: Array[Dictionary] = []
	var links: Array[Dictionary] = []
	var start_id := "native-geomorph-start"
	nodes.append(_node(start_id, START_TYPE, Vector2(60.0, 145.0), {}))

	var global_controls: Dictionary = {}
	for key: String in SCHEMA.GLOBAL_CONTROLS:
		global_controls[key] = resolved[key]
	nodes.append(_node("native-geomorph-settings", SETTINGS_TYPE,
		Vector2(60.0, 460.0), global_controls))

	var previous_id: String = start_id
	var x: float = 350.0
	for stage: Dictionary in SCHEMA.stage_specs():
		var stage_id: String = String(stage.get("id", ""))
		var node_id: String = "native-geomorph-stage-" + stage_id
		var parameters: Dictionary = {}
		for key_value: Variant in stage.get("parameters", []) as Array:
			var key: String = String(key_value)
			parameters[key] = resolved[key]
		nodes.append(_node(node_id, stage_node_type(stage_id), Vector2(x, 145.0), parameters))
		links.append(_link(previous_id, 0, node_id, 0))
		previous_id = node_id
		x += 300.0

	var compose_id := "native-geomorph-compose"
	nodes.append(_node(compose_id, COMPOSE_TYPE, Vector2(x, 145.0), {}))
	links.append(_link(previous_id, 0, compose_id, 0))
	x += 315.0

	var sculpt_id := "native-production-sculpt"
	var add_id := "native-production-final-add"
	var output_id := "native-production-output"
	nodes.append(_node(sculpt_id, SCULPT_TYPE, Vector2(x, 440.0), {}))
	nodes.append(_node(add_id, ADD_TYPE, Vector2(x, 175.0), {}))
	nodes.append(_node(output_id, OUTPUT_TYPE, Vector2(x + 340.0, 175.0), {}))
	links.append(_link(compose_id, 0, add_id, 0))
	links.append(_link(sculpt_id, 0, add_id, 1))
	links.append(_link(add_id, 0, output_id, 0))

	graph.set(&"displacement_output_mode", OUTPUT_MODE_ABSOLUTE)
	graph.set(&"nodes", nodes)
	graph.set(&"links", links)
	graph.set(&"revision", int(graph.get(&"revision")) + 1)


static func is_canonical_structural_graph(graph: Resource) -> bool:
	if graph == null or int(graph.get(&"displacement_output_mode")) != OUTPUT_MODE_ABSOLUTE:
		return false
	var nodes_value: Variant = graph.get(&"nodes")
	var links_value: Variant = graph.get(&"links")
	if not (nodes_value is Array) or not (links_value is Array):
		return false
	var expected_types := PackedStringArray([START_TYPE, SETTINGS_TYPE])
	expected_types.append_array(native_stage_types())
	expected_types.append_array(PackedStringArray([COMPOSE_TYPE, SCULPT_TYPE, ADD_TYPE, OUTPUT_TYPE]))
	var by_type: Dictionary = {}
	for node_value: Variant in nodes_value as Array:
		if not (node_value is Dictionary):
			return false
		var node: Dictionary = node_value as Dictionary
		var node_type: String = String(node.get("type", ""))
		if not expected_types.has(node_type) or by_type.has(node_type):
			return false
		by_type[node_type] = String(node.get("id", ""))
	if by_type.size() != expected_types.size():
		return false
	for node_type: String in expected_types:
		if not by_type.has(node_type) or String(by_type[node_type]).is_empty():
			return false

	var expected_links: Array = []
	var previous_id: String = String(by_type[START_TYPE])
	for stage_id: String in SCHEMA.ordered_stage_ids():
		var stage_type: String = stage_node_type(stage_id)
		expected_links.append([previous_id, 0, by_type[stage_type], 0])
		previous_id = String(by_type[stage_type])
	expected_links.append([previous_id, 0, by_type[COMPOSE_TYPE], 0])
	expected_links.append([by_type[COMPOSE_TYPE], 0, by_type[ADD_TYPE], 0])
	expected_links.append([by_type[SCULPT_TYPE], 0, by_type[ADD_TYPE], 1])
	expected_links.append([by_type[ADD_TYPE], 0, by_type[OUTPUT_TYPE], 0])
	return _links_match(links_value as Array, expected_links)


static func extract_controls(graph: Resource) -> Dictionary:
	var out: Dictionary = SCHEMA.control_defaults()
	if graph == null:
		return out
	for node_value: Variant in graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		var node_type: String = String(node.get("type", ""))
		if node_type != SETTINGS_TYPE and not node_type.begins_with(STAGE_PREFIX):
			continue
		var parameters: Dictionary = node.get("parameters", {}) as Dictionary
		for key_value: Variant in parameters.keys():
			if out.has(key_value):
				out[key_value] = parameters[key_value]
	return out


static func owner_node_type_for_control(key: String) -> String:
	var owner: String = String(SCHEMA.parameter_owner_map().get(key, ""))
	if owner == "global":
		return SETTINGS_TYPE
	if owner.is_empty():
		return ""
	return stage_node_type(owner)


static func stage_title_for_node_type(node_type: String) -> String:
	var stage_id: String = stage_id_for_node_type(node_type)
	if stage_id.is_empty():
		return ""
	for stage: Dictionary in SCHEMA.stage_specs():
		if String(stage.get("id", "")) == stage_id:
			return String(stage.get("title", stage_id.capitalize()))
	return stage_id.capitalize()


static func stage_spec_for_node_type(node_type: String) -> Dictionary:
	var stage_id: String = stage_id_for_node_type(node_type)
	for stage: Dictionary in SCHEMA.stage_specs():
		if String(stage.get("id", "")) == stage_id:
			return stage.duplicate(true)
	return {}


static func _node(node_id: String, node_type: String, position: Vector2,
		parameters: Dictionary) -> Dictionary:
	return {"id":node_id, "type":node_type, "position":position,
		"parameters":parameters.duplicate(true)}


static func _link(from_id: String, from_port: int, to_id: String, to_port: int) -> Dictionary:
	return {"from":from_id, "from_port":from_port, "to":to_id, "to_port":to_port}


static func _links_match(actual: Array, expected: Array) -> bool:
	if actual.size() != expected.size():
		return false
	var remaining: Dictionary = {}
	for item: Variant in expected:
		var row: Array = item as Array
		remaining["%s:%d>%s:%d" % [String(row[0]), int(row[1]), String(row[2]), int(row[3])]] = true
	for link_value: Variant in actual:
		if not (link_value is Dictionary):
			return false
		var link: Dictionary = link_value as Dictionary
		var key: String = "%s:%d>%s:%d" % [
			String(link.get("from", "")), int(link.get("from_port", -1)),
			String(link.get("to", "")), int(link.get("to_port", -1)),
		]
		if not remaining.has(key):
			return false
		remaining.erase(key)
	return remaining.is_empty()
