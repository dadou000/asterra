extends Node
## Parse/load smoke test for dependency-heavy terrain/authoring scripts that are not
## necessarily instantiated by the active gameplay validation scene. This test is
## launched through a .tscn so project autoload globals resolve exactly as they do
## in the editor/runtime instead of using --script MainLoop semantics.

const APPLY_PLANNER := preload("res://scripts/world_authoring/world_authoring_apply_planner.gd")
const GENERATION_PROFILE := preload("res://scripts/world_authoring/model/generation_authoring_profile.gd")

const SCRIPT_PATHS := [
	"res://scripts/terrain/planet_height_store_procedural.gd",
	"res://scripts/world_authoring/authored_water_runtime_query.gd",
	"res://scripts/world_authoring/authored_water_runtime_spatial.gd",
	"res://scripts/world_authoring/world_authoring_apply_planner.gd",
	"res://scripts/world_authoring/world_authoring_runtime_host.gd",
	"res://tools/terrain_region_compiler.gd",
]


func _ready() -> void:
	for script_path: String in SCRIPT_PATHS:
		var resource: Resource = load(script_path)
		if resource == null or not (resource is Script):
			_fail("SECONDARY_TERRAIN_SCRIPT_LOAD_FAILED: %s" % script_path)
			return
		var script: Script = resource as Script
		if not script.can_instantiate():
			_fail("SECONDARY_TERRAIN_SCRIPT_NOT_INSTANTIABLE: %s" % script_path)
			return
		print("SECONDARY_TERRAIN_SCRIPT_LOAD_OK: %s" % script_path)

	if not _validate_apply_planner():
		return
	print("SECONDARY_TERRAIN_SCRIPT_STACK_OK: %d scripts" % SCRIPT_PATHS.size())
	get_tree().quit(0)


func _validate_apply_planner() -> bool:
	var pair: Array = _fresh_system_pair()
	var previous: Resource = pair[0]
	var next: Resource = pair[1]
	var profile: Resource = _active_profile(next)
	var atmosphere: Resource = profile.get(&"atmosphere") as Resource
	atmosphere.set(&"rayleigh_strength", 1.37)
	var plan: Dictionary = APPLY_PLANNER.build(previous, next,
		WorldAuthoringSession.ApplyScope.HOT)
	if not bool(plan.get("atmosphere")) or bool(plan.get("full_rebuild")) \
			or bool(plan.get("clipmap")) or bool(plan.get("biome")):
		_fail("APPLY_PLANNER_FAILED: atmosphere HOT edit requested terrain generation")
		return false

	pair = _fresh_system_pair()
	previous = pair[0]
	next = pair[1]
	profile = _active_profile(next)
	var water: Resource = profile.get(&"water") as Resource
	water.set(&"wave_amplitude_scale", 1.42)
	plan = APPLY_PLANNER.build(previous, next, WorldAuthoringSession.ApplyScope.HOT)
	if not bool(plan.get("water_material")) or bool(plan.get("water_geometry")) \
			or bool(plan.get("full_rebuild")) or bool(plan.get("biome")):
		_fail("APPLY_PLANNER_FAILED: wave HOT edit requested mesh/terrain rebuild")
		return false

	pair = _fresh_system_pair()
	previous = pair[0]
	next = pair[1]
	var terrain: Resource = _active_terrain(next)
	terrain.call("create_biome_layer", "CI Biome")
	plan = APPLY_PLANNER.build(previous, next, WorldAuthoringSession.ApplyScope.TILES)
	if not bool(plan.get("biome")) or bool(plan.get("full_rebuild")) \
			or bool(plan.get("water_geometry")):
		_fail("APPLY_PLANNER_FAILED: biome paint escaped local authored-biome path")
		return false

	pair = _fresh_system_pair()
	previous = pair[0]
	next = pair[1]
	terrain = _active_terrain(next)
	var generation: Resource = terrain.get(&"generation_profile") as Resource
	generation.set(&"detail_amplitude", float(generation.get(&"detail_amplitude")) + 17.0)
	plan = APPLY_PLANNER.build(previous, next, WorldAuthoringSession.ApplyScope.CLIPMAP)
	if not bool(plan.get("clipmap")) or bool(plan.get("full_rebuild")) \
			or bool(plan.get("biome")):
		_fail("APPLY_PLANNER_FAILED: detail-only edit requested PlanetBake")
		return false

	pair = _fresh_system_pair()
	previous = pair[0]
	next = pair[1]
	terrain = _active_terrain(next)
	generation = terrain.get(&"generation_profile") as Resource
	generation.set(&"ocean_fraction", float(generation.get(&"ocean_fraction")) + 0.01)
	plan = APPLY_PLANNER.build(previous, next, WorldAuthoringSession.ApplyScope.FULL_REBUILD)
	if not bool(plan.get("full_rebuild")):
		_fail("APPLY_PLANNER_FAILED: generated macro-field edit did not request PlanetBake")
		return false

	pair = _fresh_system_pair()
	previous = pair[0]
	next = pair[1]
	var body: Resource = next.call("active_body") as Resource
	body.set(&"display_name", "Asterra CI")
	plan = APPLY_PLANNER.build(previous, next, WorldAuthoringSession.ApplyScope.HOT)
	if bool(plan.get("full_rebuild")) or bool(plan.get("clipmap")) \
			or bool(plan.get("biome")) or bool(plan.get("water_geometry")):
		_fail("APPLY_PLANNER_FAILED: metadata-only edit requested terrain work")
		return false

	pair = _fresh_system_pair()
	previous = pair[0]
	next = pair[1]
	body = next.call("active_body") as Resource
	body.set(&"axial_tilt_deg", float(body.get(&"axial_tilt_deg")) + 1.0)
	plan = APPLY_PLANNER.build(previous, next, WorldAuthoringSession.ApplyScope.FULL_REBUILD)
	if not bool(plan.get("frames")) or bool(plan.get("full_rebuild")):
		_fail("APPLY_PLANNER_FAILED: frame-only edit requested PlanetBake")
		return false

	print("APPLY_PLANNER_OK: HOT/local/clipmap edits avoid PlanetBake; generator edits require it")
	return true


func _fresh_system_pair() -> Array:
	var session := WorldAuthoringSession.new()
	var generation: Resource = GENERATION_PROFILE.new()
	session.bootstrap_from_generation_profile(generation)
	return [session.applied_system.duplicate(true), session.staged_system.duplicate(true)]


func _active_profile(system: Resource) -> Resource:
	var body: Resource = system.call("active_body") as Resource
	return body.get(&"planet_profile") as Resource if body != null else null


func _active_terrain(system: Resource) -> Resource:
	var profile: Resource = _active_profile(system)
	return profile.get(&"terrain") as Resource if profile != null else null


func _fail(message: String) -> void:
	push_error(message)
	get_tree().quit(1)
