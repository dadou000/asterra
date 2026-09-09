extends Node
## CPU-only Phase 3 policy tests. These run without RenderingDevice and validate
## sparse identity/allocation/frontier rules before GPU atlas state is attached.


func _ready() -> void:
	_test_tile_key_roundtrip()
	_test_pool_allocation_reuse()
	_test_frontier_wake_reachability()
	_test_sleep_hysteresis()
	_test_wet_freeze_and_thaw()
	print("SPARSE_HYDRO_SCHEDULER: PASS")
	get_tree().quit(0)


func _test_tile_key_roundtrip() -> void:
	for face in 6:
		for level in [0, 1, 4, 9, 16, HydroTileKey.MAX_LEVEL]:
			var side := 1 << level
			var samples: Array[Vector2i] = [
				Vector2i.ZERO,
				Vector2i(side - 1, side - 1),
				Vector2i(side / 3, side / 2),
			]
			for p in samples:
				var key := HydroTileKey.new(face, level, p.x, p.y)
				var decoded := HydroTileKey.unpack(key.packed())
				_require(decoded.face == key.face and decoded.level == key.level
					and decoded.x == key.x and decoded.y == key.y,
					"tile key roundtrip failed: %s -> %s" % [str(key), str(decoded)])

	var k := HydroTileKey.new(2, 8, 117, 203)
	var p := k.parent()
	_require(p.level == 7 and p.x == 58 and p.y == 101, "parent mapping wrong")
	for child_index in 4:
		var c := p.child(child_index)
		_require(c.parent().equals(p), "child/parent hierarchy mismatch")


func _test_pool_allocation_reuse() -> void:
	var pool := HydroTilePool.new(3)
	var a := HydroTileKey.new(0, 5, 3, 4)
	var b := HydroTileKey.new(0, 5, 4, 4)
	var c := HydroTileKey.new(0, 5, 5, 4)
	var d := HydroTileKey.new(0, 5, 6, 4)
	_require(pool.allocate(a) == 0, "first pool slot should be zero")
	_require(pool.allocate(b) == 1, "second pool slot should be one")
	_require(pool.allocate(c) == 2, "third pool slot should be two")
	_require(pool.allocate(d) == -1, "pool capacity must be hard-bounded")
	_require(pool.release(b), "pool release failed")
	_require(pool.allocate(d) == 1, "released slot was not recycled")
	_require(pool.slot_for(a) == 0 and pool.slot_for(c) == 2, "unrelated slot identity changed")


func _test_frontier_wake_reachability() -> void:
	var scheduler := SparseHydroScheduler.new(8)
	scheduler.wake_flux_threshold_m3s = 0.05
	var centre := HydroTileKey.new(1, 6, 20, 20)
	_require(scheduler.wake(centre, 2, "seed") >= 0, "seed tile allocation failed")

	var east := centre.same_face_neighbor(1, 0)
	var rejected := scheduler.report_boundary_flux(centre,
		SparseHydroScheduler.DIR_EAST, 2.0, false)
	_require(rejected == -1 and not scheduler.pool.contains(east),
		"unreachable neighbor must not wake")

	var too_small := scheduler.report_boundary_flux(centre,
		SparseHydroScheduler.DIR_EAST, 0.01, true)
	_require(too_small == -1 and not scheduler.pool.contains(east),
		"sub-threshold flux must not wake neighbor")

	var slot := scheduler.report_boundary_flux(centre,
		SparseHydroScheduler.DIR_EAST, 0.4, true)
	_require(slot >= 0 and scheduler.pool.contains(east),
		"reachable outgoing flux did not wake neighbor")
	var record := scheduler.pool.record(east)
	_require(int(record["state"]) == HydroTilePool.TileState.ACTIVE,
		"new frontier tile is not active")
	_require(int(record["physical_lod"]) == 2,
		"same-face frontier did not inherit physical LOD")

	# Cube seams now use the exact equi-angular topology rather than stopping at
	# face boundaries.
	var edge := HydroTileKey.new(CubeSphere.FACE_NX, 6, (1 << 6) - 1, 10)
	_require(scheduler.wake(edge, 4, "edge_seed") >= 0, "edge tile allocation failed")
	var seam_link := HydroTileTopology.neighbor(edge, SparseHydroScheduler.DIR_EAST)
	_require(not seam_link.is_empty() and bool(seam_link["crossed_face"]),
		"edge fixture did not resolve a cube seam")
	var seam_dest := seam_link["key"] as HydroTileKey
	var seam_slot := scheduler.report_boundary_flux(edge,
		SparseHydroScheduler.DIR_EAST, 1.0, true)
	_require(seam_slot >= 0 and scheduler.pool.contains(seam_dest),
		"cross-face boundary flux did not wake exact destination")
	_require(int(scheduler.pool.record(seam_dest)["physical_lod"]) == 4,
		"cross-face frontier did not inherit physical LOD")


