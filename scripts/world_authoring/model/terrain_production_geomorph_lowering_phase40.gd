class_name TerrainProductionGeomorphLoweringPhase40
extends RefCounted
## Phase 40 exact lowering for linear composition of resident production stages.
##
## Broad through Micro remain native resident-shader contribution sources. Their
## graph-visible flow may now form a restricted linear expression using:
## - ADD: contribution A + contribution B;
## - MULTIPLY: contribution * Constant Float in [0, 4];
## - MIX: mix(A, B, Constant Float in [0, 1]).
##
## The expression is never evaluated by a second terrain implementation. It is
## symbolically reduced to one non-negative coefficient per native contribution and
## those coefficients scale the exact existing production controls. Glacial remains
## the terminal nonlinear resident transform. Spatial/dynamic blend factors,
## contribution*contribution multiplication, negative coefficients, cycles and
## unsupported math stay candidate-only and cannot replace last-known-good terrain.

const BASE := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_lowering.gd")
const NATIVE := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")

const ADD_TYPE := "ADD"
const MULTIPLY_TYPE := "MULTIPLY"
const MIX_TYPE := "MIX"
const CONSTANT_TYPE := "CONSTANT_FLOAT"
const MIN_COEFFICIENT: float = 0.0
const MAX_COEFFICIENT: float = 4.0
const MAX_MULTIPLY_FACTOR: float = 4.0
const GROUP_MERGE_PORT_COUNT: int = 4
const COEFFICIENT_EPSILON: float = 0.000001


static func ordered_bypass_plan(graph: Resource) -> Dictionary:
	if NATIVE.has_merge_node(graph):
		return _branch_linear_plan(graph)
	return BASE.ordered_bypass_plan(graph)


static func commutative_reorder_plan(graph: Resource) -> Dictionary:
	if NATIVE.has_merge_node(graph):
		return _branch_linear_plan(graph)
	return BASE.commutative_reorder_plan(graph)


static func contribution_merge_plan(graph: Resource) -> Dictionary:
	return _branch_linear_plan(graph)


static func apply_bypass_controls(controls: Dictionary, plan: Dictionary) -> Dictionary:
	# Phase 39 already owns the exact control application contract. Phase 40 emits the
	# same disabled_stage_ids + scale_factors shape after symbolic reduction.
	return BASE.apply_bypass_controls(controls, plan)


static func disabled_control_overrides(stage_id: String) -> Dictionary:
	return BASE.disabled_control_overrides(stage_id)


static func scale_control_for_stage(stage_id: String) -> String:
	return BASE.scale_control_for_stage(stage_id)


static func group_merge_port_start() -> int:
	return NATIVE.contribution_stage_ids().size()


static func group_merge_port_count() -> int:
	return GROUP_MERGE_PORT_COUNT


static func is_group_merge_port(port: int) -> bool:
	return port >= group_merge_port_start() \
		and port < group_merge_port_start() + GROUP_MERGE_PORT_COUNT


