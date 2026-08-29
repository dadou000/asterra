extends SceneTree
## Headless invariants for the first Planet Studio implementation slice.

const SESSION_SCRIPT := preload("res://scripts/world_authoring/world_authoring_session.gd")
const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")

func _init() -> void:
	var session: RefCounted = SESSION_SCRIPT.new()
	session.bootstrap_from_current_world()
	_assert(session.staged_system != null, "staged system missing")
	_assert(session.applied_system != null, "applied system missing")
	var bodies: Array = session.staged_system.get(&"bodies")
	_assert(bodies.size() == 1, "bootstrap must import exactly the current Asterra body")
	var body: Resource = session.active_body()
	_assert(body != null, "active body missing")
	_assert(is_equal_approx(float(body.get(&"radius_m")), 1000000.0), "Planet Studio did not import Asterra radius")

	var planet_profile: Resource = body.get(&"planet_profile") as Resource
	var terrain: Resource = planet_profile.get(&"terrain") as Resource
	var generation: Resource = terrain.get(&"generation_profile") as Resource
	_assert(int(generation.get(&"world_seed")) == 4707684752384786688, "Planet Studio lost the 64-bit world seed")
	_assert(is_equal_approx(float(generation.get(&"ocean_fraction")), 0.62), "Planet Studio did not import ocean fraction")
	_assert(int(generation.get(&"quadtree_max_depth")) == 10, "Planet Studio did not import world.tres streaming override")

	var original_radius := float(body.get(&"radius_m"))
	session.stage_set(body, &"radius_m", original_radius + 1234.0, SESSION_SCRIPT.ApplyScope.FULL_REBUILD, "test radius")
	_assert(session.dirty, "edit did not mark session dirty")
	_assert(session.can_undo(), "edit did not create undo history")
	session.undo()
	body = session.active_body()
	_assert(is_equal_approx(float(body.get(&"radius_m")), original_radius), "undo did not restore radius")
	_assert(not session.dirty, "undo to bootstrap state should restore clean state")
	session.redo()
	body = session.active_body()
	_assert(is_equal_approx(float(body.get(&"radius_m")), original_radius + 1234.0), "redo did not restore radius edit")

	var created: Resource = session.create_body("CI Moon", BODY_SCRIPT.BodyType.MOON, String(body.get(&"body_id")))
	_assert(created != null, "create body failed")
	bodies = session.staged_system.get(&"bodies")
	_assert(bodies.size() == 2, "created body missing from system")
	_assert(String(session.active_body().get(&"body_id")) == String(created.get(&"body_id")), "created body was not selected")

	var preset_path := "user://world_authoring/tests/phase0_roundtrip.tres"
	_assert(session.save_preset(preset_path) == OK, "preset save failed")
	session.revert()
	_assert(session.load_preset(preset_path) == OK, "preset load failed")
	bodies = session.staged_system.get(&"bodies")
	_assert(bodies.size() == 2, "preset did not round-trip body list")
	session.apply()
	_assert(not session.dirty, "apply did not clear dirty state")

	var scene_resource: Resource = ResourceLoader.load("res://scenes/world_authoring/PlanetStudio.tscn")
	_assert(scene_resource is PackedScene, "PlanetStudio scene failed to load")
	print("WORLD_AUTHORING_PHASE0_OK")
	quit(0)

func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	push_error("WORLD_AUTHORING_PHASE0_FAILED: %s" % message)
	quit(1)