func _test_sleep_hysteresis() -> void:
	var scheduler := SparseHydroScheduler.new(4)
	scheduler.settle_time_s = 2.0
	scheduler.sleep_time_s = 5.0
	var key := HydroTileKey.new(3, 7, 45, 63)
	_require(scheduler.wake(key) >= 0, "hysteresis tile allocation failed")

	# Activity repeatedly resets the quiet timer.
	scheduler.report_activity(key, 0.01, 0.2, 0.0, 0.0, 10.0)
	_require(int(scheduler.pool.record(key)["state"]) == HydroTilePool.TileState.ACTIVE,
		"active tile entered settling")
	_require(float(scheduler.pool.record(key)["quiet_time_s"]) == 0.0,
		"active tile quiet time was not reset")

	scheduler.report_activity(key, 0.001, 0.0, 0.0, 0.0, 1.0)
	_require(scheduler.pool.contains(key), "tile slept before settling threshold")
	scheduler.report_activity(key, 0.001, 0.0, 0.0, 0.0, 1.2)
	_require(int(scheduler.pool.record(key)["state"]) == HydroTilePool.TileState.SETTLING,
		"quiet tile did not enter settling")
	scheduler.report_activity(key, 0.001, 0.0, 0.0, 0.0, 2.0)
	_require(scheduler.pool.contains(key), "tile slept before sleep threshold")
	scheduler.report_activity(key, 0.001, 0.0, 0.0, 0.0, 1.0)
	_require(not scheduler.pool.contains(key), "dry quiet tile did not release after hysteresis")
	_require(scheduler.pool.free_count() == 4, "released sleeping tile did not recycle slot")


func _test_wet_freeze_and_thaw() -> void:
	var scheduler := SparseHydroScheduler.new(2)
	scheduler.settle_time_s = 1.0
	scheduler.sleep_time_s = 2.0
	scheduler.dry_depth_threshold_m = 0.002
	var key := HydroTileKey.new(5, 5, 12, 14)
	var slot := scheduler.wake(key)
	_require(slot >= 0, "wet tile allocation failed")
	scheduler.report_activity(key, 0.8, 0.0, 0.0, 0.0, 1.1)
	scheduler.report_activity(key, 0.8, 0.0, 0.0, 0.0, 1.0)
	_require(scheduler.pool.contains(key), "wet settled tile was incorrectly released")
	_require(int(scheduler.pool.record(key)["state"]) == HydroTilePool.TileState.FROZEN_WATER,
		"wet quiet tile did not freeze")
	_require(scheduler.thaw(key, "boat") == slot, "thaw changed slot identity")
	_require(int(scheduler.pool.record(key)["state"]) == HydroTilePool.TileState.ACTIVE,
		"thaw did not reactivate frozen tile")
	_require(float(scheduler.pool.record(key)["quiet_time_s"]) == 0.0,
		"thaw did not reset quiet timer")


func _require(condition: bool, message: String) -> void:
	if condition:
		return
	push_error("SPARSE_HYDRO_SCHEDULER: " + message)
	get_tree().quit(1)
	set_process(false)
	assert(condition, message)
