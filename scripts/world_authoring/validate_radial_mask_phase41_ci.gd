extends Node
## Phase 41 Radial Area regression.
##
## Radial Area is serialized in the backward-compatible LATITUDE_MASK family with
## axis="radial" and compiles to opcode 28. CPU/contact and GPU vertex evaluation
## must use the same great-circle angular metric, including antimeridian and polar
## cases, exterior-only feathering, conservative bounds and transactional rollback.

const GRAPH := preload("res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
const TERRAIN := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const SLOT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const RUNTIME := preload("res://scripts/world_authoring/terrain_displacement_runtime_phase41.gd")
const GPU_VM_PATH := "res://shaders/terrain_author_displacement_bytecode.gdshaderinc"


func _ready() -> void:
	var error: String = _validate()
	if not error.is_empty():
		push_error("RADIAL_MASK_PHASE41_FAILED: " + error)
		get_tree().quit(1)
		return
	print("RADIAL_MASK_PHASE41_OK: great-circle radius, seam/pole continuity, exterior feather, inversion, bounds and rollback all match")
	get_tree().quit(0)


func _validate() -> String:
	var terrain: Resource = TERRAIN.new()
	terrain.call("ensure_valid")
	var slot: Resource = terrain.call("create_shader_slot",
		SLOT.Domain.DISPLACEMENT, "CI Radial Area") as Resource
	if slot == null:
		return "could not create displacement slot"
	slot.set(&"enabled", true)
	slot.set(&"strength", 1.0)
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return "displacement slot has no graph"
	var radial_id: String = _install_radial_graph(graph, 0.0, 0.0, 20.0, 10.0, false)
	if radial_id.is_empty():
		return "could not install Radial Area graph"

	var serialized: Dictionary = _node_by_id(graph, radial_id)
	if String(serialized.get("type", "")) != "LATITUDE_MASK":
		return "Radial Area changed the canonical serialized spatial node family"
	var parameters: Dictionary = serialized.get("parameters", {}) as Dictionary
	if String(parameters.get("axis", "")) != "radial":
		return "Radial Area axis discriminator was not serialized"
	var clone: Resource = graph.duplicate(true) as Resource
	var cloned_parameters: Dictionary = (_node_by_id(clone, radial_id).get("parameters", {}) as Dictionary)
	if String(cloned_parameters.get("axis", "")) != "radial" \
			or not is_equal_approx(float(cloned_parameters.get("radius_deg", 999.0)), 20.0) \
			or not is_equal_approx(float(cloned_parameters.get("feather_deg", 999.0)), 10.0):
		return "Radial Area parameters did not survive a deep graph clone"

	var runtime: Node = RUNTIME.new() as Node
	if runtime == null:
		return "Phase 41 runtime could not instantiate"
	var compiled: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(compiled.get("candidate_valid", false)) \
			or not bool(compiled.get("active", false)):
		runtime.free()
		return "valid Radial Area graph did not compile"
	if int(compiled.get("radial_mask_opcode", -1)) != 28 \
			or not bool(compiled.get("spatial_radial_mask", false)) \
			or String(compiled.get("radial_mask_metric", "")) != "great_circle_angle":
		runtime.free()
		return "runtime did not advertise Radial Area opcode 28 with great-circle semantics"
	var envelope: Dictionary = compiled.get("displacement_envelope", {}) as Dictionary
	if not bool(envelope.get("bounds_known", false)) \
			or not is_equal_approx(float(envelope.get("author_program_min_m", -1.0)), 0.0) \
			or not is_equal_approx(float(envelope.get("author_program_max_m", -1.0)), 100.0):
		runtime.free()
		return "Radial Area did not preserve exact [0,100] m conservative bounds"

	# Center and requested radius stay fully selected. Feather is outside-only: a
	# sample halfway through a 10 degree feather returns exactly 0.5.
	var samples: Array[Dictionary] = [
		{"lat":0.0, "lon":0.0, "expected":100.0},
		{"lat":10.0, "lon":0.0, "expected":100.0},
		{"lat":20.0, "lon":0.0, "expected":100.0},
		{"lat":25.0, "lon":0.0, "expected":50.0},
		{"lat":30.0, "lon":0.0, "expected":0.0},
		{"lat":0.0, "lon":90.0, "expected":0.0},
	]
	for sample: Dictionary in samples:
		var latitude: float = float(sample.get("lat", 0.0))
		var longitude: float = float(sample.get("lon", 0.0))
		var actual: float = float(runtime.call("evaluate_height",
			_direction(latitude, longitude), 0.0, 0, 0, 0.0, 0.0))
		var expected: float = float(sample.get("expected", 0.0))
		if absf(actual - expected) > 0.001:
			runtime.free()
			return "radial lat %.1f lon %.1f expected %.3f m, got %.6f m" % [latitude, longitude, expected, actual]

	var normalized_sample: float = float(runtime.call("evaluate_height",
		_direction(8.0, 6.0), 0.0, 0, 0, 0.0, 0.0))
	var scaled_sample: float = float(runtime.call("evaluate_height",
		_direction(8.0, 6.0) * 411.0, 0.0, 0, 0, 0.0, 0.0))
	if absf(normalized_sample - scaled_sample) > 0.001:
		runtime.free()
		return "Radial Area changed for a non-unit copy of the same planet direction"

	# A cap centered just west of +180 must continue seamlessly through -180.
	graph.call("set_node_parameter", radial_id, "center_latitude_deg", 20.0)
	graph.call("set_node_parameter", radial_id, "center_longitude_deg", 179.0)
	graph.call("set_node_parameter", radial_id, "radius_deg", 5.0)
	graph.call("set_node_parameter", radial_id, "feather_deg", 0.0)
	var seam_compiled: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(seam_compiled.get("candidate_valid", false)):
		runtime.free()
		return "antimeridian Radial Area did not compile"
	var seam_inside: float = float(runtime.call("evaluate_height",
		_direction(20.0, -179.0), 0.0, 0, 0, 0.0, 0.0))
	var seam_outside: float = float(runtime.call("evaluate_height",
		_direction(20.0, 170.0), 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(seam_inside, 100.0) or not is_equal_approx(seam_outside, 0.0):
		runtime.free()
		return "Radial Area is not continuous across the antimeridian"

	# Around a pole longitude must become irrelevant to angular distance.
	graph.call("set_node_parameter", radial_id, "center_latitude_deg", 90.0)
	graph.call("set_node_parameter", radial_id, "center_longitude_deg", 0.0)
	graph.call("set_node_parameter", radial_id, "radius_deg", 3.0)
	var pole_compiled: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(pole_compiled.get("candidate_valid", false)):
		runtime.free()
		return "polar Radial Area did not compile"
	for longitude: float in [-150.0, -20.0, 80.0, 179.0]:
		var pole_value: float = float(runtime.call("evaluate_height",
			_direction(88.0, longitude), 0.0, 0, 0, 0.0, 0.0))
		if not is_equal_approx(pole_value, 100.0):
			runtime.free()
			return "polar Radial Area incorrectly depends on longitude %.1f" % longitude

	# Inversion complements the finished spherical cap through ordinary VM math.
	graph.call("set_node_parameter", radial_id, "center_latitude_deg", 0.0)
	graph.call("set_node_parameter", radial_id, "center_longitude_deg", 0.0)
	graph.call("set_node_parameter", radial_id, "radius_deg", 20.0)
	graph.call("set_node_parameter", radial_id, "feather_deg", 0.0)
	graph.call("set_node_parameter", radial_id, "invert", true)
	var inverted: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(inverted.get("candidate_valid", false)):
		runtime.free()
		return "inverted Radial Area did not compile"
	var inverted_center: float = float(runtime.call("evaluate_height",
		_direction(0.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	var inverted_outside: float = float(runtime.call("evaluate_height",
		_direction(0.0, 90.0), 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(inverted_center, 0.0) or not is_equal_approx(inverted_outside, 100.0):
		runtime.free()
		return "Radial Area inversion did not complement the spherical cap"

	# Radius 180 is the full sphere by definition, regardless of center.
	graph.call("set_node_parameter", radial_id, "invert", false)
	graph.call("set_node_parameter", radial_id, "radius_deg", 180.0)
	graph.call("set_node_parameter", radial_id, "feather_deg", 20.0)
	var full_sphere: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(full_sphere.get("candidate_valid", false)):
		runtime.free()
		return "full-sphere Radial Area did not compile"
	for sample_direction: Vector3 in [_direction(-70.0, -120.0), _direction(0.0, 0.0), _direction(65.0, 130.0)]:
		var full_value: float = float(runtime.call("evaluate_height",
			sample_direction, 0.0, 0, 0, 0.0, 0.0))
		if not is_equal_approx(full_value, 100.0):
			runtime.free()
			return "180 degree Radial Area did not select the full sphere"

	# Last-known-good terrain must survive a candidate with an invalid radius.
	graph.call("set_node_parameter", radial_id, "radius_deg", 20.0)
	graph.call("set_node_parameter", radial_id, "feather_deg", 5.0)
	var good: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(good.get("candidate_valid", false)):
		runtime.free()
		return "could not restore valid Radial Area before rollback test"
	var good_generation: int = int(good.get("generation", -1))
	var good_value: float = float(runtime.call("evaluate_height",
		_direction(0.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	graph.call("set_node_parameter", radial_id, "radius_deg", 181.0)
	var rejected: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if bool(rejected.get("candidate_valid", true)) \
			or not bool(rejected.get("candidate_rejected", false)) \
			or int(rejected.get("generation", -2)) != good_generation:
		runtime.free()
		return "invalid Radial Area replaced the last-known-good program"
	var preserved_value: float = float(runtime.call("evaluate_height",
		_direction(0.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(preserved_value, good_value):
		runtime.free()
		return "invalid Radial Area changed active terrain output"

	var shader_source: String = FileAccess.get_file_as_string(GPU_VM_PATH)
	if shader_source.is_empty():
		runtime.free()
		return "GPU displacement VM source could not be read"
	if shader_source.find("vec3 ad_direction_from_latlon") < 0 \
			or shader_source.find("float ad_radial_mask") < 0 \
			or shader_source.find("else if (op == 28) v = ad_radial_mask(direction, p);") < 0:
		runtime.free()
		return "GPU VM is missing Radial Area opcode 28 great-circle evaluation"

	runtime.free()
	return ""


func _install_radial_graph(graph: Resource, center_latitude_deg: float,
		center_longitude_deg: float, radius_deg: float, feather_deg: float,
		invert_mask: bool) -> String:
	graph.set(&"domain", GRAPH.Domain.DISPLACEMENT)
	_clear_graph(graph)
	var height_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(80.0, 100.0), {"value":100.0}))
	var radial_id: String = String(graph.call("add_node", "LATITUDE_MASK",
		Vector2(80.0, 260.0), {
			"axis":"radial",
			"center_latitude_deg":center_latitude_deg,
			"center_longitude_deg":center_longitude_deg,
			"radius_deg":radius_deg,
			"feather_deg":feather_deg,
			"invert":invert_mask,
		}))
	var multiply_id: String = String(graph.call("add_node", "MULTIPLY",
		Vector2(400.0, 170.0), {}))
	var output_id: String = String(graph.call("add_node", "OUTPUT_DISPLACEMENT",
		Vector2(680.0, 170.0), {}))
	if height_id.is_empty() or radial_id.is_empty() or multiply_id.is_empty() or output_id.is_empty():
		return ""
	if not bool(graph.call("connect_nodes", height_id, 0, multiply_id, 0)) \
			or not bool(graph.call("connect_nodes", radial_id, 0, multiply_id, 1)) \
			or not bool(graph.call("connect_nodes", multiply_id, 0, output_id, 0)):
		return ""
	return radial_id


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
