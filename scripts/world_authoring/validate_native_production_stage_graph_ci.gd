extends Node
## Regression for exact native production contribution/merge semantics.

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
	print("NATIVE_PRODUCTION_STAGE_GRAPH_OK: contribution fan-in, exact bypasses, legacy linear migration and invalid-branch rollback preserve zero-bytecode production")
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

	# Start from the pre-native production identity so the real upgrade path is tested.
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
	if not NATIVE.has_merge_node(graph) or not NATIVE.is_canonical_structural_graph(graph):
		return "identity migration did not create canonical contribution/merge topology"
	if not _find_type(graph, NATIVE.LEGACY_GENERATED_TYPE).is_empty():
		return "opaque PRODUCTION_GENERATED_HEIGHT survived contribution migration"

	var controls: Dictionary = NATIVE.extract_controls(graph)
	if not is_equal_approx(float(controls.get("mountain_strength", 0.0)), 1.73) \
			or not is_equal_approx(float(controls.get("channel_depth_max_m", 0.0)), 81.5):
		return "identity migration lost production control values"

	var branch_plan: Dictionary = LOWERING.contribution_merge_plan(graph)
	if not bool(branch_plan.get("valid", false)) \
			or not bool(branch_plan.get("merge_semantics", false)) \
			or String(branch_plan.get("execution_mode", "")) != "resident_contribution_merge":
		return "canonical contribution graph did not lower to resident merge semantics"

	var runtime: Node = RUNTIME.new() as Node
	var first: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not _resident_zero_bytecode(first):
		runtime.free()
		return "canonical contribution graph left resident zero-bytecode production path"

	var mountain_id: String = _find_type(graph, NATIVE.stage_node_type("mountain"))
	graph.call("set_node_parameter", mountain_id, "mountain_strength", 2.25)
	var edited: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not _resident_zero_bytecode(edited):
		runtime.free()
		return "valid contribution parameter edit left zero-bytecode production path"
	var edited_controls: Dictionary = edited.get("production_geomorph_controls", {}) as Dictionary
	if not is_equal_approx(float(edited_controls.get("mountain_strength", 0.0)), 2.25):
		runtime.free()
		return "edited native contribution parameter did not commit"

	# Disconnect only Channel's dedicated merge socket. Deposition must stay active.
	var merge_id: String = _find_type(graph, NATIVE.MERGE_TYPE)
	var channel_id: String = _find_type(graph, NATIVE.stage_node_type("channel"))
	var channel_port: int = NATIVE.merge_port_for_stage("channel")
	if not bool(graph.call("disconnect_nodes", channel_id, 0, merge_id, channel_port)):
		runtime.free()
		return "could not disconnect Channel contribution"
	var channel_plan: Dictionary = LOWERING.contribution_merge_plan(graph)
	if not bool(channel_plan.get("valid", false)) \
			or not (channel_plan.get("disabled_stage_ids", PackedStringArray()) as PackedStringArray).has("channel"):
		runtime.free()
		return "Channel contribution bypass was not recognized"
	var channel_bypass: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not _resident_zero_bytecode(channel_bypass):
		runtime.free()
		return "Channel contribution bypass left resident zero-bytecode production path"
	var channel_controls: Dictionary = channel_bypass.get("production_geomorph_controls", {}) as Dictionary
	if not is_zero_approx(float(channel_controls.get("channel_strength", -1.0))):
		runtime.free()
		return "Channel contribution bypass did not disable incision exactly"
	if is_zero_approx(float(channel_controls.get("deposit_strength", 0.0))):
		runtime.free()
		return "Channel contribution bypass incorrectly disabled Deposition"

	# Glacial is a transform, not a contribution. Its exact bypass is Merge -> Compose.
	controls = NATIVE.extract_controls(graph)
	NATIVE.build_canonical_graph(graph, controls)
	merge_id = _find_type(graph, NATIVE.MERGE_TYPE)
	var glacial_id: String = _find_type(graph, NATIVE.stage_node_type("glacial"))
	var compose_id: String = _find_type(graph, NATIVE.COMPOSE_TYPE)
	if not bool(graph.call("disconnect_nodes", merge_id, 0, glacial_id, 0)) \
			or not bool(graph.call("disconnect_nodes", glacial_id, 0, compose_id, 0)) \
			or not bool(graph.call("connect_nodes", merge_id, 0, compose_id, 0)):
		runtime.free()
		return "could not construct exact Glacial bypass"
	var glacial_plan: Dictionary = LOWERING.contribution_merge_plan(graph)
	if not bool(glacial_plan.get("valid", false)) \
			or not (glacial_plan.get("disabled_stage_ids", PackedStringArray()) as PackedStringArray).has("glacial"):
		runtime.free()
		return "Glacial transform bypass was not recognized"
	var glacial_bypass: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not _resident_zero_bytecode(glacial_bypass):
		runtime.free()
		return "Glacial bypass left resident zero-bytecode production path"
	var glacial_controls: Dictionary = glacial_bypass.get("production_geomorph_controls", {}) as Dictionary
	if not is_zero_approx(float(glacial_controls.get("glacial_strength", -1.0))):
		runtime.free()
		return "Glacial bypass did not disable resident transform exactly"

	# Preserve a known-good active snapshot, then cross-wire Mid into Channel's fixed
	# merge socket. The candidate must fail without leaking controls or culling bounds.
	controls = NATIVE.extract_controls(graph)
	NATIVE.build_canonical_graph(graph, controls)
	var good: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not _resident_zero_bytecode(good):
		runtime.free()
		return "could not establish known-good contribution snapshot"
	var good_generation: int = int(good.get("generation", -1))
	var good_guard: float = float((good.get("displacement_envelope", {}) as Dictionary).get(
		"production_max_abs_m", -1.0))
	var good_controls: Dictionary = good.get("production_geomorph_controls", {}) as Dictionary
	var good_mountain_strength: float = float(good_controls.get("mountain_strength", 0.0))

	merge_id = _find_type(graph, NATIVE.MERGE_TYPE)
	var mid_id: String = _find_type(graph, NATIVE.stage_node_type("mid"))
	channel_id = _find_type(graph, NATIVE.stage_node_type("channel"))
	var mid_port: int = NATIVE.merge_port_for_stage("mid")
	channel_port = NATIVE.merge_port_for_stage("channel")
	if not bool(graph.call("disconnect_nodes", mid_id, 0, merge_id, mid_port)) \
			or not bool(graph.call("disconnect_nodes", channel_id, 0, merge_id, channel_port)) \
			or not bool(graph.call("connect_nodes", mid_id, 0, merge_id, channel_port)):
		runtime.free()
		return "could not construct wrong-socket contribution branch"
	var invalid_plan: Dictionary = LOWERING.contribution_merge_plan(graph)
	if bool(invalid_plan.get("valid", true)):
		runtime.free()
		return "wrong-socket contribution branch was accepted"
	mountain_id = _find_type(graph, NATIVE.stage_node_type("mountain"))
	graph.call("set_node_parameter", mountain_id, "mountain_strength", 4.0)
	var rejected: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if bool(rejected.get("candidate_valid", true)) or not bool(rejected.get("candidate_rejected", false)):
		runtime.free()
		return "invalid contribution branch was not transactionally rejected"
	if int(rejected.get("generation", -2)) != good_generation:
		runtime.free()
		return "rejected contribution branch changed active bytecode generation"
	var rejected_controls: Dictionary = rejected.get("production_geomorph_controls", {}) as Dictionary
	if not is_equal_approx(float(rejected_controls.get("mountain_strength", 0.0)), good_mountain_strength):
		runtime.free()
		return "rejected contribution branch leaked candidate controls"
	var rejected_guard: float = float((rejected.get("displacement_envelope", {}) as Dictionary).get(
		"production_max_abs_m", -2.0))
	if not is_equal_approx(rejected_guard, good_guard):
		runtime.free()
		return "rejected contribution branch changed displacement bounds"

	# Reconstruct a saved Phase 37 linear graph with a real reorder and Channel
	# bypass. The compatibility lowering must read it, then Phase 38 migration must
	# preserve both its edited controls and its enabled-stage set.
	controls = NATIVE.extract_controls(graph)
	controls["mountain_strength"] = 2.65
	NATIVE.build_canonical_graph(graph, controls)
	merge_id = _find_type(graph, NATIVE.MERGE_TYPE)
	if not bool(graph.call("remove_node", merge_id)):
		runtime.free()
		return "could not construct saved Phase 37 graph"
	var start_id: String = _find_type(graph, NATIVE.START_TYPE)
	compose_id = _find_type(graph, NATIVE.COMPOSE_TYPE)
	var legacy_order := PackedStringArray([
		"broad", "mid", "mountain", "deposit", "fine", "dune", "micro", "glacial"])
	var previous_id: String = start_id
	for stage_id: String in legacy_order:
		var stage_id_node: String = _find_type(graph, NATIVE.stage_node_type(stage_id))
		if not bool(graph.call("connect_nodes", previous_id, 0, stage_id_node, 0)):
			runtime.free()
			return "could not wire saved Phase 37 stage %s" % stage_id
		previous_id = stage_id_node
	if not bool(graph.call("connect_nodes", previous_id, 0, compose_id, 0)):
		runtime.free()
		return "could not close saved Phase 37 chain"
	var legacy_plan: Dictionary = LOWERING.commutative_reorder_plan(graph)
	if not bool(legacy_plan.get("valid", false)) or not bool(legacy_plan.get("reordered", false)):
		runtime.free()
		return "saved Phase 37 reordered chain is no longer readable"
	if not (legacy_plan.get("disabled_stage_ids", PackedStringArray()) as PackedStringArray).has("channel"):
		runtime.free()
		return "saved Phase 37 Channel bypass was lost"
	var legacy_controls: Dictionary = NATIVE.extract_controls(graph)
	var legacy_enabled: PackedStringArray = legacy_plan.get(
		"active_stage_ids", PackedStringArray()) as PackedStringArray
	NATIVE.build_branch_graph(graph, legacy_controls, legacy_enabled)
	var migrated_plan: Dictionary = LOWERING.contribution_merge_plan(graph)
	if not bool(migrated_plan.get("valid", false)) \
			or not (migrated_plan.get("disabled_stage_ids", PackedStringArray()) as PackedStringArray).has("channel"):
		runtime.free()
		return "Phase 37 -> contribution migration changed enabled stages"
	var migrated_controls: Dictionary = NATIVE.extract_controls(graph)
	if not is_equal_approx(float(migrated_controls.get("mountain_strength", 0.0)), 2.65):
		runtime.free()
		return "Phase 37 -> contribution migration lost edited parameters"
	var migrated: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not _resident_zero_bytecode(migrated):
		runtime.free()
		return "migrated Phase 37 graph left zero-bytecode production path"

	runtime.free()
	return ""


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
