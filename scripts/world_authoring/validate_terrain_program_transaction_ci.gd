extends "res://tests/validate_secondary_terrain_scripts.gd"
## Safety regression for interactive terrain graph authoring.
##
## A valid program is compiled first. We then reproduce ordinary GraphEdit states
## that occur while a beginner is editing (disconnecting Final Terrain, removing a
## required input, and making a cycle). Every broken candidate must be rejected
## while the previous bytecode, generation and evaluated terrain remain untouched.

const TERRAIN_PROFILE := preload(
	"res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const SLOT_MODEL := preload(
	"res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const SAFE_RUNTIME := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase32.gd")


func _ready() -> void:
	if not _validate_transaction_contract():
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
	if output_id.is_empty() or not bool(graph.call("connect_nodes",
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
	if not is_equal_approx(good_height, 37.0):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: baseline program evaluated incorrectly")
		return false

	# 1) Exact GraphEdit disconnect operation. The candidate must be rejected and
	# the baseline program must stay active even though the document revision moves.
	if not bool(graph.call("disconnect_nodes", constant_id, 0, output_id, 0)):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: could not disconnect Final Terrain")
		return false
	var disconnected: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not _assert_preserved(runtime, disconnected, good_generation,
			good_instructions, good_active_fingerprint, 37.0, "disconnected Final Terrain"):
		return false

	# Restore the graph and prove a later valid document can replace the old program.
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
	if not is_equal_approx(recovered_height, 61.0):
		runtime.free()
		_fail("TERRAIN_PROGRAM_TRANSACTION_FAILED: repaired graph did not commit")
		return false

	# 2) Missing required input. ADD requires both terrain inputs, so connecting only
	# one side is an ordinary half-finished edit and must never become active.
	var recovered_generation: int = int(recovered.get("generation", -1))
	var recovered_instructions: int = int(recovered.get("instructions", -1))
	var recovered_fingerprint: String = String(recovered.get("active_fingerprint", ""))
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
			recovered_instructions, recovered_fingerprint, 61.0, "missing required input"):
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
			recovered_instructions, recovered_fingerprint, 61.0, "cyclic graph"):
		return false

	runtime.free()
	print("TERRAIN_PROGRAM_TRANSACTION_OK: invalid graph edits preserve last-known-good terrain")
	return true


func _assert_preserved(runtime: Node, candidate: Dictionary, expected_generation: int,
		expected_instructions: int, expected_fingerprint: String,
		expected_height: float, label: String) -> bool:
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
