class_name TerrainProductionGeomorphLowering
extends RefCounted
## Exact structural lowering for the resident production geomorph pipeline.
##
## Two serialized forms are accepted during migration:
## - Phase 37 linear chains, where Broad through Micro may be reordered/bypassed;
## - contribution/merge graphs, where those same independent stages fan into one
##   explicit merge and Glacial optionally transforms the accumulated result.
## Both lower to the existing resident production shader and zero authored bytecode.

const NATIVE := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")

const NONLINEAR_TERMINAL_STAGE := "glacial"


static func ordered_bypass_plan(graph: Resource) -> Dictionary:
	# Compatibility contract used by older runtime phases. Branch graphs are exact
	# too, so accept them rather than forcing callers to know the serialization phase.
	if NATIVE.has_merge_node(graph):
		return _branch_native_plan(graph)
	return _linear_native_plan(graph, false)


static func commutative_reorder_plan(graph: Resource) -> Dictionary:
	# Compatibility entry point used by the active runtime. Phase 38 contribution
	# graphs are detected first; saved Phase 37 linear graphs remain readable.
	if NATIVE.has_merge_node(graph):
		return _branch_native_plan(graph)
	return _linear_native_plan(graph, true)


static func contribution_merge_plan(graph: Resource) -> Dictionary:
	return _branch_native_plan(graph)


static func apply_bypass_controls(controls: Dictionary, plan: Dictionary) -> Dictionary:
	var out: Dictionary = controls.duplicate(true)
	if not bool(plan.get("valid", false)):
		return out
	var disabled_value: Variant = plan.get("disabled_stage_ids", PackedStringArray())
	if not (disabled_value is PackedStringArray):
		return out
	for stage_id: String in disabled_value as PackedStringArray:
		var overrides: Dictionary = disabled_control_overrides(stage_id)
		for key_value: Variant in overrides.keys():
			out[key_value] = overrides[key_value]
	return out


static func disabled_control_overrides(stage_id: String) -> Dictionary:
	# These controls are chosen from the exact resident shader equations. In
	# particular, Fine uses amplitude rather than fine_strength because Micro Relief
	# also intentionally consumes fine_strength; Channel keeps its shared sample alive
	# so Deposition can remain enabled when incision itself is bypassed.
	match stage_id:
		"broad": return {"broad_strength": 0.0}
		"mountain": return {"mountain_strength": 0.0}
		"mid": return {"mid_strength": 0.0}
		"channel": return {"channel_strength": 0.0}
		"deposit": return {"deposit_strength": 0.0}
		"fine": return {"fine_amplitude_m": 0.0}
		"dune": return {"dune_strength": 0.0}
		"micro": return {"micro_amplitude_m": 0.0}
		"glacial": return {"glacial_strength": 0.0}
	return {}


