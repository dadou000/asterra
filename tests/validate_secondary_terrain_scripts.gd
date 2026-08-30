extends Node
## Parse/load smoke test for dependency-heavy terrain/authoring scripts that are not
## necessarily instantiated by the active gameplay validation scene. This test is
## launched through a .tscn so project autoload globals resolve exactly as they do
## in the editor/runtime instead of using --script MainLoop semantics.

const APPLY_PLANNER := preload("res://scripts/world_authoring/world_authoring_apply_planner.gd")
const RUNTIME_HOST := preload("res://scripts/world_authoring/world_authoring_runtime_host.gd")
const RUNTIME_HOST_PHASE24 := preload("res://scripts/world_authoring/world_authoring_runtime_host_phase24.gd")
const CELESTIAL_PREVIEW := preload("res://scripts/world_authoring/celestial_body_preview_runtime.gd")
const GENERATION_PROFILE := preload("res://scripts/world_authoring/model/generation_authoring_profile.gd")
const TERRAIN_PROFILE := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")
const SLOT_SCRIPT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const DISPLACEMENT_RUNTIME := preload("res://scripts/world_authoring/terrain_displacement_runtime.gd")

const SCRIPT_PATHS := [
	"res://scripts/terrain/planet_height_store_procedural.gd",
	"res://scripts/terrain/spherical_geometry_clipmap_blankaware.gd",
	"res://scripts/world_authoring/authored_water_runtime_query.gd",
	"res://scripts/world_authoring/authored_water_runtime_spatial.gd",
	"res://scripts/world_authoring/celestial_body_preview_runtime.gd",
	"res://scripts/world_authoring/model/star_authoring_profile.gd",
	"res://scripts/world_authoring/terrain_displacement_runtime.gd",
	"res://scripts/world_authoring/world_authoring_apply_planner.gd",
	"res://scripts/world_authoring/world_authoring_editor_live_phase18.gd",
	"res://scripts/world_authoring/world_authoring_editor_live_phase20.gd",
	"res://scripts/world_authoring/world_authoring_editor_live_phase22.gd",
	"res://scripts/world_authoring/world_authoring_editor_live_phase23.gd",
	"res://scripts/world_authoring/world_authoring_runtime_host.gd",
	"res://scripts/world_authoring/world_authoring_runtime_host_phase24.gd",
	"res://tools/terrain_region_compiler.gd",
]

class FakeRuntimeMain extends Node:
	var cfg: Resource
	var sky_mat: ShaderMaterial
	var _rebaking: bool = false
	var rebake_calls: int = 0

	func _on_rebake_requested() -> void:
		rebake_calls += 1


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
	if not _validate_blank_and_star_authoring():
		return
	if not _validate_blank_shader_displacement():
		return
	if not _validate_celestial_multi_preview():
		return
	if not _validate_hot_apply_execution():
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
	profile = _active_profile(next)
	profile.call("set_runtime_shader_uniform", "terrain_ground", "u_surface_bias", 0.125)
	plan = APPLY_PLANNER.build(previous, next, WorldAuthoringSession.ApplyScope.HOT)
	if not bool(plan.get("runtime_shader")) or not bool(plan.get("hot")) \
			or bool(plan.get("full_rebuild")) or bool(plan.get("clipmap")) \
			or bool(plan.get("biome")) or bool(plan.get("water_geometry")):
		_fail("APPLY_PLANNER_FAILED: direct runtime shader override escaped HOT path")
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

	print("APPLY_PLANNER_OK: HOT/local/runtime-shader/clipmap edits avoid PlanetBake; generator edits require it")
	return true


