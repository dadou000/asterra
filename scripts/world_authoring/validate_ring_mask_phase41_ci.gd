extends Node
## Phase 41 Ring Area regression.
##
## Ring Area is serialized in the backward-compatible LATITUDE_MASK family with
## axis="ring". It lowers to two opcode-28 great-circle selectors plus Multiply:
## the inner selector feathers inward into the hole, while the outer selector
## feathers outward beyond the ring. CPU/contact and GPU must remain identical.

const GRAPH := preload("res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
const TERRAIN := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const SLOT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const RUNTIME := preload("res://scripts/world_authoring/terrain_displacement_runtime_phase41.gd")
const GPU_VM_PATH := "res://shaders/terrain_author_displacement_bytecode.gdshaderinc"


func _ready() -> void:
	var error: String = _validate()
	if not error.is_empty():
		push_error("RING_MASK_PHASE41_FAILED: " + error)
		get_tree().quit(1)
		return
	print("RING_MASK_PHASE41_OK: great-circle annulus, two-sided exterior feather, seam/pole continuity, inversion, bounds and rollback all match")
	get_tree().quit(0)


func _validate() -> String:
	var terrain: Resource = TERRAIN.new()
	terrain.call("ensure_valid")
	var slot: Resource = terrain.call("create_shader_slot",
		SLOT.Domain.DISPLACEMENT, "CI Ring Area") as Resource
	if slot == null:
		return "could not create displacement slot"
	slot.set(&"enabled", true)
	slot.set(&"strength", 1.0)
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return "displacement slot has no graph"
	var ring_id: String = _install_ring_graph(graph, 0.0, 0.0, 10.0, 20.0, 5.0, false)
	if ring_id.is_empty():
		return "could not install Ring Area graph"

	var serialized: Dictionary = _node_by_id(graph, ring_id)
	if String(serialized.get("type", "")) != "LATITUDE_MASK":
		return "Ring Area changed the canonical serialized spatial node family"
	var parameters: Dictionary = serialized.get("parameters", {}) as Dictionary
	if String(parameters.get("axis", "")) != "ring":
		return "Ring Area axis discriminator was not serialized"
	var clone: Resource = graph.duplicate(true) as Resource
	var cloned_parameters: Dictionary = (_node_by_id(clone, ring_id).get("parameters", {}) as Dictionary)
	if String(cloned_parameters.get("axis", "")) != "ring" \
			or not is_equal_approx(float(cloned_parameters.get("inner_radius_deg", 999.0)), 10.0) \
			or not is_equal_approx(float(cloned_parameters.get("outer_radius_deg", 999.0)), 20.0) \
			or not is_equal_approx(float(cloned_parameters.get("feather_deg", 999.0)), 5.0):
		return "Ring Area parameters did not survive a deep graph clone"

	var runtime: Node = RUNTIME.new() as Node
	if runtime == null:
		return "Phase 41 runtime could not instantiate"
	var compiled: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(compiled.get("candidate_valid", false)) \
			or not bool(compiled.get("active", false)):
		runtime.free()
		return "valid Ring Area graph did not compile"
	if int(compiled.get("radial_mask_opcode", -1)) != 28 \
			or not bool(compiled.get("spatial_ring_mask", false)) \
			or String(compiled.get("ring_mask_lowering", "")) != "inner_outside_radial*outer_radial; invert=1-ring":
		runtime.free()
		return "runtime did not advertise Ring Area lowering through radial opcode 28"
	var envelope: Dictionary = compiled.get("displacement_envelope", {}) as Dictionary
	if not bool(envelope.get("bounds_known", false)) \
			or not is_equal_approx(float(envelope.get("author_program_min_m", -1.0)), 0.0) \
			or not is_equal_approx(float(envelope.get("author_program_max_m", -1.0)), 100.0):
		runtime.free()
		return "Ring Area did not preserve exact [0,100] m conservative bounds"

	# Requested annulus is fully selected from 10 through 20 degrees. The 5 degree
	# feather lies outside that interval: 5..10 inward and 20..25 outward.
	var samples: Array[Dictionary] = [
		{"angle":0.0, "expected":0.0},
		{"angle":5.0, "expected":0.0},
		{"angle":7.5, "expected":50.0},
		{"angle":10.0, "expected":100.0},
		{"angle":15.0, "expected":100.0},
		{"angle":20.0, "expected":100.0},
		{"angle":22.5, "expected":50.0},
		{"angle":25.0, "expected":0.0},
		{"angle":60.0, "expected":0.0},
	]
	for sample: Dictionary in samples:
		var angle: float = float(sample.get("angle", 0.0))
		var actual: float = float(runtime.call("evaluate_height",
			_direction(angle, 0.0), 0.0, 0, 0, 0.0, 0.0))
		var expected: float = float(sample.get("expected", 0.0))
		if absf(actual - expected) > 0.001:
			runtime.free()
			return "ring angle %.1f expected %.3f m, got %.6f m" % [angle, expected, actual]

	var normalized_sample: float = float(runtime.call("evaluate_height",
		_direction(14.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	var scaled_sample: float = float(runtime.call("evaluate_height",
		_direction(14.0, 0.0) * 503.0, 0.0, 0, 0, 0.0, 0.0))
	if absf(normalized_sample - scaled_sample) > 0.001:
		runtime.free()
		return "Ring Area changed for a non-unit copy of the same planet direction"

	# Antimeridian continuity: center at +179 degrees, ring interval 1..5 degrees.
	graph.call("set_node_parameter", ring_id, "center_latitude_deg", 20.0)
	graph.call("set_node_parameter", ring_id, "center_longitude_deg", 179.0)
	graph.call("set_node_parameter", ring_id, "inner_radius_deg", 1.0)
	graph.call("set_node_parameter", ring_id, "outer_radius_deg", 5.0)
	graph.call("set_node_parameter", ring_id, "feather_deg", 0.0)
	var seam_compiled: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(seam_compiled.get("candidate_valid", false)):
		runtime.free()
		return "antimeridian Ring Area did not compile"
	var seam_ring: float = float(runtime.call("evaluate_height",
		_direction(20.0, -179.0), 0.0, 0, 0, 0.0, 0.0))
	var seam_hole: float = float(runtime.call("evaluate_height",
		_direction(20.0, 179.0), 0.0, 0, 0, 0.0, 0.0))
	var seam_outside: float = float(runtime.call("evaluate_height",
		_direction(20.0, 170.0), 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(seam_ring, 100.0) \
			or not is_equal_approx(seam_hole, 0.0) \
			or not is_equal_approx(seam_outside, 0.0):
		runtime.free()
		return "Ring Area is not continuous across the antimeridian"

	# Polar ring: every longitude at 88 degrees latitude is 2 degrees from north pole.
	graph.call("set_node_parameter", ring_id, "center_latitude_deg", 90.0)
	graph.call("set_node_parameter", ring_id, "center_longitude_deg", 0.0)
	graph.call("set_node_parameter", ring_id, "inner_radius_deg", 1.0)
	graph.call("set_node_parameter", ring_id, "outer_radius_deg", 3.0)
	var pole_compiled: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(pole_compiled.get("candidate_valid", false)):
		runtime.free()
		return "polar Ring Area did not compile"
	for longitude: float in [-150.0, -20.0, 80.0, 179.0]:
		var pole_value: float = float(runtime.call("evaluate_height",
			_direction(88.0, longitude), 0.0, 0, 0, 0.0, 0.0))
		if not is_equal_approx(pole_value, 100.0):
			runtime.free()
			return "polar Ring Area incorrectly depends on longitude %.1f" % longitude

	# Invert complements the finished annulus, not either radial selector separately.
	graph.call("set_node_parameter", ring_id, "center_latitude_deg", 0.0)
	graph.call("set_node_parameter", ring_id, "center_longitude_deg", 0.0)
	graph.call("set_node_parameter", ring_id, "inner_radius_deg", 10.0)
	graph.call("set_node_parameter", ring_id, "outer_radius_deg", 20.0)
	graph.call("set_node_parameter", ring_id, "feather_deg", 0.0)
	graph.call("set_node_parameter", ring_id, "invert", true)
	var inverted: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(inverted.get("candidate_valid", false)):
		runtime.free()
		return "inverted Ring Area did not compile"
	var inverted_hole: float = float(runtime.call("evaluate_height",
		_direction(0.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	var inverted_ring: float = float(runtime.call("evaluate_height",
		_direction(15.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	var inverted_outside: float = float(runtime.call("evaluate_height",
		_direction(40.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(inverted_hole, 100.0) \
			or not is_equal_approx(inverted_ring, 0.0) \
			or not is_equal_approx(inverted_outside, 100.0):
		runtime.free()
		return "Ring Area inversion did not complement the finished annulus"

	# Inner > outer is invalid and must leave the last-known-good program active.
	graph.call("set_node_parameter", ring_id, "invert", false)
	graph.call("set_node_parameter", ring_id, "feather_deg", 5.0)
	var good: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(good.get("candidate_valid", false)):
		runtime.free()
		return "could not restore valid Ring Area before rollback test"
	var good_generation: int = int(good.get("generation", -1))
	var good_value: float = float(runtime.call("evaluate_height",
		_direction(15.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	graph.call("set_node_parameter", ring_id, "inner_radius_deg", 30.0)
	graph.call("set_node_parameter", ring_id, "outer_radius_deg", 20.0)
	var rejected: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if bool(rejected.get("candidate_valid", true)) \
			or not bool(rejected.get("candidate_rejected", false)) \
			or int(rejected.get("generation", -2)) != good_generation:
		runtime.free()
		return "invalid Ring Area replaced the last-known-good program"
	var preserved_value: float = float(runtime.call("evaluate_height",
		_direction(15.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(preserved_value, good_value):
		runtime.free()
		return "invalid Ring Area changed active terrain output"

	var shader_source: String = FileAccess.get_file_as_string(GPU_VM_PATH)
	if shader_source.is_empty():
		runtime.free()
		return "GPU displacement VM source could not be read"
	if shader_source.find("float ad_radial_mask") < 0 \
			or shader_source.find("if (p.w < 0.0)") < 0 \
			or shader_source.find("smoothstep(radius_deg - feather_deg, radius_deg, angle_deg)") < 0 \
			or shader_source.find("else if (op == 28) v = ad_radial_mask(direction, p);") < 0:
		runtime.free()
		return "GPU VM is missing Ring Area inward radial feather semantics"

	runtime.free()
	return ""


func _install_ring_graph(graph: Resource, center_latitude_deg: float,
		center_longitude_deg: float, inner_radius_deg: float, outer_radius_deg: float,
		feather_deg: float, invert_mask: bool) -> String:
	graph.set(&"domain", GRAPH.Domain.DISPLACEMENT)
	_clear_graph(graph)
	var height_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(80.0, 100.0), {"value":100.0}))
	var ring_id: String = String(graph.call("add_node", "LATITUDE_MASK",
		Vector2(80.0, 260.0), {
			"axis":"ring",
			"center_latitude_deg":center_latitude_deg,
			"center_longitude_deg":center_longitude_deg,
			"inner_radius_deg":inner_radius_deg,
			"outer_radius_deg":outer_radius_deg,
			"feather_deg":feather_deg,
			"invert":invert_mask,
		}))
	var multiply_id: String = String(graph.call("add_node", "MULTIPLY",
		Vector2(400.0, 170.0), {}))
	var output_id: String = String(graph.call("add_node", "OUTPUT_DISPLACEMENT",
		Vector2(680.0, 170.0), {}))
	if height_id.is_empty() or ring_id.is_empty() or multiply_id.is_empty() or output_id.is_empty():
		return ""
	if not bool(graph.call("connect_nodes", height_id, 0, multiply_id, 0)) \
			or not bool(graph.call("connect_nodes", ring_id, 0, multiply_id, 1)) \
			or not bool(graph.call("connect_nodes", multiply_id, 0, output_id, 0)):
		return ""
	return ring_id


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


func _direction(latitude_deg: float, longitude_deg: float) -> Vector3:
	var latitude_rad: float = deg_to_rad(latitude_deg)
	var longitude_rad: float = deg_to_rad(longitude_deg)
	var horizontal: float = cos(latitude_rad)
	return Vector3(
		horizontal * sin(longitude_rad),
		sin(latitude_rad),
		horizontal * cos(longitude_rad))