static func _branch_native_plan(graph: Resource) -> Dictionary:
	if graph == null:
		return _invalid("native production graph is unavailable")
	if int(graph.get(&"displacement_output_mode")) != NATIVE.OUTPUT_MODE_ABSOLUTE:
		return _invalid("native production graph must output absolute terrain height")

	var nodes_value: Variant = graph.get(&"nodes")
	var links_value: Variant = graph.get(&"links")
	if not (nodes_value is Array) or not (links_value is Array):
		return _invalid("native production graph arrays are malformed")

	var expected_types := PackedStringArray([
		NATIVE.START_TYPE, NATIVE.SETTINGS_TYPE, NATIVE.MERGE_TYPE])
	expected_types.append_array(NATIVE.native_stage_types())
	expected_types.append_array(PackedStringArray([
		NATIVE.COMPOSE_TYPE, NATIVE.SCULPT_TYPE, NATIVE.ADD_TYPE, NATIVE.OUTPUT_TYPE]))

	var by_type: Dictionary = {}
	var by_id: Dictionary = {}
	for node_value: Variant in nodes_value as Array:
		if not (node_value is Dictionary):
			return _invalid("native contribution graph contains an invalid node record")
		var node: Dictionary = node_value as Dictionary
		var node_id: String = String(node.get("id", ""))
		var node_type: String = String(node.get("type", ""))
		if node_id.is_empty():
			return _invalid("native contribution graph contains a node with no ID")
		if not expected_types.has(node_type):
			return _invalid("unsupported node %s is mixed into the native contribution graph" % node_type)
		if by_id.has(node_id):
			return _invalid("duplicate native node ID %s" % node_id)
		if by_type.has(node_type):
			return _invalid("duplicate native node type %s" % node_type)
		by_id[node_id] = node_type
		by_type[node_type] = node_id

	if by_type.size() != expected_types.size():
		return _invalid("native contribution graph is missing a required stage or terminal node")
	for node_type: String in expected_types:
		if not by_type.has(node_type) or String(by_type[node_type]).is_empty():
			return _invalid("native contribution graph is missing %s" % node_type)

	var links: Array = links_value as Array
	var incoming: Dictionary = {}
	var outgoing: Dictionary = {}
	var exact_links: Dictionary = {}
	for link_value: Variant in links:
		if not (link_value is Dictionary):
			return _invalid("native contribution graph contains an invalid connection")
		var link: Dictionary = link_value as Dictionary
		var from_id: String = String(link.get("from", ""))
		var to_id: String = String(link.get("to", ""))
		var from_port: int = int(link.get("from_port", -1))
		var to_port: int = int(link.get("to_port", -1))
		if not by_id.has(from_id) or not by_id.has(to_id):
			return _invalid("native contribution graph contains a connection to a missing node")
		if from_port < 0 or to_port < 0:
			return _invalid("native contribution graph contains a negative port index")
		var exact_key: String = _link_key(from_id, from_port, to_id, to_port)
		if exact_links.has(exact_key):
			return _invalid("native contribution graph contains a duplicate connection")
		exact_links[exact_key] = true
		if not outgoing.has(from_id):
			outgoing[from_id] = []
		if not incoming.has(to_id):
			incoming[to_id] = []
		(outgoing[from_id] as Array).append(link)
		(incoming[to_id] as Array).append(link)

	var start_id: String = String(by_type[NATIVE.START_TYPE])
	var settings_id: String = String(by_type[NATIVE.SETTINGS_TYPE])
	if not _links_for(incoming, start_id).is_empty() or not _links_for(outgoing, start_id).is_empty():
		return _invalid("production context is renderer-owned and must not carry height-flow links")
	if not _links_for(incoming, settings_id).is_empty() or not _links_for(outgoing, settings_id).is_empty():
		return _invalid("global geomorph settings must remain a parameter-only node")

	var merge_id: String = String(by_type[NATIVE.MERGE_TYPE])
	var compose_id: String = String(by_type[NATIVE.COMPOSE_TYPE])
	var sculpt_id: String = String(by_type[NATIVE.SCULPT_TYPE])
	var add_id: String = String(by_type[NATIVE.ADD_TYPE])
	var output_id: String = String(by_type[NATIVE.OUTPUT_TYPE])
	var used_links: Dictionary = {}
	var active_stage_ids := PackedStringArray()
	var disabled_stage_ids := PackedStringArray()

	# Every additive/subtractive native stage is an independent contribution source.
	# A fixed merge input gives the wire semantic identity, so crossing two wires is
	# rejected instead of silently changing which production equation a port means.
	for stage_id: String in NATIVE.contribution_stage_ids():
		var stage_type: String = NATIVE.stage_node_type(stage_id)
		var stage_node_id: String = String(by_type[stage_type])
		if not _links_for(incoming, stage_node_id).is_empty():
			return _invalid("%s is a contribution source and cannot consume accumulated height" % stage_id.capitalize())
		var stage_out: Array = _links_for(outgoing, stage_node_id)
		if stage_out.is_empty():
			disabled_stage_ids.append(stage_id)
			continue
		if stage_out.size() != 1:
			return _invalid("%s contribution must connect to exactly one merge input" % stage_id.capitalize())
		var link: Dictionary = stage_out[0] as Dictionary
		var expected_port: int = NATIVE.merge_port_for_stage(stage_id)
		if int(link.get("from_port", -1)) != 0 \
				or String(link.get("to", "")) != merge_id \
				or int(link.get("to_port", -1)) != expected_port:
			return _invalid("%s must connect to its dedicated Native Detail Merge input" % stage_id.capitalize())
		used_links[_link_key(stage_node_id, 0, merge_id, expected_port)] = true
		active_stage_ids.append(stage_id)

	var merge_in: Array = _links_for(incoming, merge_id)
	if merge_in.size() != active_stage_ids.size():
		return _invalid("Native Detail Merge contains an unsupported or duplicate contribution input")

	var transform_ids: PackedStringArray = NATIVE.transform_stage_ids()
	if transform_ids.size() != 1:
		return _invalid("resident production schema must expose exactly one accumulated-height transform")
	var transform_stage_id: String = transform_ids[0]
	var transform_id: String = String(by_type[NATIVE.stage_node_type(transform_stage_id)])
	var merge_out: Array = _links_for(outgoing, merge_id)
	if merge_out.size() != 1:
		return _invalid("Native Detail Merge must have exactly one forward height connection")
	var merge_link: Dictionary = merge_out[0] as Dictionary
	if int(merge_link.get("from_port", -1)) != 0 or int(merge_link.get("to_port", -1)) != 0:
		return _invalid("Native Detail Merge output must use scalar port 0")

	var transform_in: Array = _links_for(incoming, transform_id)
	var transform_out: Array = _links_for(outgoing, transform_id)
	var merge_target: String = String(merge_link.get("to", ""))
	if merge_target == transform_id:
		if transform_in.size() != 1 or transform_out.size() != 1:
			return _invalid("Glacial Shaping must be one terminal accumulated-height transform")
		var transform_link: Dictionary = transform_out[0] as Dictionary
		if int(transform_link.get("from_port", -1)) != 0 \
				or String(transform_link.get("to", "")) != compose_id \
				or int(transform_link.get("to_port", -1)) != 0:
			return _invalid("Glacial Shaping must feed Macro + Detail directly")
		used_links[_link_key(merge_id, 0, transform_id, 0)] = true
		used_links[_link_key(transform_id, 0, compose_id, 0)] = true
		active_stage_ids.append(transform_stage_id)
	elif merge_target == compose_id:
		if not transform_in.is_empty() or not transform_out.is_empty():
			return _invalid("bypassed Glacial Shaping must be fully disconnected")
		used_links[_link_key(merge_id, 0, compose_id, 0)] = true
		disabled_stage_ids.append(transform_stage_id)
	else:
		return _invalid("Native Detail Merge must feed Glacial Shaping or Macro + Detail directly")

	var compose_in: Array = _links_for(incoming, compose_id)
	if compose_in.size() != 1:
		return _invalid("Macro + Detail must consume exactly one merged native height")

	var tail: Array = [
		[compose_id, 0, add_id, 0],
		[sculpt_id, 0, add_id, 1],
		[add_id, 0, output_id, 0],
	]
	for row_value: Variant in tail:
		var row: Array = row_value as Array
		var key: String = _link_key(String(row[0]), int(row[1]), String(row[2]), int(row[3]))
		if not exact_links.has(key):
			return _invalid("native production compose/sculpt/output tail is incomplete")
		used_links[key] = true

	if _links_for(outgoing, compose_id).size() != 1 \
			or not _links_for(incoming, sculpt_id).is_empty() \
			or _links_for(outgoing, sculpt_id).size() != 1 \
			or _links_for(incoming, add_id).size() != 2 \
			or _links_for(outgoing, add_id).size() != 1 \
			or _links_for(incoming, output_id).size() != 1 \
			or not _links_for(outgoing, output_id).is_empty():
		return _invalid("native production compose/sculpt/output tail contains an unsupported branch")

	if used_links.size() != links.size():
		return _invalid("native contribution graph contains an unsupported extra connection")

	var normalized_stage_ids := PackedStringArray()
	for stage_id: String in NATIVE.SCHEMA.ordered_stage_ids():
		if active_stage_ids.has(stage_id):
			normalized_stage_ids.append(stage_id)
	return {
		"valid": true,
		"canonical": disabled_stage_ids.is_empty(),
		"exact_equivalent": true,
		"reordered": false,
		"branching": true,
		"merge_semantics": true,
		"active_stage_ids": normalized_stage_ids,
		"normalized_stage_ids": normalized_stage_ids.duplicate(),
		"disabled_stage_ids": disabled_stage_ids,
		"execution_mode": "resident_contribution_merge",
		"reason": "",
	}


