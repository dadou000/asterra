extends "res://tests/validate_secondary_terrain_scripts.gd"
## Safety regression for interactive terrain graph authoring.
##
## A valid program is compiled first. We then reproduce ordinary GraphEdit states
## that occur while a beginner is editing (disconnecting Final Terrain, removing a
## required input, and making a cycle). Every broken candidate must be rejected
## while the previous bytecode, production controls, displacement envelope and
## evaluated terrain remain untouched.

const SLOT_MODEL := preload(
	"res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const SAFE_RUNTIME := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase33.gd")
const GRAPH_SCRIPT := preload(
	"res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")

const EXPECTED_DEFAULT_PRODUCTION_GUARD_M: float = 533.4


func _ready() -> void:
	if not _validate_transaction_contract():
		return
	if not _validate_bound_contract():
		return
	super._ready()


func _validate_transaction_contract() -> bool:
	var terrain: Resource = TERRAIN_PROFILE.new()
	terrain.call("ensure_valid")
	var slot: Resource = terrain.call("create_shader_slot",
		SLOT_MODEL.Domain.DISPLACEMENT, "CI Safe Terrain") as Resource
	var graph: Resource = slot.get(&"graph") as Resource if slot != null else null
	if graph == null:
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: displacement graph was not created")
		return false

	var output_id: String = _find_node(graph, "OUTPUT_DISPLACEMENT")
	var constant_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(100.0, 100.0), {"value": 37.0}))
	var controls: Dictionary = GRAPH_SCRIPT.production_control_defaults(
		"PRODUCTION_GEOMORPH_SETTINGS")
	controls["mountain_strength"] = 1.25
	var controls_id: String = String(graph.call("add_node", "PRODUCTION_GEOMORPH_SETTINGS",
		Vector2(100.0, 280.0), controls))
	if controls_id.is_empty() or output_id.is_empty() or not bool(graph.call("connect_nodes",
			constant_id, 0, output_id, 0)):
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: initial graph did not connect")
		return false

	var runtime: Node = SAFE_RUNTIME.new() as Node
	var good: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(good.get("candidate_valid", false)):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: initial candidate was rejected: %s" \
			% str(good.get("candidate_warnings", [])))
		return false
	var good_height: float = float(runtime.call("evaluate_height",
		Vector3.RIGHT, 0.0, 0, 0, 0.0, 0.0))
	var good_generation: int = int(good.get("generation", -1))
	var good_instructions: int = int(good.get("instructions", -1))
	var good_active_fingerprint: String = String(good.get("active_fingerprint", ""))
	var good_controls: Dictionary = good.get("production_geomorph_controls", {}) as Dictionary
	var good_envelope: Dictionary = good.get("displacement_envelope", {}) as Dictionary
	if not is_equal_approx(good_height, 37.0):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: baseline program evaluated incorrectly")
		return false
	if not is_equal_approx(float(good_controls.get("mountain_strength", 0.0)), 1.25):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: baseline production controls did not commit")
		return false
	if not bool(good_envelope.get("bounds_known", false)):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: baseline displacement envelope is unknown")
		return false

	# 1) Exact GraphEdit disconnect operation. Change a production control at the
	# same time to prove an invalid candidate cannot leak uniform-only state while
	# its bytecode is rejected.
	if not bool(graph.call("disconnect_nodes", constant_id, 0, output_id, 0)):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: could not disconnect Final Terrain")
		return false
	graph.call("set_node_parameter", controls_id, "mountain_strength", 3.5)
	var disconnected: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not _assert_preserved(runtime, disconnected, good_generation,
			good_instructions, good_active_fingerprint, 37.0, 1.25,
			good_envelope, "disconnected Final Terrain"):
		return false

	# Restore the graph and prove a later valid document can atomically replace both
	# the old bytecode and the previously rejected production control value.
	if not bool(graph.call("connect_nodes", constant_id, 0, output_id, 0)):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: could not reconnect Final Terrain")
		return false
	graph.call("set_node_parameter", constant_id, "value", 61.0)
	var recovered: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(recovered.get("candidate_valid", false)):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: repaired graph was not accepted")
		return false
	var recovered_height: float = float(runtime.call("evaluate_height",
		Vector3.RIGHT, 0.0, 0, 0, 0.0, 0.0))
	var recovered_controls: Dictionary = recovered.get("production_geomorph_controls", {}) as Dictionary
	if not is_equal_approx(recovered_height, 61.0) \
			or not is_equal_approx(float(recovered_controls.get("mountain_strength", 0.0)), 3.5):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: repaired graph did not commit atomically")
		return false

	# 2) Missing required input. ADD requires both terrain inputs, so connecting only
	# one side is an ordinary half-finished edit and must never become active.
	var recovered_generation: int = int(recovered.get("generation", -1))
	var recovered_instructions: int = int(recovered.get("instructions", -1))
	var recovered_fingerprint: String = String(recovered.get("active_fingerprint", ""))
	var recovered_envelope: Dictionary = recovered.get("displacement_envelope", {}) as Dictionary
	if not bool(graph.call("disconnect_nodes", constant_id, 0, output_id, 0)):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: could not prepare missing-input graph")
		return false
	var add_id: String = String(graph.call("add_node", "ADD", Vector2(360.0, 100.0), {}))
	if not bool(graph.call("connect_nodes", constant_id, 0, add_id, 0)) \
			or not bool(graph.call("connect_nodes", add_id, 0, output_id, 0)):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: could not build missing-input graph")
		return false
	var missing_input: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not _assert_preserved(runtime, missing_input, recovered_generation,
			recovered_instructions, recovered_fingerprint, 61.0, 3.5,
			recovered_envelope, "missing required input"):
		return false

	# 3) Cycle. Fill ADD's second input from a second ADD, then make that node depend
	# on the first ADD. The validator must catch the loop before compiler state moves.
	var loop_id: String = String(graph.call("add_node", "ADD", Vector2(220.0, 260.0), {}))
	var loop_const: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(60.0, 300.0), {"value": 2.0}))
	if not bool(graph.call("connect_nodes", loop_id, 0, add_id, 1)) \
			or not bool(graph.call("connect_nodes", add_id, 0, loop_id, 0)) \
			or not bool(graph.call("connect_nodes", loop_const, 0, loop_id, 1)):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: could not construct cycle")
		return false
	var cycle: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not _assert_preserved(runtime, cycle, recovered_generation,
			recovered_instructions, recovered_fingerprint, 61.0, 3.5,
			recovered_envelope, "cyclic graph"):
		return false

	runtime.free()
	print("TERRAIN_PROGRAM_TRANSACTION_OK: invalid graph edits preserve bytecode, controls and bounds")
	return true


