extends Node
## Phase 41 regression: one deterministic latitude-mask definition must drive the
## authored displacement bytecode on CPU/contact and the resident GPU VM contract.

const TERRAIN := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const SLOT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const RUNTIME := preload("res://scripts/world_authoring/terrain_displacement_runtime_phase41.gd")
const GPU_VM_PATH := "res://shaders/terrain_author_displacement_bytecode.gdshaderinc"


func _ready() -> void:
	var error: String = _validate()
	if not error.is_empty():
		push_error("LATITUDE_MASK_PHASE41_FAILED: " + error)
		get_tree().quit(1)
		return
	print("LATITUDE_MASK_PHASE41_OK: normalized planet-space latitude mask matches CPU/contact samples, GPU opcode contract, global conservative bounds, inversion and transactional rollback")
	get_tree().quit(0)


func _validate() -> String:
	var terrain: Resource = TERRAIN.new()
	terrain.call("ensure_valid")
	var slot: Resource = terrain.call("create_shader_slot",
		SLOT.Domain.DISPLACEMENT, "CI Latitude Mask") as Resource
	if slot == null:
		return "could not create displacement slot"
	slot.set(&"enabled", true)
	slot.set(&"strength", 1.0)
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return "displacement slot has no graph"
	_install_mask_graph(graph, false, 10.0)

	var runtime: Node = RUNTIME.new() as Node
	if runtime == null:
		return "Phase 41 runtime could not instantiate"
	var compiled: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(compiled.get("candidate_valid", false)) \
			or not bool(compiled.get("active", false)) \
			or int(compiled.get("instructions", 0)) <= 0:
		runtime.free()
		return "valid latitude-mask graph did not compile into active bytecode"
	if int(compiled.get("latitude_mask_opcode", -1)) != 27 \
			or not bool(compiled.get("spatial_latitude_mask", false)):
		runtime.free()
		return "Phase 41 runtime did not advertise the latitude-mask opcode"
	var envelope: Dictionary = compiled.get("displacement_envelope", {}) as Dictionary
	if not bool(envelope.get("bounds_known", false)) \
			or not is_equal_approx(float(envelope.get("author_program_min_m", -1.0)), 0.0) \
			or not is_equal_approx(float(envelope.get("author_program_max_m", -1.0)), 100.0):
		runtime.free()
		return "Latitude Mask did not preserve the exact global [0,100] m author-program envelope"
	if not String(envelope.get("unknown_reason", "")).is_empty():
		runtime.free()
		return "Latitude Mask unexpectedly forced the renderer maximum-bounds fallback"

	var samples: Array[Dictionary] = [
		{"lat": 0.0, "expected": 100.0},
		{"lat": 29.0, "expected": 100.0},
		{"lat": 35.0, "expected": 50.0},
		{"lat": -35.0, "expected": 50.0},
		{"lat": 40.0, "expected": 0.0},
		{"lat": 60.0, "expected": 0.0},
	]
	for sample: Dictionary in samples:
		var latitude: float = float(sample.get("lat", 0.0))
		var direction: Vector3 = _direction_at_latitude(latitude)
		var actual: float = float(runtime.call("evaluate_height",
			direction, 0.0, 0, 0, 0.0, 0.0))
		var expected: float = float(sample.get("expected", 0.0))
		if not is_equal_approx(actual, expected):
			runtime.free()
			return "latitude %.1f expected %.3f m, got %.6f m" % [latitude, expected, actual]

	# Direction magnitude must never affect the planet-space mask.
	var normalized_sample: float = float(runtime.call("evaluate_height",
		_direction_at_latitude(35.0), 0.0, 0, 0, 0.0, 0.0))
	var scaled_sample: float = float(runtime.call("evaluate_height",
		_direction_at_latitude(35.0) * 173.0, 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(normalized_sample, scaled_sample):
		runtime.free()
		return "mask changed when the same planet direction was non-unit length"

	# Reverse limits are intentionally normalized to the same band, avoiding a
	# surprising empty mask when a beginner swaps North/South fields.
	var mask_node_id: String = "latitude_mask"
	graph.call("set_node_parameter", mask_node_id, "south_deg", 30.0)
	graph.call("set_node_parameter", mask_node_id, "north_deg", -30.0)
	var reversed: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(reversed.get("candidate_valid", false)):
		runtime.free()
		return "reversed latitude limits were not normalized"
	var reversed_equator: float = float(runtime.call("evaluate_height",
		Vector3.RIGHT, 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(reversed_equator, 100.0):
		runtime.free()
		return "reversed latitude limits changed the intended band"

	# Inversion is part of the same opcode, not a second graph implementation.
	graph.call("set_node_parameter", mask_node_id, "south_deg", -30.0)
	graph.call("set_node_parameter", mask_node_id, "north_deg", 30.0)
	graph.call("set_node_parameter", mask_node_id, "invert", true)
	var inverted: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(inverted.get("candidate_valid", false)):
		runtime.free()
		return "inverted latitude mask did not compile"
	var inverted_equator: float = float(runtime.call("evaluate_height",
		Vector3.RIGHT, 0.0, 0, 0, 0.0, 0.0))
	var inverted_high_lat: float = float(runtime.call("evaluate_height",
		_direction_at_latitude(60.0), 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(inverted_equator, 0.0) \
			or not is_equal_approx(inverted_high_lat, 100.0):
		runtime.free()
		return "inverted mask did not complement the original [0,1] mask"

	# Invalid staged settings must preserve the exact last-known-good bytecode.
	var good_generation: int = int(inverted.get("generation", -1))
	var good_high_lat: float = inverted_high_lat
	graph.call("set_node_parameter", mask_node_id, "feather_deg", 120.0)
	var rejected: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if bool(rejected.get("candidate_valid", true)) \
			or not bool(rejected.get("candidate_rejected", false)) \
			or int(rejected.get("generation", -2)) != good_generation:
		runtime.free()
		return "invalid latitude mask replaced the last-known-good program"
	var preserved_high_lat: float = float(runtime.call("evaluate_height",
		_direction_at_latitude(60.0), 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(preserved_high_lat, good_high_lat):
		runtime.free()
		return "invalid latitude mask changed the active terrain result"

	var shader_source: String = FileAccess.get_file_as_string(GPU_VM_PATH)
	if shader_source.is_empty():
		runtime.free()
		return "GPU displacement VM source could not be read"
	if shader_source.find("float ad_latitude_mask") < 0 \
			or shader_source.find("else if (op == 27) v = ad_latitude_mask(direction, p);") < 0:
		runtime.free()
		return "GPU VM does not expose the matching opcode 27 latitude-mask contract"

	runtime.free()
	return ""


func _install_mask_graph(graph: Resource, invert_mask: bool, feather_deg: float) -> void:
	var nodes: Array[Dictionary] = [
		{
			"id":"height",
			"type":"CONSTANT_FLOAT",
			"position":Vector2(80.0, 100.0),
			"parameters":{"value":100.0},
		},
		{
			"id":"latitude_mask",
			"type":"LATITUDE_MASK",
			"position":Vector2(80.0, 260.0),
			"parameters":{
				"south_deg":-30.0,
				"north_deg":30.0,
				"feather_deg":feather_deg,
				"invert":invert_mask,
			},
		},
		{
			"id":"multiply",
			"type":"MULTIPLY",
			"position":Vector2(340.0, 160.0),
			"parameters":{},
		},
		{
			"id":"output",
			"type":"OUTPUT_DISPLACEMENT",
			"position":Vector2(600.0, 160.0),
			"parameters":{},
		},
	]
	var links: Array[Dictionary] = [
		{"from":"height", "from_port":0, "to":"multiply", "to_port":0},
		{"from":"latitude_mask", "from_port":0, "to":"multiply", "to_port":1},
		{"from":"multiply", "from_port":0, "to":"output", "to_port":0},
	]
	graph.set(&"nodes", nodes)
	graph.set(&"links", links)
	graph.set(&"revision", int(graph.get(&"revision")) + 1)


func _direction_at_latitude(latitude_deg: float) -> Vector3:
	var latitude_rad: float = deg_to_rad(latitude_deg)
	return Vector3(cos(latitude_rad), sin(latitude_rad), 0.0)
