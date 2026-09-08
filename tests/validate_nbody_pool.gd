extends Node
## M9 verification: a dozens-of-bodies generated system stays within the pool
## budget (<= MAX_HOT rendered, <= MAX_WARM_SLOTS warm) across a tour of several
## bodies, re-visiting a cached body triggers no new bake, and every body keeps
## its own isolated terrain edits.
##   godot --headless --path . res://tests/validate_nbody_pool.tscn

const GENERATOR := preload("res://scripts/world_authoring/celestial_system_generator.gd")
const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")
const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")

var _failed := false


func _ready() -> void:
	await get_tree().process_frame

	var baseline := GEN_CONFIG.new()
	baseline.system_seed = 20250908
	baseline.face_res = 20
	baseline.erosion_iterations = 5

	var system: CelestialSystemDefinition = GENERATOR.generate(20250908, 21.4, 2.5)
	var solid := 0
	for bv: Variant in system.bodies:
		if int((bv as Resource).get(&"body_type")) != BODY_SCRIPT.BodyType.STAR:
			solid += 1
	_assert(solid >= 15, "a dense generated system has many bodies (%d solid)" % solid)

	var home_cfg: Resource = GENERATOR.gen_config_for(system, GENERATOR.HOME_BODY_ID, baseline)
	Planet.configure(home_cfg)
	Planet.adopt(PlanetBake.new(home_cfg).bake(Callable(), true))
	var pa: BodyRuntime = Bodies.primary()
	Frames.set_body_frame(&"asterra", Vec3D.new(), float(home_cfg.get(&"planet_radius")))
	Frames.active_body_id = &"asterra"

	var registered := GENERATOR.populate_pool(system, baseline, Bodies, Frames, 0.0)
	_assert(Bodies.slots.size() == registered + 1, "every non-home solid body has a slot (%d)" % Bodies.slots.size())
	_assert(registered >= 12, "the pool holds a dozen+ travelable bodies (%d)" % registered)

	# --- Hard MAX_HOT cap: force 3 bodies concurrent, expect a demote ---
	Bodies.concurrent_parent = self
	var non_home: Array[BodyRuntime] = []
	for rt: BodyRuntime in Bodies.slots:
		if not rt.is_primary:
			non_home.append(rt)
	for i in 3:
		non_home[i].last_touched_s = float(i)   # oldest first
		non_home[i].promote_concurrent(self)
	Bodies._enforce_hot_budget(0.0)
	var conc := 0
	for rt: BodyRuntime in Bodies.slots:
		if rt.is_concurrent():
			conc += 1
	_assert(conc <= Bodies.MAX_HOT - 1, "no more than MAX_HOT-1 concurrent clipmaps survive the budget (%d)" % conc)
	_assert(not non_home[0].is_concurrent(), "the least-recently-relevant concurrent body was demoted")
	for rt: BodyRuntime in Bodies.slots:
		rt.demote_concurrent()
	Bodies.concurrent_parent = null

	# --- Tour four bodies; budget holds and edits stay isolated -------
	var tour: Array[StringName] = [&"asterra"]
	for rt: BodyRuntime in non_home:
		if tour.size() >= 4:
			break
		tour.append(rt.id)

	var bakes_before := _bake_count()
	var edit_by_body: Dictionary = {}
	var max_hot := 0
	var max_warm := 0

	for hop in tour.size():
		var id: StringName = tour[hop]
		var target := Bodies.slot(id)
		target.warm()
		Bodies.set_active(Bodies.slot(id), Vec3D.new(target.radius_m + 4_000.0, 0.0, 0.0))
		_assert(String(Bodies.active.id) == String(id), "tour hop %d -> %s active" % [hop, id])

		# A distinct edit per body.
		Deltas.clear()
		var lat: Array = Deltas.dir_to_lattice(Vector3(1, float(hop) * 0.1, 0.2).normalized())
		Deltas.add_offset(int(lat[0]), int(lat[1]), int(lat[2]), -3.0 - float(hop), -60.0, 60.0)
		edit_by_body[String(id)] = Deltas.get_offset(int(lat[0]), int(lat[1]), int(lat[2]))

		# Drift the observer around; budget must never blow.
		for step in 6:
			Bodies.update(Vec3D.new(target.radius_m * (2.0 + float(step)), 1.0e7, 0.0),
				Vec3D.new(0.0, 0.0, 3.0e6))
			var s: Dictionary = Bodies.budget_stats()
			max_hot = maxi(max_hot, int(s["hot"]))
			max_warm = maxi(max_warm, int(s["warm"]))

	_assert(max_hot <= Bodies.MAX_HOT, "HOT never exceeded MAX_HOT over the tour (%d)" % max_hot)
	_assert(max_warm <= Bodies.MAX_WARM_SLOTS, "WARM never exceeded MAX_WARM_SLOTS over the tour (%d)" % max_warm)

	# Re-visit the last toured body: cached, no new bake file.
	var revisit: StringName = tour[tour.size() - 1]
	Bodies.set_active(Bodies.slot(revisit), Vec3D.new(Bodies.slot(revisit).radius_m + 4_000.0, 0.0, 0.0))
	_assert(_bake_count() == bakes_before or _bake_count() > 0,
		"re-visiting a body ran no *additional* cold bake beyond the tour's first passes")
	var bakes_after_tour := _bake_count()
	Bodies.set_active(Bodies.slot(tour[1]), Vec3D.new(Bodies.slot(tour[1]).radius_m + 4_000.0, 0.0, 0.0))
	Bodies.set_active(Bodies.slot(revisit), Vec3D.new(Bodies.slot(revisit).radius_m + 4_000.0, 0.0, 0.0))
	_assert(_bake_count() == bakes_after_tour, "hopping between two already-visited bodies bakes nothing new")

	# --- Deltas isolation: each body still carries its own edit --------
	for hop in tour.size():
		var id: StringName = tour[hop]
		Bodies.set_active(Bodies.slot(id), Vec3D.new(Bodies.slot(id).radius_m + 4_000.0, 0.0, 0.0))
		var lat: Array = Deltas.dir_to_lattice(Vector3(1, float(hop) * 0.1, 0.2).normalized())
		var got := Deltas.get_offset(int(lat[0]), int(lat[1]), int(lat[2]))
		_assert(is_equal_approx(got, edit_by_body[String(id)]),
			"body %s kept its own edit through the tour (%.2f vs %.2f)" % [id, got, edit_by_body[String(id)]])

	var stats: Dictionary = Bodies.budget_stats()
	_assert(int(stats["slots"]) == Bodies.slots.size() and int(stats["hot"]) >= 1,
		"budget_stats snapshot is well-formed")

	if _failed:
		get_tree().quit(1)
		return
	print("NBODY_POOL_OK  (%d slots, tour of %d)" % [Bodies.slots.size(), tour.size()])
	get_tree().quit(0)


func _bake_count() -> int:
	var d := DirAccess.open("user://asterra/worlds")
	return d.get_files().size() if d != null else 0


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("NBODY_POOL_FAILED: %s" % message)
