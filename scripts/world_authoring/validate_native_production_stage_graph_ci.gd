extends Node
## Regression for the opaque-macro -> native production-stage graph migration.

const GRAPH := preload("res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
const TERRAIN := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const SLOT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const NATIVE := preload("res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")
const RUNTIME := preload("res://scripts/world_authoring/terrain_displacement_runtime_phase35.gd")


func _ready() -> void:
	var error: String = _validate()
	if not error.is_empty():
		push_error("NATIVE_PRODUCTION_STAGE_GRAPH_FAILED: " + error)
		get_tree().quit(1)
		return
	print("NATIVE_PRODUCTION_STAGE_GRAPH_OK: migration preserves controls, zero-bytecode identity and rollback")
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
	var good_generation: int = int(second.get("generation", -1))
	var good_guard: float = float((second.get("displacement_envelope", {}) as Dictionary).get(
		"production_max_abs_m", -1.0))

	# Structural topology is visible but protected until exact intrinsic lowering is
	# implemented. A half-finished rewire must be rejected without leaking controls
	# or bounds into the active terrain.
	var mid_id: String = _find_type(graph, NATIVE.stage_node_type("mid"))
	var channel_id: String = _find_type(graph, NATIVE.stage_node_type("channel"))
	if mid_id.is_empty() or channel_id.is_empty() \
			or not bool(graph.call("disconnect_nodes", mid_id, 0, channel_id, 0)):
		runtime.free()
		return "could not construct broken native-stage topology"
	graph.call("set_node_parameter", mountain_id, "mountain_strength", 4.0)
	var broken: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if bool(broken.get("candidate_valid", true)) or not bool(broken.get("candidate_rejected", false)):
		runtime.free()
		return "broken native-stage topology was not rejected"
	if int(broken.get("generation", -2)) != good_generation:
		runtime.free()
		return "rejected native topology changed active bytecode generation"
	var broken_controls: Dictionary = broken.get("production_geomorph_controls", {}) as Dictionary
	if not is_equal_approx(float(broken_controls.get("mountain_strength", 0.0)), 2.25):
		runtime.free()
		return "rejected native topology leaked production controls"
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
