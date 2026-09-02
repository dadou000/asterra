extends Node
## Regression for opaque-macro migration and exact ordered native-stage bypasses.

const GRAPH := preload("res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
const TERRAIN := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const SLOT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const NATIVE := preload("res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")
const LOWERING := preload("res://scripts/world_authoring/model/terrain_production_geomorph_lowering.gd")
const RUNTIME := preload("res://scripts/world_authoring/terrain_displacement_runtime_phase36.gd")


func _ready() -> void:
	var error: String = _validate()
	if not error.is_empty():
		push_error("NATIVE_PRODUCTION_STAGE_GRAPH_FAILED: " + error)
		get_tree().quit(1)
		return
	print("NATIVE_PRODUCTION_STAGE_GRAPH_OK: migration preserves controls; ordered stage bypasses execute exactly; invalid reorder rolls back")
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
	var first_controls: Dictionary = first.get("production_geomorph_controls", {}) as Dictionary
	if not is_equal_approx(float(first_controls.get("mountain_strength", 0.0)), 1.73):
		runtime.free()
		return "native stage controls did not reach runtime"

	# Parameter edits remain the exact native production shader: no VM program is
	# introduced just because a real stage control changed.
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

	# Bypass Channels while leaving Deposition in the path. The shader shares the
	# channel sample with deposition, so exact lowering must zero incision strength
	# only; it must not remove or approximate the shared branch.
	var mid_id: String = _find_type(graph, NATIVE.stage_node_type("mid"))
	var channel_id: String = _find_type(graph, NATIVE.stage_node_type("channel"))
	var deposit_id: String = _find_type(graph, NATIVE.stage_node_type("deposit"))
	if mid_id.is_empty() or channel_id.is_empty() or deposit_id.is_empty():
		runtime.free()
		return "channel bypass fixture is missing native stage nodes"
	if not bool(graph.call("disconnect_nodes", mid_id, 0, channel_id, 0)) \
			or not bool(graph.call("disconnect_nodes", channel_id, 0, deposit_id, 0)) \
			or not bool(graph.call("connect_nodes", mid_id, 0, deposit_id, 0)):
		runtime.free()
		return "could not construct ordered Channel bypass"
	var channel_plan: Dictionary = LOWERING.ordered_bypass_plan(graph)
	if not bool(channel_plan.get("valid", false)) \
			or not (channel_plan.get("disabled_stage_ids", PackedStringArray()) as PackedStringArray).has("channel"):
		runtime.free()
		return "ordered Channel bypass was not recognized by lowering"
	var channel_bypass: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(channel_bypass.get("candidate_valid", false)) \
			or bool(channel_bypass.get("active", true)) \
			or int(channel_bypass.get("instructions", -1)) != 0:
		runtime.free()
		return "ordered Channel bypass did not remain on resident zero-bytecode production path"
	var channel_controls: Dictionary = channel_bypass.get("production_geomorph_controls", {}) as Dictionary
	if not is_zero_approx(float(channel_controls.get("channel_strength", -1.0))):
		runtime.free()
		return "Channel bypass did not disable incision exactly"
	if is_zero_approx(float(channel_controls.get("deposit_strength", 0.0))):
		runtime.free()
		return "Channel bypass incorrectly disabled Deposition"
	var channel_guard: float = float((channel_bypass.get("displacement_envelope", {}) as Dictionary).get(
		"production_max_abs_m", -1.0))
	var second_guard: float = float((second.get("displacement_envelope", {}) as Dictionary).get(
		"production_max_abs_m", -1.0))
	if channel_guard < 0.0 or second_guard < 0.0 or channel_guard > second_guard + 0.0001:
		runtime.free()
		return "Channel bypass increased the conservative production bound"

	# Fine Detail shares fine_strength with Micro Relief, while Dunes are nested in
	# the same fine-resolution branch. Bypassing Fine must therefore zero only its
	# amplitude and preserve the shared/nested stages.
	var fine_id: String = _find_type(graph, NATIVE.stage_node_type("fine"))
	var dune_id: String = _find_type(graph, NATIVE.stage_node_type("dune"))
	var micro_id: String = _find_type(graph, NATIVE.stage_node_type("micro"))
	if fine_id.is_empty() or dune_id.is_empty() or micro_id.is_empty():
		runtime.free()
		return "fine bypass fixture is missing native stage nodes"
	graph.call("set_node_parameter", fine_id, "fine_strength", 1.6)
	graph.call("set_node_parameter", dune_id, "dune_strength", 1.4)
	graph.call("set_node_parameter", micro_id, "micro_amplitude_m", 1.7)
	if not bool(graph.call("disconnect_nodes", deposit_id, 0, fine_id, 0)) \
			or not bool(graph.call("disconnect_nodes", fine_id, 0, dune_id, 0)) \
			or not bool(graph.call("connect_nodes", deposit_id, 0, dune_id, 0)):
		runtime.free()
		return "could not construct ordered Fine Detail bypass"
	var fine_plan: Dictionary = LOWERING.ordered_bypass_plan(graph)
	if not bool(fine_plan.get("valid", false)) \
			or not (fine_plan.get("disabled_stage_ids", PackedStringArray()) as PackedStringArray).has("fine"):
		runtime.free()
		return "ordered Fine bypass was not recognized by lowering"
	var fine_bypass: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(fine_bypass.get("candidate_valid", false)) \
			or bool(fine_bypass.get("active", true)) \
			or int(fine_bypass.get("instructions", -1)) != 0:
		runtime.free()
		return "ordered Fine bypass did not remain on resident zero-bytecode production path"
	var fine_controls: Dictionary = fine_bypass.get("production_geomorph_controls", {}) as Dictionary
	if not is_zero_approx(float(fine_controls.get("fine_amplitude_m", -1.0))):
		runtime.free()
		return "Fine bypass did not zero only the fine contribution"
	if not is_equal_approx(float(fine_controls.get("fine_strength", 0.0)), 1.6) \
			or not is_equal_approx(float(fine_controls.get("dune_strength", 0.0)), 1.4) \
			or not is_equal_approx(float(fine_controls.get("micro_amplitude_m", 0.0)), 1.7):
		runtime.free()
		return "Fine bypass damaged shared Dune/Micro production controls"

	var good_generation: int = int(fine_bypass.get("generation", -1))
	var good_guard: float = float((fine_bypass.get("displacement_envelope", {}) as Dictionary).get(
		"production_max_abs_m", -1.0))

	# A backwards rewire is deliberately still unsupported. Make Channel execute
	# after Dunes; the candidate must be rejected and the last valid lowered controls,
	# bytecode generation and bounds must remain active.
	if not bool(graph.call("disconnect_nodes", dune_id, 0, micro_id, 0)) \
			or not bool(graph.call("connect_nodes", dune_id, 0, channel_id, 0)) \
			or not bool(graph.call("connect_nodes", channel_id, 0, micro_id, 0)):
		runtime.free()
		return "could not construct reordered native-stage topology"
	var reordered_plan: Dictionary = LOWERING.ordered_bypass_plan(graph)
	if bool(reordered_plan.get("valid", true)):
		runtime.free()
		return "backwards native-stage order was accepted by lowering"
	graph.call("set_node_parameter", mountain_id, "mountain_strength", 4.0)
	var broken: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if bool(broken.get("candidate_valid", true)) or not bool(broken.get("candidate_rejected", false)):
		runtime.free()
		return "reordered native-stage topology was not rejected"
	if int(broken.get("generation", -2)) != good_generation:
		runtime.free()
		return "rejected native topology changed active bytecode generation"
	var broken_controls: Dictionary = broken.get("production_geomorph_controls", {}) as Dictionary
	if not is_equal_approx(float(broken_controls.get("mountain_strength", 0.0)), 2.25) \
			or not is_zero_approx(float(broken_controls.get("channel_strength", -1.0))) \
			or not is_zero_approx(float(broken_controls.get("fine_amplitude_m", -1.0))):
		runtime.free()
		return "rejected native topology leaked candidate controls over last-known-good lowering"
	var broken_guard: float = float((broken.get("displacement_envelope", {}) as Dictionary).get(
		"production_max_abs_m", -2.0))
	if not is_equal_approx(broken_guard, good_guard):
		runtime.free()
		return "rejected native topology changed displacement bounds"

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
