extends Node
## Standalone Phase 33 terrain-program safety regression.
##
## This intentionally has no inheritance relationship with the broad terrain smoke
## suite. A watchdog guarantees CI terminates even if a runtime script error occurs
## before the assertions can report normally.

const TERRAIN_PROFILE := preload(
	"res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const SLOT_MODEL := preload(
	"res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const RUNTIME := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase33.gd")
const GRAPH_SCRIPT := preload(
	"res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")

const WATCHDOG_S: float = 12.0
const DEFAULT_PRODUCTION_GUARD_M: float = 533.4
var _elapsed: float = 0.0


func _ready() -> void:
	print("TERRAIN_PROGRAM_PHASE33_STEP: start")
	if not _validate_transaction():
		return
	print("TERRAIN_PROGRAM_PHASE33_STEP: transaction_ok")
	if not _validate_bounds():
		return
	print("TERRAIN_PROGRAM_PHASE33_OK: rollback and displacement envelopes are safe")
	get_tree().quit(0)


func _process(delta: float) -> void:
	_elapsed += maxf(delta, 0.0)
	if _elapsed >= WATCHDOG_S:
		_fail("TERRAIN_PROGRAM_PHASE33_FAILED: watchdog expired after a runtime error or hang")


func _validate_transaction() -> bool:
	var terrain: Resource = TERRAIN_PROFILE.new()
	terrain.call("ensure_valid")
	var slot: Resource = terrain.call("create_shader_slot",
		SLOT_MODEL.Domain.DISPLACEMENT, "CI Transaction") as Resource
	var graph: Resource = slot.get(&"graph") as Resource if slot != null else null
	if graph == null:
		return _fail_bool("transaction graph missing")
	var output_id: String = _find_node(graph, "OUTPUT_DISPLACEMENT")
	var value_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2.ZERO, {"value": 37.0}))
	var controls: Dictionary = GRAPH_SCRIPT.production_control_defaults(
		"PRODUCTION_GEOMORPH_SETTINGS")
	controls["mountain_strength"] = 1.25
	var controls_id: String = String(graph.call("add_node", "PRODUCTION_GEOMORPH_SETTINGS",
		Vector2(0.0, 180.0), controls))
	if output_id.is_empty() or controls_id.is_empty() \
			or not bool(graph.call("connect_nodes", value_id, 0, output_id, 0)):
		return _fail_bool("transaction graph did not connect")

	var runtime: Node = RUNTIME.new() as Node
	var good: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	print("TERRAIN_PROGRAM_PHASE33_STEP: baseline_compiled")
	if not bool(good.get("candidate_valid", false)):
		runtime.free()
		return _fail_bool("baseline candidate rejected: %s" % str(good.get("candidate_warnings", [])))
	var generation: int = int(good.get("generation", -1))
	var instructions: int = int(good.get("instructions", -1))
	var fingerprint: String = String(good.get("active_fingerprint", ""))
	var good_controls: Dictionary = good.get("production_geomorph_controls", {}) as Dictionary
	var good_envelope: Dictionary = good.get("displacement_envelope", {}) as Dictionary
	var height: float = float(runtime.call("evaluate_height",
		Vector3.RIGHT, 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(height, 37.0) \
			or not is_equal_approx(float(good_controls.get("mountain_strength", 0.0)), 1.25):
		runtime.free()
		return _fail_bool("baseline bytecode/control state is incorrect")

	graph.call("disconnect_nodes", value_id, 0, output_id, 0)
	graph.call("set_node_parameter", controls_id, "mountain_strength", 3.5)
	var rejected: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	print("TERRAIN_PROGRAM_PHASE33_STEP: invalid_candidate_rejected")
	if bool(rejected.get("candidate_valid", true)) \
			or int(rejected.get("generation", -1)) != generation \
			or int(rejected.get("instructions", -1)) != instructions \
			or String(rejected.get("active_fingerprint", "")) != fingerprint:
		runtime.free()
		return _fail_bool("invalid graph replaced active bytecode")
	var rejected_controls: Dictionary = rejected.get("production_geomorph_controls", {}) as Dictionary
	var rejected_envelope: Dictionary = rejected.get("displacement_envelope", {}) as Dictionary
	if not is_equal_approx(float(rejected_controls.get("mountain_strength", 0.0)), 1.25):
		runtime.free()
		return _fail_bool("invalid graph leaked production controls")
	if not is_equal_approx(float(rejected_envelope.get("production_max_abs_m", -1.0)),
			float(good_envelope.get("production_max_abs_m", -2.0))):
		runtime.free()
		return _fail_bool("invalid graph changed active displacement envelope")
	height = float(runtime.call("evaluate_height", Vector3.RIGHT, 0.0, 0, 0, 0.0, 0.0))
	runtime.free()
	if not is_equal_approx(height, 37.0):
		return _fail_bool("invalid graph changed evaluated terrain")
	return true


func _validate_bounds() -> bool:
	var terrain: Resource = TERRAIN_PROFILE.new()
	terrain.call("ensure_valid")
	var slot: Resource = terrain.call("create_shader_slot",
		SLOT_MODEL.Domain.DISPLACEMENT, "CI Bounds") as Resource
	var graph: Resource = slot.get(&"graph") as Resource if slot != null else null
	if graph == null:
		return _fail_bool("bound graph missing")
	var output_id: String = _find_node(graph, "OUTPUT_DISPLACEMENT")
	var value_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2.ZERO, {"value": 25.0}))
	if output_id.is_empty() or not bool(graph.call("connect_nodes", value_id, 0, output_id, 0)):
		return _fail_bool("bound graph did not connect")
	var runtime: Node = RUNTIME.new() as Node
	var stats: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	print("TERRAIN_PROGRAM_PHASE33_STEP: bounded_program_compiled")
	var envelope: Dictionary = stats.get("displacement_envelope", {}) as Dictionary
	if not bool(envelope.get("bounds_known", false)):
		runtime.free()
		return _fail_bool("constant displacement was not statically bounded")
	var production: float = float(envelope.get("production_max_abs_m", 0.0))
	if production <= 220.0 or absf(production - DEFAULT_PRODUCTION_GUARD_M) > 0.01:
		runtime.free()
		return _fail_bool("production envelope is %.3f m, expected %.3f m" % [
			production, DEFAULT_PRODUCTION_GUARD_M])
	if float(envelope.get("total_max_abs_m", 0.0)) < production + 24.9:
		runtime.free()
		return _fail_bool("author displacement missing from total envelope")
	runtime.free()

	var unknown_terrain: Resource = TERRAIN_PROFILE.new()
	unknown_terrain.call("ensure_valid")
	var unknown_slot: Resource = unknown_terrain.call("create_shader_slot",
		SLOT_MODEL.Domain.DISPLACEMENT, "CI Unknown Bounds") as Resource
	var unknown_graph: Resource = unknown_slot.get(&"graph") as Resource if unknown_slot != null else null
	if unknown_graph == null:
		return _fail_bool("unknown-bound graph missing")
	var unknown_output: String = _find_node(unknown_graph, "OUTPUT_DISPLACEMENT")
	var base_id: String = String(unknown_graph.call("add_node", "GAME_INPUT",
		Vector2.ZERO, {"source": "terrain_height_m"}))
	if unknown_output.is_empty() or not bool(unknown_graph.call(
			"connect_nodes", base_id, 0, unknown_output, 0)):
		return _fail_bool("unknown-bound graph did not connect")
	var unknown_runtime: Node = RUNTIME.new() as Node
	var unknown_stats: Dictionary = unknown_runtime.call("compile_from_terrain", unknown_terrain) as Dictionary
	print("TERRAIN_PROGRAM_PHASE33_STEP: unknown_program_compiled")
	var unknown_envelope: Dictionary = unknown_stats.get("displacement_envelope", {}) as Dictionary
	unknown_runtime.free()
	if bool(unknown_envelope.get("bounds_known", true)) \
			or String(unknown_envelope.get("unknown_reason", "")).is_empty():
		return _fail_bool("absolute base-height graph was not classified as unknown")
	return true


func _find_node(graph: Resource, node_type: String) -> String:
	for value: Variant in graph.get(&"nodes") as Array:
		if value is Dictionary and String((value as Dictionary).get("type", "")) == node_type:
			return String((value as Dictionary).get("id", ""))
	return ""


func _fail_bool(reason: String) -> bool:
	_fail("TERRAIN_PROGRAM_PHASE33_FAILED: " + reason)
	return false


func _fail(message: String) -> void:
	push_error(message)
	print(message)
	get_tree().quit(1)