func _validate_blank_and_star_authoring() -> bool:
	var pair: Array = _fresh_system_pair()
	var previous: Resource = pair[0]
	var next: Resource = pair[1]
	var terrain: Resource = _active_terrain(next)
	terrain.set(&"generation_mode", TERRAIN_PROFILE.GenerationMode.BLANK)
	var generation: Resource = terrain.get(&"generation_profile") as Resource
	generation.set(&"ocean_fraction", 0.21)
	var plan: Dictionary = APPLY_PLANNER.build(previous, next,
		WorldAuthoringSession.ApplyScope.FULL_REBUILD)
	if not bool(plan.get("blank_terrain")) or not bool(plan.get("terrain_backend")) \
			or not bool(plan.get("clipmap")) or bool(plan.get("full_rebuild")) \
			or bool(plan.get("sculpt")):
		_fail("BLANK_TERRAIN_MODE_FAILED: Blank requested generator/sculpt heightfield work")
		return false

	var blank_previous: Resource = next.duplicate(true)
	var blank_next: Resource = next.duplicate(true)
	var blank_terrain: Resource = _active_terrain(blank_next)
	blank_terrain.call("create_biome_layer", "Blank Custom Biome")
	plan = APPLY_PLANNER.build(blank_previous, blank_next,
		WorldAuthoringSession.ApplyScope.TILES)
	if not bool(plan.get("blank_terrain")) or not bool(plan.get("biome")) \
			or bool(plan.get("full_rebuild")) or bool(plan.get("sculpt")):
		_fail("BLANK_TERRAIN_MODE_FAILED: custom biome paint did not remain local")
		return false
	print("BLANK_TERRAIN_MODE_OK: no generated heightmap/bake contract; custom biome paint remains local")

	var session := WorldAuthoringSession.new()
	session.bootstrap_from_generation_profile(GENERATION_PROFILE.new())
	var star: Resource = session.create_body("CI Helion", BODY_SCRIPT.BodyType.STAR)
	if star == null or session.active_star_profile() == null:
		_fail("STAR_AUTHORING_FAILED: new star has no star profile")
		return false
	var star_profile: Resource = session.active_star_profile()
	star_profile.set(&"effective_temperature_k", 9100.0)
	star_profile.set(&"flare_activity", 2.5)
	plan = APPLY_PLANNER.build(session.applied_system, session.staged_system,
		WorldAuthoringSession.ApplyScope.FULL_REBUILD)
	if not bool(plan.get("star")) or not bool(plan.get("hot")) \
			or bool(plan.get("full_rebuild")):
		_fail("STAR_AUTHORING_FAILED: stellar edit requested terrestrial PlanetBake")
		return false
	if not is_equal_approx(float(star_profile.get(&"effective_temperature_k")), 9100.0):
		_fail("STAR_AUTHORING_FAILED: stellar profile did not retain custom parameters")
		return false
	print("STAR_AUTHORING_OK: first-class customizable star profile avoids terrestrial PlanetBake")
	return true


func _validate_blank_shader_displacement() -> bool:
	var terrain: Resource = TERRAIN_PROFILE.new()
	terrain.set(&"generation_mode", TERRAIN_PROFILE.GenerationMode.BLANK)
	terrain.call("ensure_valid")
	if bool(terrain.call("uses_generated_heightmap")):
		_fail("BLANK_SHADER_DISPLACEMENT_FAILED: Blank still reports a generated heightmap")
		return false

	var slot: Resource = terrain.call("create_shader_slot",
		SLOT_SCRIPT.Domain.DISPLACEMENT, "CI Blank Mountains") as Resource
	if slot == null:
		_fail("BLANK_SHADER_DISPLACEMENT_FAILED: displacement slot was not created")
		return false
	slot.set(&"strength", 2.0)
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		_fail("BLANK_SHADER_DISPLACEMENT_FAILED: displacement graph was not created")
		return false
	var output_id: String = ""
	for node_value: Variant in graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		if String(node.get("type", "")) == "OUTPUT_DISPLACEMENT":
			output_id = String(node.get("id", ""))
			break
	if output_id.is_empty():
		_fail("BLANK_SHADER_DISPLACEMENT_FAILED: graph has no displacement output")
		return false
	var constant_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(120.0, 120.0), {"value": 37.5}))
	if constant_id.is_empty() or not bool(graph.call("connect_nodes",
		constant_id, 0, output_id, 0)):
		_fail("BLANK_SHADER_DISPLACEMENT_FAILED: test graph could not connect to output")
		return false

	var runtime: Node = DISPLACEMENT_RUNTIME.new() as Node
	if runtime == null:
		_fail("BLANK_SHADER_DISPLACEMENT_FAILED: runtime compiler could not instantiate")
		return false
	var stats: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	var height_m: float = float(runtime.call("evaluate_height",
		Vector3.RIGHT, 0.0, 0, 0, 0.0))
	runtime.free()
	if not bool(stats.get("active", false)):
		_fail("BLANK_SHADER_DISPLACEMENT_FAILED: compiled displacement program is inactive")
		return false
	if not is_equal_approx(height_m, 75.0):
		_fail("BLANK_SHADER_DISPLACEMENT_FAILED: expected 75 m shader terrain, got %.6f m" % height_m)
		return false
	print("BLANK_SHADER_DISPLACEMENT_OK: generated height off; shared bytecode produces 75.0 m physical shader terrain")
	return true