static func _linear_native_plan(graph: Resource, allow_commutative_reorder: bool) -> Dictionary:
	if graph == null:
		return _invalid("native production graph is unavailable")
	if int(graph.get(&"displacement_output_mode")) != NATIVE.OUTPUT_MODE_ABSOLUTE:
		return _invalid("native production graph must output absolute terrain height")

	var nodes_value: Variant = graph.get(&"nodes")
	var links_value: Variant = graph.get(&"links")
	if not (nodes_value is Array) or not (links_value is Array):
		return _invalid("native production graph arrays are malformed")

	var expected_types := PackedStringArray([NATIVE.START_TYPE, NATIVE.SETTINGS_TYPE])
	expected_types.append_array(NATIVE.native_stage_types())
	expected_types.append_array(PackedStringArray([
		NATIVE.COMPOSE_TYPE, NATIVE.SCULPT_TYPE, NATIVE.ADD_TYPE, NATIVE.OUTPUT_TYPE]))

	var by_type: Dictionary = {}
	var by_id: Dictionary = {}
	for node_value: Variant in nodes_value as Array:
		if not (node_value is Dictionary):
			return _invalid("native production graph contains an invalid node record")
		var node: Dictionary = node_value as Dictionary
		var node_id: String = String(node.get("id", ""))
		var node_type: String = String(node.get("type", ""))
		if node_id.is_empty():
			return _invalid("native production graph contains a node with no ID")
		if not expected_types.has(node_type):
			return _invalid("unsupported node %s is mixed into the native production chain" % node_type)
		if by_id.has(node_id):
			return _invalid("duplicate native node ID %s" % node_id)
		if by_type.has(node_type):
			return _invalid("duplicate native node type %s" % node_type)
		by_id[node_id] = node_type
		by_type[node_type] = node_id

	if by_type.size() != expected_types.size():
		return _invalid("native production graph is missing a required stage or terminal node")
	for node_type: String in expected_types:
		if not by_type.has(node_type) or String(by_type[node_type]).is_empty():
			return _invalid("native production graph is missing %s" % node_type)

	var links: Array = links_value as Array
	var outgoing: Dictionary = {}
	var exact_links: Dictionary = {}
	for link_value: Variant in links:
		if not (link_value is Dictionary):
			return _invalid("native production graph contains an invalid connection")
		var link: Dictionary = link_value as Dictionary
		var from_id: String = String(link.get("from", ""))
		var to_id: String = String(link.get("to", ""))
		var from_port: int = int(link.get("from_port", -1))
		var to_port: int = int(link.get("to_port", -1))
		if not by_id.has(from_id) or not by_id.has(to_id):
			return _invalid("native production graph contains a connection to a missing node")
		var exact_key: String = _link_key(from_id, from_port, to_id, to_port)
		if exact_links.has(exact_key):
			return _invalid("native production graph contains a duplicate connection")
		exact_links[exact_key] = true
		if not outgoing.has(from_id):
			outgoing[from_id] = []
		(outgoing[from_id] as Array).append(link)

	var used_links: Dictionary = {}
	var active_stage_ids := PackedStringArray()
	var current_id: String = String(by_type[NATIVE.START_TYPE])
	var previous_stage_index: int = -1
	var ordered_ids: PackedStringArray = NATIVE.SCHEMA.ordered_stage_ids()
	var traversal_guard: int = 0

	while current_id != String(by_type[NATIVE.COMPOSE_TYPE]):
		traversal_guard += 1
		if traversal_guard > expected_types.size():
			return _invalid("native production stage chain contains a loop")
		var next_links: Array = outgoing.get(current_id, []) as Array
		if next_links.size() != 1:
			return _invalid("native production stage chain must have exactly one forward connection")
		var link: Dictionary = next_links[0] as Dictionary
		if int(link.get("from_port", -1)) != 0 or int(link.get("to_port", -1)) != 0:
			return _invalid("native production stage flow must use scalar port 0")
		var next_id: String = String(link.get("to", ""))
		var next_type: String = String(by_id.get(next_id, ""))
		if next_type == NATIVE.COMPOSE_TYPE:
			used_links[_link_key(current_id, 0, next_id, 0)] = true
			current_id = next_id
			continue
		var stage_id: String = NATIVE.stage_id_for_node_type(next_type)
		var stage_index: int = ordered_ids.find(stage_id)
		if stage_index < 0:
			return _invalid("native stage flow reached non-stage node %s before composition" % next_type)
		if active_stage_ids.has(stage_id):
			return _invalid("native production stage %s appears more than once in the flow" % stage_id)
		if not allow_commutative_reorder and stage_index <= previous_stage_index:
			return _invalid("native production stages may be bypassed but not reordered")
		if allow_commutative_reorder and active_stage_ids.has(NONLINEAR_TERMINAL_STAGE):
			return _invalid("Glacial Shaping is nonlinear and must remain the final enabled native stage")
		previous_stage_index = stage_index
		active_stage_ids.append(stage_id)
		used_links[_link_key(current_id, 0, next_id, 0)] = true
		current_id = next_id

	var compose_id: String = String(by_type[NATIVE.COMPOSE_TYPE])
	var sculpt_id: String = String(by_type[NATIVE.SCULPT_TYPE])
	var add_id: String = String(by_type[NATIVE.ADD_TYPE])
	var output_id: String = String(by_type[NATIVE.OUTPUT_TYPE])
	var tail: Array = [
		[compose_id, 0, add_id, 0],
		[sculpt_id, 0, add_id, 1],
		[add_id, 0, output_id, 0],
	]
	for row_value: Variant in tail:
		var row: Array = row_value as Array
		var key: String = _link_key(String(row[0]), int(row[1]), String(row[2]), int(row[3]))
		if not exact_links.has(key):
			return _invalid("native production compose/sculpt/output tail is incomplete")
		used_links[key] = true

	if used_links.size() != links.size():
		return _invalid("native production graph contains a branch or unsupported extra connection")

	var disabled_stage_ids := PackedStringArray()
	var normalized_stage_ids := PackedStringArray()
	for stage_id: String in ordered_ids:
		if active_stage_ids.has(stage_id):
			normalized_stage_ids.append(stage_id)
		else:
			disabled_stage_ids.append(stage_id)

	var reordered: bool = not _packed_strings_equal(active_stage_ids, normalized_stage_ids)
	return {
		"valid": true,
		"canonical": disabled_stage_ids.is_empty() and not reordered,
		"exact_equivalent": true,
		"reordered": reordered,
		"branching": false,
		"merge_semantics": false,
		"active_stage_ids": active_stage_ids,
		"normalized_stage_ids": normalized_stage_ids,
		"disabled_stage_ids": disabled_stage_ids,
		"execution_mode": "resident_normalized_order",
		"reason": "",
	}


static func _links_for(adjacency: Dictionary, node_id: String) -> Array:
	return adjacency.get(node_id, []) as Array


static func _packed_strings_equal(a: PackedStringArray, b: PackedStringArray) -> bool:
	if a.size() != b.size():
		return false
	for index: int in a.size():
		if a[index] != b[index]:
			return false
	return true


static func _invalid(reason: String) -> Dictionary:
	return {
		"valid": false,
		"canonical": false,
		"exact_equivalent": false,
		"reordered": false,
		"branching": false,
		"merge_semantics": false,
		"active_stage_ids": PackedStringArray(),
		"normalized_stage_ids": PackedStringArray(),
		"disabled_stage_ids": PackedStringArray(),
		"execution_mode": "rejected",
		"reason": reason,
	}


static func _link_key(from_id: String, from_port: int, to_id: String, to_port: int) -> String:
	return "%s:%d>%s:%d" % [from_id, from_port, to_id, to_port]