func _validate_bound_contract() -> bool:
	# A simple bounded author delta must produce a finite envelope. Default production
	# controls deliberately exceed the old fixed 220 m CPU guard, proving this test
	# protects the disappearance regression that motivated Phase 33.
	var terrain: Resource = TERRAIN_PROFILE.new()
	terrain.call("ensure_valid")
	var slot: Resource = terrain.call("create_shader_slot",
		SLOT_MODEL.Domain.DISPLACEMENT, "CI Bound Terrain") as Resource
	var graph: Resource = slot.get(&"graph") as Resource if slot != null else null
	if graph == null:
		_fail("TERRAIN_DISPLACEMENT_BOUND_FAILED: bounded graph was not created")
		return false
	var output_id: String = _find_node(graph, "OUTPUT_DISPLACEMENT")
	var constant_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(100.0, 100.0), {"value": 25.0}))
	if output_id.is_empty() or not bool(graph.call("connect_nodes", constant_id, 0, output_id, 0)):
		_fail("TERRAIN_DISPLACEMENT_BOUND_FAILED: bounded graph did not connect")
		return false
	var runtime: Node = SAFE_RUNTIME.new() as Node
	var stats: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	var envelope: Dictionary = stats.get("displacement_envelope", {}) as Dictionary
	if not bool(envelope.get("bounds_known", false)):
		runtime.free()
		_fail("TERRAIN_DISPLACEMENT_BOUND_FAILED: constant author delta was not bounded")
		return false
	var production_guard: float = float(envelope.get("production_max_abs_m", 0.0))
	if production_guard <= 220.0 \
			or not is_equal_approx(production_guard, EXPECTED_DEFAULT_PRODUCTION_GUARD_M):
		runtime.free()
		_fail("TERRAIN_DISPLACEMENT_BOUND_FAILED: default production envelope %.3f m is incorrect" \
			% production_guard)
		return false
	if float(envelope.get("total_max_abs_m", 0.0)) < production_guard + 24.9:
		runtime.free()
		_fail("TERRAIN_DISPLACEMENT_BOUND_FAILED: author delta was not included in total envelope")
		return false
	runtime.free()

	# Absolute base-height dependence is intentionally classified as unknown until a
	# stronger symbolic contract exists. The renderer must fail open for this case,
	# never pretend it has a small finite culling envelope.
	var unbounded_terrain: Resource = TERRAIN_PROFILE.new()
	unbounded_terrain.call("ensure_valid")
	var unbounded_slot: Resource = unbounded_terrain.call("create_shader_slot",
		SLOT_MODEL.Domain.DISPLACEMENT, "CI Unknown Bound") as Resource
	var unbounded_graph: Resource = unbounded_slot.get(&"graph") as Resource \
		if unbounded_slot != null else null
	if unbounded_graph == null:
		_fail("TERRAIN_DISPLACEMENT_BOUND_FAILED: unknown-bound graph was not created")
		return false
	var unbounded_output: String = _find_node(unbounded_graph, "OUTPUT_DISPLACEMENT")
	var base_input: String = String(unbounded_graph.call("add_node", "GAME_INPUT",
		Vector2(100.0, 100.0), {"source": "terrain_height_m"}))
	if unbounded_output.is_empty() or not bool(unbounded_graph.call("connect_nodes",
			base_input, 0, unbounded_output, 0)):
		_fail("TERRAIN_DISPLACEMENT_BOUND_FAILED: unknown-bound graph did not connect")
		return false
	var unbounded_runtime: Node = SAFE_RUNTIME.new() as Node
	var unbounded_stats: Dictionary = unbounded_runtime.call(
		"compile_from_terrain", unbounded_terrain) as Dictionary
	var unknown_envelope: Dictionary = unbounded_stats.get("displacement_envelope", {}) as Dictionary
	unbounded_runtime.free()
	if bool(unknown_envelope.get("bounds_known", true)) \
			or String(unknown_envelope.get("unknown_reason", "")).is_empty():
		_fail("TERRAIN_DISPLACEMENT_BOUND_FAILED: absolute base-height program was not marked unknown")
		return false

	print("TERRAIN_DISPLACEMENT_BOUND_OK: production and author displacement envelopes are conservative")
	return true


