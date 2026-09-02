extends Node
## Phase 40 regression for exact native Add / Scale / Blend symbolic lowering.

const TERRAIN := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const SLOT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const NATIVE := preload("res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")
const LOWERING := preload("res://scripts/world_authoring/model/terrain_production_geomorph_lowering_phase40.gd")
const PHASE39 := preload("res://scripts/world_authoring/model/terrain_production_geomorph_lowering.gd")
const RUNTIME := preload("res://scripts/world_authoring/terrain_displacement_runtime_phase37.gd")


func _ready() -> void:
	var error: String = _validate()
	if not error.is_empty():
		push_error("NATIVE_LINEAR_COMPOSITION_PHASE40_FAILED: " + error)
		get_tree().quit(1)
		return
	print("NATIVE_LINEAR_COMPOSITION_PHASE40_OK: typed Add/Mix/nested scale reduce to exact resident coefficients; nonlinear and invalid candidates roll back")
	get_tree().quit(0)


func _validate() -> String:
	var terrain: Resource = TERRAIN.new()
	terrain.call("ensure_valid")
	var slot: Resource = terrain.call("create_shader_slot",
		SLOT.Domain.DISPLACEMENT, "Base Terrain Shape") as Resource
	if slot == null:
		return "could not create Base Terrain Shape slot"
	slot.set(&"slot_id", NATIVE.PRODUCTION_SHAPE_SLOT_ID)
	slot.set(&"enabled", true)
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return "Base Terrain Shape has no graph"
	NATIVE.build_canonical_graph(graph)

	var mountain_id: String = _find_type(graph, NATIVE.stage_node_type("mountain"))
	var mid_id: String = _find_type(graph, NATIVE.stage_node_type("mid"))
	if mountain_id.is_empty() or mid_id.is_empty():
		return "canonical graph is missing Mountain or Mid contribution"
	graph.call("set_node_parameter", mountain_id, "mountain_strength", 2.0)
	graph.call("set_node_parameter", mid_id, "mid_strength", 1.5)

	var runtime: Node = RUNTIME.new() as Node
	var baseline: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not _resident_zero_bytecode(baseline):
		runtime.free()
		return "canonical baseline did not remain resident zero-bytecode terrain"
	var baseline_guard: float = float((baseline.get("displacement_envelope", {}) as Dictionary).get(
		"production_max_abs_m", -1.0))

	# ADD: grouping two native contributions changes graph structure but not terrain.
	var controls: Dictionary = NATIVE.extract_controls(graph)
	NATIVE.build_canonical_graph(graph, controls)
	mountain_id = _find_type(graph, NATIVE.stage_node_type("mountain"))
	mid_id = _find_type(graph, NATIVE.stage_node_type("mid"))
	var merge_id: String = _find_type(graph, NATIVE.MERGE_TYPE)
	var add_id: String = String(graph.call("add_node", "ADD", Vector2(650.0, 350.0), {}))
	if not _disconnect_stage(graph, mountain_id, "mountain", merge_id) \
			or not _disconnect_stage(graph, mid_id, "mid", merge_id) \
			or not bool(graph.call("connect_nodes", mountain_id, 0, add_id, 0)) \
			or not bool(graph.call("connect_nodes", mid_id, 0, add_id, 1)) \
			or not bool(graph.call("connect_nodes", add_id, 0, merge_id,
				LOWERING.group_merge_port_start())):
		runtime.free()
		return "could not construct Mountain + Mid grouped Add"
	var add_plan: Dictionary = LOWERING.contribution_merge_plan(graph)
	if not bool(add_plan.get("valid", false)) \
			or not bool(add_plan.get("has_add", false)) \
			or String(add_plan.get("execution_mode", "")) != "resident_contribution_linear":
		runtime.free()
		return "grouped Add was not recognized as exact resident linear composition"
	if bool(PHASE39.contribution_merge_plan(graph).get("valid", false)):
		runtime.free()
		return "Phase 39 unexpectedly accepted the new grouped Add topology"
	var add_coeff: Dictionary = add_plan.get("linear_coefficients", {}) as Dictionary
	if not is_equal_approx(float(add_coeff.get("mountain", -1.0)), 1.0) \
			or not is_equal_approx(float(add_coeff.get("mid", -1.0)), 1.0):
		runtime.free()
		return "grouped Add did not reduce to unit Mountain + Mid coefficients"
	var added: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not _resident_zero_bytecode(added):
		runtime.free()
		return "grouped Add left the resident zero-bytecode path"
	var added_controls: Dictionary = added.get("production_geomorph_controls", {}) as Dictionary
	if not is_equal_approx(float(added_controls.get("mountain_strength", 0.0)), 2.0) \
			or not is_equal_approx(float(added_controls.get("mid_strength", 0.0)), 1.5):
		runtime.free()
		return "grouped Add changed resident production controls"

	# MIX: mix(Mountain, Mid, 0.25) = 0.75 Mountain + 0.25 Mid.
	controls = NATIVE.extract_controls(graph)
	NATIVE.build_canonical_graph(graph, controls)
	mountain_id = _find_type(graph, NATIVE.stage_node_type("mountain"))
	mid_id = _find_type(graph, NATIVE.stage_node_type("mid"))
	merge_id = _find_type(graph, NATIVE.MERGE_TYPE)
	var mix_id: String = String(graph.call("add_node", "MIX", Vector2(650.0, 350.0), {}))
	var blend_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(500.0, 500.0), {"value":0.25}))
	if not _disconnect_stage(graph, mountain_id, "mountain", merge_id) \
			or not _disconnect_stage(graph, mid_id, "mid", merge_id) \
			or not bool(graph.call("connect_nodes", mountain_id, 0, mix_id, 0)) \
			or not bool(graph.call("connect_nodes", mid_id, 0, mix_id, 1)) \
			or not bool(graph.call("connect_nodes", blend_id, 0, mix_id, 2)) \
			or not bool(graph.call("connect_nodes", mix_id, 0, merge_id,
				LOWERING.group_merge_port_start())):
		runtime.free()
		return "could not construct constant Mountain/Mid blend"
	var mix_plan: Dictionary = LOWERING.contribution_merge_plan(graph)
	var mix_coeff: Dictionary = mix_plan.get("linear_coefficients", {}) as Dictionary
	if not bool(mix_plan.get("valid", false)) or not bool(mix_plan.get("has_mix", false)) \
			or not is_equal_approx(float(mix_coeff.get("mountain", -1.0)), 0.75) \
			or not is_equal_approx(float(mix_coeff.get("mid", -1.0)), 0.25):
		runtime.free()
		return "constant Mix did not reduce to 0.75 Mountain + 0.25 Mid"
	var mixed: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not _resident_zero_bytecode(mixed):
		runtime.free()
		return "constant Mix left the resident zero-bytecode path"
	var mixed_controls: Dictionary = mixed.get("production_geomorph_controls", {}) as Dictionary
	if not is_equal_approx(float(mixed_controls.get("mountain_strength", 0.0)), 1.5) \
			or not is_equal_approx(float(mixed_controls.get("mid_strength", 0.0)), 0.375):
		runtime.free()
		return "Mix coefficients did not reach resident Mountain/Mid controls exactly"
	var mixed_guard: float = float((mixed.get("displacement_envelope", {}) as Dictionary).get(
		"production_max_abs_m", -1.0))
	if mixed_guard < 0.0 or mixed_guard >= baseline_guard:
		runtime.free()
		return "Mix did not update conservative production displacement bounds"

	# Nest Scale after Blend. Symbolic reduction must multiply both coefficients once.
	var scale_id: String = String(graph.call("add_node", "MULTIPLY", Vector2(830.0, 350.0), {}))
	var scale_factor_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(700.0, 570.0), {"value":2.0}))
	if not bool(graph.call("disconnect_nodes", mix_id, 0, merge_id,
			LOWERING.group_merge_port_start())) \
			or not bool(graph.call("connect_nodes", mix_id, 0, scale_id, 0)) \
			or not bool(graph.call("connect_nodes", scale_factor_id, 0, scale_id, 1)) \
			or not bool(graph.call("connect_nodes", scale_id, 0, merge_id,
				LOWERING.group_merge_port_start())):
		runtime.free()
		return "could not construct nested Blend -> Scale expression"
	var nested_plan: Dictionary = LOWERING.contribution_merge_plan(graph)
	var nested_coeff: Dictionary = nested_plan.get("linear_coefficients", {}) as Dictionary
	if not bool(nested_plan.get("valid", false)) \
			or not is_equal_approx(float(nested_coeff.get("mountain", -1.0)), 1.5) \
			or not is_equal_approx(float(nested_coeff.get("mid", -1.0)), 0.5):
		runtime.free()
		return "nested Blend -> Scale coefficients were not reduced exactly once"
	var nested: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not _resident_zero_bytecode(nested):
		runtime.free()
		return "nested linear expression left resident zero-bytecode production"
	var nested_controls: Dictionary = nested.get("production_geomorph_controls", {}) as Dictionary
	if not is_equal_approx(float(nested_controls.get("mountain_strength", 0.0)), 3.0) \
			or not is_equal_approx(float(nested_controls.get("mid_strength", 0.0)), 0.75):
		runtime.free()
		return "nested linear coefficients were applied incorrectly or more than once"

	# Preserve a known-good snapshot, then make the Blend Amount invalid. Candidate
	# rejection must not leak controls, generation or culling bounds.
	var good_generation: int = int(nested.get("generation", -1))
	var good_guard: float = float((nested.get("displacement_envelope", {}) as Dictionary).get(
		"production_max_abs_m", -1.0))
	var good_controls: Dictionary = nested_controls.duplicate(true)
	graph.call("set_node_parameter", blend_id, "value", 1.25)
	var invalid_blend_plan: Dictionary = LOWERING.contribution_merge_plan(graph)
	if bool(invalid_blend_plan.get("valid", true)):
		runtime.free()
		return "Blend Amount > 1 was accepted"
	var rejected_blend: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if bool(rejected_blend.get("candidate_valid", true)) \
			or not bool(rejected_blend.get("candidate_rejected", false)) \
			or int(rejected_blend.get("generation", -2)) != good_generation:
		runtime.free()
		return "invalid Blend candidate replaced the last-known-good snapshot"
	var rejected_controls: Dictionary = rejected_blend.get("production_geomorph_controls", {}) as Dictionary
	if not is_equal_approx(float(rejected_controls.get("mountain_strength", 0.0)),
			float(good_controls.get("mountain_strength", -1.0))) \
			or not is_equal_approx(float(rejected_controls.get("mid_strength", 0.0)),
				float(good_controls.get("mid_strength", -1.0))):
		runtime.free()
		return "invalid Blend leaked candidate resident controls"
	var rejected_guard: float = float((rejected_blend.get("displacement_envelope", {}) as Dictionary).get(
		"production_max_abs_m", -2.0))
	if not is_equal_approx(rejected_guard, good_guard):
		runtime.free()
		return "invalid Blend changed active displacement bounds"

	# Terrain * terrain is nonlinear. It must be rejected even though MULTIPLY is a
	# generic graph node and both inputs are otherwise valid native contributions.
	controls = NATIVE.extract_controls(graph)
	NATIVE.build_canonical_graph(graph, controls)
	mountain_id = _find_type(graph, NATIVE.stage_node_type("mountain"))
	mid_id = _find_type(graph, NATIVE.stage_node_type("mid"))
	merge_id = _find_type(graph, NATIVE.MERGE_TYPE)
	var nonlinear_id: String = String(graph.call("add_node", "MULTIPLY", Vector2(650.0, 350.0), {}))
	if not _disconnect_stage(graph, mountain_id, "mountain", merge_id) \
			or not _disconnect_stage(graph, mid_id, "mid", merge_id) \
			or not bool(graph.call("connect_nodes", mountain_id, 0, nonlinear_id, 0)) \
			or not bool(graph.call("connect_nodes", mid_id, 0, nonlinear_id, 1)) \
			or not bool(graph.call("connect_nodes", nonlinear_id, 0, merge_id,
				LOWERING.group_merge_port_start())):
		runtime.free()
		return "could not construct nonlinear terrain × terrain candidate"
	if bool(LOWERING.contribution_merge_plan(graph).get("valid", true)):
		runtime.free()
		return "terrain × terrain nonlinear composition was accepted"

	# A grouped Add must use a Custom Group socket, not masquerade as one stage's
	# dedicated socket. This preserves graph readability and provenance.
	controls = NATIVE.extract_controls(graph)
	NATIVE.build_canonical_graph(graph, controls)
	mountain_id = _find_type(graph, NATIVE.stage_node_type("mountain"))
	mid_id = _find_type(graph, NATIVE.stage_node_type("mid"))
	merge_id = _find_type(graph, NATIVE.MERGE_TYPE)
	add_id = String(graph.call("add_node", "ADD", Vector2(650.0, 350.0), {}))
	if not _disconnect_stage(graph, mountain_id, "mountain", merge_id) \
			or not _disconnect_stage(graph, mid_id, "mid", merge_id) \
			or not bool(graph.call("connect_nodes", mountain_id, 0, add_id, 0)) \
			or not bool(graph.call("connect_nodes", mid_id, 0, add_id, 1)) \
			or not bool(graph.call("connect_nodes", add_id, 0, merge_id,
				NATIVE.merge_port_for_stage("mountain"))):
		runtime.free()
		return "could not construct wrong-socket grouped Add candidate"
	if bool(LOWERING.contribution_merge_plan(graph).get("valid", true)):
		runtime.free()
		return "grouped Add was allowed to masquerade as a dedicated Mountain socket"

	runtime.free()
	return ""


func _disconnect_stage(graph: Resource, stage_node_id: String,
		stage_id: String, merge_id: String) -> bool:
	return bool(graph.call("disconnect_nodes", stage_node_id, 0, merge_id,
		NATIVE.merge_port_for_stage(stage_id)))


func _resident_zero_bytecode(result: Dictionary) -> bool:
	return bool(result.get("candidate_valid", false)) \
		and not bool(result.get("active", true)) \
		and int(result.get("instructions", -1)) == 0


func _find_type(graph: Resource, node_type: String) -> String:
	for node_value: Variant in graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		if String(node.get("type", "")) == node_type:
			return String(node.get("id", ""))
	return ""
