extends Node
## CPU-only tests for compact GPU frontier candidate -> stable tile resolution.


func _ready() -> void:
	_test_fail_closed_and_cross_face_wake()
	_test_stale_slot_rejection()
	print("HYDRO_FRONTIER_RESOLVER: PASS")
	get_tree().quit(0)


func _test_fail_closed_and_cross_face_wake() -> void:
	var scheduler := SparseHydroScheduler.new(8)
	scheduler.wake_flux_threshold_m3s = 0.05
	var side := 1 << 6
	var source := HydroTileKey.new(CubeSphere.FACE_PX, 6, side - 1, 23)
	var source_slot := scheduler.wake(source, 3, "seed")
	_require(source_slot >= 0, "source allocation failed")
	var expected_link := HydroTileTopology.neighbor(source, HydroTileTopology.DIR_EAST)
	_require(not expected_link.is_empty() and bool(expected_link["crossed_face"]),
		"fixture does not cross a cube seam")
	var destination := expected_link["key"] as HydroTileKey
	var resolver := HydroFrontierResolver.new(scheduler)
	var candidate: Array[Dictionary] = [{
		"slot": source_slot,
		"direction": HydroTileTopology.DIR_EAST,
		"flux_m3s": 2.5,
	}]

	# Missing reachability provider must never assume a boundary is passable.
	var closed := resolver.resolve_candidates(candidate)
	_require(closed.size() == 1 and not bool(closed[0]["accepted"]),
		"resolver did not fail closed")
	_require(String(closed[0]["reason"]) == "blocked_boundary",
		"unexpected fail-closed reason: %s" % str(closed[0]))
	_require(not scheduler.pool.contains(destination),
		"fail-closed candidate woke destination")

	var seen_link := {}
	var reachability := func(src: HydroTileKey, direction: int, dst: HydroTileKey,
			flux: float, link: Dictionary) -> bool:
		seen_link = link.duplicate(true)
		return src.equals(source) and direction == HydroTileTopology.DIR_EAST \
			and dst.equals(destination) and flux > 2.0
	var accepted := resolver.resolve_candidates(candidate, reachability)
	_require(accepted.size() == 1 and bool(accepted[0]["accepted"]),
		"reachable seam candidate was not accepted: %s" % str(accepted))
	_require(bool(accepted[0]["crossed_face"]),
		"resolved seam lost crossed_face metadata")
	_require(int(accepted[0]["edge_orientation"]) == int(expected_link["edge_orientation"]),
		"resolved seam orientation changed")
	_require(not seen_link.is_empty(), "reachability callback did not receive topology link")
	_require(scheduler.pool.contains(destination), "destination tile was not woken")
	var record := scheduler.pool.record(destination)
	_require(int(record["physical_lod"]) == 3,
		"destination did not inherit source physical LOD")
	_require(int(accepted[0]["destination_slot"]) == scheduler.pool.slot_for(destination),
		"resolver returned wrong destination slot")

	# A second candidate for an already-resident destination must reuse its slot.
	var first_slot := scheduler.pool.slot_for(destination)
	var again := resolver.resolve_candidates(candidate, reachability)
	_require(bool(again[0]["accepted"]) and int(again[0]["destination_slot"]) == first_slot,
		"repeat frontier wake changed resident slot")


func _test_stale_slot_rejection() -> void:
	var scheduler := SparseHydroScheduler.new(2)
	var source := HydroTileKey.new(CubeSphere.FACE_PZ, 4, 5, 5)
	var slot := scheduler.wake(source, 1, "seed")
	_require(slot >= 0, "stale-slot fixture allocation failed")
	_require(scheduler.force_release(source), "source release failed")
	var resolver := HydroFrontierResolver.new(scheduler)
	var result := resolver.resolve_candidates([{
		"slot": slot,
		"direction": HydroTileTopology.DIR_NORTH,
		"flux_m3s": 10.0,
	}], func(_src, _dir, _dst, _flux, _link): return true)
	_require(result.size() == 1 and not bool(result[0]["accepted"]),
		"stale slot candidate was accepted")
	_require(String(result[0]["reason"]) == "stale_source_slot",
		"stale slot reported wrong reason: %s" % str(result[0]))


func _require(condition: bool, message: String) -> void:
	if condition:
		return
	push_error("HYDRO_FRONTIER_RESOLVER: " + message)
	get_tree().quit(1)
	assert(condition, message)