func _validate_celestial_multi_preview() -> bool:
	var session := WorldAuthoringSession.new()
	session.bootstrap_from_generation_profile(GENERATION_PROFILE.new())
	var parent: Resource = session.active_body()
	if parent == null:
		_fail("CELESTIAL_SYSTEM_FRAME_FAILED: bootstrap has no parent planet")
		return false
	var parent_id: String = String(parent.get(&"body_id"))
	var moon: Resource = session.create_body("CI Moon", BODY_SCRIPT.BodyType.MOON, parent_id)
	if moon == null:
		_fail("CELESTIAL_SYSTEM_FRAME_FAILED: moon creation failed")
		return false
	var orbit: Resource = moon.get(&"orbit") as Resource
	var minimum_separation: float = float(parent.get(&"radius_m")) + float(moon.get(&"radius_m"))
	if orbit == null or float(orbit.get(&"semi_major_axis_m")) <= minimum_separation:
		_fail("CELESTIAL_SYSTEM_FRAME_FAILED: child body retained zero/overlapping default orbit")
		return false

	var preview: Node3D = CELESTIAL_PREVIEW.new() as Node3D
	if preview == null:
		_fail("CELESTIAL_SYSTEM_FRAME_FAILED: preview runtime did not instantiate")
		return false
	add_child(preview)
	preview.call("show_system", session.staged_system, parent_id, parent_id)
	if int(preview.call("preview_body_count")) != 2:
		preview.free()
		_fail("CELESTIAL_SYSTEM_FRAME_FAILED: parent selection did not retain both bodies")
		return false
	var parent_world: Vec3D = preview.call("body_world_position", parent_id) as Vec3D
	var parent_selected_center: Vec3D = preview.call("selected_center_world") as Vec3D
	if parent_world == null or parent_world.length_sq() > 1.0e-6 \
			or parent_selected_center == null or parent_selected_center.length_sq() > 1.0e-6:
		preview.free()
		_fail("CELESTIAL_SYSTEM_FRAME_FAILED: root planet moved away from system origin")
		return false
	if float(preview.call("family_frame_radius_m")) <= float(parent.get(&"radius_m")):
		preview.free()
		_fail("CELESTIAL_SYSTEM_FRAME_FAILED: parent family extent ignored moon orbit")
		return false

	var moon_id: String = String(moon.get(&"body_id"))
	preview.call("show_system", session.staged_system, moon_id, parent_id)
	if int(preview.call("preview_body_count")) != 2:
		preview.free()
		_fail("CELESTIAL_SYSTEM_FRAME_FAILED: moon selection discarded its parent planet")
		return false
	var parent_after_select: Vec3D = preview.call("body_world_position", parent_id) as Vec3D
	var moon_world: Vec3D = preview.call("body_world_position", moon_id) as Vec3D
	var moon_selected_center: Vec3D = preview.call("selected_center_world") as Vec3D
	if parent_after_select == null or parent_after_select.length_sq() > 1.0e-6:
		preview.free()
		_fail("CELESTIAL_SYSTEM_FRAME_FAILED: selecting moon re-centred the parent planet")
		return false
	if moon_world == null or moon_world.length() <= minimum_separation \
			or moon_selected_center == null \
			or moon_selected_center.sub(moon_world).length() > 0.001:
		preview.free()
		_fail("CELESTIAL_SYSTEM_FRAME_FAILED: moon did not retain its absolute orbital centre")
		return false
	if float(preview.call("family_frame_radius_m")) <= float(moon.get(&"radius_m")):
		preview.free()
		_fail("CELESTIAL_SYSTEM_FRAME_FAILED: moon family extent ignored parent orbit")
		return false

	# Phase 24 invariant: selecting an orbital child cannot make the detailed root
	# disappear, and the one global atmosphere remains owned by that detailed root.
	var phase24_host: Node = RUNTIME_HOST_PHASE24.new()
	phase24_host.set("_detailed_runtime_body_id", parent_id)
	phase24_host.set("_authoring_session", session)
	phase24_host.set("_celestial_preview", preview)
	if not bool(phase24_host.call("detailed_runtime_should_be_visible",
			session.staged_system, false)):
		phase24_host.free()
		preview.free()
		_fail("CELESTIAL_SYSTEM_FRAME_FAILED: moon selection hid resident root detail")
		return false
	phase24_host.call("_update_sky_owner", session.staged_system, moon, true)
	if String(phase24_host.get("_sky_owner_body_id")) != parent_id:
		phase24_host.free()
		preview.free()
		_fail("CELESTIAL_SYSTEM_FRAME_FAILED: moon selection stole the root atmosphere")
		return false
	phase24_host.free()
	preview.free()

	# Applying an orbital child must never redirect the singleton root PlanetBake to
	# that child's radius at system origin. Authoring state can apply, but runtime
	# generation remains untouched until non-zero body centres are supported.
	var fake_main := FakeRuntimeMain.new()
	fake_main.cfg = GENERATION_PROFILE.new()
	var host: Node = RUNTIME_HOST.new()
	host.set("_main", fake_main)
	host.set("_runtime_applied_snapshot", session.applied_system.duplicate(true))
	host.set("_pending_apply_scope", WorldAuthoringSession.ApplyScope.FULL_REBUILD)
	host.call("_on_runtime_apply_requested", session.staged_system)
	if fake_main.rebake_calls != 0:
		host.free()
		fake_main.free()
		_fail("CELESTIAL_SYSTEM_FRAME_FAILED: orbital moon Apply invoked root PlanetBake")
		return false
	host.free()
	fake_main.free()
	print("CELESTIAL_SYSTEM_FRAME_OK: absolute centres + persistent root detail/atmosphere + safe orbital Apply")
	return true


