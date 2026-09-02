extends Node
## Regression for opaque-macro migration, exact bypasses and commutative native-stage reorder.

const GRAPH := preload("res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
const TERRAIN := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const SLOT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const NATIVE := preload("res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")
const LOWERING := preload("res://scripts/world_authoring/model/terrain_production_geomorph_lowering.gd")
const RUNTIME := preload("res://scripts/world_authoring/terrain_displacement_runtime_phase37.gd")


func _ready() -> void:
	var error: String = _validate()
	if not error.is_empty():
		push_error("NATIVE_PRODUCTION_STAGE_GRAPH_FAILED: " + error)
		get_tree().quit(1)
		return
	print("NATIVE_PRODUCTION_STAGE_GRAPH_OK: migration, exact bypasses and commutative native reorder preserve zero-bytecode production; nonlinear reorder rolls back")
	get_tree().quit(0)


func _validate() -> String:
	var terrain: Resource = TERRAIN.new()
	terrain.call("ensure_valid")
	var slot: Resource = terrain.call("create_shader_slot",
		SLOT.Domain.DISPLACEMENT, "Base Terrain Shape") as Resource
	if slot == null:
		return "could not create displacement slot"
	slot.set(&"slot_id", NATIVE.PRODUCTION_SHAPE_SLOT_ID)
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return "slot has no graph"
	graph.call("create_production_stage_graph", GRAPH.Domain.DISPLACEMENT)

	var settings_id: String = _find_type(graph, NATIVE.SETTINGS_TYPE)
	if settings_id.is_empty():
		return "legacy production graph has no settings node"
	graph.call("set_node_parameter", settings_id, "mountain_strength", 1.73)
	graph.call("set_node_parameter", settings_id, "channel_depth_max_m", 81.5)
	if not NATIVE.is_legacy_identity_graph(graph):
		return "pre-migration production graph is not recognized as identity"
	if not NATIVE.migrate_legacy_identity(graph):
		return "legacy production graph did not migrate"
	if not NATIVE.is_canonical_structural_graph(graph):
		return "migrated native graph is not canonical"
	if not _find_type(graph, NATIVE.LEGACY_GENERATED_TYPE).is_empty():
		return "opaque PRODUCTION_GENERATED_HEIGHT survived structural migration"
	for stage_id: String in NATIVE.SCHEMA.ordered_stage_ids():
		if _find_type(graph, NATIVE.stage_node_type(stage_id)).is_empty():
			return "missing native production stage %s" % stage_id

	var controls: Dictionary = NATIVE.extract_controls(graph)
	if not is_equal_approx(float(controls.get("mountain_strength", 0.0)), 1.73) \
			or not is_equal_approx(float(controls.get("channel_depth_max_m", 0.0)), 81.5):
		return "migration lost production control values"

	var runtime: Node = RUNTIME.new() as Node
	var first: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(first.get("candidate_valid", false)):
		runtime.free()
		return "canonical native graph was rejected: %s" % str(first.get("candidate_warnings", []))
	if bool(first.get("active", true)) or int(first.get("instructions", -1)) != 0:
		runtime.free()
		return "canonical native graph emitted authored bytecode"

	var mountain_id: String = _find_type(graph, NATIVE.stage_node_type("mountain"))
	graph.call("set_node_parameter", mountain_id, "mountain_strength", 2.25)
	var second: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(second.get("candidate_valid", false)) \
			or bool(second.get("active", true)) \
			or int(second.get("instructions", -1)) != 0:
		runtime.free()
		return "valid native stage parameter edit left zero-bytecode production path"
	var second_controls: Dictionary = second.get("production_geomorph_controls", {}) as Dictionary
	if not is_equal_approx(float(second_controls.get("mountain_strength", 0.0)), 2.25):
		runtime.free()
		return "edited native stage parameter did not commit"

	# Bypass Channel while retaining Deposition. Incision alone must be disabled.
	var mid_id: String = _find_type(graph, NATIVE.stage_node_type("mid"))
	var channel_id: String = _find_type(graph, NATIVE.stage_node_type("channel"))
	var deposit_id: String = _find_type(graph, NATIVE.stage_node_type("deposit"))
	if not bool(graph.call("disconnect_nodes", mid_id, 0, channel_id, 0)) \
			or not bool(graph.call("disconnect_nodes", channel_id, 0, deposit_id, 0)) \
			or not bool(graph.call("connect_nodes", mid_id, 0, deposit_id, 0)):
		runtime.free()
		return "could not construct Channel bypass"
	var channel_plan: Dictionary = LOWERING.commutative_reorder_plan(graph)
	if not bool(channel_plan.get("valid", false)) \
			or not (channel_plan.get("disabled_stage_ids", PackedStringArray()) as PackedStringArray).has("channel"):
		runtime.free()
		return "Channel bypass was not recognized by Phase 37 lowering"
	var channel_bypass: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(channel_bypass.get("candidate_valid", false)) \
			or bool(channel_bypass.get("active", true)) \
			or int(channel_bypass.get("instructions", -1)) != 0:
		runtime.free()
		return "Channel bypass left resident zero-bytecode production path"
	var channel_controls: Dictionary = channel_bypass.get("production_geomorph_controls", {}) as Dictionary
	if not is_zero_approx(float(channel_controls.get("channel_strength", -1.0))):
		runtime.free()
		return "Channel bypass did not disable incision exactly"
	if is_zero_approx(float(channel_controls.get("deposit_strength", 0.0))):
		runtime.free()
		return "Channel bypass incorrectly disabled Deposition"

	# Reset to a complete native chain, preserving authored parameters, then reorder
	# Mid Relief before Mountains. Both contributions are independent of accumulated
	# height, so the graph order is different but the resident normalized result must
	# remain exactly equivalent and zero-bytecode.
	controls = NATIVE.extract_controls(graph)
	NATIVE.build_canonical_graph(graph, controls)
	var broad_id: String = _find_type(graph, NATIVE.stage_node_type("broad"))
	mountain_id = _find_type(graph, NATIVE.stage_node_type("mountain"))
	mid_id = _find_type(graph, NATIVE.stage_node_type("mid"))
	channel_id = _find_type(graph, NATIVE.stage_node_type("channel"))
	if not bool(graph.call("disconnect_nodes", broad_id, 0, mountain_id, 0)) \
			or not bool(graph.call("disconnect_nodes", mountain_id, 0, mid_id, 0)) \
			or not bool(graph.call("disconnect_nodes", mid_id, 0, channel_id, 0)) \
			or not bool(graph.call("connect_nodes", broad_id, 0, mid_id, 0)) \
			or not bool(graph.call("connect_nodes", mid_id, 0, mountain_id, 0)) \
			or not bool(graph.call("connect_nodes", mountain_id, 0, channel_id, 0)):
		runtime.free()
		return "could not construct exact Mid/Mountain reorder"

	var reorder_plan: Dictionary = LOWERING.commutative_reorder_plan(graph)
	if not bool(reorder_plan.get("valid", false)) or not bool(reorder_plan.get("reordered", false)):
		runtime.free()
		return "commutative Mid/Mountain reorder was not accepted"
	var authored_order: PackedStringArray = reorder_plan.get("active_stage_ids", PackedStringArray()) as PackedStringArray
	var normalized_order: PackedStringArray = reorder_plan.get("normalized_stage_ids", PackedStringArray()) as PackedStringArray
	if authored_order.find("mid") >= authored_order.find("mountain") \
			or normalized_order.find("mountain") >= normalized_order.find("mid"):
		runtime.free()
		return "reorder plan did not preserve authored order and normalized production order separately"

	var reordered: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(reordered.get("candidate_valid", false)) \
			or bool(reordered.get("active", true)) \
			or int(reordered.get("instructions", -1)) != 0:
		runtime.free()
		return "exact native reorder did not remain on zero-bytecode production path"
	var runtime_plan: Dictionary = reordered.get("native_reorder_plan", {}) as Dictionary
	if not bool(runtime_plan.get("reordered", false)) \
			or String(runtime_plan.get("execution_mode", "")) != "resident_normalized_order":
		runtime.free()
		return "runtime did not expose normalized exact-reorder execution plan"
	var reordered_controls: Dictionary = reordered.get("production_geomorph_controls", {}) as Dictionary
	if not is_equal_approx(float(reordered_controls.get("mountain_strength", 0.0)), 2.25) \
			or not is_equal_approx(float(reordered_controls.get("channel_depth_max_m", 0.0)), 81.5):
		runtime.free()
		return "exact native reorder changed production controls"

	var good_generation: int = int(reordered.get("generation", -1))
	var good_guard: float = float((reordered.get("displacement_envelope", {}) as Dictionary).get(
		"production_max_abs_m", -1.0))

	# Glacial is nonlinear: h = mix(h, h*scale + ice, mix). Moving any additive
	# stage after it changes the result. Construct Glacial -> Micro and verify the
	# candidate is rejected without leaking its parameter edit, bounds or generation.
	var dune_id: String = _find_type(graph, NATIVE.stage_node_type("dune"))
	var micro_id: String = _find_type(graph, NATIVE.stage_node_type("micro"))
	var glacial_id: String = _find_type(graph, NATIVE.stage_node_type("glacial"))
	var compose_id: String = _find_type(graph, NATIVE.COMPOSE_TYPE)
	if not bool(graph.call("disconnect_nodes", dune_id, 0, micro_id, 0)) \
			or not bool(graph.call("disconnect_nodes", micro_id, 0, glacial_id, 0)) \
			or not bool(graph.call("disconnect_nodes", glacial_id, 0, compose_id, 0)) \
			or not bool(graph.call("connect_nodes", dune_id, 0, glacial_id, 0)) \
			or not bool(graph.call("connect_nodes", glacial_id, 0, micro_id, 0)) \
			or not bool(graph.call("connect_nodes", micro_id, 0, compose_id, 0)):
		runtime.free()
		return "could not construct nonlinear Glacial/Micro reorder"
	var nonlinear_plan: Dictionary = LOWERING.commutative_reorder_plan(graph)
	if bool(nonlinear_plan.get("valid", true)):
		runtime.free()
		return "nonlinear Glacial reorder was accepted"
	graph.call("set_node_parameter", mountain_id, "mountain_strength", 4.0)
	var rejected: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if bool(rejected.get("candidate_valid", true)) or not bool(rejected.get("candidate_rejected", false)):
		runtime.free()
		return "nonlinear native-stage topology was not transactionally rejected"
	if int(rejected.get("generation", -2)) != good_generation:
		runtime.free()
		return "rejected nonlinear topology changed active bytecode generation"
	var rejected_controls: Dictionary = rejected.get("production_geomorph_controls", {}) as Dictionary
	if not is_equal_approx(float(rejected_controls.get("mountain_strength", 0.0)), 2.25):
		runtime.free()
		return "rejected nonlinear topology leaked candidate controls"
	var rejected_guard: float = float((rejected.get("displacement_envelope", {}) as Dictionary).get(
		"production_max_abs_m", -2.0))
	if not is_equal_approx(rejected_guard, good_guard):
		runtime.free()
		return "rejected nonlinear topology changed displacement bounds"

	runtime.free()
	return ""


func _find_type(graph: Resource, node_type: String) -> String:
	for node_value: Variant in graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		if String(node.get("type", "")) == node_type:
			return String(node.get("id", ""))
	return ""