static func _branch_linear_plan(graph: Resource) -> Dictionary:
	if graph == null:
		return _invalid("native production graph is unavailable")
	if int(graph.get(&"displacement_output_mode")) != NATIVE.OUTPUT_MODE_ABSOLUTE:
		return _invalid("native production graph must output absolute terrain height")

	var nodes_value: Variant = graph.get(&"nodes")
	var links_value: Variant = graph.get(&"links")
	if not (nodes_value is Array) or not (links_value is Array):
		return _invalid("native production graph arrays are malformed")

	var core_types := PackedStringArray([
		NATIVE.START_TYPE,
		NATIVE.SETTINGS_TYPE,
		NATIVE.MERGE_TYPE,
		NATIVE.COMPOSE_TYPE,
		NATIVE.SCULPT_TYPE,
		NATIVE.OUTPUT_TYPE,
	])
	core_types.append_array(NATIVE.native_stage_types())
	var utility_types := PackedStringArray([ADD_TYPE, MULTIPLY_TYPE, MIX_TYPE, CONSTANT_TYPE])

	var by_type: Dictionary = {}
	var by_id: Dictionary = {}
	var node_data_by_id: Dictionary = {}
	var utility_ids := PackedStringArray()
	for node_value: Variant in nodes_value as Array:
		if not (node_value is Dictionary):
			return _invalid("native contribution graph contains an invalid node record")
		var node: Dictionary = node_value as Dictionary
		var node_id: String = String(node.get("id", ""))
		var node_type: String = String(node.get("type", ""))
		if node_id.is_empty():
			return _invalid("native contribution graph contains a node with no ID")
		if by_id.has(node_id):
			return _invalid("duplicate native node ID %s" % node_id)
		if core_types.has(node_type):
			if by_type.has(node_type):
				return _invalid("duplicate native node type %s" % node_type)
			by_type[node_type] = node_id
		elif utility_types.has(node_type):
			utility_ids.append(node_id)
		else:
			return _invalid("unsupported node %s is mixed into Base Terrain linear composition" % node_type)
		by_id[node_id] = node_type
		node_data_by_id[node_id] = node

	if by_type.size() != core_types.size():
		return _invalid("native contribution graph is missing a required production stage or terminal node")
	for node_type: String in core_types:
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

	var output_id: String = String(by_type[NATIVE.OUTPUT_TYPE])
	var output_in: Array = _links_for(incoming, output_id)
	if output_in.size() != 1:
		return _invalid("Final Terrain must consume exactly one final height")
	var output_link: Dictionary = output_in[0] as Dictionary
	var final_add_id: String = String(output_link.get("from", ""))
	if String(by_id.get(final_add_id, "")) != ADD_TYPE \
			or int(output_link.get("from_port", -1)) != 0 \
			or int(output_link.get("to_port", -1)) != 0:
		return _invalid("Final Terrain must remain fed by the production terrain + sculpt Add")

	var start_id: String = String(by_type[NATIVE.START_TYPE])
	var settings_id: String = String(by_type[NATIVE.SETTINGS_TYPE])
	if not _links_for(incoming, start_id).is_empty() or not _links_for(outgoing, start_id).is_empty():
		return _invalid("production context is renderer-owned and must not carry height-flow links")
	if not _links_for(incoming, settings_id).is_empty() or not _links_for(outgoing, settings_id).is_empty():
		return _invalid("global geomorph settings must remain a parameter-only node")

	# Native additive/subtractive contribution stages are immutable leaves. Phase 40
	# deliberately uses expression trees rather than arbitrary DAG fan-out so one
	# native source cannot be duplicated invisibly through several branches.
	for stage_id: String in NATIVE.contribution_stage_ids():
		var stage_node_id: String = String(by_type[NATIVE.stage_node_type(stage_id)])
		if not _links_for(incoming, stage_node_id).is_empty():
			return _invalid("%s is a native contribution source and cannot consume height input" % stage_id.capitalize())
		if _links_for(outgoing, stage_node_id).size() > 1:
			return _invalid("%s may appear only once in the linear contribution tree" % stage_id.capitalize())

	var merge_id: String = String(by_type[NATIVE.MERGE_TYPE])
	var used_links: Dictionary = {}
	var expression_state: Dictionary = {
		"by_id": by_id,
		"node_data": node_data_by_id,
		"incoming": incoming,
		"outgoing": outgoing,
		"visiting": {},
		"cache": {},
		"used_nodes": {},
		"used_links": used_links,
		"final_add_id": final_add_id,
	}
	var merge_in: Array = _links_for(incoming, merge_id)
	var occupied_merge_ports: Dictionary = {}
	var claimed_stages: Dictionary = {}
	var linear_coefficients: Dictionary = {}

	for root_value: Variant in merge_in:
		var root_link: Dictionary = root_value as Dictionary
		if String(root_link.get("to", "")) != merge_id or int(root_link.get("from_port", -1)) != 0:
			return _invalid("Native Detail Merge inputs must consume scalar output port 0")
		var merge_port: int = int(root_link.get("to_port", -1))
		if occupied_merge_ports.has(merge_port):
			return _invalid("Native Detail Merge input %d is connected more than once" % merge_port)
		occupied_merge_ports[merge_port] = true

		var root_id: String = String(root_link.get("from", ""))
		var expression: Dictionary = _evaluate_height_expression(root_id, expression_state)
		if not bool(expression.get("valid", false)):
			return _invalid(String(expression.get("reason", "invalid linear contribution expression")))
		var stage_ids: PackedStringArray = expression.get("stage_ids", PackedStringArray()) as PackedStringArray
		if stage_ids.is_empty():
			return _invalid("Native Detail Merge received an expression with no terrain contribution")
		if stage_ids.size() == 1:
			var stage_id: String = stage_ids[0]
			var expected_port: int = NATIVE.merge_port_for_stage(stage_id)
			if merge_port != expected_port:
				return _invalid("%s or its scale path must return to its matching Merge socket" % stage_id.capitalize())
		else:
			if not is_group_merge_port(merge_port):
				return _invalid("combined Add/Mix contributions must feed one of the Custom Group Merge sockets")

		for stage_id: String in stage_ids:
			if claimed_stages.has(stage_id):
				return _invalid("%s appears in more than one Native Detail Merge expression" % stage_id.capitalize())
			claimed_stages[stage_id] = merge_port
		var coefficients: Dictionary = expression.get("coefficients", {}) as Dictionary
		for stage_value: Variant in coefficients.keys():
			linear_coefficients[String(stage_value)] = float(coefficients[stage_value])
		_mark_link_used(root_link, used_links)

	# Every generic utility inside Base Terrain must participate in the one validated
	# linear expression forest. The production final Add is structural and excluded.
	var used_nodes: Dictionary = expression_state.get("used_nodes", {}) as Dictionary
	for utility_id: String in utility_ids:
		if utility_id == final_add_id:
			continue
		if not used_nodes.has(utility_id):
			return _invalid("Add, Multiply, Mix and Constant Float nodes in Base Terrain must belong to a complete linear contribution path")

	var contribution_ids: PackedStringArray = NATIVE.contribution_stage_ids()
	var active_stage_ids := PackedStringArray()
	var disabled_stage_ids := PackedStringArray()
	var scale_factors: Dictionary = {}
	for stage_id: String in contribution_ids:
		var coefficient: float = float(linear_coefficients.get(stage_id, 0.0))
		if is_nan(coefficient) or is_inf(coefficient) \
				or coefficient < MIN_COEFFICIENT - COEFFICIENT_EPSILON \
				or coefficient > MAX_COEFFICIENT + COEFFICIENT_EPSILON:
			return _invalid("%s resolves to coefficient %.4f; resident lowering supports only %.1f..%.1f" \
				% [stage_id.capitalize(), coefficient, MIN_COEFFICIENT, MAX_COEFFICIENT])
		coefficient = clampf(coefficient, MIN_COEFFICIENT, MAX_COEFFICIENT)
		linear_coefficients[stage_id] = coefficient
		if coefficient <= COEFFICIENT_EPSILON:
			disabled_stage_ids.append(stage_id)
		elif scale_control_for_stage(stage_id).is_empty():
			return _invalid("%s has no exact resident-shader coefficient lowering" % stage_id.capitalize())
		else:
			active_stage_ids.append(stage_id)
			if not is_equal_approx(coefficient, 1.0):
				scale_factors[stage_id] = coefficient

	# Glacial remains a single nonlinear transform of the already-reduced native sum.
	var transform_ids: PackedStringArray = NATIVE.transform_stage_ids()
	if transform_ids.size() != 1:
		return _invalid("resident production schema must expose exactly one accumulated-height transform")
	var transform_stage_id: String = transform_ids[0]
	var transform_id: String = String(by_type[NATIVE.stage_node_type(transform_stage_id)])
	var compose_id: String = String(by_type[NATIVE.COMPOSE_TYPE])
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
			return _invalid("Glacial Shaping must remain one terminal accumulated-height transform")
		var transform_link: Dictionary = transform_out[0] as Dictionary
		if int(transform_link.get("from_port", -1)) != 0 \
				or String(transform_link.get("to", "")) != compose_id \
				or int(transform_link.get("to_port", -1)) != 0:
			return _invalid("Glacial Shaping must feed Macro + Detail directly")
		_mark_link_used(merge_link, used_links)
		_mark_link_used(transform_link, used_links)
		active_stage_ids.append(transform_stage_id)
	elif merge_target == compose_id:
		if not transform_in.is_empty() or not transform_out.is_empty():
			return _invalid("bypassed Glacial Shaping must be fully disconnected")
		_mark_link_used(merge_link, used_links)
		disabled_stage_ids.append(transform_stage_id)
	else:
		return _invalid("Native Detail Merge must feed Glacial Shaping or Macro + Detail directly")

	var compose_in: Array = _links_for(incoming, compose_id)
	if compose_in.size() != 1:
		return _invalid("Macro + Detail must consume exactly one merged native height")

	var sculpt_id: String = String(by_type[NATIVE.SCULPT_TYPE])
	var tail: Array = [
		[compose_id, 0, final_add_id, 0],
		[sculpt_id, 0, final_add_id, 1],
		[final_add_id, 0, output_id, 0],
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
			or _links_for(incoming, final_add_id).size() != 2 \
			or _links_for(outgoing, final_add_id).size() != 1 \
			or _links_for(incoming, output_id).size() != 1 \
			or not _links_for(outgoing, output_id).is_empty():
		return _invalid("native production compose/sculpt/output tail contains an unsupported branch")

	if used_links.size() != links.size():
		return _invalid("native contribution graph contains an unsupported extra connection or cycle")

	var typed_composition: bool = false
	var has_add: bool = false
	var has_multiply: bool = false
	var has_mix: bool = false
	for utility_value: Variant in used_nodes.keys():
		var utility_id: String = String(utility_value)
		match String(by_id.get(utility_id, "")):
			ADD_TYPE:
				typed_composition = true
				has_add = true
			MULTIPLY_TYPE:
				typed_composition = true
				has_multiply = true
			MIX_TYPE:
				typed_composition = true
				has_mix = true

	var canonical: bool = not typed_composition and disabled_stage_ids.is_empty()
	var execution_mode: String = "resident_contribution_merge"
	if typed_composition:
		execution_mode = "resident_contribution_linear" if has_add or has_mix \
			else "resident_contribution_scale"
	return {
		"valid": true,
		"canonical": canonical,
		"exact_equivalent": true,
		"reordered": false,
		"branching": true,
		"merge_semantics": true,
		"typed_composition": typed_composition,
		"typed_linear_composition": typed_composition,
		"has_add": has_add,
		"has_multiply": has_multiply,
		"has_mix": has_mix,
		"linear_coefficients": linear_coefficients.duplicate(true),
		"scale_factors": scale_factors.duplicate(true),
		"active_stage_ids": active_stage_ids,
		"normalized_stage_ids": active_stage_ids.duplicate(),
		"disabled_stage_ids": disabled_stage_ids,
		"execution_mode": execution_mode,
		"group_merge_port_start": group_merge_port_start(),
		"group_merge_port_count": GROUP_MERGE_PORT_COUNT,
		"reason": "",
	}


static func _evaluate_height_expression(node_id: String, state: Dictionary) -> Dictionary:
	var by_id: Dictionary = state.get("by_id", {}) as Dictionary
	var node_type: String = String(by_id.get(node_id, ""))
	var stage_id: String = NATIVE.stage_id_for_node_type(node_type)
	if NATIVE.contribution_stage_ids().has(stage_id):
		return {
			"valid": true,
			"coefficients": {stage_id:1.0},
			"stage_ids": PackedStringArray([stage_id]),
		}
	if node_type == CONSTANT_TYPE:
		return _expr_invalid("Constant Float is a factor, not a terrain contribution")
	if node_type != ADD_TYPE and node_type != MULTIPLY_TYPE and node_type != MIX_TYPE:
		return _expr_invalid("%s cannot participate in native linear contribution math" % node_type)
	if node_id == String(state.get("final_add_id", "")):
		return _expr_invalid("the production terrain + sculpt Add cannot be reused inside native contribution math")

	var visiting: Dictionary = state.get("visiting", {}) as Dictionary
	if visiting.has(node_id):
		return _expr_invalid("native linear contribution math contains a cycle")
	var cache: Dictionary = state.get("cache", {}) as Dictionary
	if cache.has(node_id):
		return (cache[node_id] as Dictionary).duplicate(true)
	visiting[node_id] = true

	var incoming: Dictionary = state.get("incoming", {}) as Dictionary
	var outgoing: Dictionary = state.get("outgoing", {}) as Dictionary
	if _links_for(outgoing, node_id).size() != 1:
		return _expr_invalid("%s must have exactly one forward connection inside Base Terrain" % _friendly_math_name(node_type))

	var result: Dictionary = {}
	match node_type:
		ADD_TYPE:
			result = _evaluate_add(node_id, state)
		MULTIPLY_TYPE:
			result = _evaluate_multiply(node_id, state)
		MIX_TYPE:
			result = _evaluate_mix(node_id, state)
		_:
			result = _expr_invalid("unsupported native linear math node")
	if not bool(result.get("valid", false)):
		return result

	visiting.erase(node_id)
	var used_nodes: Dictionary = state.get("used_nodes", {}) as Dictionary
	used_nodes[node_id] = true
	cache[node_id] = result.duplicate(true)
	return result


static func _evaluate_add(node_id: String, state: Dictionary) -> Dictionary:
	var incoming: Dictionary = state.get("incoming", {}) as Dictionary
	var inputs: Array = _links_for(incoming, node_id)
	if inputs.size() != 2:
		return _expr_invalid("Add Contributions needs exactly two terrain inputs")
	var a_link: Dictionary = _input_link(inputs, 0)
	var b_link: Dictionary = _input_link(inputs, 1)
	if a_link.is_empty() or b_link.is_empty():
		return _expr_invalid("Add Contributions requires input A and input B")
	var a: Dictionary = _evaluate_expression_input(a_link, state)
	if not bool(a.get("valid", false)):
		return a
	var b: Dictionary = _evaluate_expression_input(b_link, state)
	if not bool(b.get("valid", false)):
		return b
	var overlap: String = _first_overlap(
		a.get("stage_ids", PackedStringArray()) as PackedStringArray,
		b.get("stage_ids", PackedStringArray()) as PackedStringArray)
	if not overlap.is_empty():
		return _expr_invalid("%s is used twice inside one Add tree; use Multiply to scale a contribution instead" % overlap.capitalize())
	return _combine_linear(a, 1.0, b, 1.0)


static func _evaluate_multiply(node_id: String, state: Dictionary) -> Dictionary:
	var incoming: Dictionary = state.get("incoming", {}) as Dictionary
	var inputs: Array = _links_for(incoming, node_id)
	if inputs.size() != 2:
		return _expr_invalid("Scale Contribution needs exactly one terrain input and one Constant Float")
	var a_link: Dictionary = _input_link(inputs, 0)
	var b_link: Dictionary = _input_link(inputs, 1)
	if a_link.is_empty() or b_link.is_empty():
		return _expr_invalid("Scale Contribution requires input A and input B")
	var by_id: Dictionary = state.get("by_id", {}) as Dictionary
	var a_is_constant: bool = String(by_id.get(String(a_link.get("from", "")), "")) == CONSTANT_TYPE
	var b_is_constant: bool = String(by_id.get(String(b_link.get("from", "")), "")) == CONSTANT_TYPE
	if a_is_constant == b_is_constant:
		return _expr_invalid("Scale Contribution must multiply terrain by one Constant Float; terrain × terrain is nonlinear and unsupported")
	var constant_link: Dictionary = a_link if a_is_constant else b_link
	var terrain_link: Dictionary = b_link if a_is_constant else a_link
	var factor_result: Dictionary = _constant_value(constant_link, state, 0.0, MAX_MULTIPLY_FACTOR,
		"Scale Factor")
	if not bool(factor_result.get("valid", false)):
		return factor_result
	var terrain: Dictionary = _evaluate_expression_input(terrain_link, state)
	if not bool(terrain.get("valid", false)):
		return terrain
	return _scale_linear(terrain, float(factor_result.get("value", 0.0)))


static func _evaluate_mix(node_id: String, state: Dictionary) -> Dictionary:
	var incoming: Dictionary = state.get("incoming", {}) as Dictionary
	var inputs: Array = _links_for(incoming, node_id)
	if inputs.size() != 3:
		return _expr_invalid("Blend Contributions needs terrain A, terrain B and one Constant Float blend amount")
	var a_link: Dictionary = _input_link(inputs, 0)
	var b_link: Dictionary = _input_link(inputs, 1)
	var factor_link: Dictionary = _input_link(inputs, 2)
	if a_link.is_empty() or b_link.is_empty() or factor_link.is_empty():
		return _expr_invalid("Blend Contributions requires A, B and Blend Amount")
	var factor_result: Dictionary = _constant_value(factor_link, state, 0.0, 1.0,
		"Blend Amount")
	if not bool(factor_result.get("valid", false)):
		return factor_result
	var a: Dictionary = _evaluate_expression_input(a_link, state)
	if not bool(a.get("valid", false)):
		return a
	var b: Dictionary = _evaluate_expression_input(b_link, state)
	if not bool(b.get("valid", false)):
		return b
	var overlap: String = _first_overlap(
		a.get("stage_ids", PackedStringArray()) as PackedStringArray,
		b.get("stage_ids", PackedStringArray()) as PackedStringArray)
	if not overlap.is_empty():
		return _expr_invalid("%s is used on both sides of one Blend tree" % overlap.capitalize())
	var factor: float = float(factor_result.get("value", 0.0))
	return _combine_linear(a, 1.0 - factor, b, factor)


static func _evaluate_expression_input(link: Dictionary, state: Dictionary) -> Dictionary:
	if int(link.get("from_port", -1)) != 0:
		return _expr_invalid("native linear math inputs must consume scalar output port 0")
	var used_links: Dictionary = state.get("used_links", {}) as Dictionary
	_mark_link_used(link, used_links)
	return _evaluate_height_expression(String(link.get("from", "")), state)


static func _constant_value(link: Dictionary, state: Dictionary, minimum: float,
		maximum: float, label: String) -> Dictionary:
	if int(link.get("from_port", -1)) != 0:
		return _expr_invalid("%s must use Constant Float output port 0" % label)
	var constant_id: String = String(link.get("from", ""))
	var by_id: Dictionary = state.get("by_id", {}) as Dictionary
	if String(by_id.get(constant_id, "")) != CONSTANT_TYPE:
		return _expr_invalid("%s must come from Constant Float" % label)
	var incoming: Dictionary = state.get("incoming", {}) as Dictionary
	var outgoing: Dictionary = state.get("outgoing", {}) as Dictionary
	if not _links_for(incoming, constant_id).is_empty() or _links_for(outgoing, constant_id).size() != 1:
		return _expr_invalid("%s Constant Float must be dedicated to one linear operation" % label)
	var node_data: Dictionary = state.get("node_data", {}) as Dictionary
	var constant_node: Dictionary = node_data.get(constant_id, {}) as Dictionary
	var parameters: Dictionary = constant_node.get("parameters", {}) as Dictionary
	var value: float = float(parameters.get("value", 0.0))
	if is_nan(value) or is_inf(value) or value < minimum or value > maximum:
		return _expr_invalid("%s must be finite and between %.2f and %.2f" % [label, minimum, maximum])
	var used_nodes: Dictionary = state.get("used_nodes", {}) as Dictionary
	used_nodes[constant_id] = true
	var used_links: Dictionary = state.get("used_links", {}) as Dictionary
	_mark_link_used(link, used_links)
	return {"valid":true, "value":value}


static func _combine_linear(a: Dictionary, a_scale: float,
		b: Dictionary, b_scale: float) -> Dictionary:
	var coefficients: Dictionary = {}
	for stage_value: Variant in (a.get("coefficients", {}) as Dictionary).keys():
		var stage_id: String = String(stage_value)
		coefficients[stage_id] = float((a.get("coefficients", {}) as Dictionary)[stage_value]) * a_scale
	for stage_value: Variant in (b.get("coefficients", {}) as Dictionary).keys():
		var stage_id: String = String(stage_value)
		coefficients[stage_id] = float(coefficients.get(stage_id, 0.0)) \
			+ float((b.get("coefficients", {}) as Dictionary)[stage_value]) * b_scale
	var stage_ids := PackedStringArray()
	for stage_id: String in NATIVE.contribution_stage_ids():
		if (a.get("stage_ids", PackedStringArray()) as PackedStringArray).has(stage_id) \
				or (b.get("stage_ids", PackedStringArray()) as PackedStringArray).has(stage_id):
			stage_ids.append(stage_id)
	return {"valid":true, "coefficients":coefficients, "stage_ids":stage_ids}


static func _scale_linear(value: Dictionary, factor: float) -> Dictionary:
	var coefficients: Dictionary = {}
	var source: Dictionary = value.get("coefficients", {}) as Dictionary
	for stage_value: Variant in source.keys():
		coefficients[String(stage_value)] = float(source[stage_value]) * factor
	return {
		"valid": true,
		"coefficients": coefficients,
		"stage_ids": (value.get("stage_ids", PackedStringArray()) as PackedStringArray).duplicate(),
	}


static func _first_overlap(a: PackedStringArray, b: PackedStringArray) -> String:
	for stage_id: String in a:
		if b.has(stage_id):
			return stage_id
	return ""


static func _input_link(inputs: Array, port: int) -> Dictionary:
	var found: Dictionary = {}
	for link_value: Variant in inputs:
		var link: Dictionary = link_value as Dictionary
		if int(link.get("to_port", -1)) != port:
			continue
		if not found.is_empty():
			return {}
		found = link
	return found


static func _mark_link_used(link: Dictionary, used_links: Dictionary) -> void:
	used_links[_link_key(
		String(link.get("from", "")), int(link.get("from_port", -1)),
		String(link.get("to", "")), int(link.get("to_port", -1)))] = true


static func _links_for(adjacency: Dictionary, node_id: String) -> Array:
	return adjacency.get(node_id, []) as Array


static func _friendly_math_name(node_type: String) -> String:
	match node_type:
		ADD_TYPE: return "Add Contributions"
		MULTIPLY_TYPE: return "Scale Contribution"
		MIX_TYPE: return "Blend Contributions"
	return node_type


static func _expr_invalid(reason: String) -> Dictionary:
	return {"valid":false, "reason":reason}


static func _invalid(reason: String) -> Dictionary:
	return {
		"valid": false,
		"canonical": false,
		"exact_equivalent": false,
		"reordered": false,
		"branching": false,
		"merge_semantics": false,
		"typed_composition": false,
		"typed_linear_composition": false,
		"has_add": false,
		"has_multiply": false,
		"has_mix": false,
		"linear_coefficients": {},
		"scale_factors": {},
		"active_stage_ids": PackedStringArray(),
		"normalized_stage_ids": PackedStringArray(),
		"disabled_stage_ids": PackedStringArray(),
		"execution_mode": "rejected",
		"group_merge_port_start": group_merge_port_start(),
		"group_merge_port_count": GROUP_MERGE_PORT_COUNT,
		"reason": reason,
	}


static func _link_key(from_id: String, from_port: int, to_id: String, to_port: int) -> String:
	return "%s:%d>%s:%d" % [from_id, from_port, to_id, to_port]
