extends SceneTree
## Headless invariants for the first Planet Studio implementation slice.

func _init() -> void:
	var session := WorldAuthoringSession.new()
	session.bootstrap_from_current_world()
	_assert(session.staged_system != null, "staged system missing")
	_assert(session.applied_system != null, "applied system missing")
	_assert(session.staged_system.bodies.size() == 1, "bootstrap must import exactly the current Asterra body")
	var body := session.active_body()
	_assert(body != null, "active body missing")

	var cfg_resource: Resource = ResourceLoader.load("res://world.tres")
	_assert(cfg_resource is GenConfig, "world.tres is not GenConfig")
	var cfg := cfg_resource as GenConfig
	_assert(is_equal_approx(body.radius_m, cfg.planet_radius), "Planet Studio radius does not round-trip current world.tres")
	_assert(is_equal_approx(body.axial_tilt_deg, cfg.axial_tilt_deg), "Planet Studio axial tilt does not round-trip current world.tres")

	var original_radius := body.radius_m
	session.stage_set(body, &"radius_m", original_radius + 1234.0, WorldAuthoringSession.ApplyScope.FULL_REBUILD, "test radius")
	_assert(session.dirty, "edit did not mark session dirty")
	_assert(session.can_undo(), "edit did not create undo history")
	session.undo()
	body = session.active_body()
	_assert(is_equal_approx(body.radius_m, original_radius), "undo did not restore radius")
	_assert(not session.dirty, "undo to bootstrap state should restore clean state")
	session.redo()
	body = session.active_body()
	_assert(is_equal_approx(body.radius_m, original_radius + 1234.0), "redo did not restore radius edit")

	var created := session.create_body("CI Moon", CelestialBodyDefinition.BodyType.MOON, body.body_id)
	_assert(created != null, "create body failed")
	_assert(session.staged_system.bodies.size() == 2, "created body missing from system")
	_assert(session.active_body().body_id == created.body_id, "created body was not selected")

	var preset_path := "user://world_authoring/tests/phase0_roundtrip.tres"
	_assert(session.save_preset(preset_path) == OK, "preset save failed")
	session.revert()
	_assert(session.load_preset(preset_path) == OK, "preset load failed")
	_assert(session.staged_system.bodies.size() == 2, "preset did not round-trip body list")
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
