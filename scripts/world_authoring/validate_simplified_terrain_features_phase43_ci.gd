extends Node
## Phase 43 regression for the simplified effect × area authoring model.

const TERRAIN := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const SLOT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const GUIDED := preload("res://scripts/world_authoring/model/terrain_guided_feature_graph.gd")
const RUNTIME := preload("res://scripts/world_authoring/terrain_displacement_runtime_phase41.gd")


func _ready() -> void:
	var error: String = _validate()
	if not error.is_empty():
		push_error("SIMPLIFIED_TERRAIN_PHASE43_FAILED: " + error)
		get_tree().quit(1)
		return
	print("SIMPLIFIED_TERRAIN_PHASE43_OK: guided effects and geographic areas compile through the authoritative displacement runtime")
	get_tree().quit(0)


func _validate() -> String:
	var terrain: Resource = TERRAIN.new()
	terrain.call("ensure_valid")
	var slot: Resource = terrain.call("create_shader_slot",
		SLOT.Domain.DISPLACEMENT, "CI Simplified Feature") as Resource
	if slot == null:
		return "could not create displacement slot"
	slot.set(&"enabled", true)
	slot.set(&"strength", 1.0)
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return "created slot has no graph"

	var config: Dictionary = GUIDED.default_config()
	config["effect_kind"] = GUIDED.EFFECT_HEIGHT
	config["area_kind"] = GUIDED.AREA_RADIAL
	config["amount_m"] = 100.0
	config["center_latitude_deg"] = 0.0
	config["center_longitude_deg"] = 0.0
	config["radius_deg"] = 20.0
	config["radial_feather_deg"] = 10.0
	if not GUIDED.rebuild(graph, config):
		return "could not build default guided graph"
	if not GUIDED.is_guided_graph(graph):
		return "guided graph marker was not serialized"
	if GUIDED.summary(graph) != "Raise / Lower · Radial Area":
		return "guided summary does not describe effect and area"
	if int(graph.get(&"displacement_output_mode")) != 0:
		return "guided feature is not an additive displacement delta graph"
	if GUIDED.estimated_vertex_instruction_cost(graph) != 6:
		return "Simple radial height feature cost no longer matches its vertex-program lowering"

	var runtime: Node = RUNTIME.new() as Node
	var compiled: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(compiled.get("candidate_valid", false)) or not bool(compiled.get("active", false)):
		runtime.free()
		return "default guided feature did not compile"
	var center: float = float(runtime.call("evaluate_height", _direction(0.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	var feather: float = float(runtime.call("evaluate_height", _direction(25.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	var outside: float = float(runtime.call("evaluate_height", _direction(35.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	if absf(center - 100.0) > 0.001 or absf(feather - 50.0) > 0.01 or absf(outside) > 0.001:
		runtime.free()
		return "guided radial height feature does not match the spatial bytecode semantics"

	if not GUIDED.set_config_value(graph, "amount_m", -40.0):
		runtime.free()
		return "guided amount could not be edited in place"
	compiled = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(compiled.get("candidate_valid", false)):
		runtime.free()
		return "negative Raise / Lower feature did not compile"
	center = float(runtime.call("evaluate_height", _direction(0.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	if absf(center + 40.0) > 0.001:
		runtime.free()
		return "guided Raise / Lower amount did not update the live graph"

	# Every exposed area must lower through the same runtime used by Node Graph.
	for area_kind: String in GUIDED.AREAS:
		config = GUIDED.default_config()
		config["effect_kind"] = GUIDED.EFFECT_HEIGHT
		config["area_kind"] = area_kind
		config["amount_m"] = 100.0
		config["south_deg"] = -20.0
		config["north_deg"] = 30.0
		config["west_deg"] = 170.0
		config["east_deg"] = -170.0
		config["center_latitude_deg"] = 15.0
		config["center_longitude_deg"] = 179.0
		config["radius_deg"] = 12.0
		config["inner_radius_deg"] = 5.0
		config["outer_radius_deg"] = 14.0
		if not GUIDED.rebuild(graph, config):
			runtime.free()
			return "could not build guided area %s" % area_kind
		compiled = runtime.call("compile_from_terrain", terrain) as Dictionary
		if not bool(compiled.get("candidate_valid", false)):
			runtime.free()
			return "guided area %s was rejected by the authoritative runtime" % area_kind

	# Ring must retain the same great-circle hole/band semantics as the Node Graph node.
	config = GUIDED.default_config()
	config["effect_kind"] = GUIDED.EFFECT_HEIGHT
	config["area_kind"] = GUIDED.AREA_RING
	config["amount_m"] = 100.0
	config["center_latitude_deg"] = 0.0
	config["center_longitude_deg"] = 0.0
	config["inner_radius_deg"] = 10.0
	config["outer_radius_deg"] = 20.0
	config["radial_feather_deg"] = 0.0
	GUIDED.rebuild(graph, config)
	if GUIDED.estimated_vertex_instruction_cost(graph) != 8:
		runtime.free()
		return "Simple Ring Area cost no longer accounts for both spherical mask edges"
	compiled = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(compiled.get("candidate_valid", false)):
		runtime.free()
		return "guided Ring Area did not compile"
	var hole: float = float(runtime.call("evaluate_height", _direction(0.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	var band: float = float(runtime.call("evaluate_height", _direction(15.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	var far: float = float(runtime.call("evaluate_height", _direction(30.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	if absf(hole) > 0.001 or absf(band - 100.0) > 0.001 or absf(far) > 0.001:
		runtime.free()
		return "guided Ring Area changed the proven ring semantics"

	# Every exposed effect must compile without a second/simple-only evaluator.
	for effect_kind: String in GUIDED.EFFECTS:
		config = GUIDED.default_config()
		config["effect_kind"] = effect_kind
		config["area_kind"] = GUIDED.AREA_EVERYWHERE
		config["amount_m"] = 25.0
		if not GUIDED.rebuild(graph, config):
			runtime.free()
			return "could not build guided effect %s" % effect_kind
		compiled = runtime.call("compile_from_terrain", terrain) as Dictionary
		if not bool(compiled.get("candidate_valid", false)):
			runtime.free()
			return "guided effect %s was rejected by the authoritative runtime" % effect_kind
		var value: float = float(runtime.call("evaluate_height", _direction(12.0, 34.0), 0.0, 0, 0, 0.0, 0.0))
		if not is_finite(value):
			runtime.free()
			return "guided effect %s produced a non-finite result" % effect_kind

	# Simple mode must never claim ownership of an arbitrary graph.
	graph.call("create_default_graph", 0)
	if GUIDED.is_guided_graph(graph) or GUIDED.summary(graph) != "Custom node graph":
		runtime.free()
		return "blank/custom graph was incorrectly classified as a guided feature"

	runtime.free()
	return ""


func _direction(latitude_deg: float, longitude_deg: float) -> Vector3:
	var latitude_rad: float = deg_to_rad(latitude_deg)
	var longitude_rad: float = deg_to_rad(longitude_deg)
	var horizontal: float = cos(latitude_rad)
	return Vector3(horizontal * sin(longitude_rad), sin(latitude_rad),
		horizontal * cos(longitude_rad))