func _validate_hot_apply_execution() -> bool:
	var pair: Array = _fresh_system_pair()
	var previous: Resource = pair[0]
	var next: Resource = pair[1]
	var profile: Resource = _active_profile(next)
	var atmosphere: Resource = profile.get(&"atmosphere") as Resource
	atmosphere.set(&"cloud_coverage", 0.71)

	var fake_main := FakeRuntimeMain.new()
	fake_main.cfg = GENERATION_PROFILE.new()
	var host: Node = RUNTIME_HOST.new()
	host.set("_main", fake_main)
	host.set("_runtime_applied_snapshot", previous)
	host.set("_pending_apply_scope", WorldAuthoringSession.ApplyScope.HOT)
	host.call("_on_runtime_apply_requested", next)
	if fake_main.rebake_calls != 0:
		host.free()
		fake_main.free()
		_fail("APPLY_EXECUTION_FAILED: HOT atmosphere Apply invoked PlanetBake")
		return false

	pair = _fresh_system_pair()
	previous = pair[0]
	next = pair[1]
	profile = _active_profile(next)
	var water: Resource = profile.get(&"water") as Resource
	water.set(&"wave_frequency_scale", 1.23)
	host.set("_runtime_applied_snapshot", previous)
	host.set("_pending_apply_scope", WorldAuthoringSession.ApplyScope.HOT)
	host.call("_on_runtime_apply_requested", next)
	if fake_main.rebake_calls != 0:
		host.free()
		fake_main.free()
		_fail("APPLY_EXECUTION_FAILED: HOT wave Apply invoked PlanetBake")
		return false

	pair = _fresh_system_pair()
	previous = pair[0]
	next = pair[1]
	profile = _active_profile(next)
	profile.call("set_runtime_shader_uniform", "atmosphere_sky", "u_mie_g", 0.61)
	host.set("_runtime_applied_snapshot", previous)
	host.set("_pending_apply_scope", WorldAuthoringSession.ApplyScope.HOT)
	host.call("_on_runtime_apply_requested", next)
	if fake_main.rebake_calls != 0:
		host.free()
		fake_main.free()
		_fail("APPLY_EXECUTION_FAILED: runtime shader Apply invoked PlanetBake")
		return false

	host.free()
	fake_main.free()
	print("APPLY_EXECUTION_OK: HOT atmosphere/wave/runtime-shader Apply performs zero PlanetBake calls")
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