func _assert_preserved(runtime: Node, candidate: Dictionary, expected_generation: int,
		expected_instructions: int, expected_fingerprint: String,
		expected_height: float, expected_mountain_strength: float,
		expected_envelope: Dictionary, label: String) -> bool:
	if bool(candidate.get("candidate_valid", true)) \
			or not bool(candidate.get("candidate_rejected", false)):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: %s was not rejected" % label)
		return false
	if int(candidate.get("generation", -1)) != expected_generation \
			or int(candidate.get("instructions", -1)) != expected_instructions \
			or String(candidate.get("active_fingerprint", "")) != expected_fingerprint:
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: %s changed active bytecode state" % label)
		return false
	var controls: Dictionary = candidate.get("production_geomorph_controls", {}) as Dictionary
	if not is_equal_approx(float(controls.get("mountain_strength", 0.0)), expected_mountain_strength):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: %s leaked rejected production controls" % label)
		return false
	var envelope: Dictionary = candidate.get("displacement_envelope", {}) as Dictionary
	if not is_equal_approx(float(envelope.get("production_max_abs_m", -1.0)),
			float(expected_envelope.get("production_max_abs_m", -2.0))) \
			or bool(envelope.get("bounds_known", false)) != bool(expected_envelope.get("bounds_known", false)):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: %s changed active displacement bounds" % label)
		return false
	var height: float = float(runtime.call("evaluate_height",
		Vector3.RIGHT, 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(height, expected_height):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: %s changed rendered/contact height" % label)
		return false
	return true


func _find_node(graph: Resource, node_type: String) -> String:
	for node_value: Variant in graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		if String(node.get("type", "")) == node_type:
			return String(node.get("id", ""))
	return ""
