extends "res://scripts/world_authoring/terrain_displacement_runtime_phase30.gd"
## Phase 31: explicit production-stage Shape nodes.
##
## These node types are aliases for the exact generated-height and sparse sculpt
## inputs already used by Phase 30. Keeping them as dedicated types makes the old
## renderer-owned displacement visible/editable without changing GPU/contact math.


func _is_identity_shape_slot(slot: Resource) -> bool:
	if String(slot.get(&"slot_id")) != PRODUCTION_SHAPE_SLOT_ID:
		return false
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null or int(graph.get(&"displacement_output_mode")) != GRAPH_OUTPUT_ABSOLUTE_HEIGHT:
		return false
	var nodes: Array = graph.get(&"nodes") as Array
	var links: Array = graph.get(&"links") as Array
	if nodes.size() != 4 or links.size() != 3:
		return false
	var generated_id: String = ""
	var sculpt_id: String = ""
	var add_id: String = ""
	var output_id: String = ""
	for node_value: Variant in nodes:
		var node: Dictionary = node_value as Dictionary
		var node_type: String = String(node.get("type", ""))
		var node_id: String = String(node.get("id", ""))
		match node_type:
			"PRODUCTION_GENERATED_HEIGHT": generated_id = node_id
			"PRODUCTION_SCULPT_DELTA": sculpt_id = node_id
			"GAME_INPUT":
				var parameters: Dictionary = node.get("parameters", {}) as Dictionary
				var source: String = String(parameters.get("source", ""))
				if source == "generated_height_m": generated_id = node_id
				elif source == "sculpt_delta_m": sculpt_id = node_id
				else: return false
			"ADD": add_id = node_id
			"OUTPUT_DISPLACEMENT": output_id = node_id
			_: return false
	if generated_id.is_empty() or sculpt_id.is_empty() or add_id.is_empty() or output_id.is_empty():
		return false
	var expected: Dictionary = {
		"%s:0" % add_id: generated_id,
		"%s:1" % add_id: sculpt_id,
		"%s:0" % output_id: add_id,
	}
	for link_value: Variant in links:
		var link: Dictionary = link_value as Dictionary
		var key: String = "%s:%d" % [String(link.get("to", "")), int(link.get("to_port", -1))]
		if String(expected.get(key, "")) != String(link.get("from", "")):
			return false
		expected.erase(key)
	return expected.is_empty()


func _compile_node(node_id: String, nodes: Dictionary, inputs: Dictionary,
		memo: Dictionary, visiting: Dictionary, graph_seed: int) -> int:
	if memo.has(node_id):
		return int(memo[node_id])
	if nodes.has(node_id):
		var node: Dictionary = nodes[node_id] as Dictionary
		match String(node.get("type", "")):
			"PRODUCTION_GENERATED_HEIGHT":
				var generated: int = _append_instruction(OP_INPUT_BASE_HEIGHT)
				memo[node_id] = generated
				return generated
			"PRODUCTION_SCULPT_DELTA":
				var sculpt: int = _append_instruction(OP_INPUT_SCULPT_DELTA)
				memo[node_id] = sculpt
				return sculpt
			"SATURATE":
				var source: int = _compile_input(node_id, 0, nodes, inputs,
					memo, visiting, graph_seed)
				var zero: int = _append_instruction(OP_CONST, -1, -1, -1, Vector4.ZERO)
				var one: int = _append_instruction(OP_CONST, -1, -1, -1, Vector4(1.0, 0.0, 0.0, 0.0))
				var saturated: int = _append_instruction(OP_CLAMP, source, zero, one)
				memo[node_id] = saturated
				return saturated
			"ONE_MINUS":
				var source: int = _compile_input(node_id, 0, nodes, inputs,
					memo, visiting, graph_seed)
				var one: int = _append_instruction(OP_CONST, -1, -1, -1, Vector4(1.0, 0.0, 0.0, 0.0))
				var inverted: int = _append_instruction(OP_SUB, one, source)
				memo[node_id] = inverted
				return inverted
	return super._compile_node(node_id, nodes, inputs, memo, visiting, graph_seed)
