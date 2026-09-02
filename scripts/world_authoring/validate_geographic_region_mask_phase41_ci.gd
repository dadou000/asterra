extends Node
## Phase 41 Geographic Region regression.
##
## One editor-facing region is serialized as the backward-compatible LATITUDE_MASK
## family with axis="region". Runtime lowering must compose the already-proven
## latitude and longitude opcode-27 primitives rather than introducing separate
## CPU/GPU geographic math.

const GRAPH := preload("res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
const TERRAIN := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const SLOT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const RUNTIME := preload("res://scripts/world_authoring/terrain_displacement_runtime_phase41.gd")
const GPU_VM_PATH := "res://shaders/terrain_author_displacement_bytecode.gdshaderinc"


func _ready() -> void:
	var error: String = _validate()
	if not error.is_empty():
		push_error("GEOGRAPHIC_REGION_MASK_PHASE41_FAILED: " + error)
		get_tree().quit(1)
		return
	print("GEOGRAPHIC_REGION_MASK_PHASE41_OK: latitude/longitude intersection lowering, seam crossing, exterior feathers, inversion, bounds and rollback all match")
	get_tree().quit(0)


func _validate() -> String:
	var terrain: Resource = TERRAIN.new()
	terrain.call("ensure_valid")
	var slot: Resource = terrain.call("create_shader_slot",
		SLOT.Domain.DISPLACEMENT, "CI Geographic Region") as Resource
	if slot == null:
		return "could not create displacement slot"
	slot.set(&"enabled", true)
	slot.set(&"strength", 1.0)
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return "displacement slot has no graph"
	var region_id: String = _install_region_graph(graph,
		-30.0, 30.0, 10.0, -45.0, 45.0, 10.0, false)
	if region_id.is_empty():
		return "could not install Geographic Region graph"

	var serialized: Dictionary = _node_by_id(graph, region_id)
	if String(serialized.get("type", "")) != "LATITUDE_MASK":
		return "Geographic Region changed the canonical serialized spatial node family"
	var parameters: Dictionary = serialized.get("parameters", {}) as Dictionary
	if String(parameters.get("axis", "")) != "region":
		return "Geographic Region axis discriminator was not serialized"
	var clone: Resource = graph.duplicate(true) as Resource
	var cloned_parameters: Dictionary = (_node_by_id(clone, region_id).get("parameters", {}) as Dictionary)
	if String(cloned_parameters.get("axis", "")) != "region" \
			or not is_equal_approx(float(cloned_parameters.get("west_deg", 999.0)), -45.0) \
			or not is_equal_approx(float(cloned_parameters.get("longitude_feather_deg", 999.0)), 10.0):
		return "Geographic Region parameters did not survive a deep graph clone"

	var runtime: Node = RUNTIME.new() as Node
	if runtime == null:
		return "Phase 41 runtime could not instantiate"
	var compiled: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(compiled.get("candidate_valid", false)) \
			or not bool(compiled.get("active", false)):
		runtime.free()
		return "valid Geographic Region graph did not compile"
	if int(compiled.get("latitude_mask_opcode", -1)) != 27 \
			or not bool(compiled.get("spatial_geographic_region_mask", false)):
		runtime.free()
		return "runtime did not advertise Geographic Region lowering on spatial opcode 27"
	if int(compiled.get("instructions", 0)) < 5:
		runtime.free()
		return "Geographic Region did not expand into the expected spatial bytecode composition"
	var envelope: Dictionary = compiled.get("displacement_envelope", {}) as Dictionary
	if not bool(envelope.get("bounds_known", false)) \
			or not is_equal_approx(float(envelope.get("author_program_min_m", -1.0)), 0.0) \
			or not is_equal_approx(float(envelope.get("author_program_max_m", -1.0)), 100.0):
		runtime.free()
		return "Geographic Region did not preserve exact [0,100] m conservative bounds"

	# The region is the product of both masks. Both requested bands stay fully 1.0;
	# each feather exists only outside its requested edge.
	var samples: Array[Dictionary] = [
		{"lat":0.0, "lon":0.0, "expected":100.0},
		{"lat":30.0, "lon":45.0, "expected":100.0},
		{"lat":35.0, "lon":0.0, "expected":50.0},
		{"lat":0.0, "lon":50.0, "expected":50.0},
		{"lat":35.0, "lon":50.0, "expected":25.0},
		{"lat":40.0, "lon":0.0, "expected":0.0},
		{"lat":0.0, "lon":55.0, "expected":0.0},
	]
	for sample: Dictionary in samples:
		var latitude: float = float(sample.get("lat", 0.0))
		var longitude: float = float(sample.get("lon", 0.0))
		var actual: float = float(runtime.call("evaluate_height",
			_direction(latitude, longitude), 0.0, 0, 0, 0.0, 0.0))
		var expected: float = float(sample.get("expected", 0.0))
		if not is_equal_approx(actual, expected):
			runtime.free()
			return "region lat %.1f lon %.1f expected %.3f m, got %.6f m" % [latitude, longitude, expected, actual]

	var normalized_sample: float = float(runtime.call("evaluate_height",
		_direction(12.0, 20.0), 0.0, 0, 0, 0.0, 0.0))
	var scaled_sample: float = float(runtime.call("evaluate_height",
		_direction(12.0, 20.0) * 337.0, 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(normalized_sample, scaled_sample):
		runtime.free()
		return "Geographic Region changed for a non-unit copy of the same planet direction"

	# Cross the antimeridian while keeping latitude narrow. This exercises the exact
	# longitude primitive used by the standalone Longitude Band.
	graph.call("set_node_parameter", region_id, "south_deg", -20.0)
	graph.call("set_node_parameter", region_id, "north_deg", 20.0)
	graph.call("set_node_parameter", region_id, "feather_deg", 0.0)
	graph.call("set_node_parameter", region_id, "west_deg", 170.0)
	graph.call("set_node_parameter", region_id, "east_deg", -170.0)
	graph.call("set_node_parameter", region_id, "longitude_feather_deg", 0.0)
	var seam_compiled: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(seam_compiled.get("candidate_valid", false)):
		runtime.free()
		return "seam-crossing Geographic Region did not compile"
	for longitude: float in [175.0, 180.0, -180.0, -175.0]:
		var seam_value: float = float(runtime.call("evaluate_height",
			_direction(0.0, longitude), 0.0, 0, 0, 0.0, 0.0))
		if not is_equal_approx(seam_value, 100.0):
			runtime.free()
			return "seam longitude %.1f should be inside region but returned %.6f m" % [longitude, seam_value]
	var wrong_longitude: float = float(runtime.call("evaluate_height",
		_direction(0.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	var wrong_latitude: float = float(runtime.call("evaluate_height",
		_direction(30.0, 180.0), 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(wrong_longitude, 0.0) or not is_equal_approx(wrong_latitude, 0.0):
		runtime.free()
		return "Geographic Region intersection leaked outside one of its two axes"

	# Inversion complements the finished intersection. A point failing only one axis
	# must therefore become 1.0 as well as a point failing both axes.
	graph.call("set_node_parameter", region_id, "invert", true)
	var inverted: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(inverted.get("candidate_valid", false)):
		runtime.free()
		return "inverted Geographic Region did not compile"
	var inverted_inside: float = float(runtime.call("evaluate_height",
		_direction(0.0, 180.0), 0.0, 0, 0, 0.0, 0.0))
	var inverted_lon_out: float = float(runtime.call("evaluate_height",
		_direction(0.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	var inverted_lat_out: float = float(runtime.call("evaluate_height",
		_direction(30.0, 180.0), 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(inverted_inside, 0.0) \
			or not is_equal_approx(inverted_lon_out, 100.0) \
			or not is_equal_approx(inverted_lat_out, 100.0):
		runtime.free()
		return "Geographic Region inversion was applied per-axis instead of after intersection"

	var inverted_envelope: Dictionary = inverted.get("displacement_envelope", {}) as Dictionary
	if not bool(inverted_envelope.get("bounds_known", false)) \
			or not is_equal_approx(float(inverted_envelope.get("author_program_min_m", -1.0)), 0.0) \
			or not is_equal_approx(float(inverted_envelope.get("author_program_max_m", -1.0)), 100.0):
		runtime.free()
		return "inverted Geographic Region lost its exact conservative bound"

	var good_generation: int = int(inverted.get("generation", -1))
	var good_value: float = inverted_lat_out
	graph.call("set_node_parameter", region_id, "longitude_feather_deg", 181.0)
	var rejected: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if bool(rejected.get("candidate_valid", true)) \
			or not bool(rejected.get("candidate_rejected", false)) \
			or int(rejected.get("generation", -2)) != good_generation:
		runtime.free()
		return "invalid Geographic Region replaced the last-known-good program"
	var preserved_value: float = float(runtime.call("evaluate_height",
		_direction(30.0, 180.0), 0.0, 0, 0, 0.0, 0.0))
	if not is_equal_approx(preserved_value, good_value):
		runtime.free()
		return "invalid Geographic Region changed active terrain output"

	var shader_source: String = FileAccess.get_file_as_string(GPU_VM_PATH)
	if shader_source.is_empty():
		runtime.free()
		return "GPU displacement VM source could not be read"
	if shader_source.find("float ad_latitude_mask") < 0 \
			or shader_source.find("float ad_longitude_mask") < 0 \
			or shader_source.find("else if (op == 8) v = a * b;") < 0:
		runtime.free()
		return "GPU VM is missing a primitive required by Geographic Region lowering"

	runtime.free()
	return ""


func _install_region_graph(graph: Resource, south_deg: float, north_deg: float,
		latitude_feather_deg: float, west_deg: float, east_deg: float,
		longitude_feather_deg: float, invert_mask: bool) -> String:
	graph.set(&"domain", GRAPH.Domain.DISPLACEMENT)
	_clear_graph(graph)
	var height_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(80.0, 100.0), {"value":100.0}))
	var region_id: String = String(graph.call("add_node", "LATITUDE_MASK",
		Vector2(80.0, 260.0), {
			"axis":"region",
			"south_deg":south_deg,
			"north_deg":north_deg,
			"feather_deg":latitude_feather_deg,
			"west_deg":west_deg,
			"east_deg":east_deg,
			"longitude_feather_deg":longitude_feather_deg,
			"invert":invert_mask,
		}))
	var multiply_id: String = String(graph.call("add_node", "MULTIPLY",
		Vector2(400.0, 170.0), {}))
	var output_id: String = String(graph.call("add_node", "OUTPUT_DISPLACEMENT",
		Vector2(680.0, 170.0), {}))
	if height_id.is_empty() or region_id.is_empty() or multiply_id.is_empty() or output_id.is_empty():
		return ""
	if not bool(graph.call("connect_nodes", height_id, 0, multiply_id, 0)) \
			or not bool(graph.call("connect_nodes", region_id, 0, multiply_id, 1)) \
			or not bool(graph.call("connect_nodes", multiply_id, 0, output_id, 0)):
		return ""
	return region_id


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
