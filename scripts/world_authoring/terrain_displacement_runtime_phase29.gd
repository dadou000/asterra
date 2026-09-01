extends "res://scripts/world_authoring/terrain_displacement_runtime.gd"
## Phase 29 displacement compiler bridge.
##
## Legacy authored graphs produce additive displacement deltas. The canonical
## production graph is different: it exposes the renderer's current terrain height
## as an ordinary editable node and writes an absolute final height. Convert that
## absolute result back to a delta here so the existing watertight clipmap sampling,
## GPU bytecode path and CPU/contact evaluator stay unchanged.

const GRAPH_OUTPUT_DELTA: int = 0
const GRAPH_OUTPUT_ABSOLUTE_HEIGHT: int = 1


func _compile_graph(graph: Resource, graph_seed_index: int) -> int:
	var nodes_value: Variant = graph.get(&"nodes")
	var links_value: Variant = graph.get(&"links")
	if not (nodes_value is Array) or not (links_value is Array):
		_warnings.append("Malformed displacement graph arrays.")
		return -1

	var nodes_by_id: Dictionary = {}
	var output_id: String = ""
	for node_value: Variant in nodes_value as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		var node_id: String = String(node.get("id", ""))
		if node_id.is_empty():
			continue
		nodes_by_id[node_id] = node
		if String(node.get("type", "")) == "OUTPUT_DISPLACEMENT":
			output_id = node_id
	if output_id.is_empty():
		_warnings.append("Displacement graph has no OUTPUT_DISPLACEMENT node.")
		return -1

	var input_sources: Dictionary = {}
	for link_value: Variant in links_value as Array:
		if not (link_value is Dictionary):
			continue
		var link: Dictionary = link_value as Dictionary
		var to_id: String = String(link.get("to", ""))
		var to_port: int = int(link.get("to_port", 0))
		var from_id: String = String(link.get("from", ""))
		if not to_id.is_empty() and not from_id.is_empty():
			input_sources["%s:%d" % [to_id, to_port]] = from_id

	var output_source: String = String(input_sources.get("%s:0" % output_id, ""))
	var value_index: int = -1
	if output_source.is_empty():
		# For an absolute production graph, disconnecting Final Terrain really means
		# zero height. For a legacy delta graph the same disconnected output remains
		# the historical zero-delta behaviour.
		value_index = _append_instruction(OP_CONST, -1, -1, -1, Vector4.ZERO)
	else:
		var memo: Dictionary = {}
		var visiting: Dictionary = {}
		var graph_seed: int = abs(String(graph.get(&"graph_id")).hash() \
			^ (graph_seed_index * 97531)) & 0x7fffffff
		value_index = _compile_node(output_source, nodes_by_id, input_sources,
			memo, visiting, graph_seed)
	if value_index < 0:
		return -1

	if int(graph.get(&"displacement_output_mode")) == GRAPH_OUTPUT_ABSOLUTE_HEIGHT:
		# The clipmap still adds authored output to the already rendered production
		# height. Subtract that production height here, making the graph itself the
		# authoritative final-height editor without changing the renderer/contact ABI.
		var production_height: int = _append_instruction(OP_INPUT_BASE_HEIGHT)
		if production_height < 0:
			return -1
		value_index = _append_instruction(OP_SUB, value_index, production_height)
	return value_index


func profile_fingerprint(terrain: Resource) -> String:
	var base: String = super.profile_fingerprint(terrain)
	if terrain == null:
		return base
	var modes := PackedStringArray()
	var slots_value: Variant = terrain.get(&"displacement_slots")
	if slots_value is Array:
		for slot_value: Variant in slots_value as Array:
			var slot: Resource = slot_value as Resource
			var graph: Resource = slot.get(&"graph") as Resource if slot != null else null
			if graph != null:
				modes.append("%s=%d" % [String(slot.get(&"slot_id")),
					int(graph.get(&"displacement_output_mode"))])
	return base + "|output_modes:" + ",".join(modes)
