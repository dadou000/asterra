extends Node
## Phase 41 regression: one deterministic latitude-mask definition must drive the
## canonical serialized graph, authored displacement bytecode on CPU/contact and
## the resident GPU VM contract.

const GRAPH := preload("res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
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
	print("LATITUDE_MASK_PHASE41_OK: canonical schema, normalized planet-space CPU/contact samples, GPU opcode contract, conservative bounds, inversion and transactional rollback all match")
	get_tree().quit(0)


func _validate() -> String:
	var schema_error: String = _validate_schema_contract()
	if not schema_error.is_empty():
		return schema_error

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
	var mask_node_id: String = _install_mask_graph(graph, false, 10.0)
	if mask_node_id.is_empty():
		return "canonical graph.add_node did not create Latitude Mask"

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

	var normalized_sample: float = float(runtime.call("evaluate_height",
		_direction_at_latitude(35.0), 0.0, 0, 0, 0.0, 0.0))
	var scaled_sample: float = float(runtime.call("evaluate_height",
		_direction_at_latitude(35.0) * 173.0, 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(normalized_sample, scaled_sample):
		runtime.free()
		return "mask changed when the same planet direction was non-unit length"

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
		_direction_at_latitude(60.0), 0.0, 0, 0.0, 0.0))
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


func _validate_schema_contract() -> String:
	var displacement_catalog: Array[String] = GRAPH.node_catalog(GRAPH.Domain.DISPLACEMENT)
	var material_catalog: Array[String] = GRAPH.node_catalog(GRAPH.Domain.MATERIAL)
	if not displacement_catalog.has("LATITUDE_MASK"):
		return "canonical displacement catalog does not contain LATITUDE_MASK"
	if material_catalog.has("LATITUDE_MASK"):
		return "LATITUDE_MASK leaked into the material graph catalog"
	if GRAPH.node_category("LATITUDE_MASK") != GRAPH.CATEGORY_MASKS:
		return "LATITUDE_MASK is not grouped in the canonical Masks category"

	var graph: Resource = GRAPH.new()
	graph.set(&"domain", GRAPH.Domain.DISPLACEMENT)
	_clear_graph(graph)
	var node_id: String = String(graph.call("add_node", "LATITUDE_MASK", Vector2(12.0, 34.0), {}))
	if node_id.is_empty():
		return "canonical add_node returned an empty Latitude Mask ID"
	var node: Dictionary = _node_by_id(graph, node_id)
	if String(node.get("type", "")) != "LATITUDE_MASK":
		return "canonical add_node silently converted Latitude Mask to another type"
	var parameters: Dictionary = node.get("parameters", {}) as Dictionary
	if not is_equal_approx(float(parameters.get("south_deg", 999.0)), -30.0) \
			or not is_equal_approx(float(parameters.get("north_deg", 999.0)), 30.0) \
			or not is_equal_approx(float(parameters.get("feather_deg", 999.0)), 5.0) \
			or bool(parameters.get("invert", true)):
		return "canonical Latitude Mask defaults changed"
	graph.call("set_node_parameter", node_id, "north_deg", 44.0)
	var clone: Resource = graph.duplicate(true) as Resource
	var cloned_node: Dictionary = _node_by_id(clone, node_id)
	var cloned_parameters: Dictionary = cloned_node.get("parameters", {}) as Dictionary
	if String(cloned_node.get("type", "")) != "LATITUDE_MASK" \
			or not is_equal_approx(float(cloned_parameters.get("north_deg", 0.0)), 44.0):
		return "Latitude Mask type/parameters did not survive a deep graph clone"
	return ""


func _install_mask_graph(graph: Resource, invert_mask: bool, feather_deg: float) -> String:
	graph.set(&"domain", GRAPH.Domain.DISPLACEMENT)
	_clear_graph(graph)
	var height_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(80.0, 100.0), {"value":100.0}))
	var mask_id: String = String(graph.call("add_node", "LATITUDE_MASK",
		Vector2(80.0, 260.0), {}))
	var multiply_id: String = String(graph.call("add_node", "MULTIPLY",
		Vector2(340.0, 160.0), {}))
	var output_id: String = String(graph.call("add_node", "OUTPUT_DISPLACEMENT",
		Vector2(600.0, 160.0), {}))
	if height_id.is_empty() or mask_id.is_empty() or multiply_id.is_empty() or output_id.is_empty():
		return ""
	graph.call("set_node_parameter", mask_id, "feather_deg", feather_deg)
	graph.call("set_node_parameter", mask_id, "invert", invert_mask)
	if not bool(graph.call("connect_nodes", height_id, 0, multiply_id, 0)) \
			or not bool(graph.call("connect_nodes", mask_id, 0, multiply_id, 1)) \
			or not bool(graph.call("connect_nodes", multiply_id, 0, output_id, 0)):
		return ""
	return mask_id


func _clear_graph(graph: Resource) -> void:
	var empty_nodes: Array[Dictionary] = []
	var empty_links: Array[Dictionary] = []
	graph.set(&"nodes", empty_nodes)
	graph.set(&"links", empty_links)


func _node_by_id(graph: Resource, node_id: String) -> Dictionary:
	if graph == null:
		return {}
	for value: Variant in graph.get(&"nodes") as Array:
		if value is Dictionary and String((value as Dictionary).get("id", "")) == node_id:
			return value as Dictionary
	return {}


func _direction_at_latitude(latitude_deg: float) -> Vector3:
	var latitude_rad: float = deg_to_rad(latitude_deg)
	return Vector3(cos(latitude_rad), sin(latitude_rad), 0.0)
