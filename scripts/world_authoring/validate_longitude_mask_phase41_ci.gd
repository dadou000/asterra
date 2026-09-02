extends Node
## Phase 41 longitude-band regression.
##
## Longitude is stored as the backward-compatible LATITUDE_MASK node variant with
## parameters.axis="longitude". CPU/contact and GPU must use the same seam-safe
## eastward arc semantics, preserve conservative [0,1] bounds, and keep the
## last-known-good program active when an invalid candidate is staged.

const GRAPH := preload("res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
const TERRAIN := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const SLOT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const RUNTIME := preload("res://scripts/world_authoring/terrain_displacement_runtime_phase41.gd")
const GPU_VM_PATH := "res://shaders/terrain_author_displacement_bytecode.gdshaderinc"


func _ready() -> void:
	var error: String = _validate()
	if not error.is_empty():
		push_error("LONGITUDE_MASK_PHASE41_FAILED: " + error)
		get_tree().quit(1)
		return
	print("LONGITUDE_MASK_PHASE41_OK: seam-safe planet longitude, CPU/GPU opcode parity, conservative bounds, inversion and rollback all match")
	get_tree().quit(0)


func _validate() -> String:
	var terrain: Resource = TERRAIN.new()
	terrain.call("ensure_valid")
	var slot: Resource = terrain.call("create_shader_slot",
		SLOT.Domain.DISPLACEMENT, "CI Longitude Mask") as Resource
	if slot == null:
		return "could not create displacement slot"
	slot.set(&"enabled", true)
	slot.set(&"strength", 1.0)
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return "displacement slot has no graph"
	var mask_id: String = _install_mask_graph(graph, -45.0, 45.0, 10.0, false)
	if mask_id.is_empty():
		return "could not install longitude mask graph"

	var serialized: Dictionary = _node_by_id(graph, mask_id)
	if String(serialized.get("type", "")) != "LATITUDE_MASK":
		return "longitude variant changed the canonical serialized node type"
	var serialized_params: Dictionary = serialized.get("parameters", {}) as Dictionary
	if String(serialized_params.get("axis", "")) != "longitude":
		return "longitude axis discriminator was not serialized"
	var clone: Resource = graph.duplicate(true) as Resource
	var cloned_params: Dictionary = (_node_by_id(clone, mask_id).get("parameters", {}) as Dictionary)
	if String(cloned_params.get("axis", "")) != "longitude":
		return "longitude axis discriminator did not survive a deep graph clone"

	var runtime: Node = RUNTIME.new() as Node
	if runtime == null:
		return "Phase 41 runtime could not instantiate"
	var compiled: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(compiled.get("candidate_valid", false)) \
			or not bool(compiled.get("active", false)):
		runtime.free()
		return "valid longitude graph did not compile"
	if int(compiled.get("latitude_mask_opcode", -1)) != 27 \
			or not bool(compiled.get("spatial_longitude_mask", false)):
		runtime.free()
		return "runtime did not advertise longitude support on spatial opcode 27"
	var envelope: Dictionary = compiled.get("displacement_envelope", {}) as Dictionary
	if not bool(envelope.get("bounds_known", false)) \
			or not is_equal_approx(float(envelope.get("author_program_min_m", -1.0)), 0.0) \
			or not is_equal_approx(float(envelope.get("author_program_max_m", -1.0)), 100.0):
		runtime.free()
		return "longitude mask did not preserve exact [0,100] m conservative bounds"

	var ordinary_samples: Array[Dictionary] = [
		{"lon":0.0, "expected":100.0},
		{"lon":-40.0, "expected":50.0},
		{"lon":40.0, "expected":50.0},
		{"lon":-45.0, "expected":0.0},
		{"lon":45.0, "expected":0.0},
		{"lon":90.0, "expected":0.0},
	]
	for sample: Dictionary in ordinary_samples:
		var longitude: float = float(sample.get("lon", 0.0))
		var actual: float = float(runtime.call("evaluate_height",
			_direction_at_longitude(longitude), 0.0, 0, 0, 0.0, 0.0))
		var expected: float = float(sample.get("expected", 0.0))
		if not is_equal_approx(actual, expected):
			runtime.free()
			return "longitude %.1f expected %.3f m, got %.6f m" % [longitude, expected, actual]

	var normalized_sample: float = float(runtime.call("evaluate_height",
		_direction_at_longitude(20.0), 0.0, 0, 0, 0.0, 0.0))
	var scaled_sample: float = float(runtime.call("evaluate_height",
		_direction_at_longitude(20.0) * 271.0, 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(normalized_sample, scaled_sample):
		runtime.free()
		return "longitude mask changed for a non-unit copy of the same planet direction"

	# West > east means an eastward arc crossing the +/-180 degree seam.
	graph.call("set_node_parameter", mask_id, "south_deg", 170.0)
	graph.call("set_node_parameter", mask_id, "north_deg", -170.0)
	graph.call("set_node_parameter", mask_id, "feather_deg", 0.0)
	var seam_compiled: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(seam_compiled.get("candidate_valid", false)):
		runtime.free()
		return "seam-crossing longitude band did not compile"
	for longitude: float in [175.0, 180.0, -180.0, -175.0]:
		var seam_value: float = float(runtime.call("evaluate_height",
			_direction_at_longitude(longitude), 0.0, 0, 0, 0.0, 0.0))
		if not is_equal_approx(seam_value, 100.0):
			runtime.free()
			return "seam longitude %.1f should be inside but returned %.6f m" % [longitude, seam_value]
	var opposite_value: float = float(runtime.call("evaluate_height",
		_direction_at_longitude(0.0), 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(opposite_value, 0.0):
		runtime.free()
		return "seam-crossing mask leaked onto the opposite meridian"

	graph.call("set_node_parameter", mask_id, "invert", true)
	var inverted: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(inverted.get("candidate_valid", false)):
		runtime.free()
		return "inverted longitude mask did not compile"
	var inverted_seam: float = float(runtime.call("evaluate_height",
		_direction_at_longitude(180.0), 0.0, 0, 0, 0.0, 0.0))
	var inverted_opposite: float = float(runtime.call("evaluate_height",
		_direction_at_longitude(0.0), 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(inverted_seam, 0.0) or not is_equal_approx(inverted_opposite, 100.0):
		runtime.free()
		return "longitude inversion did not complement the original mask"

	# Equal wrapped edges mean the full circumference by definition.
	graph.call("set_node_parameter", mask_id, "south_deg", 30.0)
	graph.call("set_node_parameter", mask_id, "north_deg", 30.0)
	graph.call("set_node_parameter", mask_id, "invert", false)
	var full_circle: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(full_circle.get("candidate_valid", false)):
		runtime.free()
		return "full-circle equal-edge mask did not compile"
	for longitude: float in [-150.0, 0.0, 120.0]:
		var full_value: float = float(runtime.call("evaluate_height",
			_direction_at_longitude(longitude), 0.0, 0, 0, 0.0, 0.0))
		if not is_equal_approx(full_value, 100.0):
			runtime.free()
			return "equal-edge full-circle longitude mask returned %.6f m" % full_value

	var good_generation: int = int(full_circle.get("generation", -1))
	var good_value: float = float(runtime.call("evaluate_height",
		_direction_at_longitude(120.0), 0.0, 0, 0, 0.0, 0.0))
	graph.call("set_node_parameter", mask_id, "feather_deg", 181.0)
	var rejected: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if bool(rejected.get("candidate_valid", true)) \
			or not bool(rejected.get("candidate_rejected", false)) \
			or int(rejected.get("generation", -2)) != good_generation:
		runtime.free()
		return "invalid longitude candidate replaced the last-known-good program"
	var preserved_value: float = float(runtime.call("evaluate_height",
		_direction_at_longitude(120.0), 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(preserved_value, good_value):
		runtime.free()
		return "invalid longitude candidate changed active terrain output"

	var shader_source: String = FileAccess.get_file_as_string(GPU_VM_PATH)
	if shader_source.is_empty():
		runtime.free()
		return "GPU displacement VM source could not be read"
	if shader_source.find("float ad_longitude_mask") < 0 \
			or shader_source.find("round(p.w) >= 2.0 ? ad_longitude_mask(direction, p) : ad_latitude_mask(direction, p)") < 0:
		runtime.free()
		return "GPU VM is missing the longitude branch of spatial opcode 27"

	runtime.free()
	return ""


func _install_mask_graph(graph: Resource, west_deg: float, east_deg: float,
		feather_deg: float, invert_mask: bool) -> String:
	graph.set(&"domain", GRAPH.Domain.DISPLACEMENT)
	_clear_graph(graph)
	var height_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(80.0, 100.0), {"value":100.0}))
	var mask_id: String = String(graph.call("add_node", "LATITUDE_MASK",
		Vector2(80.0, 260.0), {
			"axis":"longitude",
			"south_deg":west_deg,
			"north_deg":east_deg,
			"feather_deg":feather_deg,
			"invert":invert_mask,
		}))
	var multiply_id: String = String(graph.call("add_node", "MULTIPLY",
		Vector2(340.0, 160.0), {}))
	var output_id: String = String(graph.call("add_node", "OUTPUT_DISPLACEMENT",
		Vector2(600.0, 160.0), {}))
	if height_id.is_empty() or mask_id.is_empty() or multiply_id.is_empty() or output_id.is_empty():
		return ""
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


func _direction_at_longitude(longitude_deg: float) -> Vector3:
	var longitude_rad: float = deg_to_rad(longitude_deg)
	return Vector3(sin(longitude_rad), 0.0, cos(longitude_rad))